/*
 * Built-in self-test + microbench harness.  Speaks directly to the
 * astrogre engine (not via backend.h) since these exercise our impl,
 * not whatever backend the user picked at the grep level.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "node.h"
#include "context.h"
#include "parse.h"

static int test_pass = 0, test_fail = 0;

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void
expect_match(const char *pat_lit, const char *input, int expect_start, int expect_end)
{
    astrogre_pattern *p = astrogre_parse_literal(pat_lit, strlen(pat_lit));
    if (!p) { fprintf(stderr, "TEST FAIL parse: %s\n", pat_lit); test_fail++; return; }
    astrogre_match_t m;
    bool r = astrogre_search(p, input, strlen(input), &m);
    bool ok;
    if (expect_start < 0) ok = !r;
    else                  ok = r && (int)m.starts[0] == expect_start && (int)m.ends[0] == expect_end;
    if (ok) test_pass++;
    else {
        test_fail++;
        if (r) fprintf(stderr, "FAIL: %s vs %s -- got %zu..%zu, want %d..%d\n",
                       pat_lit, input, m.starts[0], m.ends[0], expect_start, expect_end);
        else   fprintf(stderr, "FAIL: %s vs %s -- got nil, want %d..%d\n",
                       pat_lit, input, expect_start, expect_end);
    }
    astrogre_pattern_free(p);
}

static void
expect_capture(const char *pat_lit, const char *input, int idx, const char *expected)
{
    astrogre_pattern *p = astrogre_parse_literal(pat_lit, strlen(pat_lit));
    if (!p) { fprintf(stderr, "TEST FAIL parse: %s\n", pat_lit); test_fail++; return; }
    astrogre_match_t m;
    bool r = astrogre_search(p, input, strlen(input), &m);
    bool ok = false;
    if (expected == NULL) ok = r && !m.valid[idx];
    else if (r && m.valid[idx]) {
        size_t cap_len = m.ends[idx] - m.starts[idx];
        if (cap_len == strlen(expected) &&
            memcmp(input + m.starts[idx], expected, cap_len) == 0) ok = true;
    }
    if (ok) test_pass++;
    else { test_fail++;
        fprintf(stderr, "FAIL CAP: %s vs %s [%d] want %s\n",
                pat_lit, input, idx, expected ? expected : "(nil)");
    }
    astrogre_pattern_free(p);
}

int
astrogre_run_self_tests(void)
{
    test_pass = test_fail = 0;

    expect_match("/abc/", "xxabcyy", 2, 5);
    expect_match("/abc/", "xxabxyy", -1, -1);
    expect_match("/abc/i", "xxAbCyy", 2, 5);
    expect_match("/a.c/", "abc", 0, 3);
    expect_match("/a.c/", "a\nc", -1, -1);
    expect_match("/a.c/m", "a\nc", 0, 3);
    expect_match("/\\Aabc/", "abcd", 0, 3);
    expect_match("/\\Aabc/", "xabcd", -1, -1);
    expect_match("/abc\\z/", "xxabc", 2, 5);
    expect_match("/abc\\Z/", "xxabc\n", 2, 5);
    expect_match("/^abc/", "xy\nabcd", 3, 6);
    expect_match("/abc$/", "xxabc\nyy", 2, 5);
    expect_match("/\\bfoo\\b/", "x foo y", 2, 5);
    expect_match("/\\bfoo\\b/", "xfoo", -1, -1);
    expect_match("/\\Bfoo/", "xfoo", 1, 4);
    expect_match("/[a-c]+/", "xxbabxx", 2, 5);
    expect_match("/[^abc]+/", "abcXYZabc", 3, 6);
    expect_match("/\\d+/", "x12345y", 1, 6);
    expect_match("/\\w+/", "  hello_42  ", 2, 10);
    expect_match("/a*/", "bbb", 0, 0);
    expect_match("/a+/", "bbab", 2, 3);
    expect_match("/a{2,3}/", "aaaa", 0, 3);
    expect_match("/a{2}/", "aaaa", 0, 2);
    expect_match("/a.*?b/", "axxbyybb", 0, 4);
    expect_match("/a.*b/",  "axxbyybb", 0, 8);
    expect_match("/(a|ab)*c/", "abc", 0, 3);
    expect_capture("/(a|ab)*c/", "abc", 1, "ab");
    expect_capture("/(a)(b)(c)/", "abc", 1, "a");
    expect_capture("/(a)(b)(c)/", "abc", 2, "b");
    expect_capture("/(a)(b)(c)/", "abc", 3, "c");
    expect_capture("/(\\d+)-(\\d+)/", "x12-345y", 1, "12");
    expect_capture("/(\\d+)-(\\d+)/", "x12-345y", 2, "345");
    expect_match("/(.)\\1/", "abccd", 2, 4);
    expect_match("/(.)\\1/", "abcd", -1, -1);
    expect_match("/(?:abc){2}/", "abcabc", 0, 6);
    expect_match("/foo(?=bar)/", "xfoobaryy", 1, 4);
    expect_match("/foo(?=bar)/", "xfoozz", -1, -1);
    expect_match("/foo(?!bar)/", "xfoozz", 1, 4);
    expect_match("/foo(?!bar)/", "xfoobar", -1, -1);
    expect_match("/ a \\s b /x", "axb a bxx", 4, 7);
    expect_match("/a.c/", "a\xc3\xa9""c", 0, 4);
    expect_match("/x{1,3}y/", "xxxxy", 1, 5);

    /* search_from: enumeration */
    {
        astrogre_pattern *p = astrogre_parse_literal("/\\d+/", 5);
        astrogre_match_t m;
        const char *s = "a12 b34 c56";
        size_t pos = 0;
        int found[3], n = 0;
        while (n < 3 && astrogre_search_from(p, s, strlen(s), pos, &m)) {
            found[n++] = (int)m.starts[0];
            pos = m.ends[0] == m.starts[0] ? m.ends[0] + 1 : m.ends[0];
        }
        if (n == 3 && found[0] == 1 && found[1] == 5 && found[2] == 9) test_pass++;
        else { test_fail++;
            fprintf(stderr, "FAIL search_from: enum got %d hits\n", n);
        }
        astrogre_pattern_free(p);
    }

    /* fixed-string: regex metacharacters treated as literal */
    {
        astrogre_pattern *p = astrogre_parse_fixed(".*", 2, 0);
        astrogre_match_t m;
        if (astrogre_search(p, "abc.*xyz", 8, &m) && m.starts[0] == 3 && m.ends[0] == 5)
            test_pass++;
        else { test_fail++; fprintf(stderr, "FAIL fixed-string\n"); }
        astrogre_pattern_free(p);
    }

    printf("\n%d passed, %d failed\n", test_pass, test_fail);
    return test_fail ? 1 : 0;
}

