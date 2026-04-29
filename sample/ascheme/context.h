#ifndef ASCHEME_CONTEXT_H
#define ASCHEME_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <alloca.h>
#include <gmp.h>

// Boehm-Demers-Weiser conservative GC.  We don't depend on libgc-dev
// (which provides gc.h), so the few entry points we need are declared
// here directly.  Linking is via `-lgc`.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);          // no embedded pointers
extern void *GC_malloc_uncollectable(size_t);   // never GC'd
extern void *GC_realloc(void *, size_t);
extern void  GC_free(void *);
extern void  GC_init(void);
extern void  GC_gcollect(void);
extern size_t GC_get_heap_size(void);
extern size_t GC_get_total_bytes(void);
extern void  GC_set_finalizer(void *, void (*)(void *, void *), void *,
                              void (**)(void *, void *), void **);

// VALUE = tagged Scheme value (SVAL).  Tag bits:
//   xxxx_xxx1 → fixnum (signed 62-bit, shifted left by 1)
//   xxxx_xx10 → flonum (IEEE-754 double encoded inline; Ruby's scheme)
//   xxxx_x000 → pointer to heap-allocated `struct sobj` (8-byte aligned)
//
// Flonum encoding (taken verbatim from CRuby ≥ 2.0).  IEEE-754 doubles in
// the magnitude range ~[1e-77, 1e+77] (exponent top-3-bits == 0b011 or
// 0b100) round-trip through a 3-bit rotation + bit-1 tag.  Everything
// else — 0.0, denormals, NaN/inf, or values outside that range — falls
// back to the heap-allocated OBJ_DOUBLE path.  This keeps ~all numbers
// arising in scientific code from allocating, while preserving exact
// representation for the values that fit.
//
// The empty list, booleans, eof, and the unspecified value are statically
// allocated singleton sobj's; their addresses are exposed as SCM_NIL etc.
typedef int64_t VALUE;

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

#define SCM_FIXNUM_MAX  ((int64_t)((1LL << 62) - 1))
#define SCM_FIXNUM_MIN  ((int64_t)(-(1LL << 62)))
#define SCM_IS_FIXNUM(v) ((int64_t)(v) & 1LL)
#define SCM_FIX(n)       (((VALUE)(int64_t)(n) << 1) | 1LL)
#define SCM_FIXVAL(v)    ((int64_t)(v) >> 1)

#define SCM_FLONUM_MASK  3LL
#define SCM_FLONUM_TAG   2LL
#define SCM_IS_FLONUM(v) (((int64_t)(v) & SCM_FLONUM_MASK) == SCM_FLONUM_TAG)

#define SCM_IS_PTR(v)    (((int64_t)(v) & SCM_FLONUM_MASK) == 0)
#define SCM_PTR(v)       ((struct sobj *)(uintptr_t)(v))
#define SCM_OBJ_VAL(p)   ((VALUE)(uintptr_t)(p))

static inline uint64_t
scm_rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
static inline uint64_t
scm_rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Try to encode `d` as an inline flonum.  Returns 0 if `d` can't fit
// (including 0.0, denormals, NaN, ±inf) — caller must fall back to a
// heap-allocated OBJ_DOUBLE in that case.
static inline VALUE
scm_try_flonum(double d)
{
    union { double d; uint64_t u; } pun;
    pun.d = d;
    int bits = (int)((pun.u >> 60) & 0x7);
    if (UNLIKELY(d == 0.0 || (bits != 3 && bits != 4))) return 0;
    return (VALUE)((scm_rotl64(pun.u, 3) & ~(uint64_t)1) | SCM_FLONUM_TAG);
}

static inline double
scm_flonum_to_double(VALUE v)
{
    union { double d; uint64_t u; } pun;
    uint64_t b63 = ((uint64_t)v >> 63) & 1;
    pun.u = scm_rotr64((2 - b63) | ((uint64_t)v & ~(uint64_t)3), 3);
    return pun.d;
}

// Heap object types.
enum sobj_type {
    OBJ_NIL,
    OBJ_BOOL,
    OBJ_UNSPEC,
    OBJ_EOF,
    OBJ_PAIR,
    OBJ_SYMBOL,
    OBJ_STRING,
    OBJ_CHAR,
    OBJ_VECTOR,
    OBJ_CLOSURE,
    OBJ_PRIM,
    OBJ_DOUBLE,
    OBJ_BIGNUM,         // arbitrary-precision integer (mpz)
    OBJ_RATIONAL,       // exact rational (mpq)
    OBJ_COMPLEX,        // a + bi (two doubles)
    OBJ_MVALUES,        // multiple-values box
    OBJ_PROMISE,        // delay/force memoizing closure
    OBJ_PORT,
    OBJ_CONT,
};

