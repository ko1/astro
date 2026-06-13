/*
 * koruby_precise v2 — context.h
 *
 * Value representation, CTX, RESULT, and the precise-GC contract macros.
 * Design source: docs/v2_design.md (slots ABI) + docs/v2_spec.md.
 *
 * Core ABI invariants (v2_design §1):
 *   1. The `slots` cursor passed to every function is the FIRST FREE slot.
 *      Live values sit below it; above is the callee's scratch.
 *   2. `c->slots_top` is written ONLY by korb_alloc (the publish point).
 *      GC scans [c->slots, c->slots_top).
 *   3. No raw VALUE is held across a GC point — values that must survive
 *      live in a slot and are accessed through VALUE_REF.
 */

#ifndef KORUBY_CONTEXT_H
#define KORUBY_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#ifndef ASTRO_DEBUG
#  define ASTRO_DEBUG 0
#endif
#include "astro_debug.h"
#include "precise_gc/gc_types.h"

#define LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)

/* -----------------------------------------------------------------------------
 * Tagged VALUE.
 *
 *   LSB == 1                          fixnum (signed int63)
 *   raw == 0                          nil  singleton
 *   raw == 2                          false singleton
 *   raw == 4                          true  singleton
 *   (raw & 7) == 6                    Symbol immediate: (sym_id << 3) | 6
 *   (raw & 7) == 0, raw != 0          heap object pointer (8-byte aligned)
 *
 * nil == 0 deliberately: freshly zero-filled slots / payloads read as nil,
 * so the mmap'd zero pages of the slots buffer and AROH zero-init are
 * already in a valid state.  The two falsy values are 0 (nil) and 2
 * (false), giving the one-instruction truthiness test below.
 *
 * The GC edge filter (ARO_GC_VISIT_EDGE in precise_gc/gc.h) skips values
 * with (v & 7) != 0 or v == 0 — exactly the non-heap encodings above.
 * --------------------------------------------------------------------------- */
typedef intptr_t VALUE;
#define ARO_GC_VALUE_TYPEDEFED 1

#define KORB_NIL       ((VALUE)0)
#define KORB_FALSE     ((VALUE)2)
#define KORB_TRUE      ((VALUE)4)

#define FIXNUM_P(v)    (((uintptr_t)(v) & 1u) != 0)
#define LONG2FIX(i)    ((VALUE)(((uintptr_t)(intptr_t)(i) << 1) | 1u))
#define FIX2LONG(v)    (((intptr_t)(v)) >> 1)
#define FIXNUM_MAX     (INTPTR_MAX >> 1)
#define FIXNUM_MIN     (INTPTR_MIN >> 1)
#define FIXABLE(i)     ((i) >= FIXNUM_MIN && (i) <= FIXNUM_MAX)

#define SYMBOL_P(v)    (((uintptr_t)(v) & 7u) == 6u)
#define ID2SYM(id)     ((VALUE)(((uintptr_t)(id) << 3) | 6u))
#define SYM2ID(v)      ((uint32_t)((uintptr_t)(v) >> 3))

/* Falsy = nil (0) or false (2): one and + one compare. */
#define KORB_TRUTHY(v)   (((uintptr_t)(v) | 2u) != 2u)

/* Heap pointer test — also the GC contract macro (singletons / fixnums /
 * symbols have non-zero low bits or are 0, so they never look like heap
 * pointers). */
#define AROH_IS_GC_OBJECT(v)  ((v) != 0 && ((uintptr_t)(v) & 7u) == 0)

/* -----------------------------------------------------------------------------
 * RESULT — 2-register return carrying VALUE + control state (v2_design §4.6).
 * No c->state / c->errinfo exist; a raised exception object travels in
 * RESULT.value.  The UNWRAP / CHECK propagation path contains no GC point,
 * so register transport is safe.
 * --------------------------------------------------------------------------- */
enum korb_state {
    KORB_NORMAL = 0,
    KORB_RETURN = 1,    /* `return` — caught at the method-call boundary */
    KORB_RAISE  = 2,    /* exception in .value — unwinds to a handler / main */
};

typedef struct {
    VALUE     value;
    uintptr_t state;    /* enum korb_state */
} RESULT;

#define RESULT_OK(v)      ((RESULT){ (v), KORB_NORMAL })
#define RESULT_RETURN_(v) ((RESULT){ (v), KORB_RETURN })
#define RESULT_RAISE_(v)  ((RESULT){ (v), KORB_RAISE })

