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
    KORB_NEXT   = 3,    /* `next` in a block — caught at the yield boundary */
};

typedef struct {
    VALUE     value;
    uintptr_t state;    /* enum korb_state */
} RESULT;

#define RESULT_OK(v)      ((RESULT){ (v), KORB_NORMAL })
#define RESULT_RETURN_(v) ((RESULT){ (v), KORB_RETURN })
#define RESULT_RAISE_(v)  ((RESULT){ (v), KORB_RAISE })
#define RESULT_NEXT_(v)   ((RESULT){ (v), KORB_NEXT })

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
    KORB_OBJ_STRING      = 1,
    KORB_OBJ_EXCEPTION   = 2,
    KORB_OBJ_ARRAY       = 3,
    KORB_OBJ_VALUE_ARRAY = 4,   /* raw VALUE[] payload backing a KorbArray / KorbHash */
    KORB_OBJ_HASH        = 5,
    KORB_OBJ_RANGE       = 6,
    KORB_OBJ_OBJECT      = 7,   /* user object (incl. top-level `main`) */
    KORB_OBJ_CLASS       = 8,   /* user class */
    KORB_OBJ_FLOAT       = 9,   /* heap-boxed double */
    KORB_OBJ_STR_BUF     = 10,  /* raw char[] payload backing a (mutable) KorbString */
    KORB_OBJ_RATIONAL    = 11,  /* exact rational num/den (no GC edges) */
    KORB_OBJ_COMPLEX     = 12,  /* complex re + im*i (re/im are GC edges) */
};
/* `flags` is a dedicated 16-bit sample-owned field; 4 bits leaves room to grow. */
#define KORB_OBJ_TYPE_MASK 0x0Fu

/* growable byte buffer for a KorbString (header never moves on grow). */
typedef struct KorbStrBuf {
    AroObjectHeader head;        /* KORB_OBJ_STR_BUF */
    char data[];                 /* capa + 1 bytes, NUL-terminated; no GC edges */
} KorbStrBuf;

typedef struct KorbString {
    AroObjectHeader head;
    uint32_t len, capa;          /* byte length / buffer capacity (excl. NUL) */
    KorbStrBuf *ARO_GC_EDGE buf; /* separately-alloc'd so <<,[]= can grow in place */
} KorbString;

/* heap-boxed double (no GC edges). */
typedef struct KorbFloat {
    AroObjectHeader head;            /* KORB_OBJ_FLOAT */
    double val;
} KorbFloat;

/* exact rational (always reduced, den > 0; no GC edges). */
typedef struct KorbRational {
    AroObjectHeader head;            /* KORB_OBJ_RATIONAL */
    intptr_t num, den;
} KorbRational;

/* complex number re + im*i; components are arbitrary numerics (GC edges). */
typedef struct KorbComplex {
    AroObjectHeader head;            /* KORB_OBJ_COMPLEX */
    VALUE ARO_GC_EDGE re, im;
} KorbComplex;

typedef struct KorbException {
    AroObjectHeader head;
    uint32_t etype;          /* enum korb_etype (korb_runtime.c) */
    uint32_t line;           /* current unwind line (raise site, then each
                              * call site as the unwind passes it) */
    VALUE ARO_GC_EDGE msg;   /* KorbString | nil */
} KorbException;

/* Array: a header + a separately-allocated growable VALUE[] payload, so push
 * can grow the buffer without moving the KorbArray (moving-GC safe). */
typedef struct KorbArrayItems {
    AroObjectHeader head;            /* KORB_OBJ_VALUE_ARRAY */
    VALUE ARO_GC_EDGE data[];        /* capa slots; live count is in the owner */
} KorbArrayItems;

typedef struct KorbArray {
    AroObjectHeader head;            /* KORB_OBJ_ARRAY */
    uint32_t len, capa;
    KorbArrayItems *ARO_GC_EDGE items;
} KorbArray;

/* Hash: header + a growable VALUE[] payload of [k0,v0,k1,v1,...] pairs, kept in
 * insertion order (Ruby semantics).  Lookup is a linear scan — fine for the
 * small hashes the corpus exercises; a real table can replace the storage
 * later without changing the object shape. */
typedef struct KorbHash {
    AroObjectHeader head;            /* KORB_OBJ_HASH */
    uint32_t len, capa;              /* pair count / pair capacity */
    KorbArrayItems *ARO_GC_EDGE items;       /* 2*capa VALUEs */
    VALUE ARO_GC_EDGE default_val;   /* [] miss result (nil unless set) */
} KorbHash;

/* Range: begin/end + exclusivity.  Endpoints are arbitrary values (GC edges). */
typedef struct KorbRange {
    AroObjectHeader head;            /* KORB_OBJ_RANGE */
    uint32_t exclude_end;
    VALUE ARO_GC_EDGE rbegin;
    VALUE ARO_GC_EDGE rend;
} KorbRange;

