#include "context.h"
#include "node.h"
#include "parse.h"
#include "type.h"
#include "astro_code_store.h"
#include "astro_build.h"

struct ancaml_option OPTION;

#ifndef ANCAML_SRC_DIR
#define ANCAML_SRC_DIR "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

static char *
slurp(FILE *f)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';
    return buf;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [file.ml]\n"
        "\n"
        "ancaml — a MinCaml interpreter on ASTro.  Reads a MinCaml program from\n"
        "a file (or stdin), type-checks it (Hindley-Milner), and runs it.\n"
        "MinCaml (https://esumii.github.io/min-caml/) is a tiny monomorphic ML\n"
        "subset: unit/bool/int/float, tuples, arrays, let / let rec, and a\n"
        "handful of external functions (print_int, sqrt, ...).\n"
        "\n"
        "ancaml-specific options:\n"
        "      --dump-ast      print the parsed AST and exit\n"
        "      --dump-types    print the inferred type of the program\n"
        "      --no-typecheck  skip Hindley-Milner type inference (debug)\n"
        "\n", prog);
    astro_print_build_help(stderr);
}

static int
run(CTX *c, NODE *body)
{
    volatile int rc = 0;
    if (setjmp(c->err_jmp) == 0) {
        c->err_active = 1;
        EVAL(c, body);
    } else {
        rc = 1;   // a runtime error was reported
    }
    c->err_active = 0;
    return rc;
}

int
main(int argc, char *argv[])
{
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;
    if (bcfg.help_requested)    { usage(argv[0]); return 0; }
    if (bcfg.version_requested) { printf("ancaml (ASTro %s)\n", ASTRO_VERSION); return 0; }

    if (bcfg.quiet)       OPTION.quiet = true;
    if (bcfg.verbose)     OPTION.verbose = true;
    if (bcfg.plain)       OPTION.no_compiled_code = true;
    if (bcfg.aot_compile) OPTION.aot_compile = true;
    if (bcfg.pg_compile)  OPTION.pg_compile = true;

    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--dump-ast") == 0)     OPTION.dump_ast = true;
        else if (strcmp(argv[i], "--dump-types") == 0)   OPTION.dump_types = true;
        else if (strcmp(argv[i], "--no-typecheck") == 0) OPTION.no_typecheck = true;
        else if (argv[i][0] == '-' && argv[i][1])        { fprintf(stderr, "ancaml: unknown option %s\n", argv[i]); usage(argv[0]); return 1; }
        else file = argv[i];
    }

    INIT();
    ac_register_externals();

    char *src;
    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) { fprintf(stderr, "ancaml: cannot open %s\n", file); return 1; }
        src = slurp(f); fclose(f);
    } else {
        src = slurp(stdin);
    }

    Program prog = ac_parse_program(src);
    if (!prog.ok) return 1;

    if (!OPTION.no_typecheck) {
        if (ac_typecheck(&prog) != 0) return 1;
    }

    // Tail-call marking: every function body is a tail-position root.  Done
    // after type checking (which only knows the non-tail node kinds) and
    // before dump / AOT / build so all paths see the tail nodes.
    for (int i = 0; i < ac_n_tail_roots; i++) ac_mark_tail(ac_tail_roots[i]);

    if (OPTION.dump_ast) { DUMP(stdout, prog.body, true); printf("\n"); return 0; }

    CTX *c = ac_make_context();

    if (OPTION.aot_compile && !OPTION.no_compiled_code && !bcfg.out_exe)
        ac_aot_specialize(prog.body);

    if (bcfg.out_exe) {
        astro_build_begin_aot_session();
        if (bcfg.aot_compile || bcfg.pg_compile)
            for (int i = 0; i < ac_n_entries; i++) astro_cs_compile(ac_entries[i], NULL);
        bcfg.src_dir = ANCAML_SRC_DIR;
        bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
        static const char *sources[] = { "lexer.c", "parse.c", "type.c", "node.c", "value.c", "exe_main.c", NULL };
        bcfg.sources = sources;
        static const char *ldflags[] = { "-lgc", "-ldl", "-lm", NULL };
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
