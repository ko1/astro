#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>

#include "context.h"
#include "object.h"
#include "node.h"

struct koruby_option OPTION = {0};

NODE *koruby_parse(const char *src, size_t len, const char *filename);

/* code store + build orchestrator (via node.c). */
#include "../../runtime/astro_code_store.h"
#include "../../runtime/astro_build.h"

#ifndef KORUBY_SRC_DIR_DEFAULT
#define KORUBY_SRC_DIR_DEFAULT "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

/* code repo (defined in node.c) — exposed here so main can iterate the
 * collected per-method AST entries when AOT-compiling. */
struct code_repo {
    uint32_t size, capa;
    struct code_entry { const char *name; struct Node *body; } *entries;
};
extern struct code_repo code_repo;

/* Set when --aot-compile is on the command line. */
static bool g_aot_compile = false;

static char *read_all(FILE *fp, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = korb_xmalloc_atomic(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = korb_xrealloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "usage: koruby [options] [file] [argv...]\n"
        "\n"
        "Koruby-specific options:\n"
        "  -e <code>          eval code\n"
        "      --dump-ast     dump the parsed AST and exit\n"
        "\n");
    astro_print_build_help(stderr);
    exit(1);
}

/* koruby_setup_ctx / koruby_eval_bootstrap / koruby_run_ast live in
 * koruby_runtime.c — shared between the REPL main and the exe driver. */
extern CTX *koruby_setup_ctx(const char *current_file);
extern void koruby_eval_bootstrap(CTX *c);
extern int  koruby_run_ast(CTX *c, NODE *ast);

