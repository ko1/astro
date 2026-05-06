#ifndef NUQ_CONTEXT_H
#define NUQ_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <inttypes.h>
#include <time.h>

extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_init(void);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

/* ---- per-run arena allocator ----
 *
 * Most VALUE-bearing allocations during filter evaluation
 * (`nuq_make_array` / `nuq_make_object` / `nuq_make_string` /
 * `nuq_clone`, plus container growth) live for one filter
 * invocation: the intermediate values are printed at end of run and
 * then dropped.  Routing them through a bump-pointer arena that's
 * reset after every run drops most GC_malloc / free / sweep traffic.
 *
 * The arena chunks themselves are GC_malloc'd so Boehm scans them
 * conservatively — VALUEs in arena that point into Boehm-managed
 * objects (e.g. input JSON, interned literals) stay alive.
 *
 * Parse-time / module-load / startup paths flip `nuq_alloc_perm` to
 * route through Boehm-managed memory so the resulting values survive
 * across runs (literals, AST kname_value, --argjson values, module
 * data imports). */
extern bool nuq_alloc_perm;

void *nuq_arena_alloc_slow(size_t sz);
void  nuq_arena_reset(void);

extern char *nuq_arena_cur;
extern char *nuq_arena_end;

static inline void *
nuq_arena_alloc(size_t sz)
{
    sz = (sz + 7) & ~(size_t)7;
    char *p = nuq_arena_cur;
    char *next = p + sz;
    if (LIKELY(next <= nuq_arena_end)) {
        nuq_arena_cur = next;
        return p;
    }
    return nuq_arena_alloc_slow(sz);
}

static inline void *
nuq_value_alloc(size_t sz)
{
    if (UNLIKELY(nuq_alloc_perm)) return GC_malloc(sz);
    return nuq_arena_alloc(sz);
}

static inline void *
nuq_value_alloc_atomic(size_t sz)
{
    if (UNLIKELY(nuq_alloc_perm)) return GC_malloc_atomic(sz);
    return nuq_arena_alloc(sz);
}

static inline void *
nuq_value_realloc(void *p, size_t old_sz, size_t new_sz)
{
    if (UNLIKELY(nuq_alloc_perm)) return GC_realloc(p, new_sz);
    void *q = nuq_arena_alloc(new_sz);
    if (p && old_sz) memcpy(q, p, old_sz < new_sz ? old_sz : new_sz);
    return q;
}

/*
 * VALUE encoding:
 *   xxxx_xxx1 -> 62-bit signed fixnum
 *   xxxx_xxx0 -> pointer to `struct nuq_obj` (heap, 8-aligned)
 *
 * Singletons (statically allocated): NUQ_NULL, NUQ_TRUE, NUQ_FALSE.
 *
 * Each NODE_DEF returns VALUE = a nuq_array containing its emit
 * stream.  EVAL_ARG(c, child) returns the child's emit array directly.
 * No shared emit_buf — pure return-value composition.
 *
 * Errors propagate via c->error (non-NULL means "in flight"); the
 * caller is expected to check after each EVAL_ARG.
 */
typedef int64_t VALUE;

#define NUQ_FIX_MAX     ((int64_t)((1LL << 62) - 1))
#define NUQ_FIX_MIN     ((int64_t)(-(1LL << 62)))
#define NUQ_IS_FIX(v)   ((int64_t)(v) & 1LL)
#define NUQ_FIX(n)      (((VALUE)(int64_t)(n) << 1) | 1LL)
#define NUQ_FIX_VAL(v)  ((int64_t)(v) >> 1)
#define NUQ_IS_PTR(v)   (((int64_t)(v) & 1LL) == 0)
#define NUQ_PTR(v)      ((struct nuq_obj *)(uintptr_t)(v))
#define NUQ_OBJ_VAL(p)  ((VALUE)(uintptr_t)(p))

enum nuq_type {
    NUQ_T_NULL = 1,
    NUQ_T_BOOL,
    NUQ_T_DOUBLE,
    NUQ_T_STRING,
    NUQ_T_ARRAY,
    NUQ_T_OBJECT,
};

