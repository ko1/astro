#ifndef ASOM_CONTEXT_H
#define ASOM_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include <gmp.h>

// All allocations route through Boehm GC. The libc-shape macros
// preserve the existing call sites verbatim, so asom_runtime.c /
// asom_primitives.c / asom_parse.c / node.c continue to read as
// `calloc(1, sizeof(*x))` etc. Boehm's GC_MALLOC always zero-clears,
// so this is a behaviour-preserving redirect; free is a no-op
// (the collector reclaims unreachable blocks).
//
// Define order: this block sits *after* the system headers above, so
// libc / libgc internal callers above this point keep their direct
// libc symbols. From this point on, every `malloc` / `calloc` /
// `realloc` / `strdup` / `free` in asom code becomes a GC equivalent.
#define malloc(n)     GC_MALLOC(n)
#define calloc(n, s)  GC_MALLOC((size_t)(n) * (size_t)(s))
#define realloc(p, n) GC_REALLOC((p), (n))
#define strdup(s)     GC_STRDUP(s)
#define free(p)       ((void)(p))

#define ASOM_DEBUG 1
#if ASOM_DEBUG
#define ASOM_ASSERT(expr) assert(expr)
#else
#define ASOM_ASSERT(expr) ((void)0)
#endif

#define LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

// -----------------------------------------------------------------------------
// Tagged value representation.
//
//   ...x1   -> SmallInteger (top 63 bits, sign-extended on shift)
//   ...10   -> Flonum (immediate Double, CRuby-style biased-exponent encoding)
//   ...00   -> object pointer (8-byte aligned -> low 3 bits zero)
//
// Flonum encoding is a 1:1 port of CRuby's: doubles whose biased exponent
// is in [897, 1150] (i.e., abs values roughly in [2^-126, 2^127]) round-
// trip losslessly into 62 bits via subtracting 0x3C<<60 from their bit
// pattern and OR-ing in 0x2; values outside the range fall back to a
// boxed `struct asom_double` on the bump arena.
// -----------------------------------------------------------------------------

typedef intptr_t VALUE;

#define ASOM_TAG_INT   0x1
#define ASOM_TAG_FLO   0x2
#define ASOM_TAG_MASK  0x3

#define ASOM_IS_INT(v)  (((VALUE)(v)) & ASOM_TAG_INT)
#define ASOM_IS_FLO(v)  ((((VALUE)(v)) & ASOM_TAG_MASK) == ASOM_TAG_FLO)
#define ASOM_IS_OBJ(v)  ((((VALUE)(v)) & ASOM_TAG_MASK) == 0)
#define ASOM_INT2VAL(i) (((VALUE)(intptr_t)(i) << 1) | ASOM_TAG_INT)
#define ASOM_VAL2INT(v) ((intptr_t)(v) >> 1)
#define ASOM_OBJ2VAL(p) ((VALUE)(p))
#define ASOM_VAL2OBJ(v) ((struct asom_object *)(v))

// 62-bit signed range usable for ASOM_INT2VAL — anything outside must
// be heap-boxed as asom_bignum. Defined here so node.def (specialized
// arith nodes) and asom_primitives.c (int_plus etc.) share one bound.
#define ASOM_INT_MIN  (-(intptr_t)((intptr_t)1 << 61))
#define ASOM_INT_MAX  ((intptr_t)((intptr_t)1 << 61) - 1)

// Flonum encode/decode (CRuby-style biased-exponent shift).
// `asom_flo2val_try` returns 0 on out-of-range (caller falls back to heap);
// callers that hold a known-representable double can use the unconditional
// macros. `0.0` uses a special encoding (only positive zero — negative zero
// loses sign through this path, matching CRuby).
//
// Two range checks:
//   1. exponent in [897, 1150] — `((bits>>60) & 7) ∈ {3, 4}`
//   2. mantissa low bit == 0   — otherwise the OR with the flonum tag
//      (low 2 bits = 0b10) would collide with SmallInteger's low bit = 1.
// Doubles that fail either check fall back to heap-boxed asom_double.
// This is lossless: the decoder recovers the exact original bit pattern.
// 0x3C in the top byte (= biased exponent contribution that recentres
// the encoded payload around 0). The earlier draft wrote
// `(uint64_t)0x3C << 60` which is in fact 0xC000000000000000 (only the
// low 4 bits of 0x3C survive after a 60-bit shift), and that collided
// the -2.0 encoding with the 0.0 special tag.
#define ASOM_FLONUM_BIAS  ((uint64_t)0x3C00000000000000ULL)

