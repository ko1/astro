/*
 * main.c — command-line entry point.
 *
 * Usage:  nuq [options] <filter> [file ...]
 *
 * Options:
 *   -c               compact output (default: 2-space indent)
 *   -r               raw output (don't quote strings)
 *   -R               raw input (each line is a string)
 *   -s               slurp (collect all inputs into one array)
 *   -n               null input (don't read JSON)
 *   --tab            tab indent (currently ignored)
 *   --indent N       indent N spaces (default 2)
 *   --no-compile     disable Code Store specialisation
 *   --quiet          suppress diagnostic output
 *   --dump-ast       dump the parsed filter AST
 *
 * Reads JSON from stdin or files, applies the filter to each input
 * value, and prints the results.
 */
#include "context.h"
#include "node.h"
#include "astro_code_store.h"
#include <errno.h>

struct nuq_option OPTION = { .indent = 2 };

static char *
slurp_stream(FILE *fp, size_t *len_out)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        size_t n = fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
    }
    *len_out = len;
    return buf;
}

static int
process_input(CTX *c, NODE *filter, const char *src, size_t len)
{
    if (OPTION.null_input) {
        nuq_run(c, filter, NUQ_NULL);
        return 0;
    }
    if (OPTION.raw_input && OPTION.slurp) {
        VALUE s = nuq_make_string(src, len);
        nuq_run(c, filter, s);
        return 0;
    }
    if (OPTION.raw_input) {
        /* one line at a time */
        const char *p = src, *end = src + len;
        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            const char *line_end = nl ? nl : end;
            VALUE s = nuq_make_string(p, line_end - p);
            nuq_run(c, filter, s);
            p = nl ? nl + 1 : end;
        }
        return 0;
    }
    if (OPTION.slurp) {
        VALUE arr = nuq_make_array(0);
        const char *p = src, *end = src + len;
        while (p < end) {
            const char *np;
            char *err = NULL;
            VALUE v = nuq_json_parse(p, end - p, &np, &err);
            if (err) {
                fprintf(stderr, "nuq: parse error: %s\n", err);
                return 1;
            }
            nuq_array_push(arr, v);
            p = np;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        }
        nuq_run(c, filter, arr);
        return 0;
    }
    /* default: stream of JSON values */
    const char *p = src, *end = src + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        const char *np;
        char *err = NULL;
        VALUE v = nuq_json_parse(p, end - p, &np, &err);
        if (err) {
            fprintf(stderr, "nuq: parse error: %s\n", err);
            return 1;
        }
        nuq_run(c, filter, v);
        p = np;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    int argi = 1;
    const char *filter_src = NULL;
    char **input_files = NULL;
    int input_file_cnt = 0;

    while (argi < argc) {
        const char *a = argv[argi];
        if (a[0] != '-' || strcmp(a, "-") == 0) break;
        /* `--` ends option parsing */
        if (strcmp(a, "--") == 0) { argi++; break; }
        /* If it doesn't match a known option but does start with `-`,
         * treat it as the filter (so `-5` works as a literal-filter). */
        if (strcmp(a, "-c") == 0) { OPTION.compact_output = true; argi++; continue; }
        if (strcmp(a, "-r") == 0) { OPTION.raw_output = true; argi++; continue; }
        if (strcmp(a, "-R") == 0) { OPTION.raw_input = true; argi++; continue; }
        if (strcmp(a, "-s") == 0) { OPTION.slurp = true; argi++; continue; }
        if (strcmp(a, "-n") == 0) { OPTION.null_input = true; argi++; continue; }
        if (strcmp(a, "--tab") == 0) { OPTION.tab_indent = true; argi++; continue; }
        if (strcmp(a, "-S") == 0) { OPTION.sort_keys = true; argi++; continue; }
        if (strcmp(a, "--indent") == 0) { OPTION.indent = atoi(argv[++argi]); argi++; continue; }
        if (strcmp(a, "--no-compile") == 0) { OPTION.no_compiled_code = true; argi++; continue; }
        if (strcmp(a, "--no-specialize") == 0) { OPTION.no_generate_specialized_code = true; argi++; continue; }
        if (strcmp(a, "--quiet") == 0) { OPTION.quiet = true; argi++; continue; }
        if (strcmp(a, "--dump-ast") == 0) { OPTION.dump_ast = true; argi++; continue; }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr,
                "Usage: nuq [options] <filter> [file ...]\n"
                "  -c          compact output\n"
                "  -r          raw string output\n"
                "  -R          raw string input (each line becomes a string)\n"
                "  -s          slurp inputs into a single array\n"
                "  -n          null input\n"
                "  --indent N  indent N spaces (default 2)\n"
                "  --no-compile  disable Code Store specialisation\n"
                "  --quiet     suppress diagnostics\n"
                "  --dump-ast  dump the AST\n");
            return 0;
        }
        /* Not a recognised option — fall through to treat as filter. */
        break;
    }

    if (argi >= argc) {
        fprintf(stderr, "nuq: missing filter (use --help)\n");
        return 1;
    }
    filter_src = argv[argi++];
    input_files = &argv[argi];
    input_file_cnt = argc - argi;

    INIT();

    NODE *filter = nuq_parse_filter(filter_src);

    if (OPTION.dump_ast) {
        DUMP(stderr, filter, false);
        fputc('\n', stderr);
    }

    if (!OPTION.no_generate_specialized_code && !OPTION.no_compiled_code) {
        if (!filter->head.flags.is_specialized) {
            astro_cs_compile(filter, NULL);
            astro_cs_build(NULL);
            astro_cs_reload();
            astro_cs_load(filter, NULL);
        }
    }

    /* CTX *must* be GC-allocated: it holds pointers (var_stack,
     * funcs, emit_buf) into GC-managed memory.  A malloc'd CTX is
     * invisible to Boehm's scanner, which can then collect those
     * blocks while CTX still references them — manifests as a
     * corrupt-`$x` lookup once the foreach loop's emit_buf grows
     * enough to trigger GC. */
    CTX *c = (CTX *)GC_malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->error = NUQ_NULL;
    c->emit_buf = nuq_make_array(0);

    int rc = 0;
    if (input_file_cnt == 0) {
        size_t L; char *buf = OPTION.null_input ? NULL : slurp_stream(stdin, &L);
        rc = process_input(c, filter, buf, OPTION.null_input ? 0 : L);
        free(buf);
    } else {
        for (int i = 0; i < input_file_cnt; i++) {
            FILE *fp = fopen(input_files[i], "r");
            if (!fp) { fprintf(stderr, "nuq: %s: %s\n", input_files[i], strerror(errno)); rc = 1; continue; }
            size_t L; char *buf = slurp_stream(fp, &L);
            fclose(fp);
            int sub = process_input(c, filter, buf, L);
            free(buf);
            if (sub) rc = sub;
        }
    }
    return rc;
}