/*
 * Arrays embed a 4-slot inline buffer for the common 0-emit / 1-emit
 * case in eval results.  `nuq_make_array(N)` with N ≤ 4 sets `items`
 * to point at `inline_buf` instead of a separately-allocated heap
 * block — that's 1 allocation saved per emit-array creation, which is
 * a hot path (every NODE_DEF returns one).  When push grows past 4,
 * we GC_malloc a fresh items[] and copy out.
 */
#define NUQ_ARR_INLINE 4

struct nuq_obj {
    enum nuq_type type;
    union {
        bool b;
        double dbl;
        struct { char *bytes; size_t len; } str;
        struct {
            VALUE *items;          /* points at inline_buf when capa == NUQ_ARR_INLINE */
            size_t len;
            size_t capa;
            VALUE  inline_buf[NUQ_ARR_INLINE];
        } arr;
        struct {
            VALUE   *keys;
            VALUE   *vals;
            size_t   len;
            size_t   capa;
            /* Open-addressing hash index over keys[].  NULL until len
             * exceeds NUQ_OBJ_HASH_MIN; then idx[h & idx_mask] holds
             * the keys[]-index PLUS ONE (0 means empty slot).  keys[]
             * / vals[] still hold values in insertion order, so
             * jq-visible iteration order is preserved.  Keys are
             * always strings. */
            uint32_t *idx;
            uint32_t  idx_mask;
        } obj;
    };
};

extern struct nuq_obj NUQ_NULL_OBJ, NUQ_TRUE_OBJ, NUQ_FALSE_OBJ, NUQ_NULL_ERR_OBJ;
#define NUQ_NULL  NUQ_OBJ_VAL(&NUQ_NULL_OBJ)
/* Distinct null-typed sentinel used as `c->error` when `error/0` is
 * raised on a null input (since NUQ_NULL itself means "no error"). */
#define NUQ_NULL_ERR  NUQ_OBJ_VAL(&NUQ_NULL_ERR_OBJ)
#define NUQ_TRUE  NUQ_OBJ_VAL(&NUQ_TRUE_OBJ)
#define NUQ_FALSE NUQ_OBJ_VAL(&NUQ_FALSE_OBJ)

/* Variable binding stack */
struct nuq_var_slot {
    uint32_t id;
    VALUE    value;
};

struct nuq_func_def;

typedef struct CTX_struct {
    /* Outer input — set by main loop and by pipe / iter / map etc. */
    VALUE                 input;

    /* Emit pool — flat growable VALUE buffer.  Each NODE_DEF appends
     * its emits at c->pool_top, returns EMIT { items=pool+start,
     * count }.  Caller resets c->pool_top after consuming.  This is
     * the per-call allocation optimization — instead of GC_malloc'ing
     * a fresh nuq_array per filter call, we slice from one big buffer. */
    VALUE                *pool;
    size_t                pool_top;
    size_t                pool_capa;

    /* Variable bindings — `as $x` pushes (id, value), pops after body. */
    struct nuq_var_slot  *var_stack;
    size_t                var_top;
    size_t                var_capa;

    /* User `def`s — later definitions shadow earlier. */
    struct nuq_func_def **funcs;
    size_t                func_cnt;
    size_t                func_capa;

    /* Error in flight: NULL = OK, anything else = error VALUE. */
    VALUE                 error;

    /* Label-break in flight: 0 = none, otherwise the label's intern id. */
    uint32_t              break_label;

    /* Path-mode "drop" signal — set by `select(cond)` when cond is
     * false during a path walk; propagates through nested_apply so
     * the enclosing iter / accessor knows to drop this branch. */
    bool                  path_drop_pending;

    /* Lexical-scope skip range for func lookup inside a def body.
     * Funcs in [skip_start, skip_end) are hidden from name resolution
     * (the defs that came AFTER the enclosing fd was defined but
     * BEFORE the call site — they shadow nothing in this body).
     * skip_start == skip_end means dynamic / top-level scope. */
    size_t                func_skip_start;
    size_t                func_skip_end;
} CTX;

