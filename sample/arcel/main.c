/* arcel CLI entry point.
 *
 * Subcommands (mirrors test/celgo_ref so the same harness drives both):
 *
 *   arcel eval -e '<expr>' [-i '<json>']
 *   arcel bench -e '<expr>' [-i '<json>'] -n <iterations>
 *   arcel repl
 *
 * Global flags accepted *before* the subcommand:
 *   --no-compile       skip ASTro AOT specialization
 *   --dump-ast         print parsed AST to stderr
 *   -q / --quiet       silence hit/miss tracing
 */

#include <ctype.h>
#include <inttypes.h>
#include <time.h>
#include "context.h"
#include "node.h"
#include "value.h"
#include "input.h"
#include "parser.h"
#include "astro_code_store.h"

struct arcel_option OPTION;

/* ---- compile + eval helpers ------------------------------------- */

static NODE *
compile_expr(const char *const src, const char **const out_err)
{
    NODE *const ast = arcel_parse(src, out_err);
    if (!ast) return NULL;
    if (!OPTION.no_compiled_code) {
        if (!ast->head.flags.is_specialized) {
            astro_cs_compile(ast, NULL);
            astro_cs_build(NULL);
            astro_cs_reload();
            astro_cs_load(ast, NULL);
        }
    }
    if (OPTION.dump_ast) {
        DUMP(stderr, ast, true);
        fputc('\n', stderr);
    }
    return ast;
}

/* Set up the eval context: parse `-i` bindings into the persistent
 * arena and reset the transient arena.  Bindings then survive across
 * many EVAL() calls (bench mode loops without re-parsing JSON every
 * iteration).  Pass `reset_bindings=true` to force reparse. */
static const char *
ctx_setup(CTX *const c, const char *const input_json, bool reset_bindings)
{
    c->bind_top = 0;
    c->last_err = NULL;
    arcel_arena_reset(&c->arena);

    if (reset_bindings || !c->bindings) {
        arcel_arena_reset(&c->bind_arena);
        c->bindings = NULL;
        if (input_json && *input_json) {
            VALUE v = arcel_parse_json(&c->bind_arena, input_json, (uint32_t)strlen(input_json));
            if (v.tag == AC_ERR) return v.err;
            if (v.tag == AC_MAP) {
                c->bindings = v.map;
            } else {
                /* Wrap a non-object input under the conventional name
                 * "input" (mirrors celgo_ref so the harness sees both
                 * binaries the same way). */
                arcel_map *const m = arcel_map_new(&c->bind_arena, 1);
                m->entries[0].key = V_STR("input", 5);
                m->entries[0].val = v;
                c->bindings = m;
            }
        }
    }
    return NULL;
}

static void
ctx_init(CTX *const c)
{
    arcel_arena_init(&c->arena);
    arcel_arena_init(&c->bind_arena);
    c->bindings = NULL;
    c->bind_top = 0;
    c->last_err = NULL;
}

static void
ctx_destroy(CTX *const c)
{
    arcel_arena_free(&c->arena);
    arcel_arena_free(&c->bind_arena);
}

/* ---- subcommands ------------------------------------------------- */

static int
cmd_eval(int argc, char **argv)
{
    const char *expr = NULL;
    const char *input = NULL;
    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "-e") == 0 && i + 1 < argc) expr  = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input = argv[++i];
        else { fprintf(stderr, "arcel eval: unknown arg '%s'\n", argv[i]); return 2; }
    }
    if (!expr) { fprintf(stderr, "arcel eval: -e <expr> required\n"); return 2; }

    const char *err = NULL;
    NODE *const ast = compile_expr(expr, &err);
    if (!ast) { printf("ERROR: %s\n", err ? err : "parse failed"); return 0; }

    CTX c; ctx_init(&c);
    err = ctx_setup(&c, input, true);
    if (err) { printf("ERROR: %s\n", err); ctx_destroy(&c); return 0; }

    VALUE r = EVAL(&c, ast);
    arcel_print_json(stdout, r);
    fputc('\n', stdout);
    ctx_destroy(&c);
    return 0;
}

