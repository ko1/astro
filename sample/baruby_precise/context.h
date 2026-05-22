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
// Pull ASTroObjectHeader — sample's heap structs embed `head` as first
// field.  gc_types.h is the types-only slice of gc.h (no CTX-dependent
// static inlines) so it can be included before CTX_struct is defined.
#include "precise_gc/gc_types.h"

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

// Type tag — stored in head.flags low 3 bits.  Distinct OBJ_BYTE_DATA
// tag for raw byte buffers (= BaString.bytes wrapped in BaByteData)
// so the framework's SCAN_EDGES can dispatch uniformly via head.flags
// (no separate framework "category" needed).
enum obj_type {
    OBJ_ARRAY       = 1,
    OBJ_STRING      = 2,
    OBJ_VALUE_ARRAY = 3,    // raw VALUE[] payload (= BaArrayItems)
    OBJ_BYTE_DATA   = 4,    // raw char[] payload (= BaByteData)
};
#define OBJ_TYPE_MASK   0x07u   /* low 3 bits of head.flags */
#define OBJ_FLAG_SSO    0x08u   /* bit 3: SSO inline (BaString only) */

// Heap object holding a VALUE[] payload.  Allocated separately from the
// owning BaArray so realloc can grow it without moving the BaArray itself.
// The ASTroObjectHeader.flags is set to OBJ_VALUE_ARRAY so SCAN_EDGES
// scans each data slot.
typedef struct BaArrayItems {
    ASTroObjectHeader head;
    VALUE data[];           // flex array; capa is tracked in the owning BaArray
} BaArrayItems;

// Heap object holding raw bytes (= BaString.bytes after iter 75 Step C).
// head.flags = OBJ_BYTE_DATA, SCAN_EDGES is a no-op for these.
typedef struct BaByteData {
    ASTroObjectHeader head;
    char data[];            // flex array; size is in head.gc_size
} BaByteData;

typedef struct BaArray {
    ASTroObjectHeader head;
    uint32_t len;
    uint32_t capa;
    BaArrayItems *items;
} BaArray;

// iter 53: SSO (small-string optimization).  Strings with `len <=
// BSTR_SSO_MAX` (7) are stored inline in the `small[8]` arm of the
// union; longer strings allocate a separate BaByteData payload.
// The discriminator is OBJ_FLAG_SSO bit in head.flags.  Layout total
// stays 24 B — the union overlays the 8-byte pointer slot.
//
// Read sites must go through `BSTR_BYTES(s)` (or `bstr_bytes(s)`) —
// reading `s->bytes->data` directly is UB for SSO strings (the bytes
// alias to inline char data interpreted as a pointer).  The GC mark /
// scan paths must skip the bytes pointer when `BSTR_IS_SSO(s)`.
#define BSTR_SSO_MAX   7u    /* 7 chars + NUL fits in the 8-byte union */

typedef struct BaString {
    ASTroObjectHeader head;
    uint32_t len;            // byte length (not counting NUL)
    uint32_t capa;           // SSO: sizeof(small).  heap: len + 1.
    union {
        BaByteData *bytes;   // heap: pointer to NUL-terminated payload (separate alloc)
        char        small[8];// SSO: inline chars, NUL at small[len]
    };
} BaString;

#define BSTR_IS_SSO(s)  (((s)->head.flags & OBJ_FLAG_SSO) != 0u)

static inline const char *
bstr_bytes(const BaString * const s)
{
    return BSTR_IS_SSO(s) ? s->small : s->bytes->data;
}
static inline char *
bstr_bytes_mut(BaString * const s)
{
    return BSTR_IS_SSO(s) ? s->small : s->bytes->data;
}
#define BSTR_BYTES(s)  bstr_bytes(s)

#define OBJ_TYPE(v)   (((ASTroObjectHeader *)(v))->flags & OBJ_TYPE_MASK)
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

