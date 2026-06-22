/* koruby_precise v2 — main.c: CLI + run + AOT bake (docs/v2_spec.md §3).
 *
 * AOT semantics (koruby pattern, v2_spec §3.4): `--aot-compile` always
 * executes AND bakes at exit; a plain run swaps in cached SDs when the
 * structural hash matches; `--plain` ignores the code store entirely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "node.h"
#include "astro_code_store.h"
#include "astro_build.h"
#include "precise_gc/gc.h"

struct koruby_option OPTION;

/* Enumerable mixin, defined in Ruby in terms of the includer's `each` (which now
 * works thanks to yield-in-block).  Run as a prelude before the user program so
 * `include Enumerable` + a user `each` yields map/select/etc.  Methods use only
 * single-/no-arg yield and non-local return (both supported); `block_given?` is
 * read at method top-level into a local so the nested each-block can capture it.
 * Line numbers stay independent of the user script (separate parse). */
static const char *const KORUBY_PRELUDE =
"module Enumerable\n"
"  def map; r = []; each { |x| r << yield(x) }; r; end\n"
"  def collect; r = []; each { |x| r << yield(x) }; r; end\n"
"  def select; r = []; each { |x| r << x if yield(x) }; r; end\n"
"  def filter; r = []; each { |x| r << x if yield(x) }; r; end\n"
"  def find_all; r = []; each { |x| r << x if yield(x) }; r; end\n"
"  def reject; r = []; each { |x| r << x unless yield(x) }; r; end\n"
"  def flat_map; r = []; each { |x| v = yield(x); if v.is_a?(Array); v.each { |e| r << e }; else; r << v; end }; r; end\n"
"  def find; each { |x| return x if yield(x) }; nil; end\n"
"  def detect; each { |x| return x if yield(x) }; nil; end\n"
"  def to_a; r = []; each { |x| r << x }; r; end\n"
"  def to_h; h = {}; if block_given?; each { |x| kv = yield(x); h[kv[0]] = kv[1] }; else; each { |x| h[x[0]] = x[1] }; end; h; end\n"
"  def entries; r = []; each { |x| r << x }; r; end\n"
"  def count; n = 0; each { |x| n += 1 }; n; end\n"
"  def include?(v); each { |x| return true if x == v }; false; end\n"
"  def member?(v); each { |x| return true if x == v }; false; end\n"
"  def first(n = nil); if n.nil?; each { |x| return x }; nil; else; r = []; c = 0; each { |x| if c < n; r << x; c += 1; end }; r; end; end\n"
"  def reduce(a, b = nil); if b.nil?; acc = nil; f = true; each { |x| if f; acc = x; f = false; else; acc = acc.send(a, x); end }; acc; else; acc = a; each { |x| acc = acc.send(b, x) }; acc; end; end\n"
"  def inject(a, b = nil); if b.nil?; acc = nil; f = true; each { |x| if f; acc = x; f = false; else; acc = acc.send(a, x); end }; acc; else; acc = a; each { |x| acc = acc.send(b, x) }; acc; end; end\n"
"  def sum(init = 0); s = init; if block_given?; each { |x| s = s + yield(x) }; else; each { |x| s = s + x }; end; s; end\n"
"  def min; r = nil; f = true; each { |x| if f; r = x; f = false; elsif x < r; r = x; end }; r; end\n"
"  def max; r = nil; f = true; each { |x| if f; r = x; f = false; elsif x > r; r = x; end }; r; end\n"
"  def min_by; r = nil; rk = nil; f = true; each { |x| k = yield(x); if f; r = x; rk = k; f = false; elsif k < rk; r = x; rk = k; end }; r; end\n"
"  def max_by; r = nil; rk = nil; f = true; each { |x| k = yield(x); if f; r = x; rk = k; f = false; elsif k > rk; r = x; rk = k; end }; r; end\n"
"  def sort; to_a.sort; end\n"
"  def sort_by; a = []; each { |x| a << x }; a.sort_by { |x| yield(x) }; end\n"
"  def all?; bg = block_given?; each { |x| return false unless (bg ? yield(x) : x) }; true; end\n"
"  def any?; bg = block_given?; each { |x| return true if (bg ? yield(x) : x) }; false; end\n"
"  def none?; bg = block_given?; each { |x| return false if (bg ? yield(x) : x) }; true; end\n"
"end\n"
"class Proc\n"
"  def curry(n = (arity < 0 ? -arity - 1 : arity))\n"
"    acc = nil\n"
"    acc = ->(got) { got.length >= n ? call(*got) : ->(*more) { acc.call(got + more) } }\n"
"    acc.call([])\n"
"  end\n"
"end\n"
"class Random\n"                       /* stub: faithful MT19937 sequence is out of scope */
"  def initialize(seed = nil); @seed = seed; end\n"
"  def seed; @seed; end\n"
"end\n"
/* Minimal Encoding: enough for constant references and identity comparison so
 * specs that mention Encoding::X in setup don't crash.  Per-string encoding
 * tracking is out of scope (String#encoding returns UTF-8, the default). */
