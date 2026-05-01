#ifndef ASTROGRE_PARSE_H
#define ASTROGRE_PARSE_H 1

#include "node.h"

/* Compiled pattern. */
typedef struct astrogre_pattern {
    NODE *root;          /* root of match AST (chain ending in node_re_succ) */
    /* Lazily-built alternate roots for the per-mode action-chain
     * scanners.  All produced by the case-A factorization: scanner
     * (node_scan_lit_dual_byte) + body chain that does the per-match
     * work.  Built on first use for pure-literal patterns; NULL for
     * patterns that don't qualify or modes that haven't been hit yet. */
    NODE *count_lines_root;       /* `-c PURE_LITERAL`: count → lineskip → continue */
    NODE *print_lines_root;       /* default print: count → emit → lineskip → continue */
    uint32_t print_lines_opts;    /* the emit_opts used when print_lines_root was built */
    int n_groups;        /* number of capturing groups (excluding group 0) */
    bool case_insensitive;
    bool multiline;
    agre_encoding_t encoding;
    bool anchored_bos;   /* true when pattern starts with \A — search loop can stop after pos==0 */
    bool fixed_string;   /* true when built via astrogre_parse_fixed (-F mode) */
    char *pat;           /* original pattern text (heap, owned) */
} astrogre_pattern;

/* Match result (filled by astrogre_search / astrogre_search_from). */
typedef struct astrogre_match {
    bool matched;
    size_t starts[ASTROGRE_MAX_GROUPS];
    size_t ends[ASTROGRE_MAX_GROUPS];
    bool   valid[ASTROGRE_MAX_GROUPS];
    int    n_groups;
} astrogre_match_t;

bool astrogre_search(astrogre_pattern *p, const char *str, size_t len, astrogre_match_t *out);

/* Resume search from a starting offset.  Used by grep --color, -o, and
 * any caller that needs to enumerate non-overlapping matches on a
 * single buffer.  The caller is responsible for advancing past
 * zero-width matches. */
bool astrogre_search_from(astrogre_pattern *p, const char *str, size_t len, size_t start_from, astrogre_match_t *out);

/* Parse a Ruby-style regex source.  `pat` is the bytes between the slashes
 * (post-prism unescaping); `pat_len` is its length; `flags` is the bitmask
 * from prism's pm_regular_expression_flags_t.  Returns NULL on error
 * (and logs to stderr).  Caller frees the returned pattern with
 * astrogre_pattern_free. */
astrogre_pattern *astrogre_parse(const char *pat, size_t pat_len, uint32_t prism_flags);

/* Hash of the compiled pattern AST root — same value `astro_cs_load`
 * keys on, exposed for diagnostics (`-d`, `--cs-status`). */
uint64_t astrogre_pattern_hash(astrogre_pattern *p);

/* True iff the pattern's root is one of the SIMD / libc-prefilter
 * wrappers (memchr / memmem / byteset / range / class_scan).  Used
 * by the grep CLI to decide whether the whole-file mmap path is
 * worth taking — for plain node_grep_search the per-line streaming
 * loop is faster on typical inputs because each line is short. */
bool astrogre_pattern_has_prefilter(astrogre_pattern *p);

/* If the pattern is essentially a single literal byte sequence with
 * no other side-effects (no captures the caller reads, the body is
 * exactly cap_start(0) → lit → cap_end(0) → succ), return the
 * literal bytes / length.  Used by the grep CLI's count / line-only
 * paths to skip the engine entirely and run a tight memmem loop —
 * closes most of the gap with grep for `-c /lit/` style invocations.
 * Returns false if the pattern has any other structure. */
bool astrogre_pattern_pure_literal(astrogre_pattern *p,
                                    const char **out_bytes, size_t *out_len);

/* Drive astro_cs_compile + cs_build + cs_reload for this pattern.
 * Called once per unique-hash pattern in --aot-compile mode.  Idempotent —
 * repeated calls with the same hash hit the dedup. */
void astrogre_pattern_aot_compile(astrogre_pattern *p, bool verbose);

/* Count the matching lines of a pure-literal pattern in [str, str+len).
 * Returns -1 if the pattern doesn't qualify (not pure-literal-shaped, or
 * empty needle).  Builds an action chain `count → lineskip → continue`
 * under a `node_scan_lit_dual_byte` scanner; the framework AOT-bakes
 * everything into one SD with the needle as immediate AVX2 operands. */
long astrogre_pattern_count_lines(astrogre_pattern *p, const char *str, size_t len);

/* Default-print: emit every matching line in [str, str+len) for a
 * pure-literal pattern.  Action chain is `count → emit_match_line(opts)
 * → lineskip → continue` under the same scanner; returns the number of
 * matching lines emitted, or -1 if the pattern doesn't qualify.
 *
 * `emit_opts` is the OR of `ASTROGRE_EMIT_FNAME` / `_LINENO` / `_COLOR`
 * (see node.def).  `fname` and `out` are passed through to the emit
 * action via CTX. */
long astrogre_pattern_print_lines(astrogre_pattern *p, const char *str, size_t len,
                                   const char *fname, FILE *out, uint32_t emit_opts);

/* Parse a /pat/flags-style source string (incl. surrounding slashes and
 * trailing flag chars).  Useful from CLI / tests when not going through
 * prism. */
astrogre_pattern *astrogre_parse_literal(const char *src, size_t len);

/* Build a "literal-bytes" pattern that matches `bytes` exactly — the
 * regex parser is bypassed entirely so the bytes can contain any
 * regex metacharacters with no escaping.  This is the -F mode entry
 * point.  `prism_flags` is honoured for /i, /n, /u (case-fold and
 * encoding); /m and /x are ignored since there's no syntax to enable. */
astrogre_pattern *astrogre_parse_fixed(const char *bytes, size_t len, uint32_t prism_flags);

/* Parse `src` as Ruby code with prism, find the FIRST regex literal in it
 * (top-level, not interpolated), and return the parsed pattern.  Returns
 * NULL if no regex literal is found.  This is the path used to drive the
 * engine from Ruby source. */
astrogre_pattern *astrogre_parse_via_prism(const char *src, size_t len);

void astrogre_pattern_free(astrogre_pattern *p);

#endif
