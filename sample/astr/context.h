#ifndef ASTR_CONTEXT_H
#define ASTR_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>

// Boehm-Demers-Weiser conservative GC.  Forward-declared rather than
// pulling in <gc/gc.h> here so the generated SD .c files don't need
// a -I path to libgc — they only ever see GC_malloc as an opaque
// extern symbol.
extern void *GC_malloc(size_t);
extern void *GC_malloc_atomic(size_t);
extern void *GC_realloc(void *, size_t);
extern void  GC_init(void);
extern void  GC_set_free_space_divisor(unsigned int);

#define LIKELY(expr)   __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// VALUE encoding (1-bit fixnum tag, modeled on pystro / abruby):
//
//   xxxx_xxx1 → fixnum (signed 63-bit value, low bit = tag)
//   xxxx_xxx0 → ptr to `struct astr_obj` (heap-allocated, 8-byte aligned)
//
// Special singletons (statically allocated):
//   ASTR_NA, ASTR_NULL
//
// Booleans collapse to fixnums 1 / 0 (R promotes logical → integer →
// numeric automatically and our fast paths read fixnums directly, so
// keeping TRUE/FALSE as fixnums avoids a heap dereference on every
// `if`).
typedef int64_t VALUE;

#define ASTR_FIX_MAX     ((int64_t)((1LL << 62) - 1))
#define ASTR_FIX_MIN     ((int64_t)(-(1LL << 62)))
#define ASTR_IS_FIX(v)   ((int64_t)(v) & 1LL)
#define ASTR_FIX(n)      (((VALUE)(int64_t)(n) << 1) | 1LL)
#define ASTR_FIX_VAL(v)  ((int64_t)(v) >> 1)
#define ASTR_IS_PTR(v)   (((int64_t)(v) & 1LL) == 0)
#define ASTR_PTR(v)      ((struct astr_obj *)(uintptr_t)(v))
#define ASTR_OBJ_VAL(p)  ((VALUE)(uintptr_t)(p))

#define ASTR_TRUE   ASTR_FIX(1)
#define ASTR_FALSE  ASTR_FIX(0)

enum astr_type {
    ASTR_T_FLOAT = 1,
    ASTR_T_STRING,
    ASTR_T_NUM_VEC,        // numeric vector (double[]) — `c(1,2,3)` → this
    ASTR_T_INT_VEC,        // integer vector (int64_t[]) — `1:n`
    ASTR_T_STR_VEC,        // character vector (VALUE strings)
    ASTR_T_LIST,           // generic list (VALUE[]) — R's `list(...)`
    ASTR_T_NA,             // singleton
    ASTR_T_NULL,           // singleton
};

struct astr_obj {
    int type;
    union {
        double dbl;
        struct { char *chars; size_t len; } str;
        struct { double  *items; size_t len; size_t capa; } numvec;
        struct { int64_t *items; size_t len; size_t capa; } intvec;
        struct { VALUE   *items; size_t len; size_t capa; } lst;
    };
};

extern struct astr_obj ASTR_NA_OBJ, ASTR_NULL_OBJ;
#define ASTR_NA   ASTR_OBJ_VAL(&ASTR_NA_OBJ)
#define ASTR_NULL ASTR_OBJ_VAL(&ASTR_NULL_OBJ)

// Allocators (defined in runtime.c).
struct astr_obj *astr_alloc(int type);
VALUE astr_make_float (double d);
VALUE astr_make_int   (int64_t v);                 // fixnum if fits, else heap float
VALUE astr_make_string(const char *s, size_t len);
VALUE astr_make_numvec_n(size_t n);                // zero-initialised, len=n
VALUE astr_make_numvec_from(const VALUE *items, size_t n);
VALUE astr_make_intvec_range(int64_t start, int64_t stop);
VALUE astr_make_list  (VALUE *items, size_t n);

// Coercions / accessors.
static inline double
astr_to_double(VALUE v)
{
    if (LIKELY(ASTR_IS_FIX(v))) return (double)ASTR_FIX_VAL(v);
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:   return o->dbl;
      case ASTR_T_NUM_VEC: return o->numvec.len ? o->numvec.items[0] : 0.0;
      case ASTR_T_INT_VEC: return o->intvec.len ? (double)o->intvec.items[0] : 0.0;
      default:             return 0.0;
    }
}

static inline int64_t
astr_to_int(VALUE v)
{
    if (LIKELY(ASTR_IS_FIX(v))) return ASTR_FIX_VAL(v);
    return (int64_t)astr_to_double(v);
}

// R-style truthiness: `TRUE` / non-zero numeric / non-empty string is
// truthy.  NA / NULL / 0 / "" are falsy.  R treats NA as an error in
// boolean context but our v0 collapses it to false to keep the runtime
// simple.
static inline bool
astr_is_truthy(VALUE v)
{
    if (LIKELY(ASTR_IS_FIX(v))) return ASTR_FIX_VAL(v) != 0;
    struct astr_obj *o = ASTR_PTR(v);
    switch (o->type) {
      case ASTR_T_FLOAT:   return o->dbl != 0.0;
      case ASTR_T_STRING:  return o->str.len != 0;
      case ASTR_T_NUM_VEC: return o->numvec.len > 0 && o->numvec.items[0] != 0.0;
      case ASTR_T_INT_VEC: return o->intvec.len > 0 && o->intvec.items[0] != 0;
      case ASTR_T_LIST:    return o->lst.len > 0;
      default:             return false;
    }
}

// Equality (used for `==`).  Numeric equality coerces; string equality
// is content-based.  Mixed numeric vs string follows R: returns FALSE.
bool astr_eq(VALUE a, VALUE b);

