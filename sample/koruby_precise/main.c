/* koruby_precise v2 — main.c: CLI + run + AOT bake (docs/v2_spec.md §3).
 *
 * AOT semantics (koruby pattern, v2_spec §3.4): `--aot-compile` always
 * executes AND bakes at exit; a plain run swaps in cached SDs when the
 * structural hash matches; `--plain` ignores the code store entirely.
 */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "node.h"
#include "astro_node.h"      /* astro_emit_ast_c_program_params (--build) */
#include "astro_code_store.h"
#include "astro_build.h"
#include "precise_gc/gc.h"

struct koruby_option OPTION;

/* The prelude (Enumerable mixin, Proc#curry, minimal Encoding/Exception/Errno,
 * Object#to_enum, ...) lives as ordinary Ruby under prelude/.  The files are
 * read + concatenated in this order at startup and run before the user program;
 * their method-body SDs are baked once into preload_store/all.so (ensure_preload),
 * not into every program's code store. */
#define KORUBY_PRELUDE_DIR  KORUBY_SRC_DIR "/prelude"
/* Also the single source of truth for the baked variant: tools/
 * gen_prelude_blob.rb parses this initializer (KORUBY_PRELUDE_BLOB builds
 * read the blob and never touch the list — hence the unused attribute). */
__attribute__((unused)) static const char *const KORUBY_PRELUDE_FILES[] = {
    "enumerable.rb", "enumerator.rb", "proc.rb", "hash.rb", "set.rb", "encoding.rb", "exception.rb", "numeric.rb",
    "module.rb", "time.rb", "io.rb", "io_buffer.rb", "stringio.rb", "marshal.rb", "system.rb",
};

static void
usage(FILE *fp)
{
    fprintf(fp,
        "usage: koruby_precise [flags] [--] [script.rb] [args...]\n"
        "       koruby_precise -e 'code' [args...]\n"
        "\n"
        "sample flags:\n"
        "  -e CODE       execute CODE\n"
        "  --dump-ast    print the parsed AST and exit\n"
        "  --ccs         clear code_store/ before continuing\n"
        "\n");
    astro_print_build_help(fp);
}

