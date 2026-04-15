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
            // but still need to set dispatcher_name for this node instance
            n->head.dispatcher_name = alloc_dispatcher_name(n);
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
    void *all_handle;                 // dlopen handle for current all.so
    unsigned int reload_gen;          // pathname generation counter — see
                                      // astro_cs_reload for the rationale.
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

// Check if version has changed since last compile. If so, clear the store.
// version = 0 means skip check.
static void
astro_cs_check_version(uint64_t current)
{
    if (current == 0) return;

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
astro_cs_init(const char *store_dir, const char *src_dir, uint64_t version)
{
    astro_cs_resolve_dir(astro_cs.store_dir, ASTRO_CS_DIR_MAX, store_dir);

    // Environment variable overrides src_dir argument
    const char *env = getenv("ASTRO_CS_SRC_DIR");
    astro_cs_resolve_dir(astro_cs.src_dir, ASTRO_CS_DIR_MAX, env ? env : src_dir);

    // Check version and clear stale cache
    if (store_dir) {
        astro_cs_check_version(version);
    }

    if (store_dir) {
        // Clean up any all.<N>.so leftovers from a previous process.  Those
        // are only meaningful while the process that dlopen'd them is still
        // alive; at init time we know every generation is stale.
        char sweep_cmd[ASTRO_CS_PATH_MAX + 32];
        snprintf(sweep_cmd, sizeof(sweep_cmd),
                 "rm -f %s/all.[0-9]*.so", astro_cs.store_dir);
        (void)!system(sweep_cmd);

        // Try to load all.so
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
    // Link to a temp file then atomically rename it over all.so.  This gives
    // two things:
    //   1. dlopen(3) caches handles by inode, so the rename (new inode) lets
    //      the next dlopen of "all.so" actually pick up the freshly built
    //      image instead of returning the cached pre-rebuild handle.
    //   2. No observer ever sees a half-linked / missing all.so — the path
    //      always resolves to a complete .so, old or new.
    fprintf(fp, "all.so: $(OBJS)\n");
    fprintf(fp, "\t$(CC) -shared -o all.tmp.so $^\n");
    fprintf(fp, "\tmv all.tmp.so $@\n");
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

    // Cap parallelism at 2 * nproc — unbounded `-j` spawned hundreds of
    // cc processes on many-core boxes and thrashed the machine.
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    long jobs = ncpu * 2;

    char cmd[ASTRO_CS_PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "make -C %s -j%ld --no-print-directory -s all.so 2>&1 >/dev/null",
             astro_cs.store_dir, jobs);

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
    // hold function pointers into it.  We also can't just re-dlopen
    // "all.so": glibc's dlopen caches handles by pathname (not inode), so
    // dlopen returns the pre-rebuild handle even after make has overwritten
    // / renamed the file.  Work around this by hardlinking (or copying) the
    // freshly-built all.so to a generation-unique filename and dlopening
    // that new path.  Stale all.<N>.so files from earlier runs are swept by
    // astro_cs_init.
    char all_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(all_path, sizeof(all_path), astro_cs.store_dir, "all.so");

    char gen_file[64];
    snprintf(gen_file, sizeof(gen_file), "all.%u.so", ++astro_cs.reload_gen);
    char gen_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(gen_path, sizeof(gen_path), astro_cs.store_dir, gen_file);

    (void)unlink(gen_path);
    if (link(all_path, gen_path) != 0) {
        // EXDEV (cross-filesystem) fallback: copy.
        char cmd[ASTRO_CS_PATH_MAX * 2 + 16];
        snprintf(cmd, sizeof(cmd), "cp -f %s %s", all_path, gen_path);
        if (system(cmd) != 0) return;
    }

    void *new_handle = dlopen(gen_path, RTLD_LAZY);
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
    if (!n) return;

    char sym_name[128];
    snprintf(sym_name, sizeof(sym_name), "SD_%lx",
             (unsigned long)hash_node(n));

    char so_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(so_path, sizeof(so_path), astro_cs.store_dir, "all.so");

    char cmd[ASTRO_CS_PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd),
             "objdump -Cd --no-show-raw-insn %s "
             "| sed -n '/<%s>:/,/^$/p'",
             so_path, sym_name);

    (void)!system(cmd);
}
