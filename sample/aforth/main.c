/* aforth — Forth subset on ASTro
 *
 * Driver: parses CLI options, calls into parse.c to build the AST, then
 * runs the toplevel via EVAL().  Word-definition state and the parser
 * itself live in parse.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "context.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"

struct aforth_option OPTION;

/* ===== context ===== */

CTX *
aforth_ctx_new(void)
{
    CTX *c = calloc(1, sizeof(CTX));
    c->dstack_base = calloc(AFORTH_DSTACK_SIZE, sizeof(VALUE));
    c->dstack_end  = c->dstack_base + AFORTH_DSTACK_SIZE;
    c->dsp = c->dstack_base;
    c->rstack_base = calloc(AFORTH_RSTACK_SIZE, sizeof(VALUE));
    c->rstack_end  = c->rstack_base + AFORTH_RSTACK_SIZE;
    c->rsp = c->rstack_base;
    c->dostack_base = calloc(AFORTH_DOSTACK_SIZE, sizeof(struct aforth_do_frame));
    c->dostack_end  = c->dostack_base + AFORTH_DOSTACK_SIZE;
    c->dop = c->dostack_base;
    c->vars = calloc(AFORTH_VARS_SIZE, sizeof(VALUE));
    c->vars_used = aforth_vars_used_top;
    c->leave_flag = 0;
    return c;
}

void
aforth_ctx_free(CTX *c)
{
    free(c->dstack_base);
    free(c->rstack_base);
    free(c->dostack_base);
    free(c->vars);
    free(c);
}

void
aforth_run(CTX *c, NODE *toplevel)
{
    EVAL(c, toplevel);
}

void
aforth_aot_compile_all(NODE *toplevel)
{
    /* register every word body + the toplevel as compile entries */
    if (!OPTION.quiet)
        fprintf(stderr, "aforth: AOT compiling %u words + toplevel\n",
                aforth_word_count);
    astro_cs_compile(toplevel, NULL);
    for (uint32_t i = 0; i < aforth_word_count; i++) {
        if (aforth_word_table[i]) astro_cs_compile(aforth_word_table[i], NULL);
    }
    /* `-flto` lets gcc inline across SD .c boundaries inside all.so.  The
     * cross-TU win is 5-20% on every bench (measured 2026-05-04).  We pass
     * via the env-var hooks rather than the extra_cflags arg so the user
     * can disable by setting `ASTRO_EXTRA_CFLAGS=` etc. before invoking.   */
    setenv("ASTRO_EXTRA_CFLAGS", "-flto", 0);
    setenv("ASTRO_EXTRA_LDFLAGS", "-flto", 0);
    astro_cs_build(NULL);
    astro_cs_reload();
    /* re-resolve dispatchers so this run uses the freshly-baked SDs */
    astro_cs_load(toplevel, NULL);
    for (uint32_t i = 0; i < aforth_word_count; i++) {
        if (aforth_word_table[i]) astro_cs_load(aforth_word_table[i], NULL);
    }
}

/* ===== entry ===== */

static void
usage(void)
{
    fprintf(stderr,
        "usage: aforth [options] FILE.fs\n"
        "  -q              quiet (suppress framework chatter)\n"
        "  --no-compile    don't try to load specialized SDs\n"
        "  --no-codegen    don't generate specialized SDs\n"
        "  --aot-compile   compile every entry to code_store/all.so, then run\n"
        "  --dump-ast      dump the parsed AST and exit\n"
    );
    exit(1);
}

int
main(int argc, char *argv[])
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-q") == 0)              OPTION.quiet = true;
        else if (strcmp(a, "--no-compile") == 0)  OPTION.no_compiled_code = true;
        else if (strcmp(a, "--no-codegen") == 0)  OPTION.no_generate_specialized_code = true;
        else if (strcmp(a, "--aot-compile") == 0) OPTION.aot_compile = true;
        else if (strcmp(a, "--dump-ast") == 0)    OPTION.dump_ast = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage();
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); }
        else if (!path) path = a;
        else { fprintf(stderr, "extra arg %s\n", a); usage(); }
    }
    if (!path) usage();

    INIT();
    NODE *toplevel = aforth_parse_file(path);

    if (OPTION.dump_ast) {
        DUMP(stdout, toplevel, true);
        printf("\n");
        return 0;
    }

    if (OPTION.aot_compile) aforth_aot_compile_all(toplevel);

    CTX *c = aforth_ctx_new();
    aforth_run(c, toplevel);
    aforth_ctx_free(c);
    return 0;
}