/* UNWRAP(expr): take the VALUE out of a RESULT expression, early-returning
 * the RESULT from the *caller* when non-NORMAL.  CHECK(expr): same but the
 * value is discarded.  Hand-spelling `RESULT chk = ...; if (...) return chk;`
 * is forbidden (v2_design §4.6). */
#define UNWRAP(r) ({ RESULT _r = (r); if (UNLIKELY(_r.state != KORB_NORMAL)) return _r; _r.value; })
#define CHECK(r) do { RESULT _r = (r); if (UNLIKELY(_r.state != KORB_NORMAL)) return _r; } while (0)

/* -----------------------------------------------------------------------------
 * VALUE_REF / VALUE_SLICE — rooted references (v2_design §5).
 * Generated from the value type name by the framework template.
 * --------------------------------------------------------------------------- */
#define ASTRO_REF_VALUE VALUE
#include "astro_ref_template.h"

/* SLOTS_PUSH(slots, v): store v at the cursor, advance the (local) cursor
 * by one, return a VALUE_REF to the now-rooted cell.  The cursor is a
 * value-passed local, so returning from the function auto-pops. */
#define SLOTS_PUSH(slots, v) (*(slots) = (v), VALUE_REF_AT((slots)++))

/* -----------------------------------------------------------------------------
 * Heap objects.  All GC-managed objects embed AroObjectHeader first;
 * head.flags low bits carry the sample type tag.
 * --------------------------------------------------------------------------- */
enum korb_obj_type {
    KORB_OBJ_STRING    = 1,
    KORB_OBJ_EXCEPTION = 2,
};
#define KORB_OBJ_TYPE_MASK 0x07u

typedef struct KorbString {
    AroObjectHeader head;
    uint32_t len;            /* byte length, not counting the NUL */
    char     bytes[];        /* len + 1 bytes, NUL-terminated; inline so the
                              * object is a single allocation (moving GC
                              * copies it whole; no interior pointers kept) */
} KorbString;

typedef struct KorbException {
    AroObjectHeader head;
    uint32_t etype;          /* enum korb_etype (korb_runtime.c) */
    uint32_t line;           /* current unwind line (raise site, then each
                              * call site as the unwind passes it) */
    VALUE ARO_GC_EDGE msg;   /* KorbString | nil */
} KorbException;

#define KORB_OBJ_TYPE(v)   (((AroObjectHeader *)(uintptr_t)(v))->flags & KORB_OBJ_TYPE_MASK)
#define KORB_STRING_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_STRING)
#define KORB_EXC_P(v)      (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_EXCEPTION)
#define VAL2STR(v)         ((KorbString *)(uintptr_t)(v))
#define VAL2EXC(v)         ((KorbException *)(uintptr_t)(v))

/* -----------------------------------------------------------------------------
 * VM — interned symbols, the method table, and the unwind backtrace buffer.
 * Lives behind CTX (one VM per CTX in M0); no globals (strict rule).
 * All VM-internal storage is libc-malloc'd runtime infrastructure with no
 * GC-visible VALUE edges (method bodies are immortal NODEs, names are
 * interned C strings).
 * --------------------------------------------------------------------------- */
struct Node;

enum korb_method_kind {
    KORB_METHOD_ISEQ = 0,
    KORB_METHOD_BUILTIN = 1,
};

struct CTX_struct;
typedef struct CTX_struct CTX;

/* Builtin C method: receives the staged argument cells as a rooted slice
 * (the cells live in the caller's frame area, below `slots`). */
typedef RESULT (*korb_builtin_fn)(CTX *c, VALUE *slots, VALUE_SLICE args);

struct korb_method {
    uint32_t mid;            /* interned name */
    uint8_t  kind;           /* enum korb_method_kind */
    int32_t  params_cnt;     /* -1 = variadic (builtins only) */
    uint32_t locals_cnt;     /* ISEQ: frame size (params first) */
    struct Node *body;       /* ISEQ */
    korb_builtin_fn bfn;     /* BUILTIN */
};

struct korb_bt_entry {       /* one unwind frame for the uncaught-exception report */
    uint32_t line;
    const char *name;        /* method name, or "<main>" */
};