"class Encoding\n"
"  def initialize(name); @name = name; end\n"
"  def name; @name; end\n"
"  def to_s; @name; end\n"
"  def inspect; \"#<Encoding:\" + @name + \">\"; end\n"
"  def ==(o); o.is_a?(Encoding) && o.name == @name; end\n"
"  def ascii_compatible?; @name != \"UTF-16\" && @name != \"UTF-32\"; end\n"
"  def dummy?; false; end\n"
"  UTF_8 = Encoding.new(\"UTF-8\")\n"
"  US_ASCII = Encoding.new(\"US-ASCII\")\n"
"  ASCII = US_ASCII\n"
"  ASCII_8BIT = Encoding.new(\"ASCII-8BIT\")\n"
"  BINARY = ASCII_8BIT\n"
"  UTF_16 = Encoding.new(\"UTF-16\")\n"
"  UTF_32 = Encoding.new(\"UTF-32\")\n"
"  UTF_16LE = Encoding.new(\"UTF-16LE\")\n"
"  UTF_32LE = Encoding.new(\"UTF-32LE\")\n"
"  UTF_7 = Encoding.new(\"UTF-7\")\n"
"  CESU_8 = Encoding.new(\"CESU-8\")\n"
"  SHIFT_JIS = Encoding.new(\"Shift_JIS\")\n"
"  EUC_JP = Encoding.new(\"EUC-JP\")\n"
"  ISO_2022_JP = Encoding.new(\"ISO-2022-JP\")\n"
"  CP50221 = Encoding.new(\"CP50221\")\n"
"  ISO_8859_1 = Encoding.new(\"ISO-8859-1\")\n"
"  Windows_1252 = Encoding.new(\"Windows-1252\")\n"
"  def self.default_external; UTF_8; end\n"
"  def self.default_internal; nil; end\n"
"end\n"
"class Encoding\n"
"  class Converter; end\n"
"  class CompatibilityError < StandardError; end\n"
"  class UndefinedConversionError < StandardError; end\n"
"  class InvalidByteSequenceError < StandardError; end\n"
"  class ConverterNotFoundError < StandardError; end\n"
"end\n"
"class String\n"
"  def encoding; Encoding::UTF_8; end\n"
"end\n"
/* Minimal Errno: just enough that Errno::X constant references resolve (as
 * SystemCallError subclasses); per-errno numbers/semantics are out of scope. */
"class Exception\n"
"  def detailed_message(**opts); m = message; \"#{m} (#{self.class})\"; end\n"
"  def full_message(**opts); \"#{message} (#{self.class})\"; end\n"
"end\n"
"class SystemCallError < StandardError; end\n"
"module Errno\n"
"  EPERM = Class.new(SystemCallError); ENOENT = Class.new(SystemCallError)\n"
"  ESRCH = Class.new(SystemCallError); EINTR = Class.new(SystemCallError)\n"
"  EIO = Class.new(SystemCallError); EBADF = Class.new(SystemCallError)\n"
"  EAGAIN = Class.new(SystemCallError); ENOMEM = Class.new(SystemCallError)\n"
"  EACCES = Class.new(SystemCallError); EEXIST = Class.new(SystemCallError)\n"
"  ENOTDIR = Class.new(SystemCallError); EISDIR = Class.new(SystemCallError)\n"
"  EINVAL = Class.new(SystemCallError); EPIPE = Class.new(SystemCallError)\n"
"  ERANGE = Class.new(SystemCallError); ENOTSUP = Class.new(SystemCallError)\n"
"  ECHILD = Class.new(SystemCallError); ESPIPE = Class.new(SystemCallError)\n"
"  ECONNRESET = Class.new(SystemCallError); ETIMEDOUT = Class.new(SystemCallError)\n"
"end\n";

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

/* AOT bake: the program AST + every method body (each is its own entry —
 * call sites dispatch through body->head.dispatcher at runtime). */
static void
bake_code_store(NODE *ast)
{
    astro_cs_compile(ast, NULL);
    for (uint32_t i = 0; i < code_repo_count(); i++) {
        if (code_repo_skip_specialize_at(i)) continue;
        astro_cs_compile(code_repo_body_at(i), NULL);
    }

    char extra_cflags[2048];
    snprintf(extra_cflags, sizeof(extra_cflags),
             "--param=early-inlining-insns=100"
             " -fcf-protection=none"
             " -I" KORUBY_SRC_DIR
             " -I" ASTRO_RUNTIME_DIR
             " -I" ASTRO_PRISM_INC_DIR
#ifdef KORB_HAVE_GMP
             " -DKORB_HAVE_GMP"   /* match the main build: SDs must keep the bignum-promote arithmetic paths, not the no-GMP overflow stubs */
#endif
             " -DBARUBY_GC=%d", BARUBY_GC);   /* framework backend-select macro */
    setenv("ASTRO_EXTRA_LDFLAGS", "-Wl,-Bsymbolic", 0);
    astro_cs_build(extra_cflags);
    astro_cs_reload();
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

    /* Parse the Enumerable prelude first (registers its method bodies in the
     * code repo so AOT bakes/swaps them too); run it after the AOT swap below.
     * Captured here because koruby_toplevel_locals_cnt is overwritten by the
     * user-program parse. */
    NODE *prelude_ast = OPTION.dump_ast ? NULL
                      : koruby_parse_source(c, KORUBY_PRELUDE, strlen(KORUBY_PRELUDE), "<prelude>");
    uint32_t prelude_locals = koruby_toplevel_locals_cnt;

    NODE *ast = koruby_parse_source(c, src, src_len, src_name);

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
        fflush(stdout);
        if (r.state == KORB_RAISE) {
            korb_report_uncaught(c, r.value);
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