/* User object: class pointer (nil for top-level `main`) + lazily-allocated
 * instance-variable store ([name_sym, val, ...] pairs in a VALUE[] payload). */
typedef struct KorbObject {
    AroObjectHeader head;            /* KORB_OBJ_OBJECT */
    uint32_t ivar_len, ivar_capa;
    VALUE ARO_GC_EDGE klass;         /* KorbClass | nil */
    KorbArrayItems *ARO_GC_EDGE ivars;   /* 2*ivar_capa VALUEs, or NULL */
} KorbObject;

/* User class.  The instance-method table is a libc side-array (method bodies
 * are immortal NODEs, names interned ints → no GC edges), so `methods` is a
 * raw non-edge pointer; only `superclass` and `name` are GC-visible. */
struct korb_method;
typedef struct KorbClass {
    AroObjectHeader head;            /* KORB_OBJ_CLASS */
    uint32_t name_sym;               /* class name (interned), 0 = anonymous */
    int32_t  exc_etype;              /* builtin exception class → its etype, else -1 */
    uint32_t method_cnt, method_capa;
    uint8_t  is_module;              /* 1 = module (mixin, not instantiable) */
    struct korb_method *methods;     /* libc side-array (no GC edges) */
    VALUE ARO_GC_EDGE superclass;    /* KorbClass | nil (nil ⇒ Object) */
    VALUE ARO_GC_EDGE included;      /* KorbArray of included modules | nil */
} KorbClass;

#define KORB_OBJ_TYPE(v)   (((AroObjectHeader *)(uintptr_t)(v))->flags & KORB_OBJ_TYPE_MASK)
#define KORB_STRING_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_STRING)
#define KORB_EXC_P(v)      (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_EXCEPTION)
#define KORB_ARRAY_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_ARRAY)
#define KORB_HASH_P(v)     (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_HASH)
#define KORB_RANGE_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_RANGE)
#define KORB_OBJECT_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_OBJECT)
#define KORB_CLASS_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_CLASS)
#define KORB_FLOAT_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_FLOAT)
#define VAL2STR(v)         ((KorbString *)(uintptr_t)(v))
#define VAL2EXC(v)         ((KorbException *)(uintptr_t)(v))
#define VAL2ARY(v)         ((KorbArray *)(uintptr_t)(v))
#define VAL2HASH(v)        ((KorbHash *)(uintptr_t)(v))
#define VAL2RANGE(v)       ((KorbRange *)(uintptr_t)(v))
#define VAL2OBJ(v)         ((KorbObject *)(uintptr_t)(v))
#define VAL2CLASS(v)       ((KorbClass *)(uintptr_t)(v))
#define VAL2FLT(v)         ((KorbFloat *)(uintptr_t)(v))
#define KORB_RATIONAL_P(v) (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_RATIONAL)
#define VAL2RAT(v)         ((KorbRational *)(uintptr_t)(v))
#define KORB_COMPLEX_P(v)  (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_COMPLEX)
#define VAL2CPX(v)         ((KorbComplex *)(uintptr_t)(v))

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
    KORB_METHOD_ATTR_R = 2,   /* attr reader: return @ivar */
    KORB_METHOD_ATTR_W = 3,   /* attr writer: @ivar = arg0 */
};

struct CTX_struct;
typedef struct CTX_struct CTX;

/* Builtin C method: receives the staged argument cells as a rooted slice
 * (the cells live in the caller's frame area, below `slots`). */
typedef RESULT (*korb_builtin_fn)(CTX *c, VALUE *slots, VALUE_SLICE args);

/* Receiver-dispatch core classes + built-in method fn.  `self` is a VALUE_REF
 * (the staged recv slot) so allocating methods re-read it through the ref
 * after GC.  KORB_NCLASS must match the korb_vm.cmethods[] array size. */
enum korb_class {
    KORB_C_INTEGER = 0, KORB_C_STRING, KORB_C_SYMBOL, KORB_C_ARRAY, KORB_C_HASH,
    KORB_C_RANGE, KORB_C_NIL, KORB_C_TRUE, KORB_C_FALSE, KORB_C_CLASS,
    KORB_C_EXCEPTION, KORB_C_FLOAT, KORB_C_RATIONAL, KORB_C_COMPLEX, KORB_C_OBJECT,
    KORB_NCLASS
};
typedef RESULT (*korb_method_fn)(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE args);

/* Block-taking built-in (Array#each, Integer#times, ...).  `block` is a
 * node_entry NODE (NULL = no block given); `def_env` is the caller frame base
 * holding the block's captured outer vars.  The method drives the block via
 * korb_block_yield. */
typedef RESULT (*korb_method_blk_fn)(CTX *c, VALUE *slots, VALUE_REF self,
                                     VALUE_SLICE args, struct Node *block, VALUE *def_env,
                                     VALUE captured_self);