static char *
read_file_all(const char *path, size_t *len_out)
{
    const int fd = strcmp(path, "-") == 0 ? STDIN_FILENO : open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "koruby_precise: cannot open %s\n", path);
        exit(2);
    }
    size_t capa = 1 << 16, len = 0;
    char *buf = malloc(capa);
    if (!buf) abort();
    for (;;) {
        if (len + 1 >= capa) {
            capa *= 2;
            buf = realloc(buf, capa);
            if (!buf) abort();
        }
        const ssize_t n = read(fd, buf + len, capa - len - 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        len += (size_t)n;
    }
    if (fd != STDIN_FILENO) close(fd);
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

#define KORUBY_PRELUDE_NFILES (sizeof(KORUBY_PRELUDE_FILES) / sizeof(KORUBY_PRELUDE_FILES[0]))

#ifdef KORUBY_PRELUDE_BLOB
/* Prelude source baked into the binary (tools/gen_prelude_blob.rb — same
 * concatenation as the file path below).  Used by the wasm interpreter so
 * startup needs no filesystem mount; native keeps reading the files so a
 * prelude edit takes effect without a rebuild. */
extern const char koruby_prelude_blob[];
extern const size_t koruby_prelude_blob_len;
static char *
load_prelude_source(size_t *len_out)
{
    char *const buf = malloc(koruby_prelude_blob_len + 1);
    if (!buf) abort();
    memcpy(buf, koruby_prelude_blob, koruby_prelude_blob_len);
    buf[koruby_prelude_blob_len] = '\0';
    *len_out = koruby_prelude_blob_len;
    return buf;
}
#else
/* Read + concatenate the prelude files (in KORUBY_PRELUDE_FILES order) into one
 * malloc'd, NUL-terminated source buffer. */
static char *
load_prelude_source(size_t *len_out)
{
    char *buf = NULL; size_t total = 0;
    for (size_t i = 0; i < KORUBY_PRELUDE_NFILES; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", KORUBY_PRELUDE_DIR, KORUBY_PRELUDE_FILES[i]);
        size_t flen; char *const fsrc = read_file_all(path, &flen);
        buf = realloc(buf, total + flen + 2);
        if (!buf) abort();
        memcpy(buf + total, fsrc, flen); total += flen;
        buf[total++] = '\n';                          /* keep each file's last line terminated */
        free(fsrc);
    }
    if (!buf) { buf = malloc(1); total = 0; }
    buf[total] = '\0';
    *len_out = total;
    return buf;
}
#endif /* KORUBY_PRELUDE_BLOB */

/* Newest mtime among the prelude files — folded into the preload-store version so
 * editing a prelude .rb invalidates the baked SDs. */
#ifndef __wasi__
static uint64_t
prelude_mtime(void)
{
    uint64_t newest = 0;
    for (size_t i = 0; i < KORUBY_PRELUDE_NFILES; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", KORUBY_PRELUDE_DIR, KORUBY_PRELUDE_FILES[i]);
        struct stat st;
        if (stat(path, &st) == 0 && (uint64_t)st.st_mtime > newest) newest = (uint64_t)st.st_mtime;
    }
    return newest;
}
#endif /* !__wasi__ */

/* Number of code-repo entries that belong to the Enumerable prelude (recorded
 * right after the prelude is parsed, before the user program registers any
 * methods).  Entries [0, g_prelude_repo_count) are prelude bodies; they are
 * baked once into preload_store/all.so (see ensure_preload) instead of into
 * every program's code store. */
static uint32_t g_prelude_repo_count;

/* The prelude's specialized dispatchers live here — a fixed .so, identical for
 * every program, dlopen'd as the code store's preload handle.  Absolute path so
 * it resolves regardless of CWD and survives the harness's `rm -rf code_store`
 * (it is a sibling directory, not the program's code_store). */
#define KORUBY_PRELOAD_DIR  KORUBY_SRC_DIR "/preload_store"
#define KORUBY_PRELOAD_SO   KORUBY_SRC_DIR "/preload_store/all.so"

/* SD compile flags — identical for the prelude bake and the program bake so the
 * two .so's are ABI-compatible. */
static void
koruby_extra_cflags(char *buf, size_t n)
{
    snprintf(buf, n,
             "--param=early-inlining-insns=100"
             " -fcf-protection=none"
             " -I" KORUBY_SRC_DIR
             " -I" ASTRO_RUNTIME_DIR
             " -I" ASTRO_PRISM_INC_DIR
             " -DKORB_BIGNUM=%d"   /* SD は本体と同じ多倍長 backend で焼く */
             " -DBARUBY_GC=%d", KORB_BIGNUM, BARUBY_GC);   /* framework backend-select macros */
}

#ifndef __wasi__
/* mtime of this binary, used both as the staleness reference and as the code
 * store "version" (a rebuilt interpreter changes the SD ABI + the prelude). */
static uint64_t
exe_mtime(void)
{
    struct stat se;
    return stat("/proc/self/exe", &se) == 0 ? (uint64_t)se.st_mtime : 0;
}

/* Code-store version for the preload bake: newest of the binary and the prelude
 * sources, so a rebuilt interpreter OR an edited prelude .rb rebuilds the SDs. */
static uint64_t
preload_version(void)
{
    uint64_t v = exe_mtime(), p = prelude_mtime();
    return p > v ? p : v;
}

/* preload_store/all.so is stale if missing or older than the preload version. */
static bool
preload_stale(void)
{
    struct stat sso;
    if (stat(KORUBY_PRELOAD_SO, &sso) != 0) return true;
    uint64_t v = preload_version();
    return v != 0 && v > (uint64_t)sso.st_mtime;
}
#endif /* !__wasi__ */

/* Bake the fixed prelude's SDs once into preload_store/all.so, then register it
 * as the code store's preload handle.  The prelude is identical across all
 * programs, so this keeps ~70 prelude SDs out of every program's bake (the cold
 * `--aot-compile` cost was almost entirely the prelude — `p 1+2` baked 73
 * prelude SDs vs 1 of its own).  Rebuild only happens during an explicit bake
 * run (`--aot-compile`/PG); a plain cached run just loads the existing .so. */
static void
ensure_preload(void)
{
#ifdef __wasi__
    /* No dlopen, so there is no second .so to split the prelude into: the
     * prelude's SDs are baked into the program's store alongside its own
     * (see bake_from below) and linked into the module by the host build. */
    return;
#else
    if (g_prelude_repo_count == 0) return;

    bool stale = preload_stale();
    if (stale && (OPTION.aot_compile || OPTION.pg_compile)) {
        /* Bake into the preload store (its own store_dir), then switch the code
         * store back to the program's "code_store" in INIT() below.  Passing the
         * binary mtime as the store version makes astro_cs_init clear a stale
         * preload store, so changed prelude/ABI is actually rebuilt (the
         * file-exists skip in astro_cs_compile would otherwise keep stale SDs). */
        astro_cs_init(KORUBY_PRELOAD_DIR, KORUBY_SRC_DIR, preload_version());
        for (uint32_t i = 0; i < g_prelude_repo_count; i++) {
            if (code_repo_skip_specialize_at(i)) continue;
            astro_cs_compile(code_repo_body_at(i), NULL);
        }
        char cflags[2048];
        koruby_extra_cflags(cflags, sizeof(cflags));
        setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);
        astro_cs_build(cflags);
        stale = false;   /* freshly built */
    }
    /* Only load a preload.so we trust: a stale one (older than this binary) may
     * have a mismatched SD ABI, so leave it unloaded — the prelude then runs on
     * the interpreter (or is reported as a compile-miss under --compiled-only),
     * never on stale specialized code. */
    if (!stale) astro_cs_set_preload(KORUBY_PRELOAD_SO);
#endif
}

/* First code-repo entry a bake covers.  Normally the prelude bodies
 * [0, g_prelude_repo_count) are skipped — they live in preload.so — but a
 * dlopen-free host has no preload.so, so it bakes them here instead. */
static uint32_t
bake_from(void)
{
#ifdef __wasi__
    return 0;
#else
    return g_prelude_repo_count;
#endif
}

/* AOT bake: the program AST + every *user* method body (each is its own entry —
 * call sites dispatch through body->head.dispatcher at runtime). */
static void
bake_code_store(NODE *ast)
{
    astro_cs_compile(ast, NULL);
    for (uint32_t i = bake_from(); i < code_repo_count(); i++) {
        if (code_repo_skip_specialize_at(i)) continue;
        astro_cs_compile(code_repo_body_at(i), NULL);
    }

    char extra_cflags[2048];
    koruby_extra_cflags(extra_cflags, sizeof(extra_cflags));
    setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);
    astro_cs_build(extra_cflags);
    astro_cs_reload();
}

