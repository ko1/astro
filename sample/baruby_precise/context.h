#ifndef BARUBY_CONTEXT_H
#define BARUBY_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// baruby_precise is a testbed for the precise GC framework, so ASTRO_DEBUG
// defaults on.  Override with -DASTRO_DEBUG=0 for a release-shape build.
#ifndef ASTRO_DEBUG
#  define ASTRO_DEBUG 1
#endif
#include "astro_debug.h"

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
// 8-byte aligned heap pointer (baruby_precise: semispace allocations are
// always 8-byte aligned payloads).  Singletons (true=2, nil=4) have non-zero
// low bits so they're auto-excluded.  False=0 is excluded explicitly.
// Strict 8-byte check filters out garbage values that happen to have LSB=0
// but aren't actual heap pointers (= GC mis-trace bugs).
#define IS_PTR(v)     ((v) != VAL_FALSE \
                       && ((uintptr_t)(v) & (uintptr_t)7) == 0)

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
    VALUE *env;                  // bottom of VALUE stack (= start of mark range)
    VALUE *fp;                   // current function frame base (unused now; sp threading replaces)
    VALUE *sp;                   // current scratch top — updated by alloc API before mark
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

// Heap allocators (defined in node.c).  All take `sp_top` as the last
// argument: the caller's current scratch top, used to update c->sp before
// any potential GC poll.  Helpers that don't allocate (compare, etc.) omit it.
VALUE baruby_ary_new(uint32_t capa, VALUE *sp_top);
VALUE baruby_ary_new_from(const VALUE *items, uint32_t n, VALUE *sp_top);
// Both av_ref and x_ref are pointers to caller's sp slots; we re-read
// through them after any internal alloc so post-move addresses are
// picked up.
void  baruby_ary_push(VALUE *av_ref, VALUE *x_ref, VALUE *sp_top);
// av/bv are pointers to caller sp slots; reloaded after alloc.
VALUE baruby_ary_plus(VALUE *av_ref, VALUE *bv_ref, VALUE *sp_top);
VALUE baruby_str_new(const char *bytes, uint32_t len, VALUE *sp_top);
VALUE baruby_str_new_cstr(const char *cstr, VALUE *sp_top);
// Slice from a heap source: src_ref is a caller sp slot, re-deref'd post-GC.
VALUE baruby_str_slice(VALUE *src_ref, uint32_t offset, uint32_t len, VALUE *sp_top);
VALUE baruby_str_concat(VALUE *av_ref, VALUE *bv_ref, VALUE *sp_top);

// Value equality (Ruby `==`).  Same bits → true (catches int / nil / ptr
// identity).  Otherwise: same type → recursive byte / element compare;
// different types → false.  Mixed (int vs ptr) → false.
bool  baruby_value_eq(VALUE a, VALUE b);

// Strict-3-way string compare: <0 / 0 / >0, like memcmp + length tiebreak.
int   baruby_str_cmp(VALUE a, VALUE b);

// `s * n` / `a * n` — Ruby-style repeat into a fresh object.  Negative
// `n` returns an empty result (Ruby raises but we just clamp).
VALUE baruby_str_repeat(VALUE *sv_ref, intptr_t n, VALUE *sp_top);
VALUE baruby_ary_repeat(VALUE *av_ref, intptr_t n, VALUE *sp_top);

// In-place append (`s << t`) — grows `dst`'s buffer and returns `dst`.
// dst_ref / src_ref are caller sp slots reloaded after realloc.
void  baruby_str_append(VALUE *dst_ref, VALUE *src_ref, VALUE *sp_top);

// Stringification (Ruby `to_s`).  Heap-alloc'd in all cases except when
// `v` is already a String (returns self).
VALUE baruby_to_s(VALUE v, VALUE *sp_top);

void  baruby_print_value(FILE *fp, VALUE v);

#endif
