#ifndef BARUBY_CONTEXT_H
#define BARUBY_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <gc.h>

// Route allocations through Boehm GC.  The libc-shape macros sit AFTER
// the system includes so internal libc / libgc callers retain their
// plain symbols.  From this point, every malloc / calloc / realloc /
// strdup / free in baruby code becomes a GC equivalent; free is a
// no-op since the collector reclaims unreachable blocks.
#define malloc(n)      GC_MALLOC(n)
#define calloc(n, s)   GC_MALLOC((size_t)(n) * (size_t)(s))
#define realloc(p, n)  GC_REALLOC((p), (n))
#define strdup(s)      GC_STRDUP(s)
#define free(p)        ((void)(p))

#define BARUBY_DEBUG 1
#if BARUBY_DEBUG
#define BARUBY_ASSERT(expr) assert(expr)
#else
#define BARUBY_ASSERT(expr) 0
#endif

// Option model — see naruby parent for the rationale.  baruby keeps the
// orthogonal AOT / PG flags.  JIT (-j) is currently unwired post-fork.
struct baruby_option {
    bool static_lang;

    bool plain;            // -i / --plain
    bool compile_first;    // -c / --aot
    bool pg_at_exit;       // -p / --pg
    bool skip_bake;        // -b
    bool compile_only;     // --aot-compile
    bool clear_store;      // --ccs
    bool jit;              // -j   (unwired post-fork)

    // Referenced by framework-generated ALLOC_ helpers (lib/astrogen.rb).
    bool record_all;

    bool quiet;
};

extern struct baruby_option OPTION;

// -----------------------------------------------------------------------------
// Tagged VALUE.
//
//   LSB == 1                  -> fixnum (signed int63, sign-extends on shift)
//   raw == 0                  -> false singleton
//   raw == 2                  -> true singleton
//   raw == 4                  -> nil singleton
//   LSB == 0, v not in {0,2,4}
//                             -> heap object pointer (8-byte aligned)
//
// `false` and `nil` are now distinct (Ruby `nil != false`).  They are
// the only **falsy** values; everything else (including INT2VAL(0),
// `[]`, `""`, `true`) is truthy.  Because `nil = 4` is non-zero in C,
// node_if / node_while can NOT use a plain `if (UNWRAP(...))` — they
// test through the IS_FALSY macro.
//
// Sub-page singleton values (0, 2, 4) are guaranteed not to collide
// with libgc-returned heap pointers because libgc never hands out
// addresses below the first heap page.
//
// Arithmetic on fixnums goes through VAL2INT / INT2VAL.  The shift pair
// folds away under -O3 along most call paths; tag-preserving tricks
// like `(a + b - 1)` are skipped in favour of clarity for now.
// -----------------------------------------------------------------------------
typedef intptr_t VALUE;
typedef uint64_t state_serial_t;

#define INT2VAL(i)    ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | (uintptr_t)1))
#define VAL2INT(v)    (((intptr_t)(v)) >> 1)
#define VAL_FALSE     ((VALUE)0)
#define VAL_TRUE      ((VALUE)2)
#define VAL_NIL       ((VALUE)4)
#define IS_INT(v)     (((uintptr_t)(v) & (uintptr_t)1) != 0)
#define IS_FALSY(v)   ((v) == VAL_FALSE || (v) == VAL_NIL)
#define IS_TRUTHY(v)  (!IS_FALSY(v))
#define IS_PTR(v)     ((v) != VAL_FALSE && (v) != VAL_TRUE && (v) != VAL_NIL \
                       && ((uintptr_t)(v) & (uintptr_t)1) == 0)

// Heap object header.  Type tag lets the dispatch nodes branch on
// receiver type at eval time (e.g. call_size: array vs string).
enum obj_type {
    OBJ_ARRAY  = 1,
    OBJ_STRING = 2,
};

typedef struct ObjectHeader {
    uint32_t type;
    uint32_t flags;
} ObjectHeader;

typedef struct BaArray {
    ObjectHeader hdr;
    uint32_t len;
    uint32_t capa;
    VALUE *items;            // separate alloc; libgc scans it conservatively
} BaArray;

typedef struct BaString {
    ObjectHeader hdr;
    uint32_t len;            // byte length (not counting NUL)
    uint32_t capa;
    char *bytes;             // NUL-terminated for cheap printf interop
} BaString;

