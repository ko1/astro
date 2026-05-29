#include <setjmp.h>
#include "context.h"
#include "node.h"
#include "parse.h"
#include "check.h"
#include "astro_code_store.h"
#include "astro_build.h"

struct anpy_option OPTION;

#ifndef ANPY_SRC_DIR
#define ANPY_SRC_DIR "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

void anpy_set_jmp_active(int v);
jmp_buf *anpy_get_jmp(void);

static char *
slurp(FILE *f)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, f)) > 0) { len += r; if (len == cap) { cap *= 2; buf = realloc(buf, cap); } }
    buf[len] = '\0';
    return buf;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [file.py]\n"
        "\n"
        "AnPy — a ChocoPy interpreter on ASTro.  Reads a program from a file\n"
        "(or stdin) and runs it.  ChocoPy is a statically-typed subset of\n"
        "Python 3.6; valid programs run identically under python3.\n"
        "\n"
        "AnPy-specific options:\n"
        "      --dump-ast      print the parsed AST and exit\n"
        "      --no-typecheck  skip static type checking (debug)\n"
        "\n", prog);
    astro_print_build_help(stderr);
}

static int
run(CTX *c, NODE *body)
{
    volatile int rc = 0;
    if (setjmp(*anpy_get_jmp()) == 0) {
        anpy_set_jmp_active(1);
        EVAL(c, body);
    } else {
        rc = 1;   // a runtime error was reported
    }
    anpy_set_jmp_active(0);
    return rc;
}

int
main(int argc, char *argv[])
{
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;
    if (bcfg.help_requested)    { usage(argv[0]); return 0; }
    if (bcfg.version_requested) { printf("anpy (ASTro %s)\n", ASTRO_VERSION); return 0; }

    if (bcfg.quiet)       OPTION.quiet = true;
    if (bcfg.verbose)     OPTION.verbose = true;
    if (bcfg.plain)       OPTION.no_compiled_code = true;
    if (bcfg.aot_compile) OPTION.aot_compile = true;
    if (bcfg.pg_compile)  OPTION.pg_compile = true;

    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ast") == 0)       OPTION.dump_ast = true;
        else if (strcmp(argv[i], "--no-typecheck") == 0) OPTION.no_typecheck = true;
        else if (argv[i][0] == '-' && argv[i][1])     { fprintf(stderr, "anpy: unknown option %s\n", argv[i]); usage(argv[0]); return 1; }
        else file = argv[i];
    }

    INIT();

    char *src;
    if (file) { FILE *f = fopen(file, "rb"); if (!f) { fprintf(stderr, "anpy: cannot open %s\n", file); return 1; } src = slurp(f); fclose(f); }
    else src = slurp(stdin);

    Program prog = parse_program(src);
    if (!prog.ok) return 1;

    anpy_finalize_classes();
    if (!OPTION.no_typecheck) {
        int errs = anpy_typecheck(&prog);
        if (errs > 0) { fprintf(stderr, "anpy: %d type error(s)\n", errs); return 1; }
    }

    if (OPTION.dump_ast) { DUMP(stdout, prog.body, false); printf("\n"); return 0; }

    CTX *c = anpy_make_context();
    anpy_install_globals(c);

    if (OPTION.aot_compile && !OPTION.no_compiled_code && !bcfg.out_exe)
        anpy_aot_specialize(prog.body, prog.funcs, prog.nfuncs);

    if (bcfg.out_exe) {
        astro_build_begin_aot_session();
        if (bcfg.aot_compile || bcfg.pg_compile) astro_cs_compile(prog.body, NULL);
        bcfg.src_dir = ANPY_SRC_DIR; bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
        static const char *sources[] = { "lexer.c", "parse.c", "check.c", "node.c", "value.c", "exe_main.c", NULL };
        bcfg.sources = sources;
        static const char *ldflags[] = { "-lgc", "-ldl", NULL };
        bcfg.sample_ldflags = ldflags;
        int r = astro_build_aot_executable(prog.body, &bcfg, "code_store");
        astro_build_end_aot_session();
        astro_build_config_dispose(&bcfg);
        return r;
    }

    int rc = run(c, prog.body);
    fflush(stdout);
    return rc;
}
