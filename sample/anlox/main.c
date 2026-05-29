#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"
#include "astro_build.h"

struct anlox_option OPTION;

#ifndef ANLOX_SRC_DIR
#define ANLOX_SRC_DIR "."
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
        "Usage: %s [options] [file.lox]\n"
        "\n"
        "anlox — a Lox interpreter on ASTro.  Lox is the teaching language of\n"
        "Robert Nystrom's *Crafting Interpreters* (https://craftinginterpreters.com/):\n"
        "dynamic typing, closures, and single-inheritance classes.\n"
        "\n"
        "anlox-specific options:\n"
        "      --dump-ast      print the parsed AST and exit\n"
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
        rc = 70;   // Lox runtime-error exit code
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
    if (bcfg.version_requested) { printf("anlox (ASTro %s)\n", ASTRO_VERSION); return 0; }

    if (bcfg.quiet)       OPTION.quiet = true;
    if (bcfg.verbose)     OPTION.verbose = true;
    if (bcfg.plain)       OPTION.no_compiled_code = true;
    if (bcfg.aot_compile) OPTION.aot_compile = true;
    if (bcfg.pg_compile)  OPTION.pg_compile = true;

    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--dump-ast") == 0) OPTION.dump_ast = true;
        else if (argv[i][0] == '-' && argv[i][1])    { fprintf(stderr, "anlox: unknown option %s\n", argv[i]); usage(argv[0]); return 1; }
        else file = argv[i];
    }

    INIT();

    char *src;
    if (file) {
        FILE *f = fopen(file, "rb");
        if (!f) { fprintf(stderr, "anlox: cannot open %s\n", file); return 1; }
        src = slurp(f); fclose(f);
    } else {
        src = slurp(stdin);
    }

    Program prog = lox_parse_program(src);
    if (!prog.ok) return 65;

    if (OPTION.dump_ast) { DUMP(stdout, prog.body, true); printf("\n"); return 0; }

    CTX *c = lox_make_context();
    lox_register_natives(c);

    if (OPTION.aot_compile && !OPTION.no_compiled_code && !bcfg.out_exe)
        lox_aot_specialize(prog.body);

    if (bcfg.out_exe) {
        // Not supported: every anlox node reaches its children through the
        // LOX_BLOCK_STMTS / LOX_CALL_ARGS / LOX_FUNDEFS side-tables by index,
        // and the framework's AST embedder only reconstructs NODE* operands —
        // the side-tables would be NULL in the baked executable.  The
        // interpreter and `--aot-compile` (code_store) modes are fully
        // supported.  See docs/todo.md.
        fprintf(stderr, "anlox: --build is not supported (AST uses side-table indirection); "
                        "use --aot-compile for specialized dispatchers.\n");
        astro_build_config_dispose(&bcfg);
        return 1;
    }

    int rc = run(c, prog.body);
    fflush(stdout);
    return rc;
}