#define OBJ_TYPE(v)   (((ObjectHeader *)(v))->type)
#define IS_ARY(v)     (IS_PTR(v) && OBJ_TYPE(v) == OBJ_ARRAY)
#define IS_STR(v)     (IS_PTR(v) && OBJ_TYPE(v) == OBJ_STRING)
#define VAL2ARY(v)    ((BaArray *)(v))
#define VAL2STR(v)    ((BaString *)(v))

// RESULT: 2-register return type for non-local exit support (`return`).
// Same shape as castro / naruby's RESULT — fits in rax:rdx so the
// function return ABI carries both VALUE and a state bit without
// needing setjmp.
//
// On the fast path (no `return`), `state == RESULT_NORMAL == 0` lets
// the `if (r.state)` test fold to a single branch the predictor handles
// for free.  Within an inlined SD chain `state` is a compile-time
// constant 0 almost everywhere, so gcc DCE's the propagation tests
// entirely.

#define RESULT_NORMAL 0u
#define RESULT_RETURN 1u   /* node_return — caught at function-call boundary */

typedef struct {
    VALUE        value;
    unsigned int state;
} RESULT;

#define RESULT_OK(v)        ((RESULT){(v), RESULT_NORMAL})
#define RESULT_RETURN_(v)   ((RESULT){(v), RESULT_RETURN})

// UNWRAP: extract VALUE from RESULT, or propagate non-NORMAL state by
// returning from the *caller* function (statement expression).  Use
// this at every internal EVAL_ARG site so e.g. `return` inside a
// deeply nested if/while bubbles up to the enclosing function-call
// boundary without setjmp.  Borrowed from castro / abruby.
#define UNWRAP(r) ({ RESULT _r = (r); if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; _r.value; })

struct function_entry {
    const char *name;
    struct Node *body;
    unsigned int params_cnt;
    unsigned int locals_cnt;
};

struct callcache {
    state_serial_t serial;
    struct Node *body;
};

typedef VALUE (*builtin_func_ptr)(void);
typedef VALUE (*builtin_func1_ptr)(VALUE);
typedef VALUE (*builtin_func2_ptr)(VALUE, VALUE);
typedef VALUE (*builtin_func3_ptr)(VALUE, VALUE, VALUE);
typedef VALUE (*builtin_func4_ptr)(VALUE, VALUE, VALUE, VALUE);

typedef struct builtin_func {
    builtin_func_ptr func;
    const char *name;
    const char *func_name;
    bool have_src;
} builtin_func_t;

#ifndef DEBUG_EVAL
#define DEBUG_EVAL 0
#endif

typedef struct CTX_struct {
    VALUE *env;
    VALUE *fp;
    unsigned int func_set_cnt;
    struct function_entry *func_set;
    state_serial_t serial;

#if DEBUG_EVAL
    unsigned int frame_cnt;
    unsigned int rec_cnt;
#endif
} CTX;

#define LIKELY(expr) __builtin_expect((expr), 1)
#define UNLIKELY(expr) __builtin_expect((expr), 0)

// Heap allocators (defined in node.c).
VALUE baruby_ary_new(uint32_t capa);
VALUE baruby_ary_new_from(const VALUE *items, uint32_t n);
void  baruby_ary_push(VALUE ary, VALUE v);
VALUE baruby_ary_plus(VALUE a, VALUE b);
VALUE baruby_str_new(const char *bytes, uint32_t len);
VALUE baruby_str_new_cstr(const char *cstr);
VALUE baruby_str_concat(VALUE a, VALUE b);

// Value equality (Ruby `==`).  Same bits → true (catches int / nil / ptr
// identity).  Otherwise: same type → recursive byte / element compare;
// different types → false.  Mixed (int vs ptr) → false.
bool  baruby_value_eq(VALUE a, VALUE b);

// Strict-3-way string compare: <0 / 0 / >0, like memcmp + length tiebreak.
int   baruby_str_cmp(VALUE a, VALUE b);

// Stringification (Ruby `to_s`).  Heap-alloc'd in all cases except when
// `v` is already a String (returns self).
VALUE baruby_to_s(VALUE v);

void  baruby_print_value(FILE *fp, VALUE v);

#endif
