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
        "\n"
        "Calc-specific options:\n"
        "  -e EXPR        evaluate EXPR once and exit (no REPL)\n"
        "      --disasm   print x86 disassembly of the specialized code\n"
        "\n",
        progname);
    astro_print_build_help(stderr);
    fprintf(stderr,
        "\n"
        "With no -e, %s starts an interactive REPL.\n",
        progname);
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

int
main(int argc, char *argv[])
{
    // Extract build-related flags from argv first (--build OUT, --run,
    // --aot-compile, --pg-compile, --plain).  These are order-free and
    // disappear from argv before calc's own option loop runs.
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

    // Framework universal flags signal early.
    if (bcfg.help_requested) {
        usage(argv[0]);
        return 0;
    }
    if (bcfg.version_requested) {
        printf("calc (ASTro %s)\n", ASTRO_VERSION);
        return 0;
    }

    // Translate framework flags into calc's internal OPTION.
    if (bcfg.quiet)            OPTION.quiet = true;
    if (bcfg.plain)            OPTION.no_compiled_code = true;

    const char *eval_expr = NULL;

    // Parse calc-specific flags (--disasm, -e EXPR).
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disasm") == 0) {
            OPTION.disasm = true;
        }
        else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "calc: -e requires an argument\n");
                return 1;
            }
            eval_expr = argv[i];
        }
        else {
            fprintf(stderr, "calc: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    INIT();
    CTX *const c = malloc(sizeof(CTX));

    // Build mode (--build OUT was on argv).  Calc only has `-e EXPR` as
    // source, so eval_expr must be set.
    if (bcfg.out_exe) {
        if (!eval_expr) {
            fprintf(stderr, "calc: --build requires -e EXPR\n");
            return 1;
        }
        NODE *ast = parse(eval_expr);
        astro_build_begin_aot_session();
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
