// ASTro Code Store implementation
//
// #include this file from your node.c, AFTER #including astro_node.c.
//
// Two code variants coexist on disk:
//   - AOT: SD_<Horg>.c / SD_<Horg> symbols.  Profile-free compile.
//   - PGC: SD_<Hopt>.c / SD_<Hopt> symbols.  Profile-baked compile.
// They share `all.so`.  Load order: PGC (via hopt_index.txt key lookup)
// first, AOT fallback.

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
        // Dedup key matches the SD_ name the specializer will emit: Hopt
        // under PGC mode, Horg otherwise.  Mixing would let two nodes with
        // identical Horg but different Hopt collapse into one emission —
        // wrong, since their generated bodies differ (baked prologue etc.).
        node_hash_t h = astro_cs_use_hopt_name ? HOPT(n) : HASH(n);

        if (astro_spec_dedup_has(h)) {
            // already generated in this compile session
            // but still need to set dispatcher_name for this node instance
            n->head.dispatcher_name = alloc_dispatcher_name(n);
        }
        else if (n->head.flags.is_specializing) {
            // Cycle break: this node's specializer is already on the
            // stack via a parent (mutual or self recursion).  We can't
            // emit an inline reference because the SD_ name isn't
            // known yet, so flip no_inline=true and let DISPATCHER_NAME
            // emit a runtime `n->head.dispatcher` indirection.  The
            // pointer will be patched in by astro_cs_load when the
            // outer specialization eventually loads its SD_.
            n->head.flags.no_inline = true;
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
// Hopt index: (Horg, file, line) → Hopt
// Populated by astro_cs_compile(entry, file) in PGC mode; persisted to
// hopt_index.txt.  Read at init so the next process can find a baked SD_
// for an entry it has just parsed.
// ---------------------------------------------------------------------------

struct hopt_entry {
    node_hash_t key;   // hash_merge(Horg, hash(file, line))
    node_hash_t hopt;
};

static struct {
    struct hopt_entry *entries;
    uint32_t size;
    uint32_t capa;
} astro_cs_hopt_index;

static node_hash_t
hopt_index_key(node_hash_t horg, const char *file, int32_t line)
{
    node_hash_t fl = hash_merge(hash_cstr(file ? file : ""),
                                hash_uint32((uint32_t)line));
    return hash_merge(horg, fl);
}

static bool
hopt_index_lookup(node_hash_t key, node_hash_t *hopt_out)
{
    // Linear scan, last-match wins so an append-only file naturally
    // overrides stale entries.
    bool found = false;
    for (uint32_t i = 0; i < astro_cs_hopt_index.size; i++) {
        if (astro_cs_hopt_index.entries[i].key == key) {
            *hopt_out = astro_cs_hopt_index.entries[i].hopt;
            found = true;
        }
    }
    return found;
}

static void
hopt_index_add_mem(node_hash_t key, node_hash_t hopt)
{
    if (astro_cs_hopt_index.size >= astro_cs_hopt_index.capa) {
        uint32_t capa = astro_cs_hopt_index.capa == 0 ? 16
                                                      : astro_cs_hopt_index.capa * 2;
        astro_cs_hopt_index.entries = realloc(astro_cs_hopt_index.entries,
                                              sizeof(struct hopt_entry) * capa);
        if (!astro_cs_hopt_index.entries) {
            fprintf(stderr, "hopt_index: out of memory\n");
            exit(1);
        }
        astro_cs_hopt_index.capa = capa;
    }
    astro_cs_hopt_index.entries[astro_cs_hopt_index.size].key = key;
    astro_cs_hopt_index.entries[astro_cs_hopt_index.size].hopt = hopt;
    astro_cs_hopt_index.size++;
}

static void
hopt_index_load_file(void)
{
    if (astro_cs.store_dir[0] == '\0') return;
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "hopt_index.txt");
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    unsigned long key, hopt;
    while (fscanf(fp, "%lx %lx", &key, &hopt) == 2) {
        hopt_index_add_mem((node_hash_t)key, (node_hash_t)hopt);
    }
    fclose(fp);
}

static void
hopt_index_append_file(node_hash_t key, node_hash_t hopt)
{
    if (astro_cs.store_dir[0] == '\0') return;
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, "hopt_index.txt");
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "%lx %lx\n", (unsigned long)key, (unsigned long)hopt);
    fclose(fp);
}

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

        // Read hopt_index.txt (PGC lookup table).  Missing file is fine.
        hopt_index_load_file();
    }
}

// ---------------------------------------------------------------------------
// astro_cs_load: look up SD_<hash> in all.so and apply to node
// ---------------------------------------------------------------------------
//
// `file` is the source filename of the entry being loaded (used to build
// the (Horg, file, line) index key for PGC lookup).  Pass NULL for non-
// entry nodes or when PGC matching isn't desired — load falls back to AOT
// (SD_<Horg>) in that case.

