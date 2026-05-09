#ifndef ASML_CONTEXT_H
#define ASML_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <alloca.h>

// asml — Standard ML subset on ASTro.
//
// VALUE encoding:
//   bit0 = 1 → tagged 63-bit signed int  (SML `int`)
//   bit0 = 0 → 8-byte aligned heap pointer or static singleton.
//
// SML constructors that carry payload (`SOME of int`, etc.) and built-in
// list cons share the OOBJ_VARIANT representation, distinguished by the
// interned constructor name.  `nil` and `true` / `false` / `()` are
// statically allocated singletons.

typedef int64_t VALUE;

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#define ML_INT(n)        (((VALUE)(int64_t)(n) << 1) | 1LL)
#define ML_INT_VAL(v)    ((int64_t)(v) >> 1)
#define ML_IS_INT(v)     ((int64_t)(v) & 1LL)
#define ML_IS_PTR(v)     (((int64_t)(v) & 1LL) == 0)
#define ML_PTR(v)        ((struct mlobj *)(uintptr_t)(v))
#define ML_OBJ_VAL(p)    ((VALUE)(uintptr_t)(p))

enum mlobj_type {
    MLOBJ_UNIT,
    MLOBJ_BOOL,
    MLOBJ_NIL,
    MLOBJ_CONS,         // payload: { head, tail }
    MLOBJ_STRING,
    MLOBJ_REAL,         // boxed double
    MLOBJ_TUPLE,
    MLOBJ_REF,
    MLOBJ_CLOSURE,
    MLOBJ_PRIM,
    MLOBJ_VARIANT,      // user constructor application (zero or one payload)
    MLOBJ_EXN,          // raised value (usually wraps a variant)
    MLOBJ_RECORD,       // record { f1 = v1, f2 = v2 } — sorted fields
};

struct mlobj;
struct mlframe;
struct CTX_struct;
struct Node;

typedef VALUE (*ml_prim_fn)(struct CTX_struct *c, int argc, VALUE *argv);

struct mlobj {
    int type;
    union {
        bool   b;
        double dbl;
        VALUE  refval;
        struct { VALUE head, tail; } cons;
        struct { char *chars; size_t len; } str;
        struct {
            struct Node   *body;
            struct mlframe *env;
            int            nparams;
            bool           is_leaf;
            const char    *name;
        } closure;
        struct {
            ml_prim_fn  fn;
            const char *name;
            int         min_argc, max_argc;     // -1 = unlimited
        } prim;
        struct { int n; VALUE *items; } tup;
        struct {
            const char *name;          // interned constructor name
            int         n;             // 0 (nullary) or 1 (1-arg payload)
            VALUE      *items;
        } var;
        struct {
            int           n;           // 0 〜
            const char  **fields;      // sorted (interned)
            VALUE        *items;       // parallel to fields
        } rec;
    };
};

struct mlframe {
    struct mlframe *parent;
    int             nslots;
    VALUE           slots[];
};

struct asml_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool dump_ast;
    bool compile;        // AOT compile each top-level form
};
extern struct asml_option OPTION;

struct gentry {
    const char *name;
    VALUE       value;
};

struct gref_cache {
    uint64_t serial;
    VALUE    value;
};

struct app_cache {
    VALUE              fn;
    struct Node       *body;
    struct mlframe    *env;
};

#define ASML_HANDLER_MAX_DEPTH 256
struct ml_handler {
    jmp_buf         buf;
    VALUE           exn;
    struct mlframe *saved_env;
};

typedef struct CTX_struct {
    struct mlframe *env;

    struct gentry *globals;
    size_t         globals_size;
    size_t         globals_capa;
    uint64_t       globals_serial;

    struct ml_handler handlers[ASML_HANDLER_MAX_DEPTH];
    int               handlers_top;     // -1 = empty

    jmp_buf err_jmp;
    int     err_jmp_active;
    char    err_msg[256];

    int    tail_call_pending;
    VALUE  tc_fn;
    int    tc_argc;
    VALUE  tc_argv[16];
} CTX;

// Static singletons (defined in main.c).
extern struct mlobj ML_UNIT_OBJ, ML_TRUE_OBJ, ML_FALSE_OBJ, ML_NIL_OBJ;
#define ML_UNIT   ML_OBJ_VAL(&ML_UNIT_OBJ)
#define ML_TRUE   ML_OBJ_VAL(&ML_TRUE_OBJ)
#define ML_FALSE  ML_OBJ_VAL(&ML_FALSE_OBJ)
#define ML_NIL    ML_OBJ_VAL(&ML_NIL_OBJ)

static inline bool ML_IS_CONS(VALUE v)    { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_CONS;    }
static inline bool ML_IS_STRING(VALUE v)  { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_STRING;  }
static inline bool ML_IS_CLOSURE(VALUE v) { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_CLOSURE; }
static inline bool ML_IS_PRIM(VALUE v)    { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_PRIM;    }
static inline bool ML_IS_TUPLE(VALUE v)   { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_TUPLE;   }
static inline bool ML_IS_REF(VALUE v)     { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_REF;     }
static inline bool ML_IS_REAL(VALUE v)    { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_REAL;    }
static inline bool ML_IS_VARIANT(VALUE v) { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_VARIANT; }
static inline bool ML_IS_RECORD(VALUE v)  { return ML_IS_PTR(v) && ML_PTR(v)->type == MLOBJ_RECORD; }
static inline bool ML_IS_BOOL(VALUE v)    { return v == ML_TRUE || v == ML_FALSE; }

// Object helpers (defined in main.c).
struct mlobj *ml_alloc(int type);
VALUE ml_cons(VALUE h, VALUE t);
VALUE ml_make_string(const char *s, size_t len);
VALUE ml_make_real(double d);
VALUE ml_make_tuple(int n, VALUE *items);
VALUE ml_make_ref(VALUE init);
VALUE ml_make_closure(struct Node *body, struct mlframe *env, int nparams, bool is_leaf, const char *name);
VALUE ml_make_prim(const char *name, ml_prim_fn fn, int min_argc, int max_argc);
VALUE ml_make_variant(const char *name, int n, VALUE *items);
VALUE ml_make_record(int n, const char **fields, VALUE *items);
VALUE ml_string_concat(VALUE a, VALUE b);
bool  ml_structural_eq(VALUE a, VALUE b);
int   ml_compare(VALUE a, VALUE b);
double ml_get_real(VALUE v);

// Frames.
struct mlframe *ml_new_frame(struct mlframe *parent, int nslots);

// Apply a callable.
VALUE ml_apply(struct CTX_struct *c, VALUE fn, int argc, VALUE *argv);

// Raise.
__attribute__((noreturn)) void ml_raise(struct CTX_struct *c, VALUE exn);
__attribute__((noreturn)) void ml_type_error(struct CTX_struct *c, const char *op, const char *expected);
__attribute__((noreturn,format(printf,2,3)))
void ml_error(struct CTX_struct *c, const char *fmt, ...);

VALUE ml_run_handle(struct CTX_struct *c, struct Node *body, struct Node *handler);

// Globals.
void  ml_global_define(struct CTX_struct *c, const char *name, VALUE v);
VALUE ml_global_ref(struct CTX_struct *c, const char *name);

// Display.
void ml_display(FILE *fp, VALUE v);

#endif
