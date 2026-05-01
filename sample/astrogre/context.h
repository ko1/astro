#ifndef ASTROGRE_CONTEXT_H
#define ASTROGRE_CONTEXT_H 1

/* memmem is a GNU extension; needed by node_grep_search_memmem. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdint.h>
/* AVX2 intrinsics for SIMD class-scan nodes (node_grep_search_range,
 * node_grep_search_class_truffle).  The host's CPU has AVX2 today;
 * non-AVX2 paths fall back to a scalar loop. */
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Maximum number of capturing groups (including \0 = whole match). */
#define ASTROGRE_MAX_GROUPS 32

/* Match-result type returned by EVAL.  Width matters: the framework's
 * SPECIALIZE emits `(VALUE)<n>ULL` for uint64_t fields (class bitmaps),
 * so VALUE must be at least 64 bits wide or those casts truncate.
 * int64_t is the convention across the other samples for the same
 * reason. */
typedef int64_t VALUE;

/* Match-result codes used as VALUE return values throughout node.def.
 *
 * Two protocols share the same VALUE channel:
 *
 *   regex chain (rep / alt / lit / class / lookaround / capture / ...)
 *     uses MR_FAIL / MR_STOP only.  MR_FAIL means "this branch didn't
 *     match here"; MR_STOP means "the chain completed (re_succ)".
 *     Backtracking is encoded in this 2-value protocol — a parent
 *     node sees MR_FAIL from its child and undoes its own state.
 *
 *   scanner ↔ body chain boundary (case-A factorization)
 *     adds MR_CONTINUE for the `-c` / default-print modes.  When the
 *     body chain ends in `node_action_continue` instead of `re_succ`,
 *     it returns MR_CONTINUE to tell the scanner "match recorded,
 *     please resume scanning from c->pos".  Regex internals never
 *     see MR_CONTINUE; only the scanner has to switch on all three.
 *
 * MR_FAIL is required to be 0 so the conventional `if (!r) ...` and
 * `if (r) ...` patterns work in regex-chain code that only deals
 * with MR_FAIL / MR_STOP.  MR_CONTINUE = 2 is treated as truthy by
 * those (correctly: it's a "matched" outcome) but the scanner has
 * to compare explicitly to distinguish from MR_STOP. */
enum match_result {
    MR_FAIL     = 0,
    MR_STOP     = 1,
    MR_CONTINUE = 2,
};

typedef enum {
    AGRE_ENC_ASCII = 0,
    AGRE_ENC_UTF8  = 1,
} agre_encoding_t;

/* One frame on the repetition stack.  Pushed by node_re_rep when entering
 * a repeat, read by node_re_rep_cont (a singleton continuation node that
 * sits as `next` of every repeat-body) to decide whether to iterate again
 * or to fall through to the rep's outer next. */
struct rep_frame {
    struct Node *body;
    struct Node *outer_next;
    int32_t min;        /* remaining min iterations (>= 0) */
    int32_t max;        /* remaining max iterations; -1 = unbounded */
    uint32_t greedy;    /* 1 = greedy, 0 = lazy */
    struct rep_frame *prev;
};

typedef struct CTX_struct {
    const uint8_t *str;
    size_t str_len;
    size_t pos;

    /* Capture state.  Index 0 is the whole match. */
    size_t starts[ASTROGRE_MAX_GROUPS];
    size_t ends[ASTROGRE_MAX_GROUPS];
    bool   valid[ASTROGRE_MAX_GROUPS];
    int    n_groups;

    /* Repetition stack. */
    struct rep_frame *rep_top;
    struct Node *rep_cont_sentinel;

    /* Encoding / flags */
    agre_encoding_t encoding;
    bool case_insensitive;
    bool multiline;     /* ruby /m: dot matches newline */

    /* Output for count-mode nodes (action_count and future siblings).
     * These actions own the per-match work; the caller reads the count
     * from here after EVAL returns. */
    long count_result;

    /* Per-file state used by node_action_emit_match_line and friends.
     * Caller (CLI driver) sets `fname` / `out` once per file and zeros
     * `lineno` / `lineno_pos`; emit actions advance `lineno` by counting
     * newlines from `lineno_pos` to the next match's line start. */
    const char *fname;
    FILE *out;
    long lineno;
    size_t lineno_pos;
} CTX;

struct astrogre_option {
    /* exec mode (mirrored from calc/koruby for ASTro framework hooks) */
    bool static_lang;
    bool compile_only;
    bool pg_mode;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;

    /* misc */
    bool quiet;
    bool dump_ast;
    bool cs_verbose;
};

extern struct astrogre_option OPTION;

/* Bit flags for node_action_emit_match_line's `opts` operand.  Defined
 * here (not just in node.def) so the CLI driver in main.c can build
 * the value before constructing the AST. */
#define ASTROGRE_EMIT_FNAME  0x01u
#define ASTROGRE_EMIT_LINENO 0x02u
#define ASTROGRE_EMIT_COLOR  0x04u

#define LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

#endif /* ASTROGRE_CONTEXT_H */