/* Load-time specialization for a file loaded AFTER startup (require /
 * require_relative / eval-string), whose AST is parsed at runtime.  Called by
 * the require path once the file is parsed and its offsets are finalized (so the
 * structural hashes used below are correct — see node.c::OPTIMIZE for why binding
 * must not happen earlier, inside ALLOC).  `repo_from` = code_repo_count() taken
 * just BEFORE the file was parsed, so [repo_from, count) are exactly the bodies
 * this file registered.
 *
 *   - consuming run (default / hybrid / --compiled-only): bind the file's AST +
 *     new bodies to already-baked SDs (astro_cs_load) so require'd code runs on
 *     compiled dispatchers instead of the interpreter.
 *   - producing run (--aot-compile / --pg-compile): compile the file's entries
 *     NOW (emit SD_<hash>.c → build → reload) and then bind.  Baking at load —
 *     rather than only in the end-of-run bake_code_store — means the store grows
 *     as files load and survives an early exit / uncaught exception (main()
 *     returns before bake_code_store on an uncaught raise).  astro_cs_reload is
 *     dlclose-free (generation-unique .so), so rebinding mid-run is safe:
 *     dispatchers already pointing into an older generation stay valid.
 *
 * No-op under --plain (ignore all compiled code) and when no store is loadable. */
void
korb_load_time_specialize(NODE *ast, uint32_t repo_from, const char *file)
{
    (void)file;
    if (OPTION.plain || ast == NULL) return;

    if (OPTION.aot_compile || OPTION.pg_compile) {
        astro_cs_compile(ast, NULL);
        for (uint32_t i = repo_from; i < code_repo_count(); i++) {
            if (code_repo_skip_specialize_at(i)) continue;
            astro_cs_compile(code_repo_body_at(i), NULL);
        }
        char extra_cflags[2048];
        koruby_extra_cflags(extra_cflags, sizeof(extra_cflags));
        setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);
        astro_cs_build(extra_cflags);
        astro_cs_reload();
    }

    astro_cs_load(ast, NULL);
    for (uint32_t i = repo_from; i < code_repo_count(); i++)
        astro_cs_load(code_repo_body_at(i), NULL);
}

/* --compiled-only poison: a body that was NOT swapped to a baked SD gets this
 * dispatcher.  Reaching it means an *avoidable* interpreter dispatch would run —
 * an AOT compile-miss.  Report which body + abort.  Installed only at startup
 * (in swap_in_cached_sds), so normal execution pays nothing: the dispatch site
 * is the same indirect call either way, with no per-call branch.
 *
 * @noinline body roots (a method/lambda whose whole body is a single
 * node_make_proc / node_class / node_module) are *compile-exempt*: their entry
 * operand is a per-process NODE* that the SD machinery can't bake as a literal
 * (needs reload-time fixup — an unimplemented framework feature), so they
 * legitimately run on the interpreter and are NOT poisoned.  Without this
 * exemption any real program with a class or proc would false-positive. */
static RESULT
korb_poison_dispatch(CTX *c, NODE *n, VALUE *slots)
{
    (void)c; (void)slots;
    const char *name = "(program root)";
    for (uint32_t i = 0; i < code_repo_count(); i++)
        if (code_repo_body_at(i) == n) { name = code_repo_name_at(i); break; }
    fprintf(stderr,
            "koruby_precise: --compiled-only: AOT compile-miss — interpreter "
            "dispatch reached for body '%s' (node %s); it was not baked "
            "(hash mismatch or not specialized).\n",
            name, n->head.kind ? n->head.kind->default_dispatcher_name : "?");
    fflush(stderr);
    exit(7);   /* harness convention: 7 = interpreter fallback occurred */
}

/* Cached-SD swap: patch the program root + every method body whose hash
 * matches a baked SD.  Returns the number of swapped dispatchers (gate
 * diagnostic: "bare --aot-compile bakes nothing" must stay caught).  In
 * --compiled-only mode, any body that does NOT match a baked SD is poisoned
 * (no normal-execution overhead — this is a one-time startup pass). */
static unsigned int
swap_in_cached_sds(NODE *ast)
{
    unsigned int swaps = 0;
    if (astro_cs_load(ast, NULL)) swaps++;
    else if (OPTION.compiled_only && !ast->head.flags.no_inline) ast->head.dispatcher = korb_poison_dispatch;
    for (uint32_t i = 0; i < code_repo_count(); i++) {
        NODE *body = code_repo_body_at(i);
        if (astro_cs_load(body, NULL)) swaps++;
        else if (OPTION.compiled_only && !body->head.flags.no_inline) body->head.dispatcher = korb_poison_dispatch;
    }
    return swaps;
}

#ifndef KORUBY_EMBED
/* ------------------------------------------------------------------ --build
 * Standalone executable.  Bake every entry (prelude + program + method bodies)
 * into the code store with the compile log on, emit _embed.c — DAG AST
 * builders whose dispatchers are pre-bound to SD_<hash>, plus startup
 * metadata — and hand the source list to the framework toolchain driver.
 * The exe rebuilds both ASTs at startup via ALLOC (symbol names re-intern
 * through the builders' CTX parameter), so it parses nothing at startup.
 *
 * KORUBY_BUILD_TARGET=wasi cross-compiles to wasm32-wasip1 with the wasi-sdk
 * (see wasi/README.md): dlopen-free, wrap bignum, emulated POSIX libs. */
/* GC backend source for the exe link — the Makefile bakes the selected
 * backend's path; default matches the default GC (copy). */
