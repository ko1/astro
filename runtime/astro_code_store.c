// ASTro Code Store implementation
//
// #include this file from your node.c, AFTER #including astro_node.c.

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Specialize dedup: tracks which hashes have been generated during a single
// astro_cs_compile() call to avoid emitting duplicate SD_ functions.
// ---------------------------------------------------------------------------

static struct {
    uint32_t size;
    uint32_t capa;
    node_hash_t *hashes;
} astro_spec_dedup;

static bool
astro_spec_dedup_has(node_hash_t h)
{
    for (uint32_t i = 0; i < astro_spec_dedup.size; i++) {
        if (astro_spec_dedup.hashes[i] == h) return true;
    }
    return false;
}

static void
astro_spec_dedup_add(node_hash_t h)
{
    if (astro_spec_dedup.size >= astro_spec_dedup.capa) {
        uint32_t capa = astro_spec_dedup.capa == 0 ? 16 : astro_spec_dedup.capa * 2;
        astro_spec_dedup.hashes = realloc(astro_spec_dedup.hashes,
                                          sizeof(node_hash_t) * capa);
        if (!astro_spec_dedup.hashes) {
            fprintf(stderr, "astro_spec_dedup: out of memory\n");
            exit(1);
        }
        astro_spec_dedup.capa = capa;
    }
    astro_spec_dedup.hashes[astro_spec_dedup.size++] = h;
}

static void
astro_spec_dedup_clear(void)
{
    astro_spec_dedup.size = 0;
}

// ---------------------------------------------------------------------------
// SPECIALIZE: generate specialized code, dedup within a compile session
// ---------------------------------------------------------------------------

void
SPECIALIZE(FILE *fp, NODE *n)
{
    if (n && n->head.kind->specializer) {
        node_hash_t h = HASH(n);

        if (astro_spec_dedup_has(h)) {
            // already generated in this compile session
        }
        else if (n->head.flags.is_specializing) {
            // recursive specializing - skip
        }
        else {
            n->head.flags.is_specializing = true;
            (*n->head.kind->specializer)(fp, n, false);
            n->head.flags.is_specializing = false;

            astro_spec_dedup_add(h);
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

// Compute FNV-1a hash of a file's contents. Returns 0 on error.
static uint64_t
astro_cs_file_hash(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    uint64_t h = 14695981039346656037ULL;
    const uint64_t FNV_PRIME = 1099511628211ULL;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        h ^= (unsigned char)c;
        h *= FNV_PRIME;
    }
    fclose(fp);
    return h;
}

// Check if node.def has changed since last build. If so, clear the store.
static void
astro_cs_check_version(void)
{
    char node_def_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(node_def_path, sizeof(node_def_path),
                  astro_cs.src_dir, "node.def");

    uint64_t current = astro_cs_file_hash(node_def_path);
    if (current == 0) return; // node.def not found, skip check

    char version_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(version_path, sizeof(version_path),
                  astro_cs.store_dir, "version");

    // Read saved version
    uint64_t saved = 0;
    FILE *fp = fopen(version_path, "r");
    if (fp) {
        if (fscanf(fp, "%lx", &saved) != 1) saved = 0;
        fclose(fp);
    }

    if (saved == current) return; // up to date

    // Version mismatch: clear c/, o/, all.so
    char path[ASTRO_CS_PATH_MAX];

    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "c");
    char cmd[ASTRO_CS_PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)!system(cmd);

    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "o");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)!system(cmd);

    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "all.so");
    remove(path);

    // Save new version
    mkdir(astro_cs.store_dir, 0755);
    fp = fopen(version_path, "w");
    if (fp) {
        fprintf(fp, "%lx\n", current);
        fclose(fp);
    }
}