// Comparison (used for `<`, `<=`, etc).  Returns -1/0/1.  Mixed string vs
// numeric raises an error like real R; v0 returns 0 to keep the
// runtime simple.
int astr_cmp(VALUE a, VALUE b);

// Subscripting and length.
VALUE  astr_subscript_get(VALUE seq, VALUE idx);              // 1-based
VALUE  astr_subscript_set(VALUE seq, VALUE idx, VALUE val);
size_t astr_length(VALUE v);

// Print / cat / concat helpers.
void  astr_print(FILE *fp, VALUE v);     // R-style `print(x)` — adds [1]
void  astr_cat  (FILE *fp, VALUE v);     // raw `cat` — no brackets, no quotes
VALUE astr_paste(VALUE *items, size_t n, const char *sep);

// ---------------------------------------------------------------------------
// 2-register RESULT (modeled on naruby / castro).
// ---------------------------------------------------------------------------

#define RESULT_NORMAL  0u
#define RESULT_RETURN  1u

typedef struct {
    VALUE        value;
    unsigned int state;
} RESULT;

#define RESULT_OK(v)        ((RESULT){(v), RESULT_NORMAL})
#define RESULT_RETURN_(v)   ((RESULT){(v), RESULT_RETURN})

#define UNWRAP(r) ({ RESULT _r = (r); if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; _r.value; })

// ---------------------------------------------------------------------------
// CTX / option struct.
// ---------------------------------------------------------------------------

struct astr_option {
    bool quiet;
    bool dump_ast;
    bool plain;
    bool compile_first;
    bool skip_bake;
    bool compile_only;
    bool clear_store;
    bool record_all;
};

extern struct astr_option OPTION;

struct function_entry {
    const char *name;
    struct Node *body;
    unsigned int params_cnt;
    unsigned int locals_cnt;
};

struct astr_callcache {
    struct Node *body;
};

typedef struct CTX_struct {
    VALUE        *env;
    VALUE        *fp;
    unsigned int  func_set_cnt;
    struct function_entry *func_set;
} CTX;

// Side table for variadic call / vector literal nodes — populated by
// the parser, indexed by a 32-bit base index baked into the NODE.
extern struct Node **ASTR_NODE_TABLE;
extern uint32_t       ASTR_NODE_TABLE_LEN;

// Slow paths (defined in runtime.c) — handle vectors, floats, mixed
// types.  The inline fast paths below check fixnum-fixnum first for
// the benchmark-heavy case (fib / loop / ack) where both sides
// constant-fold to scalars and the non-fixnum branch never runs.
VALUE astr_add_slow(VALUE a, VALUE b);
VALUE astr_sub_slow(VALUE a, VALUE b);
VALUE astr_mul_slow(VALUE a, VALUE b);
VALUE astr_div_slow(VALUE a, VALUE b);
VALUE astr_idiv_slow(VALUE a, VALUE b);
VALUE astr_mod_slow(VALUE a, VALUE b);
VALUE astr_pow_slow(VALUE a, VALUE b);
VALUE astr_neg_slow(VALUE a);

static inline VALUE
astr_add(VALUE a, VALUE b)
{
    if (LIKELY(ASTR_IS_FIX(a) & ASTR_IS_FIX(b))) {
        int64_t la = ASTR_FIX_VAL(a), lb = ASTR_FIX_VAL(b), r;
        if (LIKELY(!__builtin_add_overflow(la, lb, &r) &&
                   r <= ASTR_FIX_MAX && r >= ASTR_FIX_MIN)) {
            return ASTR_FIX(r);
        }
    }
    return astr_add_slow(a, b);
}

static inline VALUE
astr_sub(VALUE a, VALUE b)
{
    if (LIKELY(ASTR_IS_FIX(a) & ASTR_IS_FIX(b))) {
        int64_t la = ASTR_FIX_VAL(a), lb = ASTR_FIX_VAL(b), r;
        if (LIKELY(!__builtin_sub_overflow(la, lb, &r) &&
                   r <= ASTR_FIX_MAX && r >= ASTR_FIX_MIN)) {
            return ASTR_FIX(r);
        }
    }
    return astr_sub_slow(a, b);
}

static inline VALUE
astr_mul(VALUE a, VALUE b)
{
    if (LIKELY(ASTR_IS_FIX(a) & ASTR_IS_FIX(b))) {
        int64_t la = ASTR_FIX_VAL(a), lb = ASTR_FIX_VAL(b), r;
        if (LIKELY(!__builtin_mul_overflow(la, lb, &r) &&
                   r <= ASTR_FIX_MAX && r >= ASTR_FIX_MIN)) {
            return ASTR_FIX(r);
        }
    }
    return astr_mul_slow(a, b);
}

static inline VALUE astr_div(VALUE a, VALUE b)  { return astr_div_slow(a, b); }
static inline VALUE astr_idiv(VALUE a, VALUE b) { return astr_idiv_slow(a, b); }
static inline VALUE astr_mod(VALUE a, VALUE b)  { return astr_mod_slow(a, b); }
static inline VALUE astr_pow(VALUE a, VALUE b)  { return astr_pow_slow(a, b); }

static inline VALUE
astr_neg(VALUE a)
{
    if (LIKELY(ASTR_IS_FIX(a))) {
        int64_t la = ASTR_FIX_VAL(a);
        if (LIKELY(la != ASTR_FIX_MIN)) return ASTR_FIX(-la);
    }
    return astr_neg_slow(a);
}

#endif // ASTR_CONTEXT_H