#ifndef KORUBY_GC_SRC
#define KORUBY_GC_SRC ASTRO_RUNTIME_DIR "/precise_gc/gc_copy.c"
#endif

static int
koruby_do_build(CTX *c, NODE *prelude_ast, uint32_t prelude_locals,
                NODE *ast, struct astro_build_config *bcfg)
{
    const char *tgt = getenv("KORUBY_BUILD_TARGET");
    const bool wasi = tgt && strcmp(tgt, "wasi") == 0;

    /* 1. Bake all entries; the log records (hash, SD name) for the emitter.
     * The wasm store is separate: its o/ cache holds wasm32 objects, which
     * must never mix with the native ones. */
    const char *const store = wasi ? "code_store_wasi" : "code_store";
    astro_cs_init(store, KORUBY_SRC_DIR, 0);
    astro_build_begin_aot_session();
    astro_cs_compile(prelude_ast, NULL);
    astro_cs_compile(ast, NULL);
    for (uint32_t i = 0; i < code_repo_count(); i++) {
        if (code_repo_skip_specialize_at(i)) continue;
        astro_cs_compile(code_repo_body_at(i), NULL);
    }

    /* SD objects: parallel make into <store>/o/ (a compile cache shared across
     * builds); the exe links the .o files below.  For wasm the environment's
     * CC / CFLAGS override the store Makefile's native defaults. */
    static char wasi_cc[512];
    if (wasi) {
        const char *sdk = getenv("WASI_SDK");
        snprintf(wasi_cc, sizeof wasi_cc, "%s/bin/clang --target=wasm32-wasip1",
                 sdk && sdk[0] ? sdk : "wasi-sdk");
        setenv("CC", wasi_cc, 1);
        static char wasi_cflags[2048];
        snprintf(wasi_cflags, sizeof wasi_cflags,
                 "-O2 -w -I%s -I%s/wasi -I%s/prism/include -I%s"
                 " -include %s/wasi/wasi_decls.h"
                 " -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
                 " -DKORB_WASI=1 -DKORB_BIGNUM=2 -DBARUBY_GC=%d -DASTRO_DEBUG=0",
                 KORUBY_SRC_DIR, KORUBY_SRC_DIR, KORUBY_SRC_DIR, ASTRO_RUNTIME_DIR,
                 KORUBY_SRC_DIR, BARUBY_GC);
        setenv("CFLAGS", wasi_cflags, 1);
        astro_cs_build_objs(NULL);
    } else {
        char cflags[2048];
        koruby_extra_cflags(cflags, sizeof cflags);
        astro_cs_build_objs(cflags);
    }

    /* 2. _embed.c: builders + metadata. */
    koruby_emit_set_vm(c->vm);
    const char *embed_path = "_embed.c";
    FILE *fp = fopen(embed_path, "w");
    if (!fp) { perror(embed_path); return 1; }
    astro_emit_ast_c_program_params(fp, prelude_ast, "koruby_embed_prelude_ast",
                                    "node.h", "CTX *_ectx");
    fprintf(fp, "\n");
    astro_emit_ast_c_program_params(fp, ast, "koruby_embed_program_ast",
                                    NULL, "CTX *_ectx");
    fprintf(fp, "\nconst uint32_t koruby_embed_prelude_locals = %uU;\n",
            prelude_locals);
    fprintf(fp, "const char koruby_embed_src_name[] = ");
    koruby_emit_cstr_len(fp, c->vm->script_name,
                         (uint32_t)strlen(c->vm->script_name));
    fprintf(fp, ";\n");
    fprintf(fp, "void\nkoruby_embed_setup(CTX *c)\n{\n");
    fprintf(fp, "    koruby_toplevel_locals_cnt = %uU;\n", koruby_toplevel_locals_cnt);
    fprintf(fp, "    koruby_toplevel_local_cnt = %uU;\n", koruby_toplevel_local_cnt);
    if (koruby_toplevel_local_cnt > 0) {
        fprintf(fp, "    koruby_toplevel_local_syms = korb_embed_syms(c, %uU",
                koruby_toplevel_local_cnt);
        for (uint32_t i = 0; i < koruby_toplevel_local_cnt; i++) {
            const char *nm = korb_sym_name(c->vm, koruby_toplevel_local_syms[i]);
            fprintf(fp, ", ");
            koruby_emit_cstr_len(fp, nm, (uint32_t)strlen(nm));
            fprintf(fp, ", %uU", (uint32_t)strlen(nm));
        }
        fprintf(fp, ");\n");
    } else {
        fprintf(fp, "    (void)c;\n");
    }
    fprintf(fp, "}\n");
    fclose(fp);

    /* 3. Host objects — the interpreter sources compiled for the target,
     * cached in <store>/host/ via a generated host.mk.  Each object depends
     * on this binary (its mtime moves whenever any interpreter source
     * changes), so a warm build compiles only _embed.c and links.  _embed.c
     * is startup-once ALLOC code, so -O0 keeps its (large) compile cheap. */
    {
        char mk[512];
        snprintf(mk, sizeof mk, "%s/host.mk", store);
        FILE *mf = fopen(mk, "w");
        if (!mf) { perror(mk); return 1; }
        if (wasi) {
            const char *sdk = getenv("WASI_SDK");
            fprintf(mf, "HOSTCC := %s/bin/clang --target=wasm32-wasip1\n",
                    sdk && sdk[0] ? sdk : "wasi-sdk");
            fprintf(mf,
                "HOSTCFLAGS := -O2 -w -I%s -I%s/wasi -I%s/prism/include -I%s"
                " -include %s/wasi/wasi_decls.h"
                " -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
                " -DKORB_WASI=1 -DASTRO_DEBUG=0 -DKORB_BIGNUM=2 -DBARUBY_GC=%d"
                " '-DKORUBY_SRC_DIR=\"/koruby\"' '-DASTRO_RUNTIME_DIR=\"/koruby\"'"
                " '-DASTRO_PRISM_INC_DIR=\"/koruby\"'\n",
                KORUBY_SRC_DIR, KORUBY_SRC_DIR, KORUBY_SRC_DIR, ASTRO_RUNTIME_DIR,
                KORUBY_SRC_DIR, BARUBY_GC);
        } else {
            fprintf(mf, "HOSTCC := %s\n", bcfg->cc ? bcfg->cc : "cc");
            fprintf(mf,
                "HOSTCFLAGS := -O2 -w -I%s -I%s/prism/include -I%s"
                " -DASTRO_DEBUG=0 -DKORB_BIGNUM=%d -DBARUBY_GC=%d"
                " '-DKORUBY_SRC_DIR=\"%s\"' '-DASTRO_RUNTIME_DIR=\"%s\"'"
                " '-DASTRO_PRISM_INC_DIR=\"%s\"'\n",
                KORUBY_SRC_DIR, KORUBY_SRC_DIR, ASTRO_RUNTIME_DIR,
                KORB_BIGNUM, BARUBY_GC,
                KORUBY_SRC_DIR, ASTRO_RUNTIME_DIR, ASTRO_PRISM_INC_DIR);
        }
        char *const cwd = getcwd(NULL, 0);
        fprintf(mf, "BIN := %s/koruby_precise\n\n", KORUBY_SRC_DIR);
        fprintf(mf, "OBJS = host/node.o host/korb_runtime.o host/parse.o"
                    " host/gc_common.o host/gc_backend.o host/main_embed.o"
                    " host/_embed.o%s\n",
                wasi ? " host/wasi_missing.o" : "");
        fprintf(mf, "all: $(OBJS)\n");
        fprintf(mf, "host:\n\tmkdir -p host\n");
        fprintf(mf, "host/%%.o: %s/%%.c $(BIN) | host\n"
                    "\t$(HOSTCC) $(HOSTCFLAGS) -c $< -o $@\n", KORUBY_SRC_DIR);
        fprintf(mf, "host/wasi_missing.o: %s/wasi/wasi_missing.c $(BIN) | host\n"
                    "\t$(HOSTCC) $(HOSTCFLAGS) -c $< -o $@\n", KORUBY_SRC_DIR);
        fprintf(mf, "host/gc_common.o: %s/precise_gc/gc_common.c $(BIN) | host\n"
                    "\t$(HOSTCC) $(HOSTCFLAGS) -c $< -o $@\n", ASTRO_RUNTIME_DIR);
        fprintf(mf, "host/gc_backend.o: %s $(BIN) | host\n"
                    "\t$(HOSTCC) $(HOSTCFLAGS) -c $< -o $@\n", KORUBY_GC_SRC);
        fprintf(mf, "host/main_embed.o: %s/main.c $(BIN) | host\n"
                    "\t$(HOSTCC) $(HOSTCFLAGS) -DKORUBY_EMBED=1 -c $< -o $@\n",
                KORUBY_SRC_DIR);
        /* _embed.c changes every build; -O0 (startup-once ALLOC code). */
        fprintf(mf, "host/_embed.o: %s/_embed.c | host\n"
                    "\t$(HOSTCC) $(filter-out -O2,$(HOSTCFLAGS)) -O0 -c $< -o $@\n",
                cwd ? cwd : ".");
        fclose(mf);
        free(cwd);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "make -C %s -f host.mk -j6 --no-print-directory -s",
                 store);
        if (system(cmd) != 0) {
            fprintf(stderr, "koruby_precise: --build: host object build failed\n");
            return 1;
        }
    }

    /* 4. Link: host objects + logged SD objects + libprism. */
    const uint32_t n_sd = astro_cs_compile_log_size();
    /* 7 host + wasi_missing + prism + regex + NULL terminator + slack */
    const char **objs = calloc(n_sd + 12, sizeof(*objs));
    uint32_t no = 0;
    static const char *const host_objs[] = {
        "host/main_embed.o", "host/_embed.o", "host/node.o", "host/korb_runtime.o",
        "host/parse.o", "host/gc_common.o", "host/gc_backend.o",
    };
    for (size_t i = 0; i < sizeof(host_objs) / sizeof(host_objs[0]); i++) {
        char *p = malloc(strlen(store) + strlen(host_objs[i]) + 2);
        sprintf(p, "%s/%s", store, host_objs[i]);
        objs[no++] = p;
    }
    if (wasi) {
        char *p = malloc(strlen(store) + 32);
        sprintf(p, "%s/host/wasi_missing.o", store);
        objs[no++] = p;
    }
    for (uint32_t i = 0; i < n_sd; i++) {
        const char *sd_name = NULL;
        node_hash_t h;
        astro_cs_compile_log_get(i, &h, &sd_name);
        (void)h;
        if (!sd_name) continue;
        char *p = malloc(strlen(store) + strlen(sd_name) + 16);
        sprintf(p, "%s/o/%s.o", store, sd_name);
        objs[no++] = p;
    }
    objs[no++] = wasi ? KORUBY_SRC_DIR "/wasi/build/libprism-wasm.a"
                      : KORUBY_SRC_DIR "/prism/build/libprism.a";
    if (wasi)   /* statically linked regex engine (native uses dlopen) */
        objs[no++] = KORUBY_SRC_DIR "/wasi/build/libkoruby-regex-wasm.a";
    bcfg->extra_objects = objs;

    bcfg->src_dir = KORUBY_SRC_DIR;
    bcfg->runtime_dir = ASTRO_RUNTIME_DIR;
    static const char *no_sources[] = { NULL };
    bcfg->sources = no_sources;

    if (wasi) {
        /* Toolchain: wasi-sdk clang (overridable via ASTRO_BUILD_OPTS --cc=). */
        static char cc_path[512];
        if (!bcfg->cc) {
            const char *sdk = getenv("WASI_SDK");
            snprintf(cc_path, sizeof cc_path, "%s/bin/clang",
                     sdk && sdk[0] ? sdk : "wasi-sdk");
            bcfg->cc = cc_path;
        }
        static const char *cflags_wasi[] = { "--target=wasm32-wasip1", NULL };
        static const char *ldflags_wasi[] = {
            "-lwasi-emulated-mman", "-lwasi-emulated-signal",
            "-lwasi-emulated-process-clocks", "-lm",
            "-Wl,-z,stack-size=16777216", NULL,
        };
        bcfg->sample_cflags  = cflags_wasi;
        bcfg->sample_ldflags = ldflags_wasi;
        bcfg->no_libdl = true;
    } else {
        static const char *ldflags_native[] = {
            "-lm",
#if KORB_BIGNUM == 1
            "-lgmp",
#endif
            "-lcrypt", NULL,
        };
        bcfg->sample_ldflags = ldflags_native;
    }

    const int rc = astro_build_executable(bcfg);
    astro_build_end_aot_session();
    if (!bcfg->keep_intermediates) unlink(embed_path);
    return rc;
}
#endif /* !KORUBY_EMBED */