void
astro_cs_init(const char *store_dir, const char *src_dir)
{
    astro_cs_resolve_dir(astro_cs.store_dir, ASTRO_CS_DIR_MAX, store_dir);

    // Environment variable overrides src_dir argument
    const char *env = getenv("ASTRO_CS_SRC_DIR");
    astro_cs_resolve_dir(astro_cs.src_dir, ASTRO_CS_DIR_MAX, env ? env : src_dir);

    // Check version and clear stale cache
    if (store_dir) {
        astro_cs_check_version();
    }

    // Try to load all.so
    if (store_dir) {
        char path[ASTRO_CS_PATH_MAX];
        astro_cs_path(path, sizeof(path), astro_cs.store_dir, "all.so");
        astro_cs.all_handle = dlopen(path, RTLD_LAZY);
        // NULL is fine: no all.so yet
    }
}

// ---------------------------------------------------------------------------
// astro_cs_load: look up SD_<hash> in all.so and apply to node
// ---------------------------------------------------------------------------

bool
astro_cs_load(NODE *n)
{
    if (!astro_cs.all_handle) return false;

    node_hash_t h = hash_node(n);
    char sym_name[128];
    snprintf(sym_name, sizeof(sym_name), "SD_%lx", (unsigned long)h);

    node_dispatcher_func_t func =
        (node_dispatcher_func_t)dlsym(astro_cs.all_handle, sym_name);

    if (func) {
        n->head.dispatcher_name = alloc_dispatcher_name(n);
        n->head.dispatcher = func;
        n->head.flags.is_specialized = true;
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

    // Create store_dir/c/ if it doesn't exist
    mkdir(astro_cs.store_dir, 0755);
    char c_dir[ASTRO_CS_PATH_MAX];
    astro_cs_path(c_dir, sizeof(c_dir), astro_cs.store_dir, "c");
    mkdir(c_dir, 0755);

    char filename[128];
    snprintf(filename, sizeof(filename), "c/SD_%lx.c", (unsigned long)h);
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, filename);

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
    fprintf(fp, "#include \"%s/node_eval.c\"\n", astro_cs.src_dir);
    fprintf(fp, "#include \"%s/node_dispatch.c\"\n\n", astro_cs.src_dir);

    // Generate specialized code (entry is public, children are static)
    astro_spec_dedup_clear();
    (*entry->head.kind->specializer)(fp, entry, true);

    fclose(fp);
}

// ---------------------------------------------------------------------------
// astro_cs_build: compile all SD_*.c → all.so
// ---------------------------------------------------------------------------

void
astro_cs_build(const char *extra_cflags)
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
    fprintf(fp, "CFLAGS ?= -O3 -fPIC");
    if (extra_cflags && extra_cflags[0]) {
        fprintf(fp, " %s", extra_cflags);
    }
    fprintf(fp, "\n");
    fprintf(fp, "\n");
    fprintf(fp, "SRCS = $(wildcard c/SD_*.c)\n");
    fprintf(fp, "OBJS = $(patsubst c/%%.c,o/%%.o,$(SRCS))\n");
    fprintf(fp, "\n");
    fprintf(fp, "all: all.so\n");
    fprintf(fp, "\n");
    fprintf(fp, "all.so: $(OBJS)\n");
    fprintf(fp, "\t$(CC) -shared -o $@ $^\n");
    fprintf(fp, "\n");
    fprintf(fp, "o/%%.o: c/%%.c | o\n");
    fprintf(fp, "\t$(CC) $(CFLAGS) -c $< -o $@\n");
    fprintf(fp, "\n");
    fprintf(fp, "o:\n");
    fprintf(fp, "\tmkdir -p o\n");
    fprintf(fp, "\n");
    fprintf(fp, "clean:\n");
    fprintf(fp, "\trm -rf o all.so\n");

    fclose(fp);

    char cmd[ASTRO_CS_PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "make -C %s -j --no-print-directory -s all.so 2>&1 >/dev/null",
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
    // Don't dlclose the old handle — previously specialized nodes may still
    // hold function pointers into it.  Just open the new all.so alongside.
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "all.so");
    void *new_handle = dlopen(path, RTLD_LAZY);
    if (new_handle) {
        astro_cs.all_handle = new_handle;
    }
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