bool
astro_cs_load(NODE *n, const char *file)
{
    if (!astro_cs.all_handle) return false;

    // Try PGC first: find a Hopt from the index, dlsym PGSD_<Hopt>.
    if (file) {
        node_hash_t horg = HORG(n);
        node_hash_t key = hopt_index_key(horg, file, n->head.line);
        node_hash_t hopt;
        bool tr = getenv("ASTRO_CS_TRACE") != NULL;
        if (tr) {
            fprintf(stderr, "cs_load pgc: horg=%lx file=%s line=%d key=%lx index.size=%u\n",
                    (unsigned long)horg, file, n->head.line, (unsigned long)key,
                    astro_cs_hopt_index.size);
        }
        if (hopt_index_lookup(key, &hopt)) {
            if (tr) fprintf(stderr, "  → hopt=%lx\n", (unsigned long)hopt);
            char sym_name[128];
            snprintf(sym_name, sizeof(sym_name), "PGSD_%lx",
                     (unsigned long)hopt);
            node_dispatcher_func_t func =
                (node_dispatcher_func_t)dlsym(astro_cs.all_handle, sym_name);
            if (func) {
                // Name + hash_opt reflect the Hopt that actually loaded.
                char *name = malloc(strlen(sym_name) + 1);
                strcpy(name, sym_name);
                n->head.dispatcher_name = name;
                n->head.hash_opt = hopt;
                n->head.flags.has_hash_opt = true;
                n->head.dispatcher = func;
                n->head.flags.is_specialized = true;
                return true;
            }
        }
    }

    // AOT fallback: SD_<Horg>.
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
    // Nothing in the code store for this node.  If compiled_only mode had
    // nulled out the dispatcher at ALLOC time (so missing entries would
    // SEGV loudly), restore the default interpreter dispatcher here so
    // cold entries skipped by the PG threshold filter keep running as
    // plain interpreter paths.  This is what lets a single process mix
    // plain-dispatched cold entries with PGSD/SD-dispatched hot ones.
    if (n->head.dispatcher == NULL && n->head.kind->default_dispatcher) {
        n->head.dispatcher = n->head.kind->default_dispatcher;
    }
    return false;
}

// ---------------------------------------------------------------------------
// astro_cs_compile: generate SD_<hash>.c
// ---------------------------------------------------------------------------
//
// Two modes selected by `file`:
//   - file == NULL: AOT.  Filename and internal SD_* names use Horg.
//   - file != NULL: PGC.  Filename and internal SD_* names use Hopt; the
//     (Horg, file, line) → Hopt mapping is appended to hopt_index.txt so
//     the next process can find the baked variant.