struct korb_method {
    uint32_t mid;            /* interned name */
    uint8_t  kind;           /* enum korb_method_kind */
    uint8_t  uses_block;     /* ISEQ: reserves 2 frame-top cells for yield/block_given? */
    int32_t  params_cnt;     /* total positional (req+opt); -1 = variadic (builtins only) */
    uint32_t req_cnt;        /* ISEQ: required positional count (== params_cnt if no opts) */
    int32_t  rest_slot;      /* ISEQ: *rest local slot (collects surplus positionals), -1 none */
    uint32_t locals_cnt;     /* ISEQ: frame size (params first, +2 if uses_block) */
    uint32_t attr_ivar;      /* ATTR_R/W: the @ivar symbol id */
    struct Node *body;       /* ISEQ */
    struct Node **opt_defaults;  /* ISEQ: default-value exprs for optionals (len = params_cnt-req_cnt), NULL if none */
    void *kw_info;           /* ISEQ: struct korb_kw_info * (keyword params), NULL if none */
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

    /* global function table (no-receiver calls: puts, p, user `def foo`) */
    struct korb_method *methods;
    uint32_t method_cnt, method_capa;
    uint64_t method_serial;  /* bumped by def — invalidates call caches */

    /* constants (class names): parallel name→value arrays.  `const_vals` holds
     * GC objects (classes) and is root-scanned by AROH_VISIT_ROOTS. */
    uint32_t *const_names;
    VALUE    *const_vals;
    uint32_t  const_cnt, const_capa;

    /* exception etype → constant name (class looked up via the const table, so
     * no separate GC root needed).  Index by enum korb_etype. */
    uint32_t  exc_name[16];
    /* korb_class enum → builtin class constant name (Integer/String/...). */
    uint32_t  class_name[KORB_NCLASS];

    /* per-core-class built-in method tables (receiver dispatch x.foo).
     * Each a flat {mid, fn, arity} list. */
    struct korb_cmethod {
        uint32_t mid; korb_method_fn fn; korb_method_blk_fn bfn;
        int32_t arity; uint8_t takes_block;
    } *cmethods[KORB_NCLASS];
    uint32_t cmethod_cnt[KORB_NCLASS], cmethod_capa[KORB_NCLASS];

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

/* The block handed to a method lives in the callee's frame (slots), not in
 * CTX — see docs/v2_blocks_design.md (no CTX mutable state, strict
 * no-globals).  Frame top 2 cells: { node_entry|1, def_env|1 } (odd-tagged
 * so the GC root scan skips these non-heap pointers). */

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
    /* constants (class values) are roots too */                            \
    for (uint32_t _ci = 0; _ci < (c)->vm->const_cnt; _ci++) {                \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->const_vals[_ci]);     \
    }                                                                        \
} while (0)

#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {         \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);                      \
    switch (_h->flags & KORB_OBJ_TYPE_MASK) {                                \
      case KORB_OBJ_FLOAT:                                                    \
      case KORB_OBJ_STR_BUF:                                                  \
      case KORB_OBJ_RATIONAL:                                                 \
        /* raw double / char[] / num,den — no edges */                       \
        (void)(payload_size);                                                \
        break;                                                               \
      case KORB_OBJ_STRING: {                                                \
        KorbString *_s = (KorbString *)(payload);                            \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_s->buf);                  \
        (void)(payload_size);                                                \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_EXCEPTION: {                                             \
        KorbException *_e = (KorbException *)(payload);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->msg);                      \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_ARRAY: {                                                 \
        KorbArray *_a = (KorbArray *)(payload);                             \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_a->items);               \
        (void)(payload_size);                                                \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_VALUE_ARRAY: {                                          \
        KorbArrayItems *_ai = (KorbArrayItems *)(payload);                  \
        size_t _n = ((payload_size) - sizeof(KorbArrayItems)) / sizeof(VALUE); \
        for (size_t _i = 0; _i < _n; _i++)                                  \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &_ai->data[_i]);          \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_HASH: {                                                  \
        KorbHash *_hh = (KorbHash *)(payload);                              \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_hh->items);              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_hh->default_val);            \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_RANGE: {                                                 \
        KorbRange *_rg = (KorbRange *)(payload);                            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_rg->rbegin);                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_rg->rend);                   \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_COMPLEX: {                                               \
        KorbComplex *_cx = (KorbComplex *)(payload);                         \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cx->re);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cx->im);                      \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_OBJECT: {                                                \
        KorbObject *_ob = (KorbObject *)(payload);                          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_ob->klass);                  \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_ob->ivars);             \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_CLASS: {                                                 \
        KorbClass *_cl = (KorbClass *)(payload);                            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->superclass);            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->included);             \
        (void)(payload_size);                                               \
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
