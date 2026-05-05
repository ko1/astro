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
 * Singletons: NUQ_NULL, NUQ_TRUE, NUQ_FALSE (statically allocated).
 *
 * VALUE doubles as the dispatcher return code (BR_OK / BR_BREAK /
 * BR_ERROR).  Real result values flow through `c->emit_buf`, not through
 * the return value, so the channels never collide.
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

#define BR_OK     ((VALUE)0)
#define BR_BREAK  ((VALUE)1)
#define BR_ERROR  ((VALUE)2)

enum nuq_type {
    NUQ_T_NULL = 1,
    NUQ_T_BOOL,
    NUQ_T_DOUBLE,
    NUQ_T_STRING,
    NUQ_T_ARRAY,
    NUQ_T_OBJECT,
};

struct nuq_obj {
    enum nuq_type type;
    union {
        bool b;
        double dbl;
        struct { char *bytes; size_t len; } str;
        struct { VALUE *items; size_t len; size_t capa; } arr;
        struct {
            VALUE *keys;
            VALUE *vals;
            size_t len;
            size_t capa;
        } obj;
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

/*
 * Emit buffer: where filters write their outputs.  When evaluating a
 * sub-filter we push a fresh buffer; the helper code reads it after the
 * sub-filter returns and consumes/forwards the values.  The buffer is
 * just a VALUE (a nuq_array) — using nuq_array gives O(1) growth and
 * GC tracking for free.
 */
typedef struct CTX_struct {
    VALUE                 input;          /* current `.` */
    VALUE                 emit_buf;       /* current emit array */

    struct nuq_var_slot  *var_stack;
    size_t                var_top;
    size_t                var_capa;

    struct nuq_func_def **funcs;
    size_t                func_cnt;
    size_t                func_capa;

    uint32_t              break_label;    /* 0 = none */
    VALUE                 error;          /* NUQ_NULL when no error */
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
    bool record_all;        /* needed by generated allocators */
    int  indent;            /* 0 = compact, default 2 */
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
void  nuq_object_del_cstr  (VALUE obj, const char *key);
void  nuq_object_del       (VALUE obj, VALUE key);

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

/* ---- emit + sub-eval helpers ---- */

void  nuq_emit(CTX *c, VALUE v);
VALUE nuq_eval_collect(CTX *c, struct Node *body, VALUE input);  /* returns array of emits */
VALUE nuq_eval_collect_status(CTX *c, struct Node *body, VALUE input, VALUE *out);

/* Side tables */
uint32_t nuq_lit_intern(VALUE v);
VALUE    nuq_lit_get(uint32_t id);

/* Operator codes used by node.def */
enum {
    NUQ_OP_ADD_K = 1, NUQ_OP_SUB_K, NUQ_OP_MUL_K, NUQ_OP_DIV_K, NUQ_OP_MOD_K,
    NUQ_CMP_EQ_K, NUQ_CMP_NE_K, NUQ_CMP_LT_K, NUQ_CMP_LE_K, NUQ_CMP_GT_K, NUQ_CMP_GE_K,
};

/* Node.def runtime helpers */
VALUE nuq_recurse_emit(CTX *c, VALUE v);
VALUE nuq_iter_emit(CTX *c, VALUE in, bool optional);
VALUE nuq_index_eval(CTX *c, struct Node *expr, bool optional);
VALUE nuq_slice_eval(CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional);
VALUE nuq_pipe_eval(CTX *c, struct Node *lhs, struct Node *rhs);
VALUE nuq_binop_eval(CTX *c, struct Node *lhs, struct Node *rhs, int op);
VALUE nuq_neg_eval(CTX *c, struct Node *expr);
VALUE nuq_cmpop_eval(CTX *c, struct Node *lhs, struct Node *rhs, int op);
VALUE nuq_andor_eval(CTX *c, struct Node *lhs, struct Node *rhs, bool is_and);
VALUE nuq_alt_eval(CTX *c, struct Node *lhs, struct Node *rhs);
VALUE nuq_array_eval(CTX *c, struct Node *body);
VALUE nuq_object_eval(CTX *c, uint32_t entries_id);
VALUE nuq_if_eval(CTX *c, struct Node *cond, struct Node *thn, struct Node *els);
VALUE nuq_try_eval(CTX *c, struct Node *body, struct Node *handler);
VALUE nuq_as_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *body);
VALUE nuq_error_eval(CTX *c, struct Node *expr);
VALUE nuq_call_eval (CTX *c, uint32_t name_id, int arity, struct Node **args);
VALUE nuq_call_eval1(CTX *c, uint32_t name_id, struct Node *a0);
VALUE nuq_call_eval2(CTX *c, uint32_t name_id, struct Node *a0, struct Node *a1);
VALUE nuq_call_eval3(CTX *c, uint32_t name_id, struct Node *a0, struct Node *a1, struct Node *a2);
VALUE nuq_defs_eval (CTX *c, uint32_t defs_id, struct Node *body);
VALUE nuq_reduce_eval (CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update);
VALUE nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract);
VALUE nuq_interp_eval(CTX *c, uint32_t parts_id);
VALUE nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body);
VALUE nuq_assign_eval(CTX *c, struct Node *path, struct Node *value);
VALUE nuq_update_assign_eval(CTX *c, struct Node *path, struct Node *value, uint32_t op);

#define SLICE_HAS_START 1
#define SLICE_HAS_STOP  2

/* Object-ctor entry */
struct nuq_obj_entry {
    int      kkind;          /* 0 = static cstr, 1 = NODE* expr, 2 = $var */
    const char *kname;
    uint32_t var_id;
    struct Node *kexpr;
    struct Node *vexpr;      /* NULL for shorthand */
};
uint32_t nuq_obj_ctor_intern(struct nuq_obj_entry *items, size_t cnt);

/* Function-arg side-table */
uint32_t nuq_args_intern(struct Node **args, size_t cnt);

/* Interp parts side-table */
uint32_t nuq_interp_intern(struct Node **parts, size_t cnt);

/* Format-string id intern */
uint32_t nuq_fmt_intern(const char *name);
const char *nuq_fmt_lookup(uint32_t id);

/* def-block side-table */
struct nuq_def_entry {
    uint32_t   name_id;
    int        arity;
    uint32_t  *param_ids;
    bool      *param_is_value;
    struct Node *body;
};
uint32_t nuq_def_block_intern(struct nuq_def_entry *items, size_t cnt);

/* Filter parser entry point */
struct Node *nuq_parse_filter(const char *src);
struct Node *nuq_compile_subexpr(const char *src, size_t len);

/* Top-level execution: run the filter over each input value. */
void nuq_run(CTX *c, struct Node *filter, VALUE input);

/* Builtins entry point */
bool nuq_builtin_call(CTX *c, uint32_t name_id, int arity, struct Node **args, VALUE *out_status);

#endif /* NUQ_CONTEXT_H */
