// pystro — Python subset on the ASTro framework.  See README.md.

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

int
main(int argc, char *argv[])
{
    py_gc_init();

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
    if (!eval_str) {
        if (ai >= argc) usage();
        file = argv[ai++];
    }

    INIT();

    // CTX is GC_malloc'd so Boehm scans its pointer fields (globals,
    // env, EXC_*, state_value, ...) — otherwise those reachable-only-
    // through-CTX heap allocations get reclaimed mid-run.
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    c->state = PY_STATE_NORMAL;
    c->current_class = PY_NONE;
    install_builtins(c);

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

    if (setjmp(c->err_jmp) == 0) {
        c->err_jmp_active = 1;
        EVAL(c, body);
        if (c->state == PY_STATE_RAISE) {
            VALUE exc = c->state_value;
            const char *cls_name = "Exception";
            if (py_is_instance(exc)) cls_name = PY_PTR(exc)->inst.cls->cls.name;
            fprintf(stderr, "Traceback (most recent call last):\n");
            fprintf(stderr, "%s: ", cls_name);
            // Print exception's `message` attribute if present.
            if (py_is_instance(exc) && PY_PTR(exc)->inst.attrs) {
                VALUE k = py_make_str("message", 7);
                struct pydict *d = PY_PTR(exc)->inst.attrs;
                uint64_t h = py_hash(c, k);
                struct pydict_entry *e = pydict_lookup(c, d, k, h);
                if (e->state == 1 && py_is_str(e->value))
                    fwrite(PY_PTR(e->value)->str.chars, 1, PY_PTR(e->value)->str.len, stderr);
            }
            fputc('\n', stderr);
            free(src);
            return 1;
        }
    }
    free(src);
    return 0;
}
