// pystro — Python subset on the ASTro framework.  See README.md.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct pys_option OPTION;

#include "runtime.c"
#include "lexer.c"
#include "parser.c"

static char *
read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "pystro: cannot open '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) {
        fprintf(stderr, "pystro: read error\n");
        exit(1);
    }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

static void
usage(void)
{
    fprintf(stderr,
        "usage: pystro [options] <file.py>\n"
        "options:\n"
        "  -e <code>      run literal program\n"
        "  -c             AOT-bake SDs into code_store/, then run with them active\n"
        "  --aot-compile  AOT-bake SDs into code_store/ and exit (no run)\n"
        "  --no-compile   don't consult / write code store (pure interpreter)\n"
        "  --dump-ast     print AST and exit\n"
        "  -q             quiet\n"
        "  -h, --help     this help\n");
    exit(2);
}

// Captured argv so that `sys.argv` (via __pystro_argv__) can return it.
int    PYS_ARGC = 0;
char **PYS_ARGV = NULL;

// Directory of the pystro binary itself; used as a sys.path-like fallback
// so built-in stdlib modules (math.py / sys.py / json.py / etc.) are
// importable regardless of cwd.
const char *PYS_BINDIR = NULL;

int
main(int argc, char *argv[])
{
    pys_gc_init();
    // Resolve pystro's own directory once so we can use it as a
    // stdlib search path.
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char *slash = strrchr(buf, '/');
            if (slash) {
                *slash = '\0';
                char *dup = (char *)malloc(strlen(buf) + 1);
                strcpy(dup, buf);
                PYS_BINDIR = dup;
            }
        }
    }

    const char *file = NULL;
    const char *eval_str = NULL;

    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != '\0') {
        const char *a = argv[ai++];
        if      (!strcmp(a, "-q"))            OPTION.quiet = true;
        else if (!strcmp(a, "--no-compile"))  OPTION.no_compiled_code = true;
        else if (!strcmp(a, "-c"))            OPTION.compile_first = true;
        else if (!strcmp(a, "--aot-compile")) OPTION.aot_only = true;
        else if (!strcmp(a, "--dump-ast"))    OPTION.dump_ast = true;
        else if (!strcmp(a, "-e")) {
            if (ai >= argc) usage();
            eval_str = argv[ai++];
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
        else if (!strcmp(a, "--")) break;
        else { fprintf(stderr, "pystro: unknown option %s\n", a); usage(); }
    }
    bool repl_mode = false;
    if (!eval_str) {
        if (ai >= argc) {
            // No file and no -e: drop into REPL.
            repl_mode = true;
        } else {
            file = argv[ai++];
        }
    }
    // Record argv from the script-name onwards for sys.argv parity.
    if (file) {
        PYS_ARGC = argc - (ai - 1);
        PYS_ARGV = &argv[ai - 1];
    } else {
        PYS_ARGC = argc - ai + 1;
        PYS_ARGV = &argv[ai - 1];
    }

    INIT();

    // CTX is GC_malloc'd so Boehm scans its pointer fields (globals,
    // env, EXC_*, state_value, ...) — otherwise those reachable-only-
    // through-CTX heap allocations get reclaimed mid-run.
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    extern struct pysglobals *pys_globals_new(void);
    c->globals = pys_globals_new();
    c->state = PYS_STATE_NORMAL;
    c->current_class = PYS_NONE;
    c->method_class = PYS_NONE;
    c->recursion_limit = 1000;
    extern CTX *pys_current_ctx;
    pys_current_ctx = c;
    install_builtins(c);
    // Register the running script's globals as `sys.modules["__main__"]`
    // (CPython's convention).  Lets unittest.main() and similar
    // introspect the entry-point module by name.
    {
        extern VALUE modules_dict(CTX *c);
        struct pysobj *mo = (struct pysobj *)GC_malloc(
            offsetof(struct pysobj, module) + sizeof(((struct pysobj *)0)->module));
        mo->type = PYS_T_MODULE;
        mo->module.name = "__main__";
        mo->module.globals = c->globals;
        VALUE mod = PYS_OBJ_VAL(mo);
        pys_dict_set(c, modules_dict(c), pys_make_str("__main__", 8), mod);
    }

    if (repl_mode) {
        // Read-eval-print loop.  Each input line is parsed as a
        // top-level program and executed in the same CTX.
        printf("pystro REPL — Ctrl+D or `exit()` to quit\n");
        for (;;) {
#ifdef USE_READLINE
            char *line = readline(">>> ");
            if (!line) break;
            if (*line) add_history(line);
#else
            char buf[4096];
            fputs(">>> ", stdout); fflush(stdout);
            if (!fgets(buf, sizeof(buf), stdin)) break;
            char *line = buf;
#endif
            // Continuation: keep reading while the buffer ends with a
            // colon (open block) or has unbalanced parens/brackets.
            size_t cap = strlen(line) + 1;
            char *prog = (char *)malloc(cap + 1024);
            strcpy(prog, line);
#ifdef USE_READLINE
            free(line);
#endif
            for (;;) {
                size_t L = strlen(prog);
                int depth = 0; bool open = false;
                for (size_t i = 0; i < L; i++) {
                    if (prog[i] == '(' || prog[i] == '[' || prog[i] == '{') depth++;
                    else if (prog[i] == ')' || prog[i] == ']' || prog[i] == '}') depth--;
                }
                // Open block heuristic: trailing ':' (after stripping spaces).
                size_t e = L;
                while (e > 0 && (prog[e-1] == ' ' || prog[e-1] == '\t' || prog[e-1] == '\n')) e--;
                if (e > 0 && prog[e-1] == ':') open = true;
                if (depth <= 0 && !open) break;
#ifdef USE_READLINE
                char *more = readline("... ");
                if (!more) break;
                size_t ml = strlen(more);
                prog = (char *)realloc(prog, L + ml + 2);
                prog[L] = '\n';
                memcpy(prog + L + 1, more, ml + 1);
                free(more);
#else
                fputs("... ", stdout); fflush(stdout);
                char buf2[4096];
                if (!fgets(buf2, sizeof(buf2), stdin)) break;
                size_t ml = strlen(buf2);
                prog = (char *)realloc(prog, L + ml + 1);
                memcpy(prog + L, buf2, ml + 1);
#endif
            }
            tokenize(prog, "<repl>");
            NODE *body = parse_program();
            EVAL(c, body);
            if (c->state == PYS_STATE_RAISE) {
                VALUE exc = c->state_value;
                const char *cls_name = pys_is_instance(exc) ? PYS_PTR(exc)->inst.cls->cls.name : "Exception";
                fprintf(stderr, "%s\n", cls_name);
                c->state = PYS_STATE_NORMAL; c->state_value = PYS_NONE;
            }
            free(prog);
        }
        return 0;
    }
    char *src;
    const char *src_name;
    if (eval_str) { src = strdup(eval_str); src_name = "<command line>"; }
    else          { src = read_file(file);  src_name = file; }

    // Bind __file__ to the running script path (CPython convention).
    pys_global_set(c, "__file__", pys_make_str(src_name, strlen(src_name)));
    // Also bind __name__ / __doc__ / __package__ / __spec__ etc. — the
    // standard module attribute set CPython exposes at the top of every
    // module.  `__name__` is "__main__" for the directly-executed script.
    pys_global_set(c, "__name__",     pys_make_str("__main__", 8));
    pys_global_set(c, "__doc__",      PYS_NONE);
    pys_global_set(c, "__package__",  pys_make_str("", 0));
    pys_global_set(c, "__builtins__", PYS_NONE);
    pys_global_set(c, "__spec__",     PYS_NONE);
    pys_global_set(c, "__loader__",   PYS_NONE);
    pys_global_set(c, "__cached__",   PYS_NONE);

    tokenize(src, src_name);
    NODE *body = parse_program();

    if (OPTION.dump_ast) { DUMP(stdout, body, false); printf("\n"); free(src); return 0; }

    if (OPTION.compile_first || OPTION.aot_only) {
        // Run the program first in interp mode so pys_make_func registers
        // every function body in code_repo.  Then bake an SD per body
        // (plus the top-level body) so the next `./pystro <script>`
        // invocation finds dispatchers for every entry point.  Without
        // this only the top-level body got a SD; function bodies stayed
        // in tree-walking interp even with all.so loaded.
        extern struct {
            uint32_t size, capa;
            struct code_entry { const char *name; struct Node *body; } *entries;
        } code_repo;
        EVAL(c, body);
        // Reset state so AOT compile doesn't trip on a propagating
        // raise from the bake-time run.
        c->state = PYS_STATE_NORMAL;
        c->state_value = PYS_NONE;
        astro_cs_compile(body, NULL);
        for (uint32_t i = 0; i < code_repo.size; i++) {
            astro_cs_compile(code_repo.entries[i].body, NULL);
        }
        // ccache fails inside the sandbox / read-only home dirs; opt out.
        setenv("CCACHE_DISABLE", "1", 0);
        astro_cs_build(NULL);
        astro_cs_reload();
        astro_cs_load(body, NULL);
        for (uint32_t i = 0; i < code_repo.size; i++) {
            astro_cs_load(code_repo.entries[i].body, NULL);
        }
        // Bake-only: don't re-run the program.  The next invocation will
        // pick up the freshly baked all.so via astro_cs_init.
        free(src);
        return 0;
    }

    OPTIMIZE(body);

    EVAL(c, body);
    if (c->state == PYS_STATE_RAISE) {
        VALUE exc = c->state_value;
        // SystemExit propagates as the process exit code (CPython
        // semantics).  Don't print a traceback for it; just translate
        // .code to the return value: None / 0 → exit(0), int → exit(int),
        // str → print to stderr + exit(1).
        if (pys_is_instance(exc)
            && pys_exc_matches(c, exc, c->EXC_SystemExit)) {
            VALUE code = pys_getattr_optional(c, exc, "code");
            free(src);
            if (!code || code == PYS_NONE) return 0;
            if (PYS_IS_FIXNUM(code)) return (int)PYS_FIXVAL(code);
            if (pys_is_str(code)) {
                fwrite(PYS_PTR(code)->str.chars, 1,
                       PYS_PTR(code)->str.len, stderr);
                fputc('\n', stderr);
                return 1;
            }
            return 1;
        }
        const char *cls_name = "Exception";
        if (pys_is_instance(exc)) cls_name = PYS_PTR(exc)->inst.cls->cls.name;
        fprintf(stderr, "Traceback (most recent call last):\n");
        if (pys_is_instance(exc)) {
            VALUE tb = pys_getattr_optional(c, exc, "__traceback__");
            if (tb && pys_is_list(tb)) {
                size_t n = PYS_PTR(tb)->list.len;
                for (size_t i = 0; i < n; i++) {
                    VALUE fn = PYS_PTR(tb)->list.items[i];
                    if (pys_is_str(fn))
                        fprintf(stderr, "  in %.*s\n",
                                (int)PYS_PTR(fn)->str.len, PYS_PTR(fn)->str.chars);
                }
            }
        }
        fprintf(stderr, "%s: ", cls_name);
        if (pys_is_instance(exc) && PYS_PTR(exc)->inst.attrs) {
            VALUE k = pys_make_str("message", 7);
            struct pysdict *d = PYS_PTR(exc)->inst.attrs;
            uint64_t h = pys_hash(c, k);
            int32_t eidx = pydict_find(c, d, k, h);
            if (eidx >= 0 && pys_is_str(d->entries[eidx].value))
                fwrite(PYS_PTR(d->entries[eidx].value)->str.chars, 1,
                       PYS_PTR(d->entries[eidx].value)->str.len, stderr);
        }
        fputc('\n', stderr);
        free(src);
        return 1;
    }
    free(src);
    return 0;
}
