#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"
#include "astro_build.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct calc_option OPTION;

// Build-time-injected paths: CALC_SRC_DIR is the directory holding
// node.c / parse.c / exe_main.c; ASTRO_RUNTIME_DIR is the framework's
// runtime directory.  Both are baked in via -D so the same `calc`
// binary, wherever it's invoked from, can still find its companion
// source files when generating an exe.
#ifndef CALC_SRC_DIR
#define CALC_SRC_DIR "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

static char *
read_line(const char *prompt)
{
#ifdef USE_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;
#else
    printf("%s", prompt);
    fflush(stdout);
    static char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
#endif
}

static void
usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options] [-e EXPR]\n"
        "       %s --build OUTPUT [build opts] -e EXPR\n"
        "\n"
        "Interpreter mode (no --build):\n"
        "  -e EXPR        evaluate EXPR once and exit (no REPL)\n"
        "      --disasm   print x86 disassembly of the specialized code\n"
        "      --no-compile  skip code-store specialization (pure interpreter)\n"
        "  -q, --quiet    suppress hit/miss progress messages\n"
        "  -h, --help     show this help\n"
        "\n"
        "Build mode (--build OUTPUT ...): writes a standalone exe to OUTPUT.\n"
        "Common build opts: --aot/--no-aot, -O0..-O3, --strip, --lto,\n"
        "                   --static, --gc-sections, --cc=PATH.\n"
        "\n"
        "With no -e, %s starts an interactive REPL.\n",
        progname, progname, progname);
}

static VALUE
evaluate(CTX *const c, const char *const input)
{
    NODE *const ast = parse(input);
    if (!OPTION.no_compiled_code) {
        if (!ast->head.flags.is_specialized) {
            astro_cs_compile(ast, NULL);
            astro_cs_build(NULL);
            astro_cs_reload();
            astro_cs_load(ast, NULL);
        }
        if (OPTION.disasm) astro_cs_disasm(ast);
    }
    return EVAL(c, ast);
}

// Source-spec parser for the --build subcommand.  Handles `-e EXPR`.
static NODE *
build_parse_source(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            return parse(argv[i + 1]);
        }
    }
    fprintf(stderr, "calc --build: missing -e EXPR\n");
    return NULL;
}

int
main(int argc, char *argv[])
{
    // --build subcommand: framework-owned argv space.
    if (argc >= 2 && strcmp(argv[1], "--build") == 0) {
        struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
        int rest_argc; char **rest_argv;
        if (astro_build_subcommand_parse(argc - 1, argv + 1, &bcfg,
                                          &rest_argc, &rest_argv) != 0) {
            return 1;
        }
        INIT();
        // Calc has only -e EXPR source spec.  rest_argv contains -e EXPR
        // (or nothing).  Build the AST from -e.
        NODE *ast = build_parse_source(rest_argc, rest_argv);
        free(rest_argv);
        if (!ast) { astro_build_config_dispose(&bcfg); return 1; }

        astro_build_begin_aot_session();
        // Bake SDs when --aot-compile is set, OR when --pg-compile (which
        // also bakes AOT in addition to running for profile).  Calc has
        // no profile-collecting machinery so --pg-compile collapses to
        // --aot-compile here.
        if (bcfg.aot_compile || bcfg.pg_compile) {
            astro_cs_compile(ast, NULL);
        }
        bcfg.src_dir = CALC_SRC_DIR;
        bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
        static const char *sources[] = {
            "parse.c", "node.c", "exe_main.c", NULL,
        };
        bcfg.sources = sources;
        int rc = astro_build_aot_executable(ast, &bcfg, "code_store");
        astro_build_end_aot_session();
        astro_build_config_dispose(&bcfg);
        return rc;
    }

    const char *eval_expr = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            OPTION.quiet = true;
        }
        else if (strcmp(argv[i], "--no-compile") == 0) {
            OPTION.no_compiled_code = true;
        }
        else if (strcmp(argv[i], "--disasm") == 0) {
            OPTION.disasm = true;
        }
        else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "calc: -e requires an argument\n");
                return 1;
            }
            eval_expr = argv[i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "calc: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    INIT();
    CTX *const c = malloc(sizeof(CTX));

    if (eval_expr) {
        printf("%ld\n", evaluate(c, eval_expr));
        return 0;
    }

    char *line;
    while ((line = read_line("calc> ")) != NULL) {
        if (line[0] != '\0') printf("=> %ld\n", evaluate(c, line));
#ifdef USE_READLINE
        free(line);
#endif
    }

    printf("\n");
    return 0;
}
