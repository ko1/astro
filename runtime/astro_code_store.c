// ASTro Code Store implementation
//
// #include this file from your node.c, AFTER #including astro_node.c.

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Specialized code repository (in-memory cache)
// ---------------------------------------------------------------------------

static struct astro_sc_repo {
    uint32_t size;
    uint32_t capa;

    struct astro_sc_entry {
        node_hash_t hash;
        const char *dispatcher_name;
        node_dispatcher_func_t dispatcher;
    } *entries;
} astro_sc_repo;

static struct astro_sc_entry *
astro_sc_repo_search(node_hash_t h)
{
    for (uint32_t i = 0; i < astro_sc_repo.size; i++) {
        if (astro_sc_repo.entries[i].hash == h) {
            return &astro_sc_repo.entries[i];
        }
    }
    return NULL;
}

static struct astro_sc_entry *
astro_sc_repo_new_entry(void)
{
    if (astro_sc_repo.size < astro_sc_repo.capa) {
        return &astro_sc_repo.entries[astro_sc_repo.size++];
    }
    else {
        uint32_t capa = astro_sc_repo.capa == 0 ? 8 : astro_sc_repo.capa * 2;
        astro_sc_repo.entries = realloc(astro_sc_repo.entries,
                                        sizeof(struct astro_sc_entry) * capa);
        if (astro_sc_repo.entries) {
            astro_sc_repo.capa = capa;
            return astro_sc_repo_new_entry();
        }
        else {
            fprintf(stderr, "astro_code_store: out of memory (capa=%u)\n", capa);
            exit(1);
        }
    }
}

static void
astro_sc_repo_add(node_hash_t h, const char *name, node_dispatcher_func_t func)
{
    struct astro_sc_entry *sc = astro_sc_repo_new_entry();
    sc->hash = h;
    sc->dispatcher_name = name;
    sc->dispatcher = func;
}

static void
astro_sc_repo_clear(void)
{
    astro_sc_repo.size = 0;
}

// ---------------------------------------------------------------------------
// Specialization helpers
// ---------------------------------------------------------------------------

static void
astro_fill_specialized(NODE *n, const char *name, node_dispatcher_func_t func)
{
    n->head.dispatcher_name = name;
    n->head.dispatcher = func;
    n->head.flags.is_specialized = true;
}

void
SPECIALIZE(FILE *fp, NODE *n)
{
    if (n && n->head.kind->specializer) {
        node_hash_t h = HASH(n);
        struct astro_sc_entry *sc = astro_sc_repo_search(h);

        if (sc) {
            if (!n->head.flags.is_specialized) {
                astro_fill_specialized(n, sc->dispatcher_name, sc->dispatcher);
            }
        }
        else if (n->head.flags.is_specializing) {
            // recursive specializing - skip
        }
        else {
            n->head.flags.is_specializing = true;
            (*n->head.kind->specializer)(fp, n, false);
            n->head.flags.is_specializing = false;

            astro_sc_repo_add(h, n->head.dispatcher_name, n->head.dispatcher);
        }
    }
}

// ---------------------------------------------------------------------------
// Code Store: state
// ---------------------------------------------------------------------------

#define ASTRO_CS_DIR_MAX 512
#define ASTRO_CS_PATH_MAX 1024

// Join dir and filename into buf. Returns buf.
static char *
astro_cs_path(char *buf, size_t bufsz, const char *dir, const char *file)
{
    snprintf(buf, bufsz, "%.*s/%s", (int)(bufsz / 2), dir, file);
    return buf;
}

static struct astro_cs_state {
    char store_dir[ASTRO_CS_DIR_MAX];
    char src_dir[ASTRO_CS_DIR_MAX];   // where node.h, node_eval.c live
    void *all_handle;                 // dlopen handle for all.so
} astro_cs;

// ---------------------------------------------------------------------------
// astro_cs_init: load all.so from store_dir
// ---------------------------------------------------------------------------

// Resolve dir to absolute path and store into buf.
static void
astro_cs_resolve_dir(char *buf, size_t bufsz, const char *dir)
{
    if (!dir) return;
    if (dir[0] == '/') {
        snprintf(buf, bufsz, "%s", dir);
    }
    else {
        char *cwd = getcwd(NULL, 0);
        if (cwd) {
            snprintf(buf, bufsz, "%s/%s", cwd, dir);
            free(cwd);
        }
        else {
            snprintf(buf, bufsz, "%s", dir);
        }
    }
}

void
astro_cs_init(const char *store_dir, const char *src_dir)
{
    astro_cs_resolve_dir(astro_cs.store_dir, ASTRO_CS_DIR_MAX, store_dir);

    // Environment variable overrides src_dir argument
    const char *env = getenv("ASTRO_CS_SRC_DIR");
    astro_cs_resolve_dir(astro_cs.src_dir, ASTRO_CS_DIR_MAX, env ? env : src_dir);

    // Try to load all.so
    if (store_dir) {
        char path[ASTRO_CS_PATH_MAX];
        astro_cs_path(path, sizeof(path), astro_cs.store_dir, "all.so");
        astro_cs.all_handle = dlopen(path, RTLD_LAZY);
        // NULL is fine: no all.so yet
    }
}

