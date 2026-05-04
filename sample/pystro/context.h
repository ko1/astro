#ifndef PYSTRO_CONTEXT_H
#define PYSTRO_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <gmp.h>

// Boehm-Demers-Weiser conservative GC.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_free(void *);
extern void  GC_init(void);
extern void  GC_gcollect(void);
extern int   GC_expand_hp(size_t);
extern void  GC_set_free_space_divisor(unsigned);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// VALUE encoding (1-bit fixnum):
//   xxxx_xxx1 → fixnum (signed 63-bit)
//   xxxx_xxx0 → ptr to `struct pyobj` (8-byte aligned)
//
// True / False / None are static singleton pyobj's; their addresses are
// the literal VALUE constants PY_TRUE / PY_FALSE / PY_NONE.
typedef int64_t VALUE;

#define PY_FIXNUM_MAX  ((int64_t)((1LL << 62) - 1))
#define PY_FIXNUM_MIN  ((int64_t)(-(1LL << 62)))
#define PY_IS_FIXNUM(v) ((int64_t)(v) & 1LL)
#define PY_FIX(n)       (((VALUE)(int64_t)(n) << 1) | 1LL)
#define PY_FIXVAL(v)    ((int64_t)(v) >> 1)

#define PY_IS_PTR(v)    (((int64_t)(v) & 1LL) == 0)
#define PY_PTR(v)       ((struct pyobj *)(uintptr_t)(v))
#define PY_OBJ_VAL(p)   ((VALUE)(uintptr_t)(p))

enum pyobj_type {
    PY_T_NONE = 0,
    PY_T_BOOL,
    PY_T_FLOAT,
    PY_T_BIGNUM,            // GMP mpz
    PY_T_STR,
    PY_T_LIST,
    PY_T_TUPLE,
    PY_T_DICT,
    PY_T_RANGE,
    PY_T_FUNC,
    PY_T_BUILTIN,
    PY_T_BOUND_METHOD,
    PY_T_CLASS,
    PY_T_INSTANCE,
};

struct pyobj;
struct pyframe;
struct pyclass;
struct pydict;
struct CTX_struct;
struct Node;

typedef VALUE (*py_builtin_fn)(struct CTX_struct *c, int argc, VALUE *argv);

// Class definition: name + method table + base class.  Methods are
// stored as a small array of (name, value) — fine until classes start
// having dozens of methods, at which point upgrading to a hash table
// is straightforward.  `is_exception` distinguishes user/builtin
// exception classes for `except` matching.
struct pyclass_method {
    const char *name;
    VALUE       value;
};
struct pyclass {
    const char *name;
    struct pyclass_method *methods;
    int  nmethods, methods_capa;
    bool is_exception;
    VALUE base;             // PY_NONE if no base class
};

// Open-addressed hash dict.  state: 0=empty, 1=used, 2=tombstone.
struct pydict_entry {
    VALUE     key;
    VALUE     value;
    uint64_t  hash;
    uint8_t   state;
};
struct pydict {
    struct pydict_entry *entries;
    size_t used;            // # used (excludes tombstones)
    size_t fill;            // used + tombstones
    size_t capa;            // power of 2
};

struct pyobj {
    int type;
    union {
        bool b;
        double dbl;
        mpz_t mpz;
        struct { char *chars; size_t len; } str;
        struct { VALUE *items; size_t len; size_t capa; } list;        // also tuple
        struct pydict *dict;
        struct { int64_t start, stop, step; } range;
        struct {
            struct Node *body;
            struct pyframe *env;
            const char *name;
            int nparams;            // total positional params
            int ndefaults;          // # of trailing params with defaults
            VALUE *defaults;        // length ndefaults
            int nlocals;            // params + locals slot count
            bool leaf;              // body contains no inner def/class →
                                    // py_apply can alloca the frame
        } func;
        struct {
            py_builtin_fn fn;
            const char *name;
            int min_argc, max_argc;
        } builtin;
        struct {
            VALUE self;
            VALUE func;             // a func or builtin
        } bound;
        struct pyclass cls;
        struct {
            struct pyobj *cls;
            struct pydict *attrs;
        } inst;
    };
};

