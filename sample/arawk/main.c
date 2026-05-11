// arawk — POSIX awk subset on the ASTro framework.
//
// Usage:
//   arawk [options] 'program' [input_file ...]
//   arawk [options] -e 'program' [input_file ...]
//   arawk [options] -f program.awk [input_file ...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "context.h"
#include "node.h"
#include "astro_code_store.h"

NODE *PARSE_SOURCE(const char *source);
uint32_t arawk_globals_count(void);

struct awk_option OPTION;
size_t node_cnt;

void
code_repo_add(const char *name, NODE *body, bool force_add)
{
    (void)name; (void)body; (void)force_add;
}

// Look up a user-defined function body by name.  Called from
// `arawk_node_call_user` once per call site (no cache yet — Phase 2+
// can add a callcache pattern like astr's `astr_callcache`).
NODE *
arawk_resolve_body(CTX *c, const char *name, uint32_t *out_params_cnt)
{
    for (unsigned int i = 0; i < c->func_set_cnt; i++) {
        if (strcmp(c->func_set[i].name, name) == 0) {
            if (out_params_cnt) *out_params_cnt = c->func_set[i].params_cnt;
            return c->func_set[i].body;
        }
    }
    fprintf(stderr, "arawk: undefined function `%s`\n", name);
    exit(2);
}

static CTX *
create_context(void)
{
    CTX *c = (CTX *)GC_malloc(sizeof(CTX));
    uint32_t nglob = arawk_globals_count();
    size_t env_size = nglob < 4096 ? 4096 : nglob;
    c->env = c->fp = (VALUE *)GC_malloc(sizeof(VALUE) * env_size);
    c->func_set = (struct function_entry *)GC_malloc(sizeof(struct function_entry) * 256);
    c->func_set_cnt = 0;
    // Initialise all globals to AWK_UNINIT, then set specials to
    // their POSIX defaults.
    for (size_t i = 0; i < env_size; i++) c->env[i] = AWK_UNINIT;
    c->env[AWK_GLOB_NR]       = AWK_FIX(0);
    c->env[AWK_GLOB_NF]       = AWK_FIX(0);
    c->env[AWK_GLOB_FNR]      = AWK_FIX(0);
    c->env[AWK_GLOB_FS]       = awk_make_string(" ", 1);
    c->env[AWK_GLOB_OFS]      = awk_make_string(" ", 1);
    c->env[AWK_GLOB_ORS]      = awk_make_string("\n", 1);
    c->env[AWK_GLOB_RS]       = awk_make_string("\n", 1);
    c->env[AWK_GLOB_FILENAME] = awk_make_string("", 0);
    c->env[AWK_GLOB_SUBSEP]   = awk_make_string("\034", 1);
    c->rec.record = NULL;
    c->rec.record_len = 0;
    c->rec.record_v = 0;
    c->rec.fields = NULL;
    c->rec.fields_capa = 0;
    c->rec.nf = 0;
    c->rec.fields_split = false;
    c->cur_input = NULL;
    c->cur_input_idx = 0;
    c->input_done = false;
    return c;
}

static char *
read_file_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "arawk: cannot open `%s`\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "arawk: read error\n");
        exit(2);
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void
build_code_store_aot(NODE *ast)
{
    // ccache misbehaves under sandboxed runs; disable to keep `cc`
    // straight-through.  Inherited from astr.
    setenv("CCACHE_DISABLE", "1", 0);

    if (ast) astro_cs_compile(ast, NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
}

static void
usage(void)
{
    fputs("usage: arawk [options] 'program' [file ...]\n"
          "       arawk -e 'program' [file ...]\n"
          "       arawk -f program.awk [file ...]\n"
          "options:\n"
          "  -e PROG       program text\n"
          "  -f FILE       read program from FILE\n"
          "  -i / --plain  skip AOT bake (interpret only)\n"
          "  -c            AOT-compile before running\n"
          "  --dump-ast    dump AST then run\n"
          "  --ccs         clear code_store before run\n",
          stderr);
}

static void
parse_argv(int argc, char *argv[])
{
    char **inputs = NULL;
    int    inputs_cnt = 0, inputs_capa = 0;
    bool program_set = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-i") || !strcmp(a, "--plain"))  OPTION.plain = true;
        else if (!strcmp(a, "-c") || !strcmp(a, "--aot"))    OPTION.compile_first = true;
        else if (!strcmp(a, "-b"))                           OPTION.skip_bake = true;
        else if (!strcmp(a, "--aot-compile"))                OPTION.compile_only = true;
        else if (!strcmp(a, "--ccs"))                        OPTION.clear_store = true;
        else if (!strcmp(a, "--dump-ast"))                   OPTION.dump_ast = true;
        else if (!strcmp(a, "-e")) {
            if (i + 1 >= argc) { usage(); exit(2); }
            OPTION.program_text = argv[++i];
            program_set = true;
        }
        else if (!strcmp(a, "-f")) {
            if (i + 1 >= argc) { usage(); exit(2); }
            OPTION.program_file = argv[++i];
            program_set = true;
        }
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "arawk: unknown option: %s\n", a);
            usage();
            exit(2);
        }
        else {
            if (!program_set) {
                OPTION.program_text = a;
                program_set = true;
            }
            else {
                if (inputs_cnt >= inputs_capa) {
                    inputs_capa = inputs_capa ? inputs_capa * 2 : 4;
                    inputs = (char **)realloc(inputs, sizeof(char *) * (size_t)inputs_capa);
                }
                inputs[inputs_cnt++] = (char *)a;
            }
        }
    }
    OPTION.input_files = inputs;
    OPTION.input_file_cnt = inputs_cnt;
}

int
main(int argc, char *argv[])
{
    GC_init();

    parse_argv(argc, argv);

    char *prog_text = NULL;
    if (OPTION.program_file) {
        prog_text = read_file_text(OPTION.program_file);
    }
    else if (OPTION.program_text) {
        prog_text = strdup(OPTION.program_text);
    }
    else {
        usage();
        return 2;
    }

    NODE *ast = PARSE_SOURCE(prog_text);

    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    if (OPTION.clear_store) {
        int rc = system("rm -rf code_store");
        if (rc != 0) fprintf(stderr, "arawk: --ccs: rm -rf code_store failed (rc=%d)\n", rc);
    }
    if (!OPTION.plain) INIT();

    if (OPTION.compile_first && !OPTION.plain && !OPTION.skip_bake) {
        build_code_store_aot(ast);
    }

    if (!OPTION.plain) OPTIMIZE(ast);

    if (OPTION.compile_only) return 0;

    CTX *c = create_context();
    RESULT r = EVAL(c, ast, c->env);
    int rc = 0;
    if (r.state == RESULT_EXIT) {
        if (AWK_IS_FIX(r.value)) rc = (int)AWK_FIX_VAL(r.value);
        else                     rc = (int)awk_to_num(r.value);
    }
    // Flush + close any pipes / output files opened via `print | ...`,
    // `print > ...`, `print >> ...`.  Pipes need explicit pclose so
    // `sort` etc. finish writing before the awk process exits.
    awk_close_all_streams();
    return rc;
}