static char *
koruby_read_file(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    size_t cap = 4096, len = 0;
    char *buf = korb_xmalloc_atomic(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = korb_xrealloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    fclose(fp);
    *len_out = len;
    return buf;
}

/* Build + bake helper.  Called from main() after parse + (optional)
 * EVAL when bcfg.out_exe is set. */
static int
koruby_do_build(NODE *ast, struct astro_build_config *bcfg)
{
    if (bcfg->aot_compile || bcfg->pg_compile) {
        astro_cs_compile(ast, NULL);
        if (bcfg->run) {
            for (uint32_t i = 0; i < code_repo.size; i++) {
                astro_cs_compile(code_repo.entries[i].body, NULL);
            }
        }
        setenv("CCACHE_DISABLE", "1", 0);
    }

    bcfg->src_dir = KORUBY_SRC_DIR_DEFAULT;
    bcfg->runtime_dir = ASTRO_RUNTIME_DIR;
    static const char *sources[] = {
        "node.c", "parse.c", "object.c", "builtins.c",
        "bootstrap_src.c", "koruby_runtime.c", "exe_main.c", NULL,
    };
    bcfg->sources = sources;
    static const char *koruby_sample_ldflags[] = {
        "-Wl,-rpath", KORUBY_SRC_DIR_DEFAULT "/prism/build",
        "-L", KORUBY_SRC_DIR_DEFAULT "/prism/build",
        "-lprism", "-lgc", "-lgmp", "-lm",
        NULL,
    };
    static const char *koruby_sample_cflags[] = {
        "-I" KORUBY_SRC_DIR_DEFAULT "/prism/include",
        "-Wno-unused-function", "-Wno-unused-variable",
        "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
        NULL,
    };
    bcfg->sample_ldflags = koruby_sample_ldflags;
    bcfg->sample_cflags = koruby_sample_cflags;

    return astro_build_aot_executable(ast, bcfg, "code_store");
}

int main(int argc, char *argv[])
{
    /* Pre-scan argv for build-related flags.  Order-free among
     * themselves, but must come before the source file. */
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

    INIT();
    CTX *c = korb_runtime_init();

    /* Initialize the code store: dlopen code_store/all.so if it exists.
     * The src_dir is the directory of node.h / node_eval.c — used by the
     * generated SD_*.c files for #include resolution. */
    {
        const char *cs = getenv("KORUBY_CODE_STORE");
        const char *src = getenv("KORUBY_SRC_DIR");
        char default_cs[PATH_MAX];
        char default_src[PATH_MAX];
        if (!cs || !src) {
            char self[PATH_MAX] = {0};
            ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
            if (n > 0) {
                self[n] = 0;
                /* dirname */
                char *slash = strrchr(self, '/');
                if (slash) *slash = 0;
                if (!cs) {
                    snprintf(default_cs, sizeof(default_cs), "%s/code_store", self);
                    cs = default_cs;
                }
                if (!src) {
                    snprintf(default_src, sizeof(default_src), "%s", self);
                    src = default_src;
                }
            }
        }
        astro_cs_init(cs, src, 0);
    }

    /* Framework universal flags signal early. */
    if (bcfg.help_requested) {
        usage();   /* exits */
        return 0;
    }
    if (bcfg.version_requested) {
        printf("koruby (ASTro %s)\n", ASTRO_VERSION);
        return 0;
    }

    /* Translate framework flags into koruby's internal OPTION. */
    if (bcfg.quiet)   OPTION.quiet   = true;
    if (bcfg.verbose) OPTION.verbose = true;
    /* koruby always has to evaluate to discover method ASTs into
     * code_repo before they can be AOT-baked; the framework's --run
     * distinction therefore has no analogue here.  Either way we
     * funnel into g_aot_compile, which drives the all.so build path. */
    if (bcfg.aot_compile) g_aot_compile = true;

    const char *e_code = NULL;
    const char *file = NULL;
    int script_arg_start = argc;  /* args beyond the script are passed to ARGV */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0 && i+1 < argc) { e_code = argv[++i]; }
        else if (strcmp(a, "--dump-ast") == 0) { OPTION.dump_ast = true; }
        else if (a[0] == '-' && a[1] == '-' && file) { script_arg_start = i; break; }
        else if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage();
        }
        else if (!file) {
            file = a;
            script_arg_start = i + 1;
        }
        else {
            /* extra positional argument */
            script_arg_start = i;
            break;
        }
    }
    /* Build ARGV from args after the script path */
    VALUE argv_array = korb_ary_new(c, c->sp);
    for (int i = script_arg_start; i < argc; i++) {
        korb_ary_push(argv_array, korb_str_new_cstr(c, c->sp, argv[i]));
    }
    korb_const_set(korb_vm->object_class, korb_intern("ARGV"), argv_array);
    /* $0 / $PROGRAM_NAME — the script path. */
    {
        VALUE pn = korb_str_new_cstr(c, c->sp, file ? file : (e_code ? "-e" : argv[0]));
        korb_gvar_set(korb_intern("$0"), pn);
        korb_gvar_set(korb_intern("$PROGRAM_NAME"), pn);
    }

    char *src = NULL;
    size_t len = 0;
    const char *filename = "(eval)";
    if (e_code) {
        src = (char *)e_code;
        len = strlen(e_code);
    }
    else if (file) {
        FILE *fp = fopen(file, "rb");
        if (!fp) { perror(file); exit(1); }
        src = read_all(fp, &len);
        fclose(fp);
        filename = file;
    }
    else usage();

    /* Hold ast through a static so Boehm's data-section scan keeps it
     * rooted regardless of register/spill placement.  Long runs (lots
     * of GC cycles) had main's local `ast` register-evicted between
     * GC sample points, leading to ast being collected before AOT
     * compile read it back. */
    static NODE *ast;
    ast = koruby_parse(src, len, filename);
    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    /* For -e mode, current_file = ./script.rb so require_relative resolves
     * against cwd. */
    static char ecwd[PATH_MAX] = {0};
    const char *current_file;
    if (!file) {
        if (!getcwd(ecwd, sizeof(ecwd))) strcpy(ecwd, ".");
        strcat(ecwd, "/-e");
        current_file = ecwd;
    } else {
        current_file = file;
    }
    c = koruby_setup_ctx(current_file);

    OPTIMIZE(ast);

    /* Bootstrap: load Ruby-side helpers (Enumerable, Comparable include
     * targets, Rational/Complex, etc.) before running the user program. */
    koruby_eval_bootstrap(c);

    /* Decide if we should EVAL.  Runtime: always.
     * Build mode (bcfg.out_exe set): only if --run / --pg-compile. */
    int rc = 0;
    bool should_eval = bcfg.out_exe ? bcfg.run : true;
    if (should_eval) {
        astro_build_begin_aot_session();
        if (!bcfg.out_exe) {
            rc = koruby_run_ast(c, ast);
            if (rc != 0 && c->state == KORB_RAISE) return rc;
        } else {
            (void)koruby_run_ast(c, ast);
        }
        if (!bcfg.out_exe) astro_build_end_aot_session();
    } else if (bcfg.out_exe) {
        astro_build_begin_aot_session();
    }

    if (g_aot_compile) {
        fprintf(stderr, "[koruby] AOT compile: writing SD_*.c\n");
        astro_cs_compile(ast, NULL);
        for (uint32_t i = 0; i < code_repo.size; i++) {
            astro_cs_compile(code_repo.entries[i].body, NULL);
        }
        setenv("CCACHE_DISABLE", "1", 0);
        fprintf(stderr, "[koruby] AOT compile: building all.so\n");
        astro_cs_build(NULL);
    }

    /* Build mode: emit exe. */
    if (bcfg.out_exe) {
        int erc = koruby_do_build(ast, &bcfg);
        astro_build_end_aot_session();
        astro_build_config_dispose(&bcfg);
        return erc;
    }
    return rc;
}