struct pyframe {
    struct pyframe *parent;
    int nslots;
    VALUE slots[];
};

enum py_state {
    PY_STATE_NORMAL = 0,
    PY_STATE_RETURN,
    PY_STATE_RAISE,
    PY_STATE_BREAK,
    PY_STATE_CONTINUE,
};

struct pystro_option {
    bool quiet;
    bool dump_ast;
    bool no_compiled_code;
    bool compile_first;
    bool aot_only;
    bool record_all;
};
extern struct pystro_option OPTION;

struct gentry {
    const char *name;
    VALUE value;
    bool defined;
};

// Inline cache stamped at every node_gref / node_gset call site.  The
// cache holds (serial, value): hot path is two 8-byte loads + a compare.
// Mutated by `py_global_set` / `py_global_define` via globals_serial bump.
struct gref_cache {
    uint64_t serial;
    int      idx;          // index into c->globals (-1 if not yet resolved)
};

// Inline cache for `o.method(args)`.  Hot path: if `recv` has the same
// type-tag as `type_tag`, call `fn` directly with `recv` prepended to
// argv.  This skips both the bound-method heap allocation and the
// strcmp scan through the per-type method registry.  Cache miss falls
// through to `py_getattr` + `py_apply`.
struct method_cache {
    int   type_tag;        // PY_T_xxx; -1 ⇒ uninitialised
    void *fn;              // py_builtin_fn (or NULL for slow-only)
};

typedef struct CTX_struct {
    struct pyframe *env;

    struct gentry *globals;
    size_t globals_size, globals_capa;
    uint64_t globals_serial;

    int    state;
    VALUE  state_value;             // return value / raised exception
    int    raise_line;              // source line of the raise (best-effort)

    // While executing a `class C:` body, `current_class` holds the
    // class object so nested `def` ALLOC nodes register methods there
    // instead of globals.  PY_NONE outside class-body scope.
    VALUE  current_class;

    jmp_buf err_jmp;
    int     err_jmp_active;

    // Stack of jmp_buf set up by `node_try`.  `py_raise_exc` longjmps
    // to the innermost frame so dispatch unwinds without every node on
    // the path checking `c->state`.  Empty stack ⇒ longjmp to err_jmp.
    jmp_buf *try_stack[64];
    int      try_top;

    // Built-in exception classes (constructed once at install_builtins).
    VALUE EXC_Exception;
    VALUE EXC_TypeError;
    VALUE EXC_ValueError;
    VALUE EXC_NameError;
    VALUE EXC_IndexError;
    VALUE EXC_KeyError;
    VALUE EXC_ZeroDivisionError;
    VALUE EXC_AttributeError;
    VALUE EXC_RuntimeError;
    VALUE EXC_StopIteration;
} CTX;

extern struct pyobj PY_NONE_OBJ, PY_TRUE_OBJ, PY_FALSE_OBJ;
#define PY_NONE  PY_OBJ_VAL(&PY_NONE_OBJ)
#define PY_TRUE  PY_OBJ_VAL(&PY_TRUE_OBJ)
#define PY_FALSE PY_OBJ_VAL(&PY_FALSE_OBJ)

