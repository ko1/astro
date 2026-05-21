#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"
#include "astro_build.h"
#include "astro_node.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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

// Build a standalone exe from a single -e expression.  Steps:
//   1. parse + AST construction (already done by caller)
//   2. astro_cs_compile → SD_<hash>.c in code_store/c/
//   3. emit _embed.c (ASTRO_BUILD_EMBEDDED_AST function)
//   4. emit _static_table.c (static SD table)
//   5. call astro_build_executable with the right source list
static int
generate_executable(NODE *ast, const struct astro_build_config *bcfg_in)
{
    // SD_*.c generation is the caller's responsibility (so they can
    // honor --no-compile).  We only emit the embed/table files and
    // collect whatever SD_*.c happens to live in code_store/c/.

    // Emit _embed.c next to the calc binary's CWD.
    char embed_path[] = "_embed.c";
    FILE *fp = fopen(embed_path, "w");
    if (!fp) { perror(embed_path); return 1; }
    astro_emit_ast_c_file(fp, ast, "astro_build_embedded_ast", "node.h");
    fclose(fp);

    // Emit _static_table.c.
    char table_path[] = "_static_table.c";
    fp = fopen(table_path, "w");
    if (!fp) { perror(table_path); return 1; }
    astro_cs_emit_static_table(fp, "ASTRO_SD_PROTO");
    fclose(fp);

    // Enumerate SD_*.c under code_store/c/ (best-effort; if empty,
    // build still proceeds — static table will be empty too).
    const char **sd_files = NULL;
    size_t sd_n = 0, sd_capa = 0;
    DIR *d = opendir("code_store/c");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *nm = ent->d_name;
            size_t l = strlen(nm);
            if (l < 5) continue;
            if (strcmp(nm + l - 2, ".c") != 0) continue;
            if (strncmp(nm, "SD_", 3) != 0 &&
                strncmp(nm, "PGSD_", 5) != 0) continue;
            if (sd_n + 1 >= sd_capa) {
                sd_capa = sd_capa == 0 ? 8 : sd_capa * 2;
                sd_files = realloc(sd_files, sizeof(*sd_files) * sd_capa);
            }
            char *full = malloc(l + 32);
            snprintf(full, l + 32, "code_store/c/%s", nm);
            sd_files[sd_n++] = full;
        }
        closedir(d);
    }
    // NULL-terminate.
    if (sd_n + 1 >= sd_capa) {
        sd_capa = sd_n + 1;
        sd_files = realloc(sd_files, sizeof(*sd_files) * sd_capa);
    }
    sd_files[sd_n] = NULL;

    // Compose extra_sources_abs: _embed.c, _static_table.c, then each SD.
    size_t extra_n = 2 + sd_n + 1;
    const char **extras = malloc(sizeof(*extras) * extra_n);
    extras[0] = "_embed.c";
    extras[1] = "_static_table.c";
    for (size_t i = 0; i < sd_n; i++) extras[2 + i] = sd_files[i];
    extras[2 + sd_n] = NULL;

    struct astro_build_config bcfg = *bcfg_in;
    bcfg.src_dir = CALC_SRC_DIR;
    bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
    static const char *sources[] = {
        "parse.c", "node.c", "exe_main.c", NULL,
    };
    bcfg.sources = sources;
    bcfg.extra_sources_abs = extras;

    int rc = astro_build_executable(&bcfg);

    // Cleanup intermediates unless --keep-intermediates.
    if (!bcfg.keep_intermediates) {
        unlink(embed_path);
        unlink(table_path);
    }
    free(extras);
    for (size_t i = 0; i < sd_n; i++) free((void *)sd_files[i]);
    free(sd_files);
    return rc;
}

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
        "  -e EXPR        evaluate EXPR once and exit (no REPL)\n"
        "      --disasm   print x86 disassembly of the specialized code\n"
        "      --no-compile  skip code-store specialization (pure interpreter)\n"
        "  -q, --quiet    suppress hit/miss progress messages\n"
        "  -h, --help     show this help\n"
        "\n"
        "With no -e, %s starts an interactive REPL.\n",
        progname, progname);
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
    const char *eval_expr = NULL;

    // Build-orchestrator framework flags (--generate-executable, --cc,
    // -O0..-O3, --static, --strip, etc.) are consumed from argv first.
    // What's left is parsed by calc's own per-sample option loop below.
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_parse_args(&argc, argv, &bcfg) != 0) {
        return 1;
    }

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

    if (bcfg.out_exe) {
        if (!eval_expr) {
            fprintf(stderr, "calc: --generate-executable requires -e EXPR\n");
            return 1;
        }
        NODE *ast = parse(eval_expr);
        // AOT-bake unless --no-compile was passed: with --no-compile the
        // exe runs as a pure-interpreter (smaller, slower).  Without it,
        // the SD_*.c files are generated and statically linked.
        if (!OPTION.no_compiled_code) {
            astro_cs_compile(ast, NULL);
        }
        // We deliberately skip astro_cs_build/_reload — for exe builds
        // there's no all.so and no dlopen; the static table picks up
        // the SDs at link time.
        int rc = generate_executable(ast, &bcfg);
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