static inline VALUE
asom_flo2val_try(double d)
{
    // memcpy-based bit reinterpret keeps strict-aliasing happy.
    uint64_t bits;
    __builtin_memcpy(&bits, &d, sizeof(bits));
    if (bits == 0) return ((VALUE)1 << 1) | ASOM_TAG_FLO;
    int top = (int)((bits >> 60) & 0x7);
    if ((top - 3) & ~0x01) return 0;
    // Bit-exact encoding requires both low mantissa bits to be 0,
    // since the encoded VALUE's low 2 bits hold the flonum tag (10)
    // and the decoder clears both. Doubles whose low bits are not
    // 00 fall back to heap-boxed asom_double — preserves precision
    // for Mandelbrot's escape-count and DoubleTest's exact-value
    // assertions.
    if (bits & 0x3) return 0;
    return (VALUE)((bits - ASOM_FLONUM_BIAS) | ASOM_TAG_FLO);
}

static inline double
asom_val2flo(VALUE v)
{
    if (v == (((VALUE)1 << 1) | ASOM_TAG_FLO)) return 0.0;
    uint64_t bits = ((uint64_t)v & ~(uint64_t)0x3) + ASOM_FLONUM_BIAS;
    double d;
    __builtin_memcpy(&d, &bits, sizeof(d));
    return d;
}

// Forward declarations.
struct asom_object;
struct asom_class;
struct asom_method;
struct asom_block;
struct asom_string;
struct Node;
typedef uint64_t state_serial_t;

// -----------------------------------------------------------------------------
// Object headers.
// -----------------------------------------------------------------------------

struct asom_object {
    struct asom_class *klass;
    // followed by inline fields[] (count tracked on the class)
};

struct asom_string {
    struct asom_object hdr;
    size_t len;
    const char *bytes; // NUL-terminated, static or arena-owned
};

struct asom_array {
    struct asom_object hdr;
    uint32_t len;
    VALUE *data;        // initialised to nil
};

struct asom_double {
    struct asom_object hdr;
    double value;
};

// Bignum: SmallInteger overflow promotes to a heap-boxed mpz_t. The
// 62-bit tagged Integer covers most arithmetic; this wrapper takes
// over for anything outside, matching SOM's transparent
// SmallInteger -> LargeInteger hierarchy.
struct asom_bignum {
    struct asom_object hdr;
    mpz_t value;
};

// Tag identifying which built-in primitive a method is, so call sites can
// rewrite their AST node to a type-specialized variant on first cache fill.
// Generic primitives (and all AST methods) carry ASOM_PRIM_GENERIC.
enum asom_prim_kind {
    ASOM_PRIM_GENERIC = 0,
    ASOM_PRIM_INT_PLUS,
    ASOM_PRIM_INT_MINUS,
    ASOM_PRIM_INT_TIMES,
    ASOM_PRIM_INT_LT,
    ASOM_PRIM_INT_GT,
    ASOM_PRIM_INT_LE,
    ASOM_PRIM_INT_GE,
    ASOM_PRIM_INT_EQ,
    ASOM_PRIM_DBL_PLUS,
    ASOM_PRIM_DBL_MINUS,
    ASOM_PRIM_DBL_TIMES,
    ASOM_PRIM_DBL_LT,
    ASOM_PRIM_DBL_GT,
    ASOM_PRIM_DBL_LE,
    ASOM_PRIM_DBL_GE,
    ASOM_PRIM_DBL_EQ,
};

struct asom_method {
    const char *selector;
    uint32_t num_params;          // not counting receiver
    uint32_t num_locals;
    struct Node *body;            // method body AST (NULL = primitive)
    void *primitive;              // C function pointer if primitive
    struct asom_class *holder;
    enum asom_prim_kind prim_kind; // tag for type-feedback specialization
    bool no_nlr;                  // true if body has no node_block descendants;
                                  // asom_invoke skips setjmp on the home frame
                                  // because no NLR can target this method.
};