// Type predicates.
static inline bool py_is_none(VALUE v)    { return v == PY_NONE; }
static inline bool py_is_bool(VALUE v)    { return v == PY_TRUE || v == PY_FALSE; }
static inline bool py_is_fix(VALUE v)     { return PY_IS_FIXNUM(v); }
static inline bool py_is_float(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FLOAT; }
static inline bool py_is_bignum(VALUE v)  { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BIGNUM; }
static inline bool py_is_int(VALUE v)     { return py_is_fix(v) || py_is_bignum(v); }
static inline bool py_is_str(VALUE v)     { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_STR; }
static inline bool py_is_list(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_LIST; }
static inline bool py_is_tuple(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_TUPLE; }
static inline bool py_is_dict(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_DICT; }
static inline bool py_is_range(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_RANGE; }
static inline bool py_is_func(VALUE v)    { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_FUNC; }
static inline bool py_is_builtin(VALUE v) { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BUILTIN; }
static inline bool py_is_bound(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_BOUND_METHOD; }
static inline bool py_is_class(VALUE v)   { return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_CLASS; }
static inline bool py_is_instance(VALUE v){ return PY_IS_PTR(v) && PY_PTR(v)->type == PY_T_INSTANCE; }
static inline bool py_is_callable(VALUE v) {
    return py_is_func(v) || py_is_builtin(v) || py_is_bound(v) || py_is_class(v);
}

// Python truthiness.
static inline bool
py_is_truthy(VALUE v)
{
    if (v == PY_NONE || v == PY_FALSE) return false;
    if (PY_IS_FIXNUM(v)) return PY_FIXVAL(v) != 0;
    struct pyobj *o = PY_PTR(v);
    switch (o->type) {
      case PY_T_FLOAT:  return o->dbl != 0.0;
      case PY_T_BIGNUM: return mpz_sgn(o->mpz) != 0;
      case PY_T_STR:    return o->str.len != 0;
      case PY_T_LIST:
      case PY_T_TUPLE:  return o->list.len != 0;
      case PY_T_DICT:   return o->dict->used != 0;
      default:          return true;
    }
}

// Allocators / builders.
struct pyobj *py_alloc(int type);
VALUE py_make_float (double d);
VALUE py_make_int   (int64_t v);                  // fixnum if fits, else bignum
VALUE py_make_bignum(mpz_srcptr z);
VALUE py_make_str   (const char *s, size_t len);
VALUE py_make_str_take(char *s, size_t len);      // takes ownership
VALUE py_make_list  (VALUE *items, size_t n);     // copies items; capa=max(n,4)
VALUE py_make_tuple (VALUE *items, size_t n);
VALUE py_make_dict  (void);
VALUE py_make_range (int64_t start, int64_t stop, int64_t step);
VALUE py_make_func  (struct Node *body, struct pyframe *env,
                     const char *name, int nparams, int nlocals,
                     int ndefaults, VALUE *defaults, bool leaf);
VALUE py_make_builtin(const char *name, py_builtin_fn fn, int min_argc, int max_argc);
VALUE py_make_bound (VALUE self, VALUE func);
VALUE py_make_class (const char *name, VALUE base, bool is_exception);
VALUE py_make_instance(VALUE cls);

// Frame.
struct pyframe *py_new_frame(struct pyframe *parent, int nslots);

// Globals.
void  py_global_define(CTX *c, const char *name, VALUE v);
VALUE py_global_ref   (CTX *c, const char *name);
void  py_global_set   (CTX *c, const char *name, VALUE v);
bool  py_global_has   (CTX *c, const char *name);

// Apply.  The fast path (closure with matching arity) is `static inline`
// in node.h; everything else routes here.
VALUE py_apply_slow(CTX *c, VALUE fn, int argc, VALUE *argv);

// Display + repr.
void  py_display(FILE *fp, VALUE v, bool repr);
VALUE py_to_str(CTX *c, VALUE v);
VALUE py_to_repr(CTX *c, VALUE v);

// Error / raise.
__attribute__((noreturn,format(printf,2,3)))
void py_error(CTX *c, const char *fmt, ...);
__attribute__((noreturn,format(printf,3,4)))
void py_raise_exc(CTX *c, VALUE cls, const char *fmt, ...);