struct sobj;
struct sframe;
struct CTX_struct;
struct Node;

typedef VALUE (*scm_prim_fn)(struct CTX_struct *c, int argc, VALUE *argv);

struct sobj {
    int type;
    union {
        struct { VALUE car, cdr; } pair;
        struct { char *chars; size_t len; } str;
        struct { char *name; } sym;
        uint32_t ch;
        bool b;
        struct { VALUE *items; size_t len; } vec;
        struct {
            struct Node *body;
            struct sframe *env;
            int nparams;
            int has_rest;
            const char *name;
        } closure;
        struct {
            scm_prim_fn fn;
            const char *name;
            int min_argc, max_argc;   // max=-1 → unlimited
        } prim;
        double dbl;
        mpz_t mpz;
        mpq_t mpq;
        struct { double re, im; } cpx;
        struct { VALUE *items; size_t len; } mv;
        struct { VALUE thunk; VALUE value; bool forced; } promise;
        struct { FILE *fp; bool input; bool closed; bool owned; } port;
        struct {
            jmp_buf buf;
            VALUE result;
            int active;     // 0 = consumed; calling triggers error
            int tag;        // unique tag for nested call/cc disambiguation
        } cont;
    };
};

struct sframe {
    struct sframe *parent;
    int nslots;
    VALUE slots[];
};

struct ascheme_option {
    bool quiet;
    bool no_compiled_code;
    bool no_generate_specialized_code;
    bool record_all;
    bool dump_ast;
    bool trace;
};
extern struct ascheme_option OPTION;

struct gentry {
    const char *name;
    VALUE value;
    bool defined;
};

// Inline cache stamped at every node_gref call site.  Stored as `@ref`
// (embedded in the NODE union, not on the structural hash).  `cached`
// goes from 0 → 1 once we've resolved the name; the index is stable
// across the lifetime of `c->globals` (we never remove globals, and
// realloc keeps numeric positions intact even when the buffer moves).
struct gref_cache {
    int32_t cached;
    uint32_t index;
};

// Inline cache for the specialized arithmetic / comparison nodes.  Each
// such node also baked an "expected" prim sobj at install_prims time
// (e.g. PRIM_PLUS_VAL); the EVAL body confirms `c->globals[index].value
// == PRIM_<op>_VAL` before taking the fast path.  When the user does
// `(set! + my-add)` the global value at this index changes, the check
// fails, and we fall back to a regular `scm_apply` against whatever the
// global now points to — preserving R5RS semantics.
struct arith_cache {
    int32_t resolved;
    uint32_t index;
};

// The original primitive sobj for each specialized operator.  Set by
// install_prims; checked by node_arith_* / node_pred_* / node_vec_* on
// every call to detect user redefinition.
extern VALUE PRIM_PLUS_VAL, PRIM_MINUS_VAL, PRIM_MUL_VAL;
extern VALUE PRIM_NUM_LT_VAL, PRIM_NUM_LE_VAL, PRIM_NUM_GT_VAL, PRIM_NUM_GE_VAL, PRIM_NUM_EQ_VAL;
extern VALUE PRIM_NULL_P_VAL, PRIM_PAIR_P_VAL, PRIM_CAR_VAL, PRIM_CDR_VAL, PRIM_NOT_VAL;
extern VALUE PRIM_VECTOR_REF_VAL, PRIM_VECTOR_SET_VAL;

typedef struct CTX_struct {
    // Current lexical environment chain (closures + call frames).
    struct sframe *env;

    // Global definitions: linear array (small N for now).
    struct gentry *globals;
    size_t globals_size;
    size_t globals_capa;

    // Tail-call trampoline state (set by tail-position call nodes).
    struct Node *next_body;
    struct sframe *next_env;
    int tail_call_pending;

    // call/cc tag generator + active continuations stack.
    int cont_tag_seq;

    // Top-level error escape.
    jmp_buf err_jmp;
    int err_jmp_active;
} CTX;

// Singleton scheme values, initialized at INIT().
extern struct sobj S_NIL_OBJ, S_TRUE_OBJ, S_FALSE_OBJ, S_UNSPEC_OBJ, S_EOF_OBJ;
#define SCM_NIL      SCM_OBJ_VAL(&S_NIL_OBJ)
#define SCM_TRUE     SCM_OBJ_VAL(&S_TRUE_OBJ)
#define SCM_FALSE    SCM_OBJ_VAL(&S_FALSE_OBJ)
#define SCM_UNSPEC   SCM_OBJ_VAL(&S_UNSPEC_OBJ)
#define SCM_EOFV     SCM_OBJ_VAL(&S_EOF_OBJ)