#ifdef KORUBY_EMBED
/* Provided by the generated _embed.c linked into this executable. */
extern NODE *koruby_embed_prelude_ast(CTX *_ectx);
extern NODE *koruby_embed_program_ast(CTX *_ectx);
extern const uint32_t koruby_embed_prelude_locals;
extern const char koruby_embed_src_name[];
extern void koruby_embed_setup(CTX *c);
#endif


int
main(int argc, char *argv[])
{
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 2;

    /* Point RUBY_EXE at this interpreter (unless already set) so ruby/spec's mspec
     * can resolve the executable and run.  Harmless for normal programs. */
    if (!getenv("RUBY_EXE")) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { exe[n] = '\0'; setenv("RUBY_EXE", exe, 0); }
    }

    if (bcfg.help_requested)    { usage(stdout); return 0; }
    if (bcfg.version_requested) { printf("koruby_precise %s\n", ASTRO_VERSION); return 0; }

    if (bcfg.plain)         OPTION.plain         = true;
    if (bcfg.compiled_only) OPTION.compiled_only = true;   /* poison unswapped bodies */
    if (bcfg.aot_compile)   OPTION.aot_compile   = true;
    if (bcfg.pg_compile)  OPTION.pg_compile  = true;   /* M0: same bake as AOT */
    if (bcfg.quiet)       OPTION.quiet       = true;
    if (bcfg.verbose)     OPTION.verbose     = true;

    /* Run convention (docs/sample_cli.md): `--aot-compile` alone bakes WITHOUT
     * running; `--run` (or `--pg-compile`) opts the run back in.  koruby
     * registers every body in the code repo at parse time, so a no-run bake
     * still covers the whole program. */
    const bool skip_run = bcfg.aot_compile && !bcfg.run && !bcfg.pg_compile;

    /* sample flags + positional script */
    const char *load_dirs[64]; uint32_t nload_dirs = 0;    /* -I */
    const char *req_libs[64];  uint32_t nreq_libs  = 0;    /* -r */
    int i = 1;