// Numeric tower.
VALUE py_add (CTX *c, VALUE a, VALUE b);
VALUE py_sub (CTX *c, VALUE a, VALUE b);
VALUE py_mul (CTX *c, VALUE a, VALUE b);
VALUE py_truediv(CTX *c, VALUE a, VALUE b);
VALUE py_fdiv(CTX *c, VALUE a, VALUE b);
VALUE py_mod (CTX *c, VALUE a, VALUE b);
VALUE py_pow (CTX *c, VALUE a, VALUE b);
VALUE py_neg (CTX *c, VALUE a);
VALUE py_bit_and(CTX *c, VALUE a, VALUE b);
VALUE py_bit_or (CTX *c, VALUE a, VALUE b);
VALUE py_bit_xor(CTX *c, VALUE a, VALUE b);
VALUE py_bit_inv(CTX *c, VALUE a);
VALUE py_lshift (CTX *c, VALUE a, VALUE b);
VALUE py_rshift (CTX *c, VALUE a, VALUE b);
int   py_cmp (CTX *c, VALUE a, VALUE b);
VALUE py_eq  (CTX *c, VALUE a, VALUE b);
bool  py_eq_bool(CTX *c, VALUE a, VALUE b);

// Containers.
VALUE py_list_get   (CTX *c, VALUE list, VALUE idx);    // also tuple/str
VALUE py_list_set   (CTX *c, VALUE list, VALUE idx, VALUE val);
VALUE py_list_slice (CTX *c, VALUE seq, VALUE start, VALUE stop, VALUE step);
void  py_list_append(CTX *c, VALUE list, VALUE v);
size_t py_seq_len   (CTX *c, VALUE v);
VALUE py_dict_get   (CTX *c, VALUE d, VALUE key);
void  py_dict_set   (CTX *c, VALUE d, VALUE key, VALUE val);
bool  py_dict_has   (CTX *c, VALUE d, VALUE key);
bool  py_dict_remove(CTX *c, VALUE d, VALUE key);
uint64_t py_hash    (CTX *c, VALUE v);

// Membership: `x in y`.
bool py_contains(CTX *c, VALUE container, VALUE v);

// Attribute access.
VALUE py_getattr(CTX *c, VALUE v, const char *name);
void  py_setattr(CTX *c, VALUE v, const char *name, VALUE val);

// Method-call support: `o.m(...)` resolves to a callable; for instance
// methods we wrap as bound; for built-in type methods (str.split, ...)
// we look up via an opaque per-type table.
VALUE py_builtin_method(CTX *c, VALUE recv, const char *name);

// Iteration: returns an opaque iterator handle; `py_iter_next` returns
// PY_NONE on stop (and sets *done=true), else the next element.
struct py_iter {
    int kind;               // 0=list/tuple, 1=str, 2=range, 3=dict
    VALUE container;
    int64_t i;
    int64_t end;
    int64_t step;
};
void py_iter_init(CTX *c, struct py_iter *it, VALUE iterable);
bool py_iter_next(CTX *c, struct py_iter *it, VALUE *out);

// Try-handler descriptor.  The parser packs an array of these into the
// global PYSTRO_HANDLERS table; node_try walks `nhandlers` of them
// starting at `handlers_idx`.
struct pyhandler {
    struct Node *exc_class;     // NULL ⇒ bare except (catch all)
    struct Node *body;
    const char  *name;          // NULL ⇒ no `as name`
    bool         name_is_global;
    int          name_slot;
};

// Unpack-assignment target.
struct pyunpack_target {
    bool        is_local;
    int         slot;
    const char *global_name;
};

bool py_exc_matches(CTX *c, VALUE exc, VALUE cls);
void py_unpack_assign(CTX *c, struct pyunpack_target *t, uint32_t n, VALUE rhs);
void py_class_add_method(CTX *c, VALUE cls, const char *name, VALUE fn);

// Side tables populated by the parser, consumed by node_eval.c.
struct Node;
extern struct Node            **PYSTRO_NODE_TABLE;
extern struct pyhandler        *PYSTRO_HANDLERS;
extern struct pyunpack_target  *PYSTRO_UNPACK_TARGETS;

#endif // PYSTRO_CONTEXT_H