// Type predicates (work for both immediates and heap objects).
static inline bool scm_is_pair(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_PAIR; }
static inline bool scm_is_null(VALUE v) { return v == SCM_NIL; }
static inline bool scm_is_true(VALUE v) { return v != SCM_FALSE; }   // R5RS: only #f is false
static inline bool scm_is_false(VALUE v) { return v == SCM_FALSE; }
static inline bool scm_is_bool(VALUE v) { return v == SCM_TRUE || v == SCM_FALSE; }
static inline bool scm_is_symbol(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_SYMBOL; }
static inline bool scm_is_string(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_STRING; }
static inline bool scm_is_char(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_CHAR; }
static inline bool scm_is_vector(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_VECTOR; }
static inline bool scm_is_closure(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_CLOSURE; }
static inline bool scm_is_prim(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_PRIM; }
// "double" covers both inline flonums and the heap-allocated OBJ_DOUBLE.
// scm_is_heap_double matches only the latter — used by `scm_get_double`
// and friends to decide whether to dereference.
static inline bool scm_is_heap_double(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_DOUBLE; }
static inline bool scm_is_double(VALUE v) { return SCM_IS_FLONUM(v) || scm_is_heap_double(v); }
static inline bool scm_is_bignum(VALUE v)  { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_BIGNUM; }
static inline bool scm_is_rational(VALUE v){ return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_RATIONAL; }
static inline bool scm_is_complex(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_COMPLEX; }
static inline bool scm_is_mvalues(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_MVALUES; }
static inline bool scm_is_promise(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_PROMISE; }
static inline bool scm_is_port(VALUE v)    { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_PORT; }
static inline bool scm_is_cont(VALUE v) { return SCM_IS_PTR(v) && SCM_PTR(v)->type == OBJ_CONT; }
static inline bool scm_is_proc(VALUE v) {
    return scm_is_closure(v) || scm_is_prim(v) || scm_is_cont(v);
}
static inline bool scm_is_exact(VALUE v) {
    return SCM_IS_FIXNUM(v) || scm_is_bignum(v) || scm_is_rational(v);
}
static inline bool scm_is_inexact(VALUE v) {
    return scm_is_double(v) || scm_is_complex(v);
}
static inline bool scm_is_real(VALUE v) {
    return SCM_IS_FIXNUM(v) || scm_is_double(v) || scm_is_bignum(v) || scm_is_rational(v);
}
static inline bool scm_is_number(VALUE v) {
    return scm_is_real(v) || scm_is_complex(v);
}
bool scm_is_integer_value(VALUE v);     // defined in main.c (handles all numeric kinds)

// Object-construction helpers (defined in main.c).
struct sobj *scm_alloc(int type);
VALUE scm_cons(VALUE a, VALUE d);
VALUE scm_intern(const char *name);
VALUE scm_make_string(const char *s, size_t len);
VALUE scm_make_string_n(size_t len, char fill);
VALUE scm_make_char(uint32_t cp);
VALUE scm_make_vector(size_t len, VALUE fill);
VALUE scm_make_double(double d);
VALUE scm_make_int(int64_t v);          // fixnum or bignum if overflows
VALUE scm_make_bignum_z(mpz_srcptr z);  // copy mpz_t into a fresh bignum sobj
VALUE scm_make_rational_q(mpq_srcptr q);
VALUE scm_make_rational_zz(mpz_srcptr num, mpz_srcptr den);
VALUE scm_make_complex(double re, double im);
VALUE scm_make_mvalues(int count, VALUE *items);
VALUE scm_make_closure(struct Node *body, struct sframe *env, int nparams, int has_rest);
VALUE scm_make_prim(const char *name, scm_prim_fn fn, int min_argc, int max_argc);
double scm_get_double(VALUE v);          // converts any numeric to C double
VALUE scm_normalize_int(mpz_srcptr z);   // → fixnum if fits, bignum otherwise
VALUE scm_normalize_rat(mpq_t q);        // → fixnum / bignum / rational
VALUE scm_simplify_complex(double re, double im);  // im=0 ⇒ real

// Frame helpers.
struct sframe *scm_new_frame(struct sframe *parent, int nslots);

// Apply a procedure value.  Used by primitives like apply / map.
VALUE scm_apply(CTX *c, VALUE fn, int argc, VALUE *argv);

// Globals.
void scm_global_define(CTX *c, const char *name, VALUE v);
VALUE scm_global_ref(CTX *c, const char *name);
void scm_global_set(CTX *c, const char *name, VALUE v);

// Error.
__attribute__((noreturn,format(printf,2,3)))
void scm_error(CTX *c, const char *fmt, ...);

// Print + read.
void scm_display(FILE *fp, VALUE v, bool readable);
VALUE scm_read(CTX *c, FILE *fp);

#endif