/* iter 62: process-scope GC instance への pointer。 sample 全体で唯一の
 * GC instance を CTX 経由でアクセスする (contract: ASTRO_GC_INSTANCE(c)
 * = (c)->astro_gc)。 multi-instance 拡張なら CTX 1 つに 1 instance を
 * bind するだけで対応可能。 struct ASTroGC の中身は各 backend (gc_*.c)
 * が定義 — sample 視点では opaque pointer。
 *
 * stats / stress / timer は per-instance なので ASTroGC 内に保持する
 * (= 「共通ヘッダ」 pattern: 各 backend の `struct ASTroGC` の先頭に
 * `AroGcCommonState common` を置く約束。 gc.h 側で
 * `(AroGcCommonState *)c->astro_gc` で安全に取り出せる)。 */
struct ASTroGC;

typedef struct CTX_struct {
    VALUE *env;                  // bottom of VALUE stack (= start of mark range)
    VALUE *sp;                   // current scratch top — updated by alloc API before mark
    struct ASTroGC *astro_gc;    // process-scope GC instance (backend が中身定義)
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

// ============================================================================
// GC contract macros (iter 62) — sample が framework に提供する「contract」。
//
// 各 backend (gc_*.c) はこれらを compile-time に展開して使う。
// 詳細は docs/gc_design.md §2 を参照。
// ============================================================================

/* Instance accessor: CTX → ASTroGC *.  各 backend は自分の ASTroGC
 * struct を typedef して、 process 起動時 1 つ (or 複数) allocate +
 * `(c)->astro_gc` に bind する。 framework 関数は引数 CTX 経由で
 * ASTroGC を取り出して操作する (= module-static なし)。 */
#define ASTRO_GC_INSTANCE(c)  ((c)->astro_gc)

/* Object shape: outgoing reference を slot pointer 列挙。 visit callback は
 * `void (void *ctx, void **slot)`、 同じ macro で mark / forward / update 全
 * phase 共有。 `ctx` は backend が好きに使える explicit closure
 * (典型的には `ASTroGC *gc`)。 GCHeader は forward 宣言 (各 backend が
 * typedef する)。 module-static を global として使わないために ctx 経由
 * を必須にしている。 */
/* Sample が提供する shape macro。 framework が CAT_OBJECT category の
 * payload に対してのみ呼び出す (= VALS / BYTE / FREE は framework が
 * 直接 dispatch、 sample に届かない)。 sample 側は自分の object header
 * (BaArray / BaString) の type tag を見て edge を visit。
 *
 * Args:
 *   payload    : void * — GCHeader 直後の object payload pointer
 *   payload_size : size_t — payload バイト数 (= 通常 ObjectHeader だけ
 *                  なので使わないが、 framework が一律で渡す)
 *   ctx, edge_visit : 各 slot を visit する callback (= 通常 ASTroGC *)
 */
#define ASTRO_GC_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {  \
    ASTroObjectHeader *_h = (ASTroObjectHeader *)(payload);                 \
    switch (_h->flags & OBJ_TYPE_MASK) {                                    \
      case OBJ_ARRAY: {                                                     \
          BaArray *_a = (BaArray *)(payload);                               \
          edge_visit((ctx), (void **)&_a->items);                           \
          (void)(payload_size);                                              \
          break;                                                             \
      }                                                                      \
      case OBJ_STRING: {                                                    \
          BaString *_s = (BaString *)(payload);                             \
          if (!BSTR_IS_SSO(_s)) edge_visit((ctx), (void **)&_s->bytes);     \
          break;                                                             \
      }                                                                      \
      case OBJ_VALUE_ARRAY: {                                               \
          BaArrayItems *_ai = (BaArrayItems *)(payload);                    \
          size_t _n = ((payload_size) - sizeof(BaArrayItems)) / sizeof(VALUE); \
          for (size_t _i = 0; _i < _n; _i++)                                \
              edge_visit((ctx), (void **)&_ai->data[_i]);                   \
          break;                                                             \
      }                                                                      \
      case OBJ_BYTE_DATA:                                                   \
          /* raw bytes — no edges to scan */                                \
          break;                                                             \
      default:                                                               \
          ASTRO_ASSERT(0 && "SCAN_EDGES: unknown head.flags type");         \
    }                                                                        \
} while (0)

