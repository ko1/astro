/* arcel CLI entry point.
 *
 * Three subcommands, mirroring test/celgo_ref / test/celcpp_ref so the
 * same harness drives all three:
 *
 *   arcel eval -e '<expr>' [-i '<json>']
 *   arcel bench -e '<expr>' [-i '<json>'] -n <iterations>
 *   arcel repl                            (1 JSON envelope per stdin line)
 *
 * Global flags accepted *before* the subcommand:
 *   --no-compile       skip ASTro AOT specialization
 *   -q / --quiet       silence hit/miss tracing
 *
 * Implementation: this file is a thin wrapper over the public API in
 * arcel.h / arcel_lib.c.  Same calls embedders use.  Keeping the CLI
 * on the library API ensures we can't drift into "library says X but
 * the CLI does Y" territory.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdbool.h>
#include "arcel.h"

/* CLI-only options; arcel.h has no equivalent because they're
 * presentation choices for the binary, not eval semantics. */
static struct {
    bool no_compile;
    bool quiet;
} cli;

/* ---- helpers ----------------------------------------------------- */

static void
die_compile(const char *const expr, const char *const err)
{
    /* Match `ERROR: <msg>` form so the harness's line-protocol stays
     * synchronized.  expr unused; future could include source span. */
    (void)expr;
    printf("ERROR: %s\n", err && *err ? err : "compile failed");
}

static void
print_value(arcel_value v)
{
    char buf[4096];
    arcel_format_json(v, buf, sizeof buf);
    fputs(buf, stdout);
    fputc('\n', stdout);
}

/* ---- subcommands ------------------------------------------------- */

static int
cmd_eval(arcel_env *const env, const int argc, char **const argv)
{
    const char *expr = NULL;
    const char *input = NULL;
    for (int i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "-e") == 0 && i + 1 < argc) expr  = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input = argv[++i];
        else { fprintf(stderr, "arcel eval: unknown arg '%s'\n", argv[i]); return 2; }
    }
    if (!expr) { fprintf(stderr, "arcel eval: -e <expr> required\n"); return 2; }

    char err_buf[256];
    arcel_program *const prg = arcel_compile(env, expr, -1, err_buf, sizeof err_buf);
    if (!prg) { die_compile(expr, err_buf); return 0; }

    arcel_activation *const act = arcel_activation_new(env);
    if (input && *input) {
        if (arcel_activation_load_json(act, input, strlen(input), err_buf, sizeof err_buf) < 0) {
            printf("ERROR: %s\n", err_buf);
            arcel_activation_free(act);
            arcel_program_free(prg);
            return 0;
        }
    }

    print_value(arcel_eval(prg, act));
    arcel_activation_free(act);
    arcel_program_free(prg);
    return 0;
}

static int
cmd_bench(arcel_env *const env, const int argc, char **const argv)
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

    char err_buf[256];
    arcel_program *const prg = arcel_compile(env, expr, -1, err_buf, sizeof err_buf);
    if (!prg) { fprintf(stderr, "arcel bench: %s\n", err_buf); return 1; }

    arcel_activation *const act = arcel_activation_new(env);
    if (input && *input) {
        if (arcel_activation_load_json(act, input, strlen(input), err_buf, sizeof err_buf) < 0) {
            fprintf(stderr, "arcel bench: input: %s\n", err_buf);
            return 1;
        }
    }

    /* Warm. */
    for (int i = 0; i < 1000; i++) (void)arcel_eval(prg, act);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < iters; i++) (void)arcel_eval(prg, act);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                   (long long)(t1.tv_nsec - t0.tv_nsec);
    double per = (double)ns / (double)iters;
    printf("%ld %lld %.3f\n", iters, ns, per);

    arcel_activation_free(act);
    arcel_program_free(prg);
    return 0;
}

/* ---- repl: minimal JSON envelope parser ------------------------- */

/* Pull a top-level string field "<key>" from `buf` as a fresh
 * allocation (caller frees).  Returns NULL if key missing or value
 * isn't a string.  Used for the `e` field which is the CEL source. */
static char *
extract_json_string(const char *buf, const char *key, uint32_t *out_len)
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
    if (*q != '"') return NULL;
    q++;
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

/* Pull the raw `"i": <value>` field as a JSON snippet (caller frees). */
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
cmd_repl(arcel_env *const env, int argc, char **argv)
{
    (void)argc; (void)argv;

    char  *buf = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&buf, &cap, stdin)) > 0) {
        uint32_t expr_len = 0;
        char *const expr = extract_json_string(buf, "e", &expr_len);
        if (!expr) { puts("ERROR: bad envelope"); fflush(stdout); continue; }

        char *const input = extract_json_raw(buf, "i");

        char err_buf[256];
        arcel_program *const prg = arcel_compile(env, expr, expr_len, err_buf, sizeof err_buf);
        if (!prg) {
            printf("ERROR: %s\n", err_buf);
            fflush(stdout);
            free(expr); free(input);
            continue;
        }

        arcel_activation *const act = arcel_activation_new(env);
        if (input) {
            if (arcel_activation_load_json(act, input, strlen(input), err_buf, sizeof err_buf) < 0) {
                printf("ERROR: %s\n", err_buf);
                fflush(stdout);
                arcel_activation_free(act);
                arcel_program_free(prg);
                free(expr); free(input);
                continue;
            }
        }

        print_value(arcel_eval(prg, act));
        fflush(stdout);

        arcel_activation_free(act);
        arcel_program_free(prg);
        free(expr);
        free(input);
    }
    free(buf);
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
        "  -q, --quiet          suppress hit/miss progress (default in repl/bench)\n",
        progname);
}

int
main(int argc, char **argv)
{
    cli.quiet = true;     /* default; -v re-enables internally via the OPTION knob */

    int idx = 1;
    while (idx < argc && argv[idx][0] == '-') {
        if      (strcmp(argv[idx], "--no-compile") == 0) cli.no_compile = true;
        else if (strcmp(argv[idx], "-q") == 0 || strcmp(argv[idx], "--quiet") == 0) cli.quiet = true;
        else if (strcmp(argv[idx], "-h") == 0 || strcmp(argv[idx], "--help") == 0) { usage(stdout, argv[0]); return 0; }
        else { fprintf(stderr, "arcel: unknown global flag '%s'\n", argv[idx]); return 2; }
        idx++;
    }
    if (idx >= argc) { usage(stderr, argv[0]); return 2; }

    arcel_env *const env = arcel_env_new();
    if (cli.no_compile) arcel_env_set_no_compile(env, true);

    const char *const cmd = argv[idx++];
    int rc;
    if      (strcmp(cmd, "eval")  == 0) rc = cmd_eval (env, argc - idx, argv + idx);
    else if (strcmp(cmd, "bench") == 0) rc = cmd_bench(env, argc - idx, argv + idx);
    else if (strcmp(cmd, "repl")  == 0) rc = cmd_repl (env, argc - idx, argv + idx);
    else {
        fprintf(stderr, "arcel: unknown subcommand '%s'\n", cmd);
        usage(stderr, argv[0]);
        rc = 2;
    }
    arcel_env_free(env);
    return rc;
}
