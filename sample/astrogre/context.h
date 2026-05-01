#ifndef ASTROGRE_CONTEXT_H
#define ASTROGRE_CONTEXT_H 1

/* memmem is a GNU extension; needed by node_grep_search_memmem. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Maximum number of capturing groups (including \0 = whole match). */
#define ASTROGRE_MAX_GROUPS 32

/* Match-result type returned by EVAL.  1 = continuation succeeded,
 * 0 = failed.  Width matters: the framework's SPECIALIZE emits
 * `(VALUE)<n>ULL` for uint64_t fields (class bitmaps), so VALUE must
 * be at least 64 bits wide or those casts truncate.  int64_t is the
 * convention across the other samples for the same reason. */
typedef int64_t VALUE;

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

#define LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

#endif /* ASTROGRE_CONTEXT_H */