// ---------------------------------------------------------------------------
// astro_cs_load: look up SD_<hash> and apply to node
// ---------------------------------------------------------------------------

bool
astro_cs_load(NODE *n)
{
    if (!astro_cs.all_handle) return false;

    node_hash_t h = hash_node(n);

    // Check in-memory cache first
    struct astro_sc_entry *sc = astro_sc_repo_search(h);
    if (sc) {
        astro_fill_specialized(n, sc->dispatcher_name, sc->dispatcher);
        return true;
    }

    // Look up in all.so
    char sym_name[128];
    snprintf(sym_name, sizeof(sym_name), "SD_%lx", (unsigned long)h);

    node_dispatcher_func_t func =
        (node_dispatcher_func_t)dlsym(astro_cs.all_handle, sym_name);

    if (func) {
        const char *name = alloc_dispatcher_name(n);
        astro_fill_specialized(n, name, func);
        astro_sc_repo_add(h, name, func);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// astro_cs_compile: generate SD_<hash>.c
// ---------------------------------------------------------------------------

void
astro_cs_compile(NODE *entry)
{
    if (!entry || !entry->head.kind->specializer) return;

    node_hash_t h = HASH(entry);
    char filename[128];
    snprintf(filename, sizeof(filename), "SD_%lx.c", (unsigned long)h);
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, filename);

    // Create store_dir if it doesn't exist
    mkdir(astro_cs.store_dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "astro_cs_compile: cannot open %s\n", path);
        return;
    }

    // Header: include language files (absolute paths) + provide dispatch_info stub
    fprintf(fp, "// Auto-generated by ASTro Code Store\n");
    fprintf(fp, "#include \"%s/node.h\"\n", astro_cs.src_dir);
    fprintf(fp, "\n");
    fprintf(fp, "static void dispatch_info(CTX *c, NODE *n, bool end) {\n");
    fprintf(fp, "    (void)c; (void)n; (void)end;\n");
    fprintf(fp, "}\n\n");
    fprintf(fp, "#include \"%s/node_eval.c\"\n\n", astro_cs.src_dir);

    // Generate specialized code (entry is public, children are static)
    astro_sc_repo_clear();
    (*entry->head.kind->specializer)(fp, entry, true);

    fclose(fp);
}

// ---------------------------------------------------------------------------
// astro_cs_build: compile all SD_*.c → all.so
// ---------------------------------------------------------------------------

void
astro_cs_build(void)
{
    char makefile_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(makefile_path, sizeof(makefile_path),
                  astro_cs.store_dir, "Makefile");

    FILE *fp = fopen(makefile_path, "w");
    if (!fp) {
        fprintf(stderr, "astro_cs_build: cannot create Makefile\n");
        return;
    }

    fprintf(fp, "CC ?= gcc\n");
    fprintf(fp, "CFLAGS ?= -O3 -fPIC\n");
    fprintf(fp, "\n");
    fprintf(fp, "SRCS = $(wildcard SD_*.c)\n");
    fprintf(fp, "OBJS = $(SRCS:.c=.o)\n");
    fprintf(fp, "\n");
    fprintf(fp, "all: all.so\n");
    fprintf(fp, "\n");
    fprintf(fp, "all.so: $(OBJS)\n");
    fprintf(fp, "\t$(CC) -shared -o $@ $^\n");
    fprintf(fp, "\n");
    fprintf(fp, "%%.o: %%.c\n");
    fprintf(fp, "\t$(CC) $(CFLAGS) -c $< -o $@\n");
    fprintf(fp, "\n");
    fprintf(fp, "clean:\n");
    fprintf(fp, "\trm -f *.o all.so\n");

    fclose(fp);

    char cmd[ASTRO_CS_PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "make -C %s -j --no-print-directory -s all.so",
             astro_cs.store_dir);

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "astro_cs_build: make failed (exit %d)\n", ret);
    }
}

// ---------------------------------------------------------------------------
// astro_cs_reload: close and reopen all.so
// ---------------------------------------------------------------------------

void
astro_cs_reload(void)
{
    if (astro_cs.all_handle) {
        dlclose(astro_cs.all_handle);
        astro_cs.all_handle = NULL;
    }

    // Clear in-memory cache (old function pointers are now invalid)
    astro_sc_repo_clear();

    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "all.so");
    astro_cs.all_handle = dlopen(path, RTLD_LAZY);
}

// ---------------------------------------------------------------------------
// astro_cs_disasm: show disassembly of specialized dispatcher
// ---------------------------------------------------------------------------

void
astro_cs_disasm(NODE *n)
{
    if (!n || !n->head.flags.is_specialized) return;

    char sym_name[128];
    snprintf(sym_name, sizeof(sym_name), "SD_%lx",
             (unsigned long)hash_node(n));

    char so_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(so_path, sizeof(so_path), astro_cs.store_dir, "all.so");

    char cmd[ASTRO_CS_PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd),
             "objdump -d --no-show-raw-insn %s "
             "| sed -n '/<%s>:/,/^$/p'",
             so_path, sym_name);

    (void)!system(cmd);
}