/* Scan-safe init: payload slots may be scanned right after alloc (before
 * caller writes anything).  baruby's VAL_FALSE == 0 so zero-fill is GC-safe. */
#define ASTRO_GC_INIT_PAYLOAD(payload, size_bytes) \
    memset((payload), 0, (size_bytes))

/* Byte payload init: GC never scans these so skip memset.  Caller fills
 * the bytes before any further alloc. */
#define ASTRO_GC_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)

/* Header layout accessors (framework default).  Each backend's GCHeader
 * has `size` (uint32_t) at the canonical offset; `fwd` is moving-only.  */
#define ASTRO_GC_HEADER_SIZE(h)         ((h)->size)
#define ASTRO_GC_HEADER_SET_SIZE(h, s)  ((h)->size = (uint32_t)(s))
#define ASTRO_GC_HEADER_GET_FWD(h)      ((h)->fwd)
#define ASTRO_GC_HEADER_SET_FWD(h, p)   ((h)->fwd = (p))

// Heap allocators (defined in node.c).  All take `sp` as the last
// argument: the caller's current scratch top.  Each helper sets
// c->sp = sp (or sp+N) internally before alloc.
VALUE baruby_ary_new(CTX *c, uint32_t capa);
VALUE baruby_ary_new_from(CTX *c, const VALUE *items, uint32_t n);
// Both av_ref and x_ref are pointers to caller's sp slots; we re-read
// through them after any internal alloc so post-move addresses are
// picked up.  fast-path (= no realloc) is inlined in node.h (after
// gc.h is visible so aro_gc_wb is in scope); the realloc grow path
// stays in node.c.  See iter 73 sieve perf note.
void  baruby_ary_push_grow(CTX *c, VALUE *av_ref, VALUE *x_ref);
// av/bv are pointers to caller sp slots; reloaded after alloc.
VALUE baruby_ary_plus(CTX *c, VALUE *av_ref, VALUE *bv_ref);
VALUE baruby_str_new(CTX *c, const char *bytes, uint32_t len);
VALUE baruby_str_new_cstr(CTX *c, const char *cstr);
// Slice from a heap source: src_ref is a caller sp slot, re-deref'd post-GC.
VALUE baruby_str_slice(CTX *c, VALUE *src_ref, uint32_t offset, uint32_t len);
VALUE baruby_str_concat(CTX *c, VALUE *av_ref, VALUE *bv_ref);

// Value equality (Ruby `==`).  Same bits → true (catches int / nil / ptr
// identity).  Otherwise: same type → recursive byte / element compare;
// different types → false.  Mixed (int vs ptr) → false.
bool  baruby_value_eq(VALUE a, VALUE b);

// Strict-3-way string compare: <0 / 0 / >0, like memcmp + length tiebreak.
int   baruby_str_cmp(VALUE a, VALUE b);

// `s * n` / `a * n` — Ruby-style repeat into a fresh object.  Negative
// `n` returns an empty result (Ruby raises but we just clamp).
VALUE baruby_str_repeat(CTX *c, VALUE *sv_ref, intptr_t n);
VALUE baruby_ary_repeat(CTX *c, VALUE *av_ref, intptr_t n);

// In-place append (`s << t`) — grows `dst`'s buffer and returns `dst`.
// dst_ref / src_ref are caller sp slots reloaded after realloc.
void  baruby_str_append(CTX *c, VALUE *dst_ref, VALUE *src_ref);

// Stringification (Ruby `to_s`).  Heap-alloc'd in all cases except when
// `v` is already a String (returns self).
VALUE baruby_to_s(CTX *c, VALUE v);

void  baruby_print_value(FILE *fp, VALUE v);

#endif