#ifdef KORUBY_EMBED
    /* Embedded executable: the program is baked in — argv[1..] is Ruby ARGV. */
    if (bcfg.out_exe) {
        fprintf(stderr, "koruby_precise: --build inside a built executable is not supported\n");
        return 2;
    }
    const char *src_name = koruby_embed_src_name;
#else
    const char *eval_code = NULL;
    const char *script = NULL;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0) {
            if (eval_code) {
                fprintf(stderr, "koruby_precise: multiple -e is not supported in M0\n");
                return 2;
            }
            if (i + 1 >= argc) { fprintf(stderr, "koruby_precise: -e requires an argument\n"); return 2; }
            eval_code = argv[++i];
        }
        else if (strcmp(a, "--dump-ast") == 0) {
            OPTION.dump_ast = true;
        }
        else if (strcmp(a, "--ccs") == 0 || strcmp(a, "--clear-code-store") == 0) {
            OPTION.clear_store = true;
        }
        else if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        /* CRuby flags that scripts and spec harnesses pass through: -I adds to
         * $LOAD_PATH, -r requires, and the warning / feature switches are
         * accepted so an invocation that only decorates itself with them runs. */
        else if (strncmp(a, "-I", 2) == 0) {
            const char *const dir = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (dir) { if (nload_dirs < 64) load_dirs[nload_dirs++] = dir; }
        }
        else if (strncmp(a, "-r", 2) == 0) {
            const char *const lib = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (lib) { if (nreq_libs < 64) req_libs[nreq_libs++] = lib; }
        }
        else if (strcmp(a, "-w") == 0 || strncmp(a, "-W", 2) == 0 ||
                 strncmp(a, "--disable", 9) == 0 || strncmp(a, "--enable", 8) == 0 ||
                 strcmp(a, "--copyright") == 0) {
            /* accepted, no effect */
        }
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "koruby_precise: unknown flag %s\n", a);
            usage(stderr);
            return 2;
        }
        else {
            break;   /* first positional = script; rest = ARGV */
        }
    }
    if (!eval_code && i < argc) {
        script = argv[i];
        i++;
    }
    /* argv[i..] would be Ruby ARGV — M0 has no ARGV object yet. */

    const char *src_name;
    char *src;
    size_t src_len;
    if (eval_code) {
        src_name = "-e";
        src = strdup(eval_code);
        src_len = strlen(src);
    }
    else if (script) {
        src_name = script;
        src = read_file_all(script, &src_len);
    }
    else {
        src_name = "-";
        src = read_file_all("-", &src_len);
    }
