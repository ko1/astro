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

struct pystro_option OPTION;

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
int    PYSTRO_ARGC = 0;
char **PYSTRO_ARGV = NULL;
// Directory of the pystro binary itself; used as a sys.path-like fallback
// so built-in stdlib modules (math.py / sys.py / json.py / etc.) are
// importable regardless of cwd.
const char *PYSTRO_BINDIR = NULL;

int
main(int argc, char *argv[])
{
    py_gc_init();
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
                PYSTRO_BINDIR = dup;
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
        PYSTRO_ARGC = argc - (ai - 1);
        PYSTRO_ARGV = &argv[ai - 1];
    } else {
        PYSTRO_ARGC = argc - ai + 1;
        PYSTRO_ARGV = &argv[ai - 1];
    }

    INIT();

    // CTX is GC_malloc'd so Boehm scans its pointer fields (globals,
    // env, EXC_*, state_value, ...) — otherwise those reachable-only-
    // through-CTX heap allocations get reclaimed mid-run.
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    extern struct pyglobals *py_globals_new(void);
    c->globals = py_globals_new();
    c->state = PY_STATE_NORMAL;
    c->current_class = PY_NONE;
    c->method_class = PY_NONE;
    c->recursion_limit = 1000;
    extern CTX *py_current_ctx;
    py_current_ctx = c;
    install_builtins(c);
    // Register the running script's globals as `sys.modules["__main__"]`
    // (CPython's convention).  Lets unittest.main() and similar
    // introspect the entry-point module by name.
    {
        extern VALUE modules_dict(CTX *c);
        struct pyobj *mo = (struct pyobj *)GC_malloc(
            offsetof(struct pyobj, module) + sizeof(((struct pyobj *)0)->module));
        mo->type = PY_T_MODULE;
        mo->module.name = "__main__";
        mo->module.globals = c->globals;
        VALUE mod = PY_OBJ_VAL(mo);
        py_dict_set(c, modules_dict(c), py_make_str("__main__", 8), mod);
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
            if (setjmp(c->err_jmp) == 0) {
                c->err_jmp_active = 1;
                EVAL(c, body);
                if (c->state == PY_STATE_RAISE) {
                    VALUE exc = c->state_value;
                    const char *cls_name = py_is_instance(exc) ? PY_PTR(exc)->inst.cls->cls.name : "Exception";
                    fprintf(stderr, "%s\n", cls_name);
                    c->state = PY_STATE_NORMAL; c->state_value = PY_NONE;
                }
            }
            c->err_jmp_active = 0;
            free(prog);
        }
        return 0;
    }
    char *src;
    const char *src_name;
    if (eval_str) { src = strdup(eval_str); src_name = "<command line>"; }
    else          { src = read_file(file);  src_name = file; }

    tokenize(src, src_name);
    NODE *body = parse_program();

    if (OPTION.dump_ast) { DUMP(stdout, body, false); printf("\n"); free(src); return 0; }

    if (OPTION.compile_first || OPTION.aot_only) {
        astro_cs_compile(body, NULL);
        astro_cs_build(NULL);
        astro_cs_reload();
        astro_cs_load(body, NULL);
        if (OPTION.aot_only) { free(src); return 0; }
    }

    OPTIMIZE(body);

    int jmp_status = setjmp(c->err_jmp);
    if (jmp_status == 0) {
        c->err_jmp_active = 1;
        EVAL(c, body);
    }
    c->err_jmp_active = 0;
    if (c->state == PY_STATE_RAISE) {
        VALUE exc = c->state_value;
        const char *cls_name = "Exception";
        if (py_is_instance(exc)) cls_name = PY_PTR(exc)->inst.cls->cls.name;
        fprintf(stderr, "Traceback (most recent call last):\n");
        if (py_is_instance(exc)) {
            VALUE tb = py_getattr_optional(c, exc, "__traceback__");
            if (tb && py_is_list(tb)) {
                size_t n = PY_PTR(tb)->list.len;
                for (size_t i = 0; i < n; i++) {
                    VALUE fn = PY_PTR(tb)->list.items[i];
                    if (py_is_str(fn))
                        fprintf(stderr, "  in %.*s\n",
                                (int)PY_PTR(fn)->str.len, PY_PTR(fn)->str.chars);
                }
            }
        }
        fprintf(stderr, "%s: ", cls_name);
        if (py_is_instance(exc) && PY_PTR(exc)->inst.attrs) {
            VALUE k = py_make_str("message", 7);
            struct pydict *d = PY_PTR(exc)->inst.attrs;
            uint64_t h = py_hash(c, k);
            int32_t eidx = pydict_find(c, d, k, h);
            if (eidx >= 0 && py_is_str(d->entries[eidx].value))
                fwrite(PY_PTR(d->entries[eidx].value)->str.chars, 1,
                       PY_PTR(d->entries[eidx].value)->str.len, stderr);
        }
        fputc('\n', stderr);
        free(src);
        return 1;
    }
    free(src);
    return 0;
}