static int
cmd_bench(int argc, char **argv)
{
    const char *expr = NULL;
    const char *input = NULL;
    long iters = 1000000;
    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "-e") == 0 && i + 1 < argc) expr  = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) iters = strtol(argv[++i], NULL, 10);
        else { fprintf(stderr, "arcel bench: unknown arg '%s'\n", argv[i]); return 2; }
    }
    if (!expr) { fprintf(stderr, "arcel bench: -e <expr> required\n"); return 2; }

    const char *err = NULL;
    NODE *const ast = compile_expr(expr, &err);
    if (!ast) { fprintf(stderr, "arcel bench: %s\n", err ? err : "parse failed"); return 1; }

    CTX c; ctx_init(&c);
    /* Set up bindings ONCE before the loop — they live in bind_arena
     * which arena_reset() doesn't touch.  cel-go's bench API is
     * `prg.Eval(binds)` with bindings reused across calls, so this
     * keeps the comparison apples-to-apples. */
    err = ctx_setup(&c, input, true);
    if (err) { fprintf(stderr, "arcel bench: input: %s\n", err); ctx_destroy(&c); return 1; }

    /* warm */
    for (int i = 0; i < 1000; i++) {
        arcel_arena_reset(&c.arena);
        c.bind_top = 0;
        (void)EVAL(&c, ast);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < iters; i++) {
        arcel_arena_reset(&c.arena);
        c.bind_top = 0;
        (void)EVAL(&c, ast);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                   (long long)(t1.tv_nsec - t0.tv_nsec);
    double per = (double)ns / (double)iters;
    printf("%ld %lld %.3f\n", iters, ns, per);
    ctx_destroy(&c);
    return 0;
}

/* ---- repl: minimal JSON envelope parser ------------------------- */

/* Pull a top-level string field "<key>" from `buf` as a fresh
 * allocation (caller frees).  Returns NULL if key missing or value
 * isn't a string. */
