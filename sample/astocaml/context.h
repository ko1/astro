#ifndef ASTOCAML_CONTEXT_H
#define ASTOCAML_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <alloca.h>

// VALUE representation.
//
//   bit 0 = 1 → fixnum  (63-bit signed integer, value <<1 | 1)
//   bit 0 = 0 → 8-byte aligned heap pointer or singleton
//
// Singletons live as static `struct oobj` whose addresses are exposed as
// OC_UNIT / OC_TRUE / OC_FALSE / OC_NIL.  Heap objects are obtained via
// plain malloc — interpreter-grade, leaks at shutdown.
typedef int64_t VALUE;

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#define OC_INT(n)        (((VALUE)(int64_t)(n) << 1) | 1LL)
#define OC_INT_VAL(v)    ((int64_t)(v) >> 1)
#define OC_IS_INT(v)     ((int64_t)(v) & 1LL)
#define OC_IS_PTR(v)     (((int64_t)(v) & 1LL) == 0)
#define OC_PTR(v)        ((struct oobj *)(uintptr_t)(v))
#define OC_OBJ_VAL(p)    ((VALUE)(uintptr_t)(p))

enum oobj_type {
    OOBJ_UNIT,
    OOBJ_BOOL,
    OOBJ_NIL,
    OOBJ_CONS,
    OOBJ_STRING,
    OOBJ_CLOSURE,
    OOBJ_PRIM,
    OOBJ_TUPLE,
    OOBJ_REF,
    OOBJ_FLOAT,
    OOBJ_EXN,           // exception value
    OOBJ_VARIANT,       // user-defined variant constructor application
    OOBJ_RECORD,        // user-defined record
    OOBJ_ARRAY,         // mutable Array.t
    OOBJ_LAZY,          // lazy thunk (forced flag + body / cached value)
    OOBJ_BYTES,         // mutable byte string
    OOBJ_OBJECT,        // class instance
};

struct oobj;
struct oframe;
struct CTX_struct;
struct Node;

typedef VALUE (*oc_prim_fn)(struct CTX_struct *c, int argc, VALUE *argv);

struct oobj {
    int type;
    union {
        bool b;
        double dbl;
        VALUE refval;                                                // OOBJ_REF
        struct { VALUE head, tail; } cons;
        struct { char *chars; size_t len; } str;
        struct {
            struct Node *body;
            struct oframe *env;
            int nparams;
            bool is_leaf;           // body creates no inner closures →
                                    // oc_apply may alloca the frame
        } closure;
        struct {
            oc_prim_fn fn;
            const char *name;
            int min_argc, max_argc;     // -1 = unlimited
        } prim;
        struct { int n; VALUE *items; } tup;                         // OOBJ_TUPLE
        struct { int n; VALUE *items; } arr;                         // OOBJ_ARRAY
        struct {
            const char *name;       // exception constructor name
            int n;                  // payload size (0 if no payload)
            VALUE *items;           // payload values
        } exn;
        struct {
            const char *name;       // variant constructor (e.g., "Some", "Foo")
            int n;                  // arg count (0 = nullary)
            VALUE *items;
        } var;
        struct {
            int n;
            const char **fields;    // field names (interned)
            VALUE *items;           // values, parallel to fields[]
        } rec;
        struct {
            bool forced;
            struct Node *body;      // valid if !forced
            struct oframe *env;     // valid if !forced
            VALUE value;            // valid if forced
        } lazy;
        struct {
            char *bytes;            // mutable
            size_t len;
        } bytes;
        struct {
            int n_methods;
            const char **method_names;
            VALUE *method_closures;     // closures
            int n_fields;
            const char **field_names;
            VALUE *field_values;        // mutable instance state
        } obj;
    };
};

struct oframe {
    struct oframe *parent;
    int nslots;
    VALUE slots[];
};

struct astocaml_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool dump_ast;
    bool compile;       // AOT compile each top-level expression
    bool type_check;    // run static type inference before evaluating
};
extern struct astocaml_option OPTION;

struct gentry {
    const char *name;
    VALUE value;
};

// Inline cache for a `node_gref` site.  `serial` matches `c->globals_serial`
// when `value` is fresh; otherwise the gref re-walks the globals table.
struct gref_cache {
    uint64_t serial;
    VALUE    value;
};

// Inline cache for a `node_appN` site.  `fn` is the last seen
// closure VALUE; on a hit we skip the IS_PTR / OOBJ_CLOSURE / arity
// / is_leaf chain (they were validated when the cache was filled),
// reuse the cached `body` dispatcher and captured `env`, and go
// straight to frame setup + body call.  Saves ~6 instructions per
// call vs `APPN_FAST_PATH`.  For top-level recursive functions
// (fib, ack, tak ...) the cache hits 100 % after the first call.
struct app_cache {
    VALUE              fn;
    struct Node       *body;
    struct oframe     *env;
};

// Inline cache for a `node_send` site.  `names_ptr` keys on the receiver
// object's method-name array (each instance has its own malloc'd array,
// so this naturally caches per-receiver — recursive method calls and
// hot loops on the same instance hit fast).  `closure` is the resolved
// method to apply (with self prepended at call time).  `idx` is the
// index into the method table; we keep it for diagnostics.
struct send_cache {
    void  *names_ptr;       // o->obj.method_names; NULL if cache empty
    VALUE  closure;
};

// Try / with stack: each `try ... with ...` pushes one entry; `raise`
// longjmps to the topmost.  We support nested handlers up to a fixed depth.
#define OC_HANDLER_MAX_DEPTH 256
struct oc_handler {
    jmp_buf buf;
    VALUE   exn;            // set by raise; read by handler
    struct oframe *saved_env;
};