struct korb_vm {
    /* symbol intern table: id -> name (libc strings, never freed) */
    const char **sym_names;
    uint32_t sym_cnt, sym_capa;

    /* global method table (M0: no classes) */
    struct korb_method *methods;
    uint32_t method_cnt, method_capa;
    uint64_t method_serial;  /* bumped by def — invalidates call caches */

    /* in-flight raise backtrace (filled during unwind; libc only — the
     * unwind path must not allocate GC memory while the exception rides
     * in RESULT.value) */
    struct korb_bt_entry *bt;
    uint32_t bt_cnt, bt_capa;

    const char *script_name; /* for error messages */
};

/* -----------------------------------------------------------------------------
 * CTX
 * --------------------------------------------------------------------------- */
struct ASTroGC;

struct CTX_struct {
    VALUE *slots;            /* base of the slot stack (fixed mmap — never moves) */
    VALUE *slots_top;        /* GC scan upper bound; written ONLY by korb_alloc */
    VALUE *slots_limit;      /* overflow check bound (frame push compares) */
    VALUE *slots_high_water; /* highest published top — see AROH_VISIT_ROOTS */
    const char *cstack_limit;/* native C stack floor + margin — the AST walker
                              * recurses on the C stack, so frame push checks
                              * this too (CRuby-style machine stack check) */
    struct ASTroGC *astro_gc;
    struct korb_vm *vm;
};

#define ARO_GC_INSTANCE(c)  ((c)->astro_gc)

/* -----------------------------------------------------------------------------
 * GC contract macros (consumed by runtime/precise_gc backends).
 * --------------------------------------------------------------------------- */

/* Root scan = the published slot range, plus stale-residue hygiene:
 * any slot in (top, high_water) may hold a VALUE that was live before an
 * earlier pop and has NOT been fixed up by intervening GCs (it was above
 * top then).  If the mutator later claims such a slot and publishes past
 * it without storing first, the stale bits would be scanned.  Zeroing
 * [top, high_water) at every collection makes unwritten claimed slots
 * read as nil (= 0, skipped by the edge filter). */
#define AROH_VISIT_ROOTS(c, ctx, edge_visit) do {                            \
    VALUE *_aro_top = (c)->slots_top;                                        \
    if ((c)->slots_high_water == NULL || _aro_top > (c)->slots_high_water) { \
        (c)->slots_high_water = _aro_top;                                    \
    }                                                                        \
    else {                                                                   \
        for (VALUE *_p = _aro_top; _p < (c)->slots_high_water; _p++)         \
            *_p = 0;                                                         \
    }                                                                        \
    for (VALUE *_p = (c)->slots; _p < _aro_top; _p++) {                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, _p);                            \
    }                                                                        \
} while (0)

#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {         \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);                      \
    switch (_h->flags & KORB_OBJ_TYPE_MASK) {                                \
      case KORB_OBJ_STRING:                                                  \
        /* inline bytes — no edges */                                        \
        (void)(payload_size);                                                \
        break;                                                               \
      case KORB_OBJ_EXCEPTION: {                                             \
        KorbException *_e = (KorbException *)(payload);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->msg);                      \
        break;                                                               \
      }                                                                      \
      default:                                                               \
        ASTRO_ASSERT(0 && "SCAN_EDGES: unknown head.flags type");            \
    }                                                                        \
} while (0)

/* Scan-safe init for backends that delegate payload init to the sample
 * (gc_copy zero-inits internally; gc_mark_freelist calls this). */
#define AROH_INIT_PAYLOAD(payload, size_bytes)                               \
    memset((char *)(payload) + sizeof(AroObjectHeader), 0,                   \
           (size_bytes) - sizeof(AroObjectHeader))
#define AROH_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)

/* No sample-managed external resources in M0. */
#define AROH_FINALIZE(payload) ((void)(payload))

/* -----------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */
struct koruby_option {
    bool plain;          /* --plain: ignore the code store */
    bool aot_compile;    /* --aot-compile: run + bake at exit */
    bool pg_compile;     /* --pg-compile: M0 = same bake as AOT */
    bool clear_store;    /* --ccs */
    bool dump_ast;       /* --dump-ast */
    bool quiet;
    bool verbose;

    /* referenced by framework-generated ALLOC_ helpers */
    bool record_all;
};

extern struct koruby_option OPTION;

#endif /* KORUBY_CONTEXT_H */