void
astro_cs_compile(NODE *entry, const char *file)
{
    if (!entry || !entry->head.kind->specializer) return;

    node_hash_t horg = HORG(entry);
    node_hash_t h;

    if (file) {
        astro_cs_use_hopt_name = 1;  // alloc_dispatcher_name → SD_<Hopt>
        h = HOPT(entry);
    } else {
        astro_cs_use_hopt_name = 0;
        h = horg;
    }

    // Create store_dir/c/ if it doesn't exist
    mkdir(astro_cs.store_dir, 0755);
    char c_dir[ASTRO_CS_PATH_MAX];
    astro_cs_path(c_dir, sizeof(c_dir), astro_cs.store_dir, "c");
    mkdir(c_dir, 0755);

    const char *prefix = file ? "PGSD" : "SD";  // PGC vs AOT
    char filename[128];
    snprintf(filename, sizeof(filename), "c/%s_%lx.c", prefix, (unsigned long)h);
    char path[ASTRO_CS_PATH_MAX];
    astro_cs_path(path, sizeof(path), astro_cs.store_dir, filename);

    // SD_<hash>.c already exists?  Same hash ⇒ same generated content,
    // so rewriting would only bump mtime and force a no-op recompile.
    // Skip the write and the index append — any prior run that produced
    // this file also registered it in hopt_index.txt.
    struct stat st;
    if (stat(path, &st) == 0) {
        astro_cs_use_hopt_name = 0;
        return;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "astro_cs_compile: cannot open %s\n", path);
        astro_cs_use_hopt_name = 0;
        return;
    }

    // Header: include language files (absolute paths) + provide dispatch_info stub
    fprintf(fp, "// Auto-generated by ASTro Code Store\n");
    fprintf(fp, "#include \"%s/node.h\"\n", astro_cs.src_dir);
    fprintf(fp, "\n");
    fprintf(fp, "static void dispatch_info(CTX *c, NODE *n, bool end) {\n");
    fprintf(fp, "    (void)c; (void)n; (void)end;\n");
    fprintf(fp, "}\n\n");
    // NODE_SKIP_COLD: skip the cold helper definitions that node.def guards.
    // The bodies live in abruby.so (compiled from node.c → node_eval.c
    // without this macro); SD_*.o references them via extern declarations
    // in node.h and resolves at dlopen time.  Saves both per-SD .text size
    // and per-SD compile work (preprocessor elides the guarded blocks).
    fprintf(fp, "#define NODE_SKIP_COLD 1\n");
    fprintf(fp, "#include \"%s/node_eval.c\"\n", astro_cs.src_dir);
    fprintf(fp, "#include \"%s/node_dispatch.c\"\n\n", astro_cs.src_dir);

    // Generate specialized code (entry is public, children are static).
    // Set the entry's is_specializing flag so that recursive references
    // back to this node from inside its own subtree (mutual or self
    // recursion via call_N body slots) are caught as a cycle by
    // SPECIALIZE() below and emit a runtime dispatcher read instead
    // of a duplicate compile-time SD_ definition.
    astro_spec_dedup_clear();
    entry->head.flags.is_specializing = true;
    (*entry->head.kind->specializer)(fp, entry, true);
    entry->head.flags.is_specializing = false;
    astro_spec_dedup_add(h);

    fclose(fp);

    if (file) {
        // Index this entry so the next process can find SD_<Hopt>.
        node_hash_t key = hopt_index_key(horg, file, entry->head.line);
        hopt_index_add_mem(key, h);
        hopt_index_append_file(key, h);
        if (getenv("ASTRO_CS_TRACE")) {
            fprintf(stderr, "cs_compile pgc: horg=%lx file=%s line=%d key=%lx hopt=%lx\n",
                    (unsigned long)horg, file, entry->head.line,
                    (unsigned long)key, (unsigned long)h);
        }
    }

    astro_cs_use_hopt_name = 0;
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
    fprintf(fp, "CFLAGS ?= -O3 -fPIC -fno-plt -march=native");
    if (extra_cflags && extra_cflags[0]) {
        fprintf(fp, " %s", extra_cflags);
    }
    // Extra flags for experimentation, settable from outside without
    // recompiling wastro.  Set ASTRO_EXTRA_CFLAGS in the environment.
    const char *env_extra = getenv("ASTRO_EXTRA_CFLAGS");
    if (env_extra && env_extra[0]) {
        fprintf(fp, " %s", env_extra);
    }
    fprintf(fp, "\n");
    // Linker flags.  Default empty; embedders that emit cross-SD
    // direct calls (e.g. castro's SPECIALIZE_node_call) will set
    // -Wl,-Bsymbolic via ASTRO_EXTRA_LDFLAGS so intra-.so symbol
    // references bind locally without a GOT round-trip.
    fprintf(fp, "LDFLAGS ?= ");
    const char *env_ld = getenv("ASTRO_EXTRA_LDFLAGS");
    if (env_ld && env_ld[0]) {
        fprintf(fp, "%s", env_ld);
    }
    fprintf(fp, "\n");
    fprintf(fp, "\n");
    fprintf(fp, "SRCS = $(wildcard c/SD_*.c) $(wildcard c/PGSD_*.c)\n");
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
    fprintf(fp, "\t$(CC) -shared $(LDFLAGS) -o all.tmp.so $^\n");
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
    // Redirect both stdout AND stderr to /dev/null.  The previous
    // form `2>&1 >/dev/null` is a classic shell-redirect bug: it
    // sends stderr to whatever stdout is *currently*, then redirects
    // stdout to null — leaving stderr still attached to the parent
    // (so compile warnings leaked into the host process's stdout
    // capture, breaking output-comparison test runners).
    snprintf(cmd, sizeof(cmd), "make -C %s -j%ld --no-print-directory -s all.so >/dev/null 2>&1",
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

    // If PGC-loaded, point at PGSD_<Hopt>; otherwise the AOT SD_<Horg>
    // that hash_node(n) identifies.
    char sym_name[128];
    if (n->head.flags.has_hash_opt) {
        snprintf(sym_name, sizeof(sym_name), "PGSD_%lx",
                 (unsigned long)n->head.hash_opt);
    } else {
        snprintf(sym_name, sizeof(sym_name), "SD_%lx",
                 (unsigned long)hash_node(n));
    }

    char so_path[ASTRO_CS_PATH_MAX];
    astro_cs_path(so_path, sizeof(so_path), astro_cs.store_dir, "all.so");

    // Prepend a "# compiled with <producer>" banner pulled from the .o's
    // .comment section so the reader can tell which compiler produced the
    // code (handy when toggling CC=clang vs gcc between runs).
    char obj_path[ASTRO_CS_PATH_MAX];
    snprintf(obj_path, sizeof(obj_path), "%s/o/%s.o",
             astro_cs.store_dir, sym_name);

    // Format string has three path expansions (obj_path × 2, so_path × 1)
    // plus sym_name; each path can grow up to ASTRO_CS_PATH_MAX, so size
    // the buffer for 3 × PATH_MAX + slack to keep gcc -Wformat-truncation
    // happy.
    char cmd[ASTRO_CS_PATH_MAX * 4 + 512];
    snprintf(cmd, sizeof(cmd),
             "{ if [ -f %s ]; then "
             "    producer=$(readelf -p .comment %s 2>/dev/null "
             "               | awk '/\\[/{sub(/^ *\\[[^]]*\\] */,\"\"); print; exit}'); "
             "    [ -n \"$producer\" ] && echo \"# compiled with $producer\"; "
             "  fi; "
             "  objdump -Cd --no-show-raw-insn %s "
             "  | sed -n '/<%s>:/,/^$/p'; }",
             obj_path, obj_path, so_path, sym_name);

    (void)!system(cmd);
}
