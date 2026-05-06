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

/* Parse the entire `src` of length `len` into a vector of VALUEs and
 * push it into the global input queue.  Returns 0 on success.  After
 * this the main loop and `input` / `inputs` both pull from the same
 * cursor, matching jq's semantics where mid-filter `input` consumes
 * what would otherwise be the next iteration. */
static int
load_input_queue_json(const char *src, size_t len)
{
    /* Use a growable VALUE array so we can later hand the items[] off
     * to the runtime. */
    size_t cap = 16, n = 0;
    VALUE *items = (VALUE *)GC_malloc(cap * sizeof(VALUE));
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
        if (n == cap) {
            cap *= 2;
            items = (VALUE *)GC_realloc(items, cap * sizeof(VALUE));
        }
        items[n++] = v;
        p = np;
    }
    nuq_input_queue_set(items, n);
    return 0;
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
    /* default: stream of JSON values, run filter once per value.
     * The queue is shared with `input` / `inputs` so mid-filter
     * pulls advance our cursor. */
    int rc = load_input_queue_json(src, len);
    if (rc) return rc;
    VALUE v;
    while (nuq_input_pull(&v)) nuq_run(c, filter, v);
    return 0;
}

int
main(int argc, char **argv)
{
    int argi = 1;
    const char *filter_src = NULL;
    char **input_files = NULL;
    int input_file_cnt = 0;

    /* Set a single short flag character.  Returns true if recognised. */
#define SHORT_FLAG(ch) ({                                                \
        bool _ok = true;                                                 \
        switch (ch) {                                                    \
          case 'c': OPTION.compact_output = true; break;                 \
          case 'r': OPTION.raw_output = true; break;                     \
          case 'R': OPTION.raw_input = true; break;                      \
          case 's': OPTION.slurp = true; break;                          \
          case 'n': OPTION.null_input = true; break;                     \
          case 'S': OPTION.sort_keys = true; break;                      \
          case 'e': OPTION.exit_status = true; break;                    \
          default:  _ok = false; break;                                  \
        }                                                                \
        _ok;                                                             \
    })

    while (argi < argc) {
        const char *a = argv[argi];
        if (a[0] != '-' || strcmp(a, "-") == 0) break;
        /* `--` ends option parsing */
        if (strcmp(a, "--") == 0) { argi++; break; }
        /* Long options first. */
        if (strcmp(a, "--tab") == 0) { OPTION.tab_indent = true; argi++; continue; }
        if (strcmp(a, "--indent") == 0) { OPTION.indent = atoi(argv[++argi]); argi++; continue; }
        if (strcmp(a, "--no-compile") == 0) { OPTION.no_compiled_code = true; argi++; continue; }
        if (strcmp(a, "--no-specialize") == 0) { OPTION.no_generate_specialized_code = true; argi++; continue; }
        if (strcmp(a, "--quiet") == 0) { OPTION.quiet = true; argi++; continue; }
        if (strcmp(a, "--dump-ast") == 0) { OPTION.dump_ast = true; argi++; continue; }
        if (strcmp(a, "--exit-status") == 0) { OPTION.exit_status = true; argi++; continue; }
        if (strcmp(a, "--seq") == 0) { OPTION.seq_output = true; argi++; continue; }
        if (strcmp(a, "--arg") == 0) {
            if (argi + 2 >= argc) { fprintf(stderr, "nuq: --arg needs name and value\n"); return 2; }
            nuq_user_arg_add(argv[argi+1], argv[argi+2], false);
            argi += 3; continue;
        }
        if (strcmp(a, "--argjson") == 0) {
            if (argi + 2 >= argc) { fprintf(stderr, "nuq: --argjson needs name and value\n"); return 2; }
            nuq_user_arg_add(argv[argi+1], argv[argi+2], true);
            argi += 3; continue;
        }
        if (strcmp(a, "--slurpfile") == 0) {
            if (argi + 2 >= argc) { fprintf(stderr, "nuq: --slurpfile needs name and file\n"); return 2; }
            if (!nuq_user_arg_add_file(argv[argi+1], argv[argi+2], false)) return 2;
            argi += 3; continue;
        }
        if (strcmp(a, "--rawfile") == 0) {
            if (argi + 2 >= argc) { fprintf(stderr, "nuq: --rawfile needs name and file\n"); return 2; }
            if (!nuq_user_arg_add_file(argv[argi+1], argv[argi+2], true)) return 2;
            argi += 3; continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr,
                "Usage: nuq [options] <filter> [file ...]\n"
                "  -c          compact output\n"
                "  -r          raw string output\n"
                "  -R          raw string input (each line becomes a string)\n"
                "  -s          slurp inputs into a single array\n"
                "  -n          null input\n"
                "  -S          sort object keys on output\n"
                "  -e          set exit status (5 if no truthy output)\n"
                "  --indent N  indent N spaces (default 2)\n"
                "  --tab       indent with tabs\n"
                "  --arg N V   bind variable $N to string V\n"
                "  --argjson N V  bind variable $N to JSON V\n"
                "  --slurpfile N F  bind variable $N to JSON values in F\n"
                "  --rawfile N F  bind variable $N to raw contents of F\n"
                "  --seq       output RFC 7464 record-separated JSON\n"
                "  --no-compile  disable Code Store specialisation\n"
                "  --quiet     suppress diagnostics\n"
                "  --dump-ast  dump the AST\n");
            return 0;
        }
        /* Short flag bundle: `-nc`, `-rsR`, etc.  Each character is
         * looked up via SHORT_FLAG; on first miss we fall through and
         * treat the original arg as a filter (so `-5` still works as a
         * literal-filter, and `-x` for unknown x is an error). */
        if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
            bool all_ok = true;
            for (const char *p = a + 1; *p; p++) {
                if (!SHORT_FLAG(*p)) { all_ok = false; break; }
            }
            if (all_ok) { argi++; continue; }
        }
        /* Not a recognised option — fall through to treat as filter. */
        break;
    }
#undef SHORT_FLAG

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
            nuq_compile_all_def_bodies();
            astro_cs_build(NULL);
            astro_cs_reload();
            astro_cs_load(filter, NULL);
            nuq_load_all_def_bodies();
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
    /* Pre-grow the EMIT pool so the UNLIKELY realloc branch in
     * nuq_pool_push stays cold for typical bench sizes. */
    c->pool_capa = 4096;
    c->pool = (VALUE *)GC_malloc(c->pool_capa * sizeof(VALUE));
    nuq_user_args_bind(c);

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
    /* jq-compatible exit codes:
     *   0 = OK with at least one truthy output (or `-e` not set)
     *   1 = JSON parse / process error encountered
     *   2 = compile / option error (caller already set rc=2)
     *   5 = `-e` set and no truthy output produced (jq's --exit-status) */
    if (nuq_had_error && rc == 0) rc = 1;
    if (OPTION.exit_status && !nuq_had_truthy_output && rc == 0) rc = 5;
    return rc;
}