struct nuq_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool dump_ast;
    bool compact_output;
    bool null_input;
    bool slurp;
    bool raw_input;
    bool raw_output;
    bool tab_indent;
    bool sort_keys;
    bool record_all;
    bool exit_status;       /* -e: exit 5 if no truthy emit */
    bool seq_output;        /* --seq: RFC 7464 record-separator output */
    int  indent;
    /* Module search path list (`-L <dir>`).  Searched in order when
     * resolving `import "X"` / `include "X"` directives.  NULL-
     * terminated. */
    char **module_search;
    size_t module_search_cnt;
};

/* CLI-supplied bindings: --arg / --argjson / --slurpfile / --rawfile. */
void nuq_user_arg_add(const char *name, const char *value, bool json);
void nuq_user_arg_add_value(const char *name, VALUE v);
bool nuq_user_arg_add_file(const char *name, const char *file, bool raw);
void nuq_user_args_bind(struct CTX_struct *c);   /* push all into var_stack */

/* Run state for -e / --exit-status. */
extern bool nuq_had_truthy_output;
extern bool nuq_had_error;

/* When non-zero, suppress the stderr error diagnostics emitted by
 * value-level helpers ("nuq error: cannot add string and number" etc.).
 * `try` / `isvalid` increment it before evaluating their body so the
 * error stays in c->error without polluting stderr.  Saved and
 * restored across boundaries by the caller — never assigned, only
 * inc/dec to handle nesting. */
extern int nuq_suppress_error_print;

/* Active CTX for value-level helpers.  `nuq_run` sets this at the top
 * of each filter invocation so helpers like `nuq_op_add_slow` can
 * propagate errors via `nuq_active_ctx->error` instead of just
 * returning NUQ_NULL silently.  This makes `try ("x" + 1) catch ...`
 * actually catch, matching jq. */
extern struct CTX_struct *nuq_active_ctx;
VALUE nuq_helper_error(const char *fmt, ...);

/* Pending-input queue for jq-compatible `input` / `inputs`.  main.c
 * fills this with all parsed JSON values up front; nuq_input_pull()
 * returns the next one (advancing the cursor).  Both the main loop
 * and the `input` builtin pull from the same cursor. */
void  nuq_input_queue_set(VALUE *items, size_t cnt);
bool  nuq_input_pull(VALUE *out);   /* true if a value was returned */

extern struct nuq_option OPTION;

/* ---- value.c API ---- */
VALUE nuq_make_double  (double d);
/* nuq_make_int — defined as `static inline` below.  The slow case
 * (out of fixnum range) is `nuq_make_int_slow`. */
VALUE nuq_make_string  (const char *s, size_t len);
VALUE nuq_make_string_take(char *s, size_t len);
VALUE nuq_make_array   (size_t initial_capa);
VALUE nuq_make_object  (size_t initial_capa);

void  nuq_array_push_slow (VALUE arr, VALUE v);

/* Fast path inline so hot loops (`[range(N)]`, `map`, sort/group_by
 * staging arrays) avoid the function-call cost.  Slow path handles
 * capacity grow + inline→heap migration. */
static inline void
nuq_array_push(VALUE arr, VALUE v)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    if (LIKELY(o->arr.len < o->arr.capa)) {
        o->arr.items[o->arr.len++] = v;
        return;
    }
    nuq_array_push_slow(arr, v);
}
VALUE nuq_array_get    (VALUE arr, int64_t idx);
size_t nuq_array_len   (VALUE arr);

void  nuq_object_set       (VALUE obj, VALUE key, VALUE val);
void  nuq_object_set_cstr  (VALUE obj, const char *key, VALUE val);
VALUE nuq_object_get       (VALUE obj, VALUE key);
VALUE nuq_object_get_cstr  (VALUE obj, const char *key);
bool  nuq_object_has       (VALUE obj, VALUE key);
size_t nuq_object_len      (VALUE obj);

const char *nuq_string_cstr(VALUE s);
size_t      nuq_string_len (VALUE s);

/* Slow paths — invoked from `static inline` wrappers in node.h once
 * the fixnum / pointer-equal fast path didn't apply.  Callers should
 * use the wrappers (`nuq_eq` etc.); these `_slow` symbols are public
 * only so the inlines can fall through. */
