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

extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_init(void);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

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
        struct { VALUE *keys; VALUE *vals; size_t len; size_t capa; } obj;
    };
};

extern struct nuq_obj NUQ_NULL_OBJ, NUQ_TRUE_OBJ, NUQ_FALSE_OBJ;
#define NUQ_NULL  NUQ_OBJ_VAL(&NUQ_NULL_OBJ)
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
    int  indent;
};

extern struct nuq_option OPTION;

/* ---- value.c API ---- */
VALUE nuq_make_double  (double d);
VALUE nuq_make_int     (int64_t v);
VALUE nuq_make_string  (const char *s, size_t len);
VALUE nuq_make_string_take(char *s, size_t len);
VALUE nuq_make_array   (size_t initial_capa);
VALUE nuq_make_object  (size_t initial_capa);

void  nuq_array_push   (VALUE arr, VALUE v);
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

bool nuq_eq        (VALUE a, VALUE b);
int  nuq_cmp       (VALUE a, VALUE b);
bool nuq_truthy    (VALUE v);
const char *nuq_type_name(VALUE v);
VALUE nuq_length(VALUE v);
VALUE nuq_keys(VALUE v, bool sorted);
VALUE nuq_values(VALUE v);
VALUE nuq_clone(VALUE v);

VALUE nuq_op_add(VALUE a, VALUE b);
VALUE nuq_op_sub(VALUE a, VALUE b);
VALUE nuq_op_mul(VALUE a, VALUE b);
VALUE nuq_op_div(VALUE a, VALUE b);
VALUE nuq_op_mod(VALUE a, VALUE b);
VALUE nuq_op_neg(VALUE a);

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

/* Operator codes used by node.def */
enum {
    NUQ_OP_ADD_K = 1, NUQ_OP_SUB_K, NUQ_OP_MUL_K, NUQ_OP_DIV_K, NUQ_OP_MOD_K,
    NUQ_CMP_EQ_K, NUQ_CMP_NE_K, NUQ_CMP_LT_K, NUQ_CMP_LE_K, NUQ_CMP_GT_K, NUQ_CMP_GE_K,
};

#define SLICE_HAS_START 1
#define SLICE_HAS_STOP  2

/* Built-in value-level helpers (called from per-node EVAL bodies) */
VALUE nuq_builtin_add(VALUE input);
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