#endif /* !KORUBY_EMBED */

    CTX *c = korb_ctx_new();
    c->vm->script_name = src_name;
    c->vm->cur_load_file = src_name;   /* __dir__ / require_relative base for top-level code */
    korb_define_argv(c, argc - i, &argv[i], src_name);   /* ARGV = remaining args; $0 = script */

#ifdef KORUBY_EMBED
    /* Rebuild the baked ASTs (dispatchers pre-bound; symbols re-intern through
     * `c`).  No parse: the exe starts straight into evaluation. */
    NODE *prelude_ast = koruby_embed_prelude_ast(c);
    uint32_t prelude_locals = koruby_embed_prelude_locals;
    g_prelude_repo_count = code_repo_count();   /* the exe registers no bodies at build time */
    NODE *ast = koruby_embed_program_ast(c);
    koruby_embed_setup(c);   /* toplevel locals count + TOPLEVEL_BINDING sym table */
#else
    /* Parse the Enumerable prelude first (registers its method bodies in the
     * code repo so AOT bakes/swaps them too); run it after the AOT swap below.
     * Captured here because koruby_toplevel_locals_cnt is overwritten by the
     * user-program parse. */
    size_t prelude_len = 0;
    char *prelude_src = OPTION.dump_ast ? NULL : load_prelude_source(&prelude_len);
    NODE *prelude_ast = OPTION.dump_ast ? NULL
                      : koruby_parse_source(c, prelude_src, prelude_len, "<prelude>", true);
    uint32_t prelude_locals = koruby_toplevel_locals_cnt;
    /* Prelude method bodies registered so far form [0, g_prelude_repo_count);
     * they are baked into preload.so, not the program's code store. */
    g_prelude_repo_count = code_repo_count();

    NODE *ast = koruby_parse_source(c, src, src_len, src_name, true);
#endif /* KORUBY_EMBED */

    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
        korb_io_flush_std(c->vm);   /* stdio is gone: the std streams flush here */
        return 0;
    }

    if (OPTION.clear_store) {
        int rc = system("rm -rf code_store");
        if (rc != 0) fprintf(stderr, "koruby_precise: --ccs: rm -rf code_store failed\n");
    }

#ifndef KORUBY_EMBED
    /* --build OUT: bake + emit + link a standalone executable, no run
     * (run the built exe instead — same convention as `--aot-compile` alone). */
    if (bcfg.out_exe) {
        const int rc = koruby_do_build(c, prelude_ast, prelude_locals, ast, &bcfg);
        if (rc == 0 && !OPTION.quiet)
            fprintf(stderr, "koruby_precise: built %s (program + prelude + %u bodies embedded)\n",
                    bcfg.out_exe, code_repo_count());
        korb_io_flush_std(c->vm);
        return rc;
    }

    if (!OPTION.plain) {
        ensure_preload();                        /* bake (if stale) + dlopen preload.so */
        INIT();                                  /* dlopen code_store/all.so if present */
        unsigned int swaps = swap_in_cached_sds(ast);
        if (OPTION.verbose) {
            fprintf(stderr, "koruby_precise: aot: swapped %u dispatchers "
                    "(program + %u method bodies)\n", swaps, code_repo_count());
        }
        /* rubyharness aot+cached contract: a cached run must actually run on
         * SDs.  Silent interpreter fallback was the v1 failure mode. */
        if (getenv("ASTRO_AOT_STRICT") && swaps == 0) {
            fprintf(stderr, "koruby_precise: ASTRO_AOT_STRICT: no cached SD matched "
                    "(hash mismatch or empty code store)\n");
            korb_io_flush_std(c->vm);   /* stdio is gone: the std streams flush here */
            return 3;
        }
    } else {
        /* --plain means "don't run USER code compiled"; the fixed prelude is
         * known code and stays on its baked SDs (its per-method behavior is
         * identical either way — this only stops prelude helpers from
         * distorting user-code tree-walk measurements the other direction).
         * No INIT(): the program's code_store stays untouched. */
        ensure_preload();
        if (prelude_ast) {
            unsigned int pswaps = astro_cs_load(prelude_ast, NULL) ? 1 : 0;
            for (uint32_t i = 0; i < g_prelude_repo_count; i++)
                if (astro_cs_load(code_repo_body_at(i), NULL)) pswaps++;
            if (OPTION.verbose)
                fprintf(stderr, "koruby_precise: plain: prelude on %u baked SDs "
                        "(%u bodies)\n", pswaps, g_prelude_repo_count);
        }
    }