bool  nuq_eq_slow      (VALUE a, VALUE b);
int   nuq_cmp_slow     (VALUE a, VALUE b);
bool  nuq_truthy_slow  (VALUE v);
VALUE nuq_make_int_slow(int64_t v);
const char *nuq_type_name(VALUE v);
size_t nuq_value_descr(VALUE v, char *dst, size_t n);
VALUE nuq_length(VALUE v);
VALUE nuq_keys(VALUE v, bool sorted);
VALUE nuq_values(VALUE v);
VALUE nuq_clone(VALUE v);

bool  nuq_contains      (VALUE a, VALUE b);

VALUE nuq_op_add_slow(VALUE a, VALUE b);
VALUE nuq_op_sub_slow(VALUE a, VALUE b);
VALUE nuq_op_mul_slow(VALUE a, VALUE b);
VALUE nuq_op_div_slow(VALUE a, VALUE b);
VALUE nuq_op_mod_slow(VALUE a, VALUE b);
VALUE nuq_op_neg_slow(VALUE a);

/* Fast-path inlines.  Defined here (not in node.h) so value.c —
 * which only includes context.h — also picks them up.  Each does
 * the common typed case in registers and tail-calls `_slow` on miss. */
static inline VALUE
nuq_make_int(int64_t v)
{
    if (LIKELY(v >= NUQ_FIX_MIN && v <= NUQ_FIX_MAX)) return NUQ_FIX(v);
    return nuq_make_int_slow(v);
}

static inline bool
nuq_eq(VALUE a, VALUE b)
{
    if (a == b) return true;
    /* Two distinct fixnums can't be equal (the FIX tag differs only
     * in the payload bits). */
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) return false;
    return nuq_eq_slow(a, b);
}

static inline int
nuq_cmp(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b);
        return (la > lb) - (la < lb);
    }
    return nuq_cmp_slow(a, b);
}

static inline bool
nuq_truthy(VALUE v)
{
    /* fixnum, double, string, array, object — all truthy.
     * Only NUQ_NULL_OBJ and NUQ_FALSE_OBJ are falsy. */
    if (NUQ_IS_FIX(v)) return true;
    return nuq_truthy_slow(v);
}

static inline VALUE
nuq_op_add(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r)))
            return nuq_make_int(r);
    }
    return nuq_op_add_slow(a, b);
}

static inline VALUE
nuq_op_sub(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_sub_overflow(la, lb, &r)))
            return nuq_make_int(r);
    }
    return nuq_op_sub_slow(a, b);
}

static inline VALUE
nuq_op_mul(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b), r;
        if (LIKELY(!__builtin_mul_overflow(la, lb, &r)))
            return nuq_make_int(r);
    }
    return nuq_op_mul_slow(a, b);
}

static inline VALUE
nuq_op_neg(VALUE a)
{
    if (LIKELY(NUQ_IS_FIX(a))) {
        int64_t la = NUQ_FIX_VAL(a);
        if (LIKELY(la != NUQ_FIX_MIN)) return NUQ_FIX(-la);
    }
    return nuq_op_neg_slow(a);
}

/* `/` is jq-double division (5/2 == 2.5) so the fast fixnum path
 * still has to box.  Skip the inline fast path. */
static inline VALUE nuq_op_div(VALUE a, VALUE b) { return nuq_op_div_slow(a, b); }

/* `%` is integer modulo for fixnum operands — common case in
 * `group_by(. % N)` etc.  Inline `la % lb` for the typical hot loop
 * (~5× over the function-call slow path). */
static inline VALUE
nuq_op_mod(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_FIX(a) && NUQ_IS_FIX(b))) {
        int64_t lb = NUQ_FIX_VAL(b);
        if (LIKELY(lb != 0)) {
            int64_t la = NUQ_FIX_VAL(a);
            /* la == INT64_MIN && lb == -1 would overflow, but fixnum
             * range is 62-bit so INT64_MIN is unreachable here. */
            return NUQ_FIX(la % lb);
        }
    }
    return nuq_op_mod_slow(a, b);
}