// Open-hash table for fast lookup by interned selector pointer, plus a
// parallel insertion-order array so reflective enumerations
// (`Class>>methods`) return methods in source-definition order. The
// ordered array doesn't dedupe — if a selector is redefined, both entries
// stay so iteration order is stable.
struct asom_method_table {
    uint32_t cap;
    uint32_t cnt;
    struct asom_method **slots;

    uint32_t order_cnt;
    uint32_t order_cap;
    struct asom_method **ordered;
};

struct asom_class {
    struct asom_object hdr;       // metaclass instance header
    const char *name;             // interned
    struct asom_class *superclass;
    struct asom_class *metaclass;
    uint32_t num_instance_fields;
    const char **field_names;     // length = num_instance_fields
    struct asom_method_table methods;
    // Class-side instance fields (the metaclass's slots, lifted onto the
    // class object itself). NULL when the class has no class-side state.
    uint32_t num_class_side_fields;
    VALUE *class_side_fields;
    const char **class_side_field_names;
};

struct asom_block {
    struct asom_object hdr;
    struct asom_method *method;   // per-block method (body, num_params, num_locals)
    struct asom_frame *home;      // home (method) frame, target of ^ NLR
    struct asom_frame *lexical_parent; // immediate enclosing frame (for var lookup)
    VALUE captured_self;          // captured self at block-creation time
};

// -----------------------------------------------------------------------------
// Frame / context.
// -----------------------------------------------------------------------------

struct asom_frame {
    VALUE self;
    VALUE *locals;                  // [args..., locals...]
    struct asom_method *method;
    struct asom_frame *parent;      // caller (for tracebacks)
    struct asom_frame *home;        // home method frame (NLR target)
    struct asom_frame *lexical_parent; // outer frame in lexical chain (var lookup)
    int returned;
    // Frame pooling. `captured` is set when a closure created during this
    // frame's body stores us as its lexical_parent — that pins the frame
    // to the heap (the closure may outlive this call). `pool_slots` records
    // the size class so block_invoke knows which free list to push to.
    bool captured;
    uint16_t pool_slots;            // 0 = not poolable (heap-allocated, leak)
};

struct asom_option {
    bool quiet;
    bool dump_ast;
    bool no_compiled_code;        // --plain
    bool record_all;              // legacy flag still referenced by generated allocators
    bool aot_compile_first;       // -c / --aot-compile-first: bake every method (SD_<Horg>) before run
    bool pgc_at_exit;             // -p / --pg-compile: bake hot entries after a clean run
    bool aot_only;                // --aot-only: skip PGC index lookup at cs_load time
    bool compiled_only;           // --compiled-only: warn if interpreter dispatcher is used
    int  pg_threshold;            // --pg-threshold=N (default 100, env ASOM_PG_THRESHOLD)
    bool verbose;                 // --verbose: trace cs_* operations
    const char *classpath;        // ":"-separated list of dirs to search
    const char *entry_class;      // class name to instantiate and run
    const char *code_store_dir;   // --code-store=DIR (default code_store/, env ASOM_CODE_STORE)
    const char *preload;          // --preload=A,B,C: extra classes to load (and bake) before run
};

extern struct asom_option OPTION;

typedef struct asom_ctx {
    // Globals / well-known objects.
    struct asom_class *cls_object;
    struct asom_class *cls_class;
    struct asom_class *cls_metaclass;
    struct asom_class *cls_nil;
    struct asom_class *cls_boolean;
    struct asom_class *cls_true;
    struct asom_class *cls_false;
    struct asom_class *cls_integer;
    struct asom_class *cls_double;
    struct asom_class *cls_bignum;
    struct asom_class *cls_string;
    struct asom_class *cls_symbol;
    struct asom_class *cls_array;
    struct asom_class *cls_block;
    struct asom_class *cls_block1;     // 0-arg blocks
    struct asom_class *cls_block2;     // 1-arg blocks
    struct asom_class *cls_block3;     // 2-arg blocks
    struct asom_class *cls_method;
    struct asom_class *cls_system;
    struct asom_class *cls_random;
    VALUE val_nil;
    VALUE val_true;
    VALUE val_false;

    // Active frame chain.
    struct asom_frame *frame;

    state_serial_t serial;
} CTX;

#endif // ASOM_CONTEXT_H