static void
bench_one(const char *name, const char *pat_lit, const char *input, int iters)
{
    astrogre_pattern *p = astrogre_parse_literal(pat_lit, strlen(pat_lit));
    if (!p) return;
    astrogre_match_t m;
    astrogre_search(p, input, strlen(input), &m);
    double t0 = mono_seconds();
    int hits = 0;
    for (int i = 0; i < iters; i++)
        if (astrogre_search(p, input, strlen(input), &m)) hits++;
    double t1 = mono_seconds();
    printf("%-24s %s  (iters=%d, hits=%d) %.3f s  (%.2f ns/match)\n",
           name, pat_lit, iters, hits, t1 - t0, (t1 - t0) * 1e9 / iters);
    astrogre_pattern_free(p);
}

extern void astrogre_pattern_aot_compile(astrogre_pattern *p, bool verbose);
extern struct astrogre_option OPTION;
extern int errno;
#include <errno.h>

/* Reads `file` into memory once, then runs N iterations of
 * astrogre_search over the whole buffer.  This isolates the regex
 * engine cost from per-line getline / CTX-init overhead, so the
 * AOT-vs-interp delta becomes visible. */
int
astrogre_run_file_bench(const char *file, const char *pat_lit, int iters, bool aot, bool plain)
{
    if (plain) OPTION.no_compiled_code = true;

    FILE *fp = fopen(file, "rb");
    if (!fp) { fprintf(stderr, "open %s: %s\n", file, strerror(errno)); return 2; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 2; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return 2; }
    buf[sz] = 0;
    fclose(fp);

    astrogre_pattern *p = astrogre_parse_literal(pat_lit, strlen(pat_lit));
    if (!p) { free(buf); return 2; }

    if (aot) astrogre_pattern_aot_compile(p, false);

    /* warmup — count all matches once (also confirms total
     * matches in the buffer for the iter loop below) */
    astrogre_match_t m;
    int total_matches = 0;
    {
        size_t pos = 0;
        while (astrogre_search_from(p, buf, (size_t)sz, pos, &m)) {
            total_matches++;
            pos = (m.ends[0] == m.starts[0]) ? m.ends[0] + 1 : m.ends[0];
        }
    }

    /* Bench: full-sweep count (mimics grep -c semantics) so
     * comparison with grep / ripgrep / onigmo is apples-to-apples. */
    double t0 = mono_seconds();
    int hits = 0;
    for (int i = 0; i < iters; i++) {
        size_t pos = 0;
        while (astrogre_search_from(p, buf, (size_t)sz, pos, &m)) {
            hits++;
            pos = (m.ends[0] == m.starts[0]) ? m.ends[0] + 1 : m.ends[0];
        }
    }
    double t1 = mono_seconds();
    double total = t1 - t0;
    double per = total / iters;
    double bytes_per_sec = (double)sz / per;
    printf("%-22s %s  total_matches=%d  iters=%d hits=%d  total=%.3fs  per=%.3fms  %.1f MB/s\n",
           pat_lit,
           plain ? "interp"     : (aot ? "aot-cached" : "default"),
           total_matches, iters, hits, total, per * 1e3, bytes_per_sec / (1024 * 1024));

    astrogre_pattern_free(p);
    free(buf);
    return 0;
}

int
astrogre_run_microbench(void)
{
    size_t input_len = 1 << 14;
    char *input = (char *)malloc(input_len + 1);
    for (size_t i = 0; i < input_len; i++)
        input[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[(i * 17 + 3) % 36];
    memcpy(input + input_len - 5, "match", 5);
    input[input_len] = 0;

    int iters_lit = 200000, iters_re = 50000;
    bench_one("literal-tail", "/match/",         input, iters_lit);
    bench_one("literal-i",    "/MATCH/i",        input, iters_lit);
    bench_one("class-digit",  "/\\d+/",          input, iters_re);
    bench_one("class-word",   "/\\w+/",          input, iters_re);
    bench_one("alt-3",        "/cat|dog|match/", input, iters_re);
    bench_one("rep-greedy",   "/a.*z/",          input, iters_re);
    bench_one("group-alt",    "/(a|b|c)+m/",     input, iters_re);
    bench_one("anchored",     "/\\Amatch/",      input, 2000000);
    bench_one("dot-star",     "/.*match/",       input, iters_re);
    free(input);
    return 0;
}