bool nuq_field_lookup(VALUE in, const char *name, bool optional, VALUE *out);
bool nuq_to_number(VALUE v, VALUE *out);

/* JSON I/O */
VALUE nuq_json_parse(const char *src, size_t len, const char **endp, char **errmsg);
void  nuq_json_print(FILE *fp, VALUE v, int indent);
VALUE nuq_to_json_string(VALUE v);

/* Variable interning */
uint32_t nuq_intern(const char *name);
const char *nuq_intern_lookup(uint32_t id);

/* Variable bindings */
void  nuq_var_push(CTX *c, uint32_t id, VALUE v);
void  nuq_var_pop (CTX *c, size_t to_top);
VALUE nuq_var_get (CTX *c, uint32_t id);

/* Function definitions */
struct nuq_func_def {
    uint32_t   name_id;
    int        arity;
    uint32_t  *param_ids;
    bool      *param_is_value;
    struct Node *body;
    /* Lexical-scope boundary: at call time, only functions defined
     * BEFORE this one (i.e. funcs[0..scope_top-1]) are visible — so a
     * later redefinition of a name doesn't shadow the version this def
     * was compiled against.  Set by nuq_func_define from current
     * func_cnt; 0 means "no constraint" (top-level / dynamic). */
    size_t     scope_top;
    /* Call-by-name closure: a param-def synthesised from `f(EXPR)`
     * captures EXPR + the caller's var environment so each reference
     * to the param re-evaluates EXPR in caller's scope (jq's lazy
     * thunk semantics).  When `var_snap` is non-NULL, the runtime
     * masks the live var stack down to `var_snap[0..var_snap_cnt]`
     * for the duration of the body eval. */
    struct nuq_var_slot *var_snap;
    size_t     var_snap_cnt;
};

void nuq_func_define(CTX *c, struct nuq_func_def *fd);
struct nuq_func_def *nuq_func_lookup(CTX *c, uint32_t name_id, int arity);

/* Side tables */
uint32_t nuq_lit_intern(VALUE v);
VALUE    nuq_lit_get(uint32_t id);

uint32_t nuq_interp_intern(struct Node **parts, size_t cnt);
struct nuq_obj_entry {
    int      kkind;          /* 0 = static cstr, 1 = NODE* expr, 2 = $var */
    const char *kname;
    uint32_t var_id;
    struct Node *kexpr;      /* expr WITH input chain (for 1-style key) */
    struct Node *vexpr;      /* expr (sub-chain), NULL for shorthand */
    VALUE    kname_value;    /* pre-interned VALUE for kkind 0/2 — built
                              * once at parse time so each ctor call
                              * doesn't redo nuq_make_string. */
};
uint32_t nuq_obj_ctor_intern(struct nuq_obj_entry *items, size_t cnt);
uint32_t nuq_args_intern(struct Node **args, size_t cnt);

uint32_t nuq_fmt_intern(const char *name);
const char *nuq_fmt_lookup(uint32_t id);

struct nuq_def_entry {
    uint32_t   name_id;
    int        arity;
    uint32_t  *param_ids;
    bool      *param_is_value;
    struct Node *body;
};
uint32_t nuq_def_block_intern(struct nuq_def_entry *items, size_t cnt);

/* Destructuring patterns for `as`.  jq syntax:
 *   . as $x | ...                       — PAT_VAR
 *   . as [$a, $b] | ...                 — PAT_ARRAY
 *   . as {a: $x, b: $y} | ...           — PAT_OBJECT
 *   . as {$a, $b} | ...                 — shorthand (key == varname)
 *   . as {a: [$b, {c: $d}]} | ...       — nested
 */