static char *
extract_json_string(const char *buf, const char *key, uint32_t *out_len)
{
    /* find `"key":` */
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *q = strstr(buf, pat);
    if (!q) return NULL;
    q += strlen(pat);
    while (*q == ' ' || *q == '\t') q++;
    if (*q != ':') return NULL;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '"') return NULL;
    q++;
    /* consume escaped string into a fresh buffer */
    char *const out = (char *)malloc(strlen(buf) + 1);
    uint32_t n = 0;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) {
            switch (q[1]) {
                case 'n':  out[n++] = '\n'; break;
                case 't':  out[n++] = '\t'; break;
                case 'r':  out[n++] = '\r'; break;
                case 'b':  out[n++] = '\b'; break;
                case 'f':  out[n++] = '\f'; break;
                case '"':  out[n++] = '"';  break;
                case '\\': out[n++] = '\\'; break;
                case '/':  out[n++] = '/';  break;
                case 'u': {
                    if (q[2] && q[3] && q[4] && q[5]) {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = q[2 + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                            else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                        }
                        if (cp < 0x80) out[n++] = (char)cp;
                        else if (cp < 0x800) {
                            out[n++] = (char)(0xC0 | (cp >> 6));
                            out[n++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[n++] = (char)(0xE0 | (cp >> 12));
                            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[n++] = (char)(0x80 | (cp & 0x3F));
                        }
                        q += 4;
                    }
                    break;
                }
                default:   out[n++] = q[1]; break;
            }
            q += 2;
        } else {
            out[n++] = *q++;
        }
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

/* Pull the raw `"i": <value>` field as a JSON-encoded string, so the
 * arcel JSON parser can consume it directly.  Returns NULL if `"i"`
 * is missing or null. */
static char *
extract_json_raw(const char *buf, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *q = strstr(buf, pat);
    if (!q) return NULL;
    q += strlen(pat);
    while (*q == ' ' || *q == '\t') q++;
    if (*q != ':') return NULL;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (!*q) return NULL;
    if (strncmp(q, "null", 4) == 0) return NULL;
    /* find the matching end of value; respect nested quotes/braces.
     * good enough for the harness's compact JSON. */
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const char *start = q;
    while (*q) {
        char c = *q;
        if (esc) { esc = false; q++; continue; }
        if (in_str) {
            if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            q++;
            continue;
        }
        if (c == '"') { in_str = true; q++; continue; }
        if (c == '{' || c == '[') { depth++; q++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) break;
            depth--;
            q++; continue;
        }
        if ((c == ',' || c == '\n') && depth == 0) break;
        q++;
    }
    uint32_t n = (uint32_t)(q - start);
    char *const out = (char *)malloc(n + 1);
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

static int
cmd_repl(int argc, char **argv)
{
    (void)argc; (void)argv;

    char  *buf = NULL;
    size_t cap = 0;
    ssize_t len;

    CTX c; ctx_init(&c);

    while ((len = getline(&buf, &cap, stdin)) > 0) {
        /* Rewind the variadic-children side array before each parse.
         * Each repl iteration discards the previous AST, so its
         * arcel_node_arr entries are no longer reachable.  Without
         * this the array grows linearly with envelope count
         * (3 entries per `[1,2,3]` literal × N envelopes). */
        arcel_node_arr_reset();
        arcel_const_list_reset();

        uint32_t expr_len = 0;
        char *const expr = extract_json_string(buf, "e", &expr_len);
        if (!expr) { puts("ERROR: bad envelope"); fflush(stdout); continue; }

        char *const input = extract_json_raw(buf, "i");

        const char *err = NULL;
        /* Use _n form so embedded NULs in bytes-literal source
         * (`b'\x00'` etc.) don't truncate parsing. */
        NODE *const ast = arcel_parse_n(expr, expr_len, &err);
        if (!ast) {
            printf("ERROR: %s\n", err ? err : "parse failed");
            fflush(stdout);
            free(expr); free(input);
            continue;
        }
        if (!OPTION.no_compiled_code) {
            if (!ast->head.flags.is_specialized) {
                astro_cs_compile(ast, NULL);
                astro_cs_build(NULL);
                astro_cs_reload();
                astro_cs_load(ast, NULL);
            }
        }

        const char *serr = ctx_setup(&c, input, true);
        if (serr) { printf("ERROR: %s\n", serr); fflush(stdout); free(expr); free(input); continue; }

        VALUE r = EVAL(&c, ast);
        arcel_print_json(stdout, r);
        fputc('\n', stdout);
        fflush(stdout);

        free(expr);
        free(input);
    }
    free(buf);
    ctx_destroy(&c);
    return 0;
}

/* ---- main -------------------------------------------------------- */

static void
usage(FILE *const out, const char *const progname)
{
    fprintf(out,
        "Usage: %s [global-flags] {eval|bench|repl} [options]\n"
        "\n"
        "Subcommands:\n"
        "  eval  -e EXPR [-i JSON]                  evaluate once, print result\n"
        "  bench -e EXPR [-i JSON] -n ITERATIONS    measure ns/op\n"
        "  repl                                     stream JSON envelopes from stdin\n"
        "\n"
        "Global flags (must precede the subcommand):\n"
        "      --no-compile     interpreter only (skip AOT specialization)\n"
        "      --dump-ast       print parsed AST to stderr before eval\n"
        "  -q, --quiet          suppress hit/miss progress (default in repl/bench)\n",
        progname);
}

int
main(int argc, char **argv)
{
    OPTION.quiet = true;          /* silent by default; matches harness expectations */

    int idx = 1;
    while (idx < argc && argv[idx][0] == '-') {
        if      (strcmp(argv[idx], "--no-compile") == 0) OPTION.no_compiled_code = true;
        else if (strcmp(argv[idx], "-q") == 0 || strcmp(argv[idx], "--quiet") == 0) OPTION.quiet = true;
        else if (strcmp(argv[idx], "--dump-ast") == 0)   OPTION.dump_ast = true;
        else if (strcmp(argv[idx], "-v") == 0)           OPTION.quiet = false;
        else if (strcmp(argv[idx], "-h") == 0 || strcmp(argv[idx], "--help") == 0) { usage(stdout, argv[0]); return 0; }
        else { fprintf(stderr, "arcel: unknown global flag '%s'\n", argv[idx]); return 2; }
        idx++;
    }
    if (idx >= argc) { usage(stderr, argv[0]); return 2; }

    INIT();

    const char *const cmd = argv[idx++];
    if      (strcmp(cmd, "eval")  == 0) return cmd_eval(argc - idx,  argv + idx);
    else if (strcmp(cmd, "bench") == 0) return cmd_bench(argc - idx, argv + idx);
    else if (strcmp(cmd, "repl")  == 0) return cmd_repl(argc - idx,  argv + idx);
    else {
        fprintf(stderr, "arcel: unknown subcommand '%s'\n", cmd);
        usage(stderr, argv[0]);
        return 2;
    }
}
