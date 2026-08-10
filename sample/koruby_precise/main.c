/* koruby_precise v2 — main.c: CLI + run + AOT bake (docs/v2_spec.md §3).
 *
 * AOT semantics (koruby pattern, v2_spec §3.4): `--aot-compile` always
 * executes AND bakes at exit; a plain run swaps in cached SDs when the
 * structural hash matches; `--plain` ignores the code store entirely.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "node.h"
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
static const char *const KORUBY_PRELUDE_FILES[] = {
    "enumerable.rb", "enumerator.rb", "proc.rb", "hash.rb", "set.rb", "encoding.rb", "exception.rb", "numeric.rb",
    "module.rb", "time.rb", "io.rb", "stringio.rb", "marshal.rb", "system.rb",
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
    FILE *fp = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "koruby_precise: cannot open %s\n", path);
        exit(2);
    }
    size_t capa = 1 << 16, len = 0;
    char *buf = malloc(capa);
    if (!buf) abort();
    size_t n;
    while ((n = fread(buf + len, 1, capa - len, fp)) > 0) {
        len += n;
        if (len == capa) {
            capa *= 2;
            buf = realloc(buf, capa);
            if (!buf) abort();
        }
    }
    if (fp != stdin) fclose(fp);
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

#define KORUBY_PRELUDE_NFILES (sizeof(KORUBY_PRELUDE_FILES) / sizeof(KORUBY_PRELUDE_FILES[0]))

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

/* Newest mtime among the prelude files — folded into the preload-store version so
 * editing a prelude .rb invalidates the baked SDs. */
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
#ifdef KORB_HAVE_GMP
             " -DKORB_HAVE_GMP"   /* match the main build: SDs must keep the bignum-promote arithmetic paths, not the no-GMP overflow stubs */
#endif
             " -DBARUBY_GC=%d", BARUBY_GC);   /* framework backend-select macro */
}

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

/* Bake the fixed prelude's SDs once into preload_store/all.so, then register it
 * as the code store's preload handle.  The prelude is identical across all
 * programs, so this keeps ~70 prelude SDs out of every program's bake (the cold
 * `--aot-compile` cost was almost entirely the prelude — `p 1+2` baked 73
 * prelude SDs vs 1 of its own).  Rebuild only happens during an explicit bake
 * run (`--aot-compile`/PG); a plain cached run just loads the existing .so. */
static void
ensure_preload(void)
{
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
}

/* AOT bake: the program AST + every *user* method body (each is its own entry —
 * call sites dispatch through body->head.dispatcher at runtime).  Prelude bodies
 * [0, g_prelude_repo_count) are skipped — they live in preload.so. */
static void
bake_code_store(NODE *ast)
{
    astro_cs_compile(ast, NULL);
    for (uint32_t i = g_prelude_repo_count; i < code_repo_count(); i++) {
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

    if (bcfg.out_exe) {
        fprintf(stderr, "koruby_precise: --build is not supported in M0\n");
        return 2;
    }

    /* sample flags + positional script */
    const char *eval_code = NULL;
    const char *script = NULL;
    int i = 1;
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

    CTX *c = korb_ctx_new();
    c->vm->script_name = src_name;
    c->vm->cur_load_file = src_name;   /* __dir__ / require_relative base for top-level code */
    korb_define_argv(c, argc - i, &argv[i], src_name);   /* ARGV = remaining args; $0 = script */

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

    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
        return 0;
    }

    if (OPTION.clear_store) {
        int rc = system("rm -rf code_store");
        if (rc != 0) fprintf(stderr, "koruby_precise: --ccs: rm -rf code_store failed\n");
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
            return 3;
        }
    }

    /* Run the Enumerable prelude in its own toplevel frame (self = a throwaway
     * `main`), defining its methods on the global Enumerable module before the
     * user program runs.  Bodies were registered in the code repo at parse time,
     * so the AOT swap above already patched their dispatchers. */
    if (prelude_ast && !skip_run) {
        VALUE *pcur = c->slots + prelude_locals;
        RESULT pm = korb_obj_new(c, pcur, KORB_NIL);
        if (pm.state == KORB_RAISE) { korb_report_uncaught(c, pm.value); return 1; }
        c->slots[-1] = pm.value;                  /* prelude self at base[-1] (bottom header) */
        RESULT pr = EVAL(c, prelude_ast, pcur);
        if (pr.state == KORB_RAISE) { korb_report_uncaught(c, pr.value); return 1; }
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
            if (mr.state == KORB_RAISE) { korb_report_uncaught(c, mr.value); return 1; }
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
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        RESULT r = EVAL(c, ast, toplevel_cursor);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* Run at_exit blocks (reverse registration order) — this is how mspec and
         * other suites trigger their run.  They execute even after an uncaught
         * exception, matching CRuby (Kernel#exit shares korb_drain_at_exit). */
        korb_drain_at_exit(c, toplevel_cursor);
        fflush(stdout);
        if (r.state == KORB_RAISE) {
            korb_report_uncaught(c, r.value);
            return 1;
        }
        if (r.state == KORB_THROW) {
            fprintf(stderr, "uncaught throw\n");
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

    if (OPTION.aot_compile || OPTION.pg_compile) {
        /* Bodies were registered in the code repo at parse time, so the whole
         * program bakes whether or not it ran. */
        bake_code_store(ast);
        if (OPTION.verbose) {
            fprintf(stderr, "koruby_precise: aot: baked program + %u method bodies\n",
                    code_repo_count());
        }
    }

    korb_ctx_free(c);
    return 0;
}