enum nuq_pat_kind { NUQ_PAT_VAR = 1, NUQ_PAT_ARRAY, NUQ_PAT_OBJECT };
struct nuq_pat;
struct nuq_pat_obj_entry {
    const char *key;            /* literal field name (NULL if dynamic) */
    struct Node *key_expr;      /* dynamic key expression (else NULL) */
    struct nuq_pat *val;
};
struct nuq_pat {
    enum nuq_pat_kind kind;
    union {
        uint32_t var_id;
        struct { struct nuq_pat **items; size_t len; } arr;
        struct { struct nuq_pat_obj_entry *items; size_t len; } obj;
    } u;
};
uint32_t nuq_pat_intern(struct nuq_pat *p);
struct nuq_pat *nuq_pat_get(uint32_t id);
/* Walk pattern and value pairwise, pushing each var onto var_stack.
 * Returns the new var_top before push (caller pops to that). */
size_t nuq_pat_bind(struct CTX_struct *c, struct nuq_pat *p, VALUE v);
uint32_t nuq_pat_alt_intern(uint32_t *pids, size_t cnt);

/* Each user `def` body is reachable only via runtime dispatch
 * (`EVAL(c, fd->body)` in node_call), so the SD specialiser on the
 * top-level filter cannot inline it.  These helpers expose every
 * def body as its own AOT entry node — see usage.md "Entry nodes". */
void nuq_compile_all_def_bodies(void);
void nuq_load_all_def_bodies(void);

/* Operator codes used by node.def */
enum {
    NUQ_OP_ADD_K = 1, NUQ_OP_SUB_K, NUQ_OP_MUL_K, NUQ_OP_DIV_K, NUQ_OP_MOD_K,
    NUQ_CMP_EQ_K, NUQ_CMP_NE_K, NUQ_CMP_LT_K, NUQ_CMP_LE_K, NUQ_CMP_GT_K, NUQ_CMP_GE_K,
};

/* Assignment kinds for node_assign. */
enum {
    NUQ_ASSIGN_PLAIN = 1,   /* =     */
    NUQ_ASSIGN_UPDATE,      /* |=    */
    NUQ_ASSIGN_PLUS,        /* +=    */
    NUQ_ASSIGN_MINUS,       /* -=    */
    NUQ_ASSIGN_MUL,         /* *=    */
    NUQ_ASSIGN_DIV,         /* /=    */
    NUQ_ASSIGN_MOD,         /* %=    */
    NUQ_ASSIGN_ALT,         /* //=   */
};

#define SLICE_HAS_START 1
#define SLICE_HAS_STOP  2

/* Built-in value-level helpers (called from per-node EVAL bodies) */
VALUE nuq_builtin_add(VALUE input);
VALUE nuq_add_fold_items(const VALUE *items, size_t len);
VALUE nuq_builtin_min(VALUE input);
VALUE nuq_builtin_max(VALUE input);
VALUE nuq_builtin_sort(VALUE input);
VALUE nuq_builtin_reverse(VALUE input);
VALUE nuq_builtin_unique(VALUE input);
VALUE nuq_builtin_to_entries(VALUE input);
VALUE nuq_builtin_from_entries(VALUE input);
VALUE nuq_builtin_floor(VALUE input);
VALUE nuq_builtin_ceil(VALUE input);
VALUE nuq_builtin_round(VALUE input);
VALUE nuq_builtin_fabs(VALUE input);
VALUE nuq_builtin_sqrt(VALUE input);
VALUE nuq_builtin_explode(VALUE input);
VALUE nuq_builtin_implode(VALUE input);
VALUE nuq_builtin_ascii_upcase(VALUE input);
VALUE nuq_builtin_ascii_downcase(VALUE input);
bool  nuq_builtin_fromjson(VALUE in, VALUE *out);
bool  nuq_builtin_fromjson_err(VALUE in, VALUE *out, char **err_out);
void  nuq_recurse_collect(VALUE r, VALUE v);

/* Helpers in runtime.c (called from node.def) — return EMIT (pool slice).
 * Defined in node.h after EMIT typedef. */
struct Node;
typedef struct EMIT EMIT_fwd_;
/* Forward signatures appear in node.h */

/* Filter parser entry point */
struct Node *nuq_parse_filter(const char *src);
struct Node *nuq_compile_subexpr(const char *src, size_t len);

/* Top-level execution */
void nuq_run(CTX *c, struct Node *filter, VALUE input);

#endif /* NUQ_CONTEXT_H */