#endif /* !KORUBY_EMBED */

    /* Run the Enumerable prelude in its own toplevel frame (self = a throwaway
     * `main`), defining its methods on the global Enumerable module before the
     * user program runs.  Bodies were registered in the code repo at parse time,
     * so the AOT swap above already patched their dispatchers. */
    if (prelude_ast && !skip_run) {
        VALUE *pcur = c->slots + prelude_locals;
        RESULT pm = korb_obj_new(c, pcur, KORB_NIL);
        if (pm.state == KORB_RAISE) { korb_report_uncaught(c, pm.value); korb_io_flush_std(c->vm); return 1; }
        c->slots[-1] = pm.value;                  /* prelude self at base[-1] (bottom header) */
        RESULT pr = EVAL(c, prelude_ast, pcur);
        if (pr.state == KORB_RAISE) { korb_report_uncaught(c, pr.value); korb_io_flush_std(c->vm); return 1; }
    }

    /* Run (unless `--aot-compile` alone — then we bake below without running).
     * Toplevel frame: locals at c->slots[0..L); the self cell is the frame top
     * (base[fs-1] = c->slots[koruby_toplevel_locals_cnt-1]) holding the `main`
     * object; cursor starts above it. */
    if (!skip_run) {
        VALUE *toplevel_cursor = c->slots + koruby_toplevel_locals_cnt;
        /* builtin/exception class objects are now set up inside korb_ctx_new
         * (they must exist before core-method registration). */
        {
            RESULT mr = korb_obj_new(c, toplevel_cursor, KORB_NIL);   /* klass=nil → `main` */
            if (mr.state == KORB_RAISE) { korb_report_uncaught(c, mr.value); korb_io_flush_std(c->vm); return 1; }
            c->slots[-1] = mr.value;                  /* main self at base[-1] (bottom header) */
        }
        /* TOPLEVEL_BINDING: a Binding over the (persistent) toplevel frame. */
        {
            RESULT tb = korb_make_binding(c, toplevel_cursor, c->slots,
                                          koruby_toplevel_local_syms, koruby_toplevel_local_cnt,
                                          c->slots[-1]);
            if (tb.state == KORB_NORMAL)
                korb_const_define(c, korb_intern(c->vm, "TOPLEVEL_BINDING", 16), tb.value);
        }
        /* -I directories join $LOAD_PATH and -r libraries are required before
         * the program runs, as CRuby does. */
        for (uint32_t k = nload_dirs; k-- > 0; ) korb_load_path_unshift(c, toplevel_cursor, load_dirs[k]);
        for (uint32_t k = 0; k < nreq_libs; k++) {
            const RESULT rr = korb_require_feature(c, toplevel_cursor, req_libs[k]);
            if (rr.state == KORB_RAISE) { korb_report_uncaught(c, rr.value); korb_io_flush_std(c->vm); return 1; }
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        RESULT r = EVAL(c, ast, toplevel_cursor);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* Run at_exit blocks (reverse registration order) — this is how mspec and
         * other suites trigger their run.  They execute even after an uncaught
         * exception, matching CRuby (Kernel#exit shares korb_drain_at_exit). */
        /* Kernel#exit raises SystemExit; reaching here uncaught means the program
         * really is ending — take its status and stay quiet (CRuby prints no
         * message for SystemExit).  at_exit still runs, as it does for any exit. */
        int exit_status = -1;
        bool uncaught = false;
        if (r.state == KORB_RAISE) {
            exit_status = korb_system_exit_status(c, r.value);
            if (exit_status < 0) {                        /* a real error: report it first, then
                                                           * let the handlers see it as $! */
                korb_report_uncaught(c, r.value);
                korb_errinfo_push(c, r.value);
                uncaught = true;
            }
        }
        const int handler_status = korb_drain_at_exit(c, toplevel_cursor);
        if (handler_status >= 0) exit_status = handler_status;   /* a handler's exit wins */
        fflush(stdout);
        if (exit_status >= 0) {
            korb_io_flush_std(c->vm);
            return exit_status;
        }
        if (uncaught) {
            korb_io_flush_std(c->vm);   /* stdio is gone: the std streams flush here */
            return 1;
        }
        if (r.state == KORB_THROW) {
            fprintf(stderr, "uncaught throw\n");
            korb_io_flush_std(c->vm);   /* stdio is gone: the std streams flush here */
            return 1;
        }

        if (getenv("KORUBY_GC_STATS")) {
            double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "__KORUBY_GC__ alloc_bytes=%zu gc_count=%zu minor=%zu major=%zu "
                            "gc_seconds=%.6f max_pause=%.6f elapsed=%.6f\n",
                    aro_gc_total_bytes(c), aro_gc_count(c), aro_gc_minor_count(c), aro_gc_major_count(c),
                    aro_gc_total_seconds(c), aro_gc_max_pause_seconds(c), elapsed);
        }
    }

#ifndef KORUBY_EMBED
    if (OPTION.aot_compile || OPTION.pg_compile) {
        /* Bodies were registered in the code repo at parse time, so the whole
         * program bakes whether or not it ran. */
        bake_code_store(ast);
        if (OPTION.verbose) {
            fprintf(stderr, "koruby_precise: aot: baked program + %u method bodies\n",
                    code_repo_count());
        }
    }
#endif

    korb_ctx_free(c);
    korb_io_flush_std(c->vm);   /* stdio is gone: the std streams flush here */
    return 0;
}
