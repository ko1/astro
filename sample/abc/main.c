#include <setjmp.h>
#include <unistd.h>
#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"
#include "astro_build.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct abc_option OPTION;

#ifndef ABC_SRC_DIR
#define ABC_SRC_DIR "."
#endif
#ifndef ASTRO_RUNTIME_DIR
#define ASTRO_RUNTIME_DIR "."
#endif

// Provided by node.c for runtime-error unwinding.
jmp_buf *bc_get_jmp(void);
void bc_set_jmp_active(int v);

// Optional embedded bc math library (defined at the bottom of this file).
static const char *abc_math_lib(void);

// --- run one source buffer, statement by statement -------------------
// Each top-level statement runs under its own error guard so a runtime
// error (divide by zero, etc.) aborts just that statement, like bc.

static void
run_source(CTX *const c, const char *const src)
{
    Program prog = parse_program(c, src);
    if (prog.count < 0) return;                 // syntax error already reported
    if (OPTION.aot_compile && !OPTION.no_compiled_code && prog.count > 0)
        bc_aot_specialize(c, prog.stmts, prog.count);
    for (int i = 0; i < prog.count; i++) {
        if (setjmp(*bc_get_jmp()) == 0) {
            bc_set_jmp_active(1);
            EVAL(c, prog.stmts[i]);
        }
        bc_set_jmp_active(0);
    }
    free(prog.stmts);
}

static char *
slurp_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "abc: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static char *
slurp_stdin(void)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
    }
    buf[len] = '\0';
    return buf;
}

// --- interactive REPL: accumulate until braces/quotes balance --------

static int
input_complete(const char *s)
{
    int depth = 0, instr = 0;
    for (; *s; s++) {
        if (instr) { if (*s == '\\' && s[1]) s++; else if (*s == '"') instr = 0; continue; }
        if (*s == '"') instr = 1;
        else if (*s == '{') depth++;
        else if (*s == '}') depth--;
    }
    return depth <= 0 && !instr;
}

static void
repl(CTX *const c)
{
    char acc[1 << 16]; acc[0] = '\0';
    for (;;) {
#ifdef USE_READLINE
        char *line = readline(acc[0] ? "" : "");
        if (!line) break;
#else
        char lbuf[4096];
        if (!fgets(lbuf, sizeof(lbuf), stdin)) break;
        char *line = lbuf;
#endif
        if (strlen(acc) + strlen(line) + 2 < sizeof(acc)) { strcat(acc, line); strcat(acc, "\n"); }
#ifdef USE_READLINE
        if (*line) add_history(line);
        free(line);
#endif
        if (input_complete(acc)) { run_source(c, acc); acc[0] = '\0'; fflush(stdout); }
    }
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [file ...]\n"
        "\n"
        "abc — an arbitrary-precision bc calculator on ASTro.\n"
        "\n"
        "abc-specific options:\n"
        "  -e EXPR        evaluate EXPR and exit\n"
        "  -l             load the standard math library and set scale=20\n"
        "  -w             warn about POSIX-incompatible constructs (accepted)\n"
        "      --disasm   print the specialized code disassembly (AOT)\n"
        "\n"
        "With no file and a tty on stdin, %s starts an interactive REPL.\n"
        "\n",
        prog, prog);
    astro_print_build_help(stderr);
}

int
main(int argc, char *argv[])
{
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

    if (bcfg.help_requested)    { usage(argv[0]); return 0; }
    if (bcfg.version_requested) { printf("abc (ASTro %s)\n", ASTRO_VERSION); return 0; }

    if (bcfg.quiet)       OPTION.quiet = true;
    if (bcfg.verbose)     OPTION.verbose = true;
    if (bcfg.plain)       OPTION.no_compiled_code = true;
    if (bcfg.aot_compile) OPTION.aot_compile = true;
    if (bcfg.pg_compile)  OPTION.pg_compile = true;

    const char *eval_expr = NULL;
    const char *files[256]; int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) { fprintf(stderr, "abc: -e requires an argument\n"); return 1; }
            eval_expr = argv[i];
        }
        else if (strcmp(argv[i], "-l") == 0)        OPTION.math_lib = true;
        else if (strcmp(argv[i], "-w") == 0)        OPTION.warn = true;
        else if (strcmp(argv[i], "--disasm") == 0)  OPTION.disasm = true;
        else if (argv[i][0] == '-' && argv[i][1])   { fprintf(stderr, "abc: unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
        else if (nfiles < 256)                      files[nfiles++] = argv[i];
    }

    INIT();
    CTX *const c = bc_make_context();

    if (OPTION.math_lib) run_source(c, abc_math_lib());

    // Build mode: bake the program (and function bodies) into an exe.
    if (bcfg.out_exe) {
        const char *src = eval_expr ? eval_expr : (nfiles ? slurp_file(files[0]) : "");
        Program prog = parse_program(c, src);
        NODE *root = program_to_root(&prog);
        astro_build_begin_aot_session();
        if (bcfg.aot_compile || bcfg.pg_compile) astro_cs_compile(root, NULL);
        bcfg.src_dir = ABC_SRC_DIR;
        bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
        static const char *sources[] = { "parse.c", "node.c", "bcnum.c", "exe_main.c", NULL };
        bcfg.sources = sources;
        static const char *ldflags[] = { "-lgmp", "-lgc", "-ldl", NULL };
        bcfg.sample_ldflags = ldflags;
        int rc = astro_build_aot_executable(root, &bcfg, "code_store");
        astro_build_end_aot_session();
        astro_build_config_dispose(&bcfg);
        return rc;
    }

    if (eval_expr) { run_source(c, eval_expr); fflush(stdout); return 0; }

    for (int i = 0; i < nfiles; i++) {
        char *src = slurp_file(files[i]);
        if (!src) return 1;
        run_source(c, src);
    }

    if (nfiles == 0) {
        if (isatty(fileno(stdin))) repl(c);
        else { char *src = slurp_stdin(); run_source(c, src); free(src); }
    }
    else if (isatty(fileno(stdin))) {
        repl(c);   // files then interactive (bc behaviour), only on a tty
    }

    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------
// Embedded math library (loaded with -l).
//
// The transcendental functions a/c/e/l/s/j are not yet implemented; -l
// currently just sets the default scale to 20 (done in bc_make_context)
// and reserves the names.  sqrt() is always available as a builtin.
// ---------------------------------------------------------------------
static const char *
abc_math_lib(void)
{
    return "scale = 20\n";
}