typedef struct CTX_struct {
    struct oframe *env;

    struct gentry *globals;
    size_t globals_size;
    size_t globals_capa;
    uint64_t globals_serial;        // bumped by every oc_global_define

    // Handler stack — `try` pushes, `raise` longjmps to top.  When the
    // top handler runs, the entry is popped (so `raise` inside the handler
    // unwinds further).
    struct oc_handler handlers[OC_HANDLER_MAX_DEPTH];
    int handlers_top;       // -1 means empty

    jmp_buf err_jmp;
    int err_jmp_active;
    char err_msg[256];

    // Tail-call trampoline.  Set by node_tail_app_K when a tail-position
    // application targets a closure with matching arity.  oc_apply's
    // trampoline catches the flag and re-enters with the new fn / argv.
    int    tail_call_pending;
    VALUE  tc_fn;
    int    tc_argc;
    VALUE  tc_argv[16];
} CTX;

// Static singletons (defined in main.c).
extern struct oobj OC_UNIT_OBJ, OC_TRUE_OBJ, OC_FALSE_OBJ, OC_NIL_OBJ;
#define OC_UNIT   OC_OBJ_VAL(&OC_UNIT_OBJ)
#define OC_TRUE   OC_OBJ_VAL(&OC_TRUE_OBJ)
#define OC_FALSE  OC_OBJ_VAL(&OC_FALSE_OBJ)
#define OC_NIL    OC_OBJ_VAL(&OC_NIL_OBJ)

static inline bool OC_IS_CONS(VALUE v)    { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_CONS;    }
static inline bool OC_IS_STRING(VALUE v)  { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_STRING;  }
static inline bool OC_IS_CLOSURE(VALUE v) { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_CLOSURE; }
static inline bool OC_IS_PRIM(VALUE v)    { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_PRIM;    }
static inline bool OC_IS_TUPLE(VALUE v)   { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_TUPLE;   }
static inline bool OC_IS_REF(VALUE v)     { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_REF;     }
static inline bool OC_IS_FLOAT(VALUE v)   { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_FLOAT;   }
static inline bool OC_IS_EXN(VALUE v)     { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_EXN;     }
static inline bool OC_IS_VARIANT(VALUE v) { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_VARIANT; }
static inline bool OC_IS_RECORD(VALUE v)  { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_RECORD;  }
static inline bool OC_IS_ARRAY(VALUE v)   { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_ARRAY;   }
static inline bool OC_IS_LAZY(VALUE v)    { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_LAZY;    }
static inline bool OC_IS_BYTES(VALUE v)   { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_BYTES;   }
static inline bool OC_IS_OBJECT(VALUE v)  { return OC_IS_PTR(v) && OC_PTR(v)->type == OOBJ_OBJECT;  }

// Object helpers (defined in main.c).
struct oobj *oc_alloc(int type);
VALUE oc_cons(VALUE h, VALUE t);
VALUE oc_make_string(const char *s, size_t len);
VALUE oc_make_closure(struct Node *body, struct oframe *env, int nparams);
VALUE oc_make_closure_ex(struct Node *body, struct oframe *env, int nparams, bool is_leaf);
VALUE oc_make_prim(const char *name, oc_prim_fn fn, int min_argc, int max_argc);
VALUE oc_make_tuple(int n, VALUE *items);          // copies items
VALUE oc_make_ref(VALUE init);
VALUE oc_make_float(double d);
VALUE oc_make_exn(const char *name, int n, VALUE *items);
VALUE oc_make_variant(const char *name, int n, VALUE *items);
VALUE oc_make_record(int n, const char **fields, VALUE *items);
VALUE oc_make_array(int n, VALUE *items);
VALUE oc_make_lazy(struct Node *body, struct oframe *env);
VALUE oc_make_bytes(size_t len, char fill);
VALUE oc_force_lazy(struct CTX_struct *c, VALUE v);
VALUE oc_make_object(int n_methods, const char **method_names, VALUE *closures,
                     int n_fields,  const char **field_names, VALUE *field_init);
VALUE oc_object_send(struct CTX_struct *c, VALUE obj, const char *method, int argc, VALUE *argv);
VALUE oc_object_lookup_method(VALUE obj, const char *method);
VALUE oc_object_get_field(VALUE obj, const char *field);
void  oc_object_set_field(VALUE obj, const char *field, VALUE v);
VALUE oc_string_concat(VALUE a, VALUE b);
bool  oc_structural_eq(VALUE a, VALUE b);
int   oc_compare(VALUE a, VALUE b);                // polymorphic ordering

double oc_get_float(VALUE v);                       // converts int → float as well

// Frames.
struct oframe *oc_new_frame(struct oframe *parent, int nslots);

// Apply a callable value.
VALUE oc_apply(struct CTX_struct *c, VALUE fn, int argc, VALUE *argv);

// Raise an exception value (longjmps to the topmost try/with handler).
__attribute__((noreturn)) void oc_raise(struct CTX_struct *c, VALUE exn);
__attribute__((noreturn)) void oc_type_error(struct CTX_struct *c, const char *op, const char *expected);

// `try ... with ...` runner — kept out-of-line because ASTroGen marks
// EVAL_node_try as always_inline, and setjmp can't appear in inlined
// functions (gcc enforces this).
VALUE oc_run_try(struct CTX_struct *c, struct Node *body, struct Node *handler);

// Globals.
void  oc_global_define(struct CTX_struct *c, const char *name, VALUE v);
VALUE oc_global_ref(struct CTX_struct *c, const char *name);
VALUE oc_global_ref2(struct CTX_struct *c, const char *first, const char *second);

// Error handling.
__attribute__((noreturn,format(printf,2,3)))
void oc_error(struct CTX_struct *c, const char *fmt, ...);

// Display.
void oc_display(FILE *fp, VALUE v);

#endif
