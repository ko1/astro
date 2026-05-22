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

extern void sc_repo_clear(void);

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
        "usage: koruby [options] [file]\n"
        "  -e <code>      eval code\n"
        "  --dump         dump AST\n"
        "  -c             compile only (no run, generate node_specialized.c)\n"
        "  -q             quiet\n"
        "  -v             verbose\n");
    exit(1);
}

static void
generate_specialized_code(NODE *ast)
{
    FILE *fp = fopen("node_specialized.c", "w");
    if (!fp) { perror("node_specialized.c"); return; }
    sc_repo_clear();

    /* main */
    SPECIALIZE(fp, ast);

    /* code repo entries (methods) */
    extern NODE *code_repo_find(node_hash_t);
    /* cheating: walk our internal repo via DUMP-friendly iteration not available;
       Instead, iterate entries directly through accessor below. */
    extern void koruby_specialize_repo(FILE *fp);
    koruby_specialize_repo(fp);

    fprintf(fp, "struct specialized_code sc_entries[] = {\n");
    /* main entry */
    if (ast && HASH(ast)) {
        fprintf(fp, "    { .hash = 0x%lxLL, .dispatcher_name = \"%s\", .dispatcher = %s },\n",
                (unsigned long)HASH(ast), ast->head.dispatcher_name, ast->head.dispatcher_name);
    }
    extern void koruby_emit_sc_entries(FILE *fp);
    koruby_emit_sc_entries(fp);
    fprintf(fp, "};\n#define NODE_SPECIALIZED_INCLUDED 1\n");
    fclose(fp);
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

/* --build OUTPUT [opts] -e EXPR | file.rb */
static int
koruby_build_subcommand(int argc, char **argv)
{
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    int rest_argc; char **rest_argv;
    if (astro_build_subcommand_parse(argc, argv, &bcfg,
                                      &rest_argc, &rest_argv) != 0) {
        return 1;
    }

    /* Find source spec in rest_argv: either `-e EXPR` or a file path. */
    const char *e_code = NULL;
    const char *file = NULL;
    for (int i = 0; i < rest_argc; i++) {
        const char *a = rest_argv[i];
        if (strcmp(a, "-e") == 0 && i + 1 < rest_argc) {
            e_code = rest_argv[++i];
        } else if (!file && a[0] != '-') {
            file = a;
        }
    }
    free(rest_argv);
    if (!e_code && !file) {
        fprintf(stderr, "koruby --build: missing source (-e EXPR or file path)\n");
        astro_build_config_dispose(&bcfg);
        return 1;
    }

    INIT();
    korb_runtime_init();
    astro_cs_init("code_store", KORUBY_SRC_DIR_DEFAULT, 0);

    /* Read source.  We DO NOT EVAL it at build time — the build is
     * side-effect-free (no print, no file I/O).  Consequence: methods
     * registered at EVAL time (via koruby's node_def) aren't visible
     * here, so only the program AST is baked.  When require-interception
     * lands (file-map embedding) all required files will be parsed at
     * build time, and their methods will be baked too. */
    char *src = NULL;
    size_t srclen = 0;
    if (e_code) {
        src = (char *)e_code;
        srclen = strlen(e_code);
    } else {
        src = koruby_read_file(file, &srclen);
        if (!src) { astro_build_config_dispose(&bcfg); return 1; }
    }

    static NODE *ast;
    ast = koruby_parse(src, srclen, file ? file : "(eval)");

    astro_build_begin_aot_session();
    if (bcfg.aot_compile || bcfg.pg_compile) {
        astro_cs_compile(ast, NULL);
        setenv("CCACHE_DISABLE", "1", 0);
    }

    /* If bcfg.run is set, execute the program once during build for
     * file discovery (and PG profile if --pg-compile).  TODO: hook
     * the PGSD bake here once it's wired through the new model. */
    if (bcfg.run) {
        CTX *c = koruby_setup_ctx(file ? file : "(eval)");
        koruby_eval_bootstrap(c);
        (void)koruby_run_ast(c, ast);
        /* Re-bake methods registered by the run. */
        if (bcfg.aot_compile || bcfg.pg_compile) {
            for (uint32_t i = 0; i < code_repo.size; i++) {
                astro_cs_compile(code_repo.entries[i].body, NULL);
            }
        }
    }

    bcfg.src_dir = KORUBY_SRC_DIR_DEFAULT;
    bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
    static const char *sources[] = {
        "node.c", "parse.c", "object.c", "builtins.c",
        "bootstrap_src.c", "koruby_runtime.c", "exe_main.c", NULL,
    };
    bcfg.sources = sources;
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
    bcfg.sample_ldflags = koruby_sample_ldflags;
    bcfg.sample_cflags = koruby_sample_cflags;

    int rc = astro_build_aot_executable(ast, &bcfg, "code_store");
    astro_build_end_aot_session();
    astro_build_config_dispose(&bcfg);
    return rc;
}

int main(int argc, char *argv[])
{
    /* --build subcommand — framework-owned argv space. */
    if (argc >= 2 && strcmp(argv[1], "--build") == 0) {
        return koruby_build_subcommand(argc - 1, argv + 1);
    }

    INIT();
    korb_runtime_init();

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

    const char *e_code = NULL;
    const char *file = NULL;
    int script_arg_start = argc;  /* args beyond the script are passed to ARGV */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0 && i+1 < argc) { e_code = argv[++i]; }
        else if (strcmp(a, "--dump") == 0) { OPTION.dump_ast = true; }
        else if (strcmp(a, "-c") == 0) { OPTION.compile_only = true; }
        else if (strcmp(a, "-q") == 0) { OPTION.quiet = true; }
        else if (strcmp(a, "-v") == 0) { OPTION.verbose = true; }
        else if (strcmp(a, "--aot-compile") == 0) { g_aot_compile = true; }
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
    VALUE argv_array = korb_ary_new();
    for (int i = script_arg_start; i < argc; i++) {
        korb_ary_push(argv_array, korb_str_new_cstr(argv[i]));
    }
    korb_const_set(korb_vm->object_class, korb_intern("ARGV"), argv_array);
    /* $0 / $PROGRAM_NAME — the script path. */
    {
        VALUE pn = korb_str_new_cstr(file ? file : (e_code ? "-e" : argv[0]));
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
    CTX *c = koruby_setup_ctx(current_file);

    OPTIMIZE(ast);

    /* Bootstrap: load Ruby-side helpers (Enumerable, Comparable include
     * targets, Rational/Complex, etc.) before running the user program. */
    koruby_eval_bootstrap(c);

    /* In compile_only mode (-c), still run the program so that
     * `require_relative` chains parse all source files (registering all
     * methods into code_repo) before we emit node_specialized.c. */
    int rc = 0;
    if (!OPTION.compile_only) {
        rc = koruby_run_ast(c, ast);
        if (rc != 0 && c->state == KORB_RAISE) return rc;
    } else {
        /* compile_only: run and ignore exit code (we still want to bake). */
        (void)koruby_run_ast(c, ast);
    }

    if (OPTION.compile_only) {
        generate_specialized_code(ast);
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
    return rc;
}

/* hooks for specialized-code generation */

void koruby_specialize_repo(FILE *fp) {
    extern void SPECIALIZE(FILE *, NODE *);
    for (uint32_t i = 0; i < code_repo.size; i++) {
        SPECIALIZE(fp, code_repo.entries[i].body);
    }
}

void koruby_emit_sc_entries(FILE *fp) {
    for (uint32_t i = 0; i < code_repo.size; i++) {
        NODE *b = code_repo.entries[i].body;
        if (HASH(b)) {
            fprintf(fp, "    { .hash = 0x%lxLL, .dispatcher_name = \"%s\", .dispatcher = %s },\n",
                    (unsigned long)HASH(b), b->head.dispatcher_name, b->head.dispatcher_name);
        }
    }
}
