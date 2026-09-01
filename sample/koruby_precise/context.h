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
#include "builtins/bignum_backend.h"   /* korb_mp_*: 多倍長整数 backend (KORB_OBJ_BIGNUM) */
#include <string.h>
#include <stdlib.h>

#ifndef ASTRO_DEBUG
#  define ASTRO_DEBUG 0
#endif
#include "astro_debug.h"
#include "precise_gc/gc_types.h"
#include "aro_gc_effect.h"   /* ARO_MAYGC / ARO_NOGC / ARO_BORROW markers */

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
typedef int64_t  VALUE;
#define ARO_GC_VALUE_TYPEDEFED 1
/* VALUE is a 64-bit word, NOT a pointer: on a 32-bit target (wasm32) a tagged
 * Float still needs all 64 bits.  Tag arithmetic therefore goes through
 * korb_word_t; only the VAL2* casts below may narrow to a real pointer. */
typedef uint64_t korb_word_t;
typedef int64_t  korb_sword_t;
#define KORB_WORD(v)  ((korb_word_t)(v))

#define KORB_NIL       ((VALUE)0)
/* Special singletons live in the low-nibble 0b0100 quadrant, with a variant
 * selector in the upper bits — leaving room for Qundef-style sentinels beyond
 * the false/true booleans.  (v & 0xF) == 4 tags the quadrant; (v >> 4) selects.
 * All have (v & 7) == 4 ≠ 0, so the GC never mistakes one for a heap pointer. */
#define KORB_SPECIAL(n)    ((VALUE)((KORB_WORD(n) << 4) | 0x4u))
#define KORB_SPECIAL_P(v)  ((KORB_WORD(v) & 0xFu) == 0x4u)
#define KORB_FALSE     KORB_SPECIAL(0)   /* 0b000100 =  4 */
#define KORB_TRUE      KORB_SPECIAL(1)   /* 0b010100 = 20 */
#define KORB_UNDEF     KORB_SPECIAL(2)   /* 0b100100 = 36 — never a valid Ruby value (uninitialized / removed marker) */

#define FIXNUM_P(v)    ((KORB_WORD(v) & 1u) != 0)
#define LONG2FIX(i)    ((VALUE)((KORB_WORD((korb_sword_t)(i)) << 1) | 1u))
#define FIX2LONG(v)    (((korb_sword_t)(v)) >> 1)
#define FIXNUM_MAX     (INT64_MAX >> 1)
#define FIXNUM_MIN     (INT64_MIN >> 1)
#define FIXABLE(i)     ((i) >= FIXNUM_MIN && (i) <= FIXNUM_MAX)

/* Symbol — low nibble 0b1100 (static-symbol id in the upper bits). */
#define SYMBOL_P(v)    ((KORB_WORD(v) & 0xFu) == 0xCu)
#define ID2SYM(id)     ((VALUE)((KORB_WORD(id) << 4) | 0xCu))
#define SYM2ID(v)      ((uint32_t)(KORB_WORD(v) >> 4))

/* -----------------------------------------------------------------------------
 * Flonum — immediate Float, no heap box.  Now that Symbol has vacated the
 * low-3-bit 110 quadrant, Flonum uses CRuby's clean 2-bit tag: low2 == 10
 * (i.e. (v & 3) == 2), occupying quadrants {010, 110} with bit2 = sign — no
 * remap needed.  Representable: top-3 exponent bits ∈ {3,4} → |d| ∈ roughly
 * [2^-255, 2^256), both signs (covers all ordinary floats); ±0.0 use a magic
 * constant; out-of-range doubles heap-box.
 * GC-safe: (flonum & 7) ∈ {2,6} ≠ 0, so the edge filter / AROH_IS_GC_OBJECT
 * never treat a flonum as a heap pointer. --------------------------------- */
#define FLONUM_P(v)    ((KORB_WORD(v) & 3u) == 2u)
#define KORB_FLO_ZERO  ((VALUE)(korb_sword_t)0x8000000000000002ULL)

static inline VALUE korb_d2flo(double d) {   /* 0 → not representable (caller heap-boxes) */
    union { double d; korb_word_t v; } t; t.d = d;
    if (t.v == 0u) return KORB_FLO_ZERO;          /* +0.0 (−0.0 falls through → heap-box, keeps sign) */
    unsigned top3 = (unsigned)((t.v >> 60) & 7u);
    if (top3 != 3u && top3 != 4u) return 0;
    korb_word_t e = ((t.v << 3 | t.v >> 61) & ~(korb_word_t)1u) | 2u;       /* rotl3, low2=10, bit2=sign */
    if (e == KORB_WORD(KORB_FLO_ZERO)) return 0;                            /* the lone non-zero double colliding with the +0.0 magic → heap-box */
    return (VALUE)e;
}
static inline double korb_flo2d(VALUE fv) {
    korb_word_t v = KORB_WORD(fv);
    if (v == KORB_WORD(KORB_FLO_ZERO)) return 0.0;
    korb_word_t b63 = v >> 63;
    union { double d; korb_word_t v; } t;
    korb_word_t x = (2u - b63) | (v & ~(korb_word_t)3u);
    t.v = (x >> 3) | (x << 61);                                            /* rotr3 */
    return t.d;
}

/* Falsy = nil (0) or false (4): clearing bit2 maps both to 0. */
#define KORB_TRUTHY(v)   ((KORB_WORD(v) & ~(korb_word_t)4u) != 0)

/* Heap pointer test — also the GC contract macro (singletons / fixnums /
 * symbols have non-zero low bits or are 0, so they never look like heap
 * pointers). */
#define AROH_IS_GC_OBJECT(v)  ((v) != 0 && (KORB_WORD(v) & 7u) == 0)

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
    KORB_BREAK  = 4,    /* `break` — caught at the nearest loop / block-call boundary */
    KORB_RETRY  = 5,    /* `retry` in a rescue — re-runs the begin body (caught by node_begin) */
    KORB_THROW  = 6,    /* `throw tag, val` — unwinds (past rescue) to the matching `catch`; tag in c->throw_tag */
    KORB_REDO   = 7,    /* `redo` in a block/loop — re-runs the body with the same bindings */
};

typedef struct {
    VALUE     value;
    uintptr_t state;    /* enum korb_state */
} RESULT;

#define RESULT_OK(v)      ((RESULT){ (v), KORB_NORMAL })
#define RESULT_RETURN_(v) ((RESULT){ (v), KORB_RETURN })
#define RESULT_RAISE_(v)  ((RESULT){ (v), KORB_RAISE })
#define RESULT_NEXT_(v)   ((RESULT){ (v), KORB_NEXT })
#define RESULT_BREAK_(v)  ((RESULT){ (v), KORB_BREAK })
#define RESULT_RETRY_     ((RESULT){ KORB_NIL, KORB_RETRY })
#define RESULT_REDO_      ((RESULT){ KORB_NIL, KORB_REDO })

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
    KORB_OBJ_ENUMERATOR  = 13,  /* eager Enumerator: materialized values + inspect desc */
    KORB_OBJ_SET         = 14,  /* Set: backed by an array of unique elements */
    KORB_OBJ_REGEXP      = 15,  /* Regexp: source string + flags (matching via astrogre .so) */
    KORB_OBJ_METHOD      = 16,  /* bound Method: receiver + method id (needs 5-bit tag) */
    KORB_OBJ_FIBER       = 17,  /* stackful coroutine (separate value/C stacks) */
    KORB_OBJ_ARITHSEQ    = 18,  /* Enumerator::ArithmeticSequence (step/% lazy seq) */
    KORB_OBJ_BIGNUM      = 19,  /* arbitrary-precision Integer (.class is Integer; no GC edges) */
    KORB_OBJ_ENV         = 20,  /* closure env: [prev|loc|vals], open(loc→slots)/closed(loc→vals) */
    KORB_OBJ_PROC        = 21,  /* Proc/lambda: iseq(immortal) + captured env + self */
    KORB_OBJ_MATCHDATA   = 22,  /* MatchData (whole-match substring; no captures) */
    KORB_OBJ_BINDING     = 23,  /* Binding: captured local scope (env) + self + name table */
    KORB_OBJ_THREAD      = 24,  /* green thread handle (rep is libc; docs/io_design.md) */
    KORB_OBJ_MUTEX       = 25,  /* Thread::Mutex (owner/waiters は libc thread rep へのポインタ; no GC edges) */
    KORB_OBJ_CONDVAR     = 26,  /* Thread::ConditionVariable (waiters のみ; no GC edges) */
};
/* `flags` is a dedicated 16-bit sample-owned field; low 5 bits = type tag
 * (1..16; widened from 4 bits to make room for KORB_OBJ_METHOD). */
#define KORB_OBJ_TYPE_MASK 0x1Fu
/* bit 5: this heap object has a per-instance class override (subclass instance /
 * extended / singleton-method'd) recorded in the VM's sklass table.  Gates both
 * class lookup and dispatch so the common (no-override) path pays only a bit test. */
#define KORB_FL_HAS_KLASS  0x20u
/* bit 6: object has been frozen (koruby does not enforce immutability, but
 * tracks the bit so frozen? is accurate). */
#define KORB_FL_FROZEN     0x40u
/* bit 7 (Hash only): compare_by_identity — key lookup uses object identity (==)
 * instead of value equality. */
#define KORB_FL_CMP_BY_ID  0x80u
/* bit 8 (IO only): this is the init-time default $stdout/$stderr — output methods
 * take the fast direct-fwrite path while $stdout/$stderr still holds it. */
#define KORB_FL_DEFAULT_IO 0x100u
/* bit 15 (avoids the STR_ENC field 0x7000 so strings can use it too): a
 * non-KorbObject (String/Array/Hash/Proc/...) carries @ivars in the vm generic-ivar
 * side table.  Cheap "has ivars?" gate before the linear scan. */
#define KORB_FL_HAS_IVARS  0x8000u
/* bit 9 (Hash only): the lookup index is permanently disabled because a key
 * with ambiguous hash/equality (Float / heap object) was inserted — stay linear. */
/* bit 12 (Hash only — the 0x7000 field is the String encoding, which a Hash has
 * no use for): this Hash was written as keyword arguments at a call site, so the
 * callee binds it to keyword parameters.  A plain trailing Hash argument is
 * positional (Ruby 3 semantics). */
#define KORB_FL_KWARGS     0x1000u
#define KORB_FL_HASH_NOINDEX 0x200u
/* bit 9 (String only — shares the bit with Hash's HASH_NOINDEX, which is never
 * examined here): the String is CHILLED (Ruby 3.4+).  A chilled String also has
 * KORB_FL_FROZEN set, so every existing mutation guard already stops on it; the
 * single choke point korb_raise_frozen then warns, clears both bits and lets the
 * mutation through instead of raising.  #frozen? reports false while it is set. */
#define KORB_FL_CHILLED    0x200u

/* bit 10 (Array, transient): array is on the current Array#join recursion path.
 * GC-safe cycle detection (survives moves) so join can dispatch element #to_s. */
#define KORB_FL_JOIN_VISITING 0x400u
/* bit 11 (transient): object is on the current Comparable#== path — breaks the
 * Comparable#== → #<=> → Object#<=> → #== recursion for a Comparable with no #<=>. */
#define KORB_FL_CMP_VISITING 0x800u
/* bits 12-14 (String only): the string's encoding, as a 3-bit index.  Header
 * flags survive GC (copied with the object), so this needs no GC support.  The
 * index drives every character-level operation (length / [] / each_char / …):
 *   0 UTF-8 (default)   1 US-ASCII   2 ASCII-8BIT   3..7 "other"
 * The three concrete encodings are handled directly; an "other" index names an
 * entry in vm->str_enc_names (an interned encoding name) and character-level ops
 * on it raise NotImplementedError until per-encoding hooks are filled in. */
#define KORB_STR_ENC_MASK   0x7000u
#define KORB_STR_ENC_SHIFT  12u
/* String only: bits 7-8 (KORB_FL_CMP_BY_ID / KORB_FL_DEFAULT_IO, which belong to
 * Hash and IO) extend the index to 5 bits, so 29 named encodings fit. */
#define KORB_STR_ENC_HI_MASK 0x0180u
#define KORB_STR_ENC_MAX    32u
#define KORB_ENC_UTF8       0u
#define KORB_ENC_USASCII    1u
#define KORB_ENC_BINARY     2u
#define KORB_ENC_OTHER_MIN  3u
#define KORB_STR_ENC(v)     ((uint32_t)(((((const AroObjectHeader *)(uintptr_t)(v))->flags & KORB_STR_ENC_MASK) >> KORB_STR_ENC_SHIFT) | \
                             ((((const AroObjectHeader *)(uintptr_t)(v))->flags & KORB_STR_ENC_HI_MASK) >> 4)))
#define KORB_STR_ENC_SET(v, idx) do { AroObjectHeader *h__ = (AroObjectHeader *)(uintptr_t)(v); \
    h__->flags = (uint16_t)((h__->flags & ~(KORB_STR_ENC_MASK | KORB_STR_ENC_HI_MASK)) | \
                            (((uint16_t)(idx) << KORB_STR_ENC_SHIFT) & KORB_STR_ENC_MASK) | \
                            (((uint16_t)(idx) << 4) & KORB_STR_ENC_HI_MASK)); } while (0)
/* single-byte encodings: 1 byte = 1 character.  US-ASCII / ASCII-8BIT always,
 * plus any "other" slot holding a single-byte encoding (Latin-1 family, KOI8,
 * Windows-125x, 8-bit code pages) — vm->str_enc_sb_mask records which. */
#define KORB_ENC_SB(vm, idx)         ((idx) == KORB_ENC_USASCII || (idx) == KORB_ENC_BINARY || \
                                      ((((vm)->str_enc_sb_mask) >> (idx)) & 1u) != 0u)
/* character-level ops on this encoding need hooks koruby does not have yet */
#define KORB_ENC_NEEDS_HOOK(vm, idx) ((idx) >= KORB_ENC_OTHER_MIN && !KORB_ENC_SB(vm, idx))

/* growable byte buffer for a KorbString (header never moves on grow). */
typedef struct KorbStrBuf {
    AroObjectHeader head;        /* KORB_OBJ_STR_BUF */
    char data_priv[];                 /* capa + 1 bytes, NUL-terminated; no GC edges */
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
    VALUE ARO_GC_EDGE num, den;      /* Integer (Fixnum or Bignum); den > 0, reduced */
} KorbRational;

/* complex number re + im*i; components are arbitrary numerics (GC edges). */
typedef struct KorbComplex {
    AroObjectHeader head;            /* KORB_OBJ_COMPLEX */
    VALUE ARO_GC_EDGE re, im;
} KorbComplex;

/* eager Enumerator: `values` is the fully materialized array of yielded items;
 * `desc` is the inspect string (or nil); `cursor` drives next/peek. */
typedef struct KorbEnumerator {
    AroObjectHeader head;            /* KORB_OBJ_ENUMERATOR */
    uint32_t cursor;
    uint8_t  mode;                   /* 0 eager (values materialized), 1 lazy, 2 cycle */
    uint8_t  op;                     /* eager reduce when finally given a block: 0 map/each, 1 select, 2 reject */
    uint8_t  size_inf;               /* 1 → #size is Float::INFINITY (can't be stored in the immediate `size` slot) */
    uint8_t  size_unknown;           /* 1 → #size is nil even though `values` is materialized (String#gsub's enumerator) */
    VALUE ARO_GC_EDGE values;
    VALUE ARO_GC_EDGE desc;
    VALUE ARO_GC_EDGE source;        /* lazy/cycle: the underlying Array/Range */
    VALUE ARO_GC_EDGE ops;           /* lazy: Array of [op_sym, block_proc] pairs */
    VALUE size;                      /* known #size (Fixnum) or nil/0 (unknown); immediate → not a GC edge */
    VALUE ARO_GC_EDGE size_proc;     /* generator: a callable #size (Enumerator.new(->{…}){…}); nil otherwise */
} KorbEnumerator;

/* Enumerator::ArithmeticSequence — a lazy step/% sequence.  `recv` is the begin
 * (Numeric) or the source Range; `a0`/`a1` are the literal call args (reproduced
 * by inspect); `nargs` 0..2; `is_pct` selects the `%` vs `step` method name. */
typedef struct KorbArithSeq {
    AroObjectHeader head;            /* KORB_OBJ_ARITHSEQ */
    uint8_t nargs;
    uint8_t is_pct;
    VALUE ARO_GC_EDGE recv;
    VALUE ARO_GC_EDGE a0;
    VALUE ARO_GC_EDGE a1;
} KorbArithSeq;

/* Arbitrary-precision Integer.  .class reports Integer (CRuby-unified).  The
 * payload type belongs to the backend (bignum_backend.h); with GMP the limbs are
 * external malloc — a moving GC copies this struct (limb pointer stays valid).
 * Collected bignums release the payload via AROH_FINALIZE, and its bytes are
 * tracked as GC external pressure so the GC fires on bignum-heavy load. */
typedef struct KorbBignum {
    AroObjectHeader head;            /* KORB_OBJ_BIGNUM */
    korb_mp_t z;                     /* backend-owned payload */
} KorbBignum;

/* Set: a thin wrapper over an array of unique elements (dedup by korb_value_eq). */
typedef struct KorbSet {
    AroObjectHeader head;            /* KORB_OBJ_SET */
    VALUE ARO_GC_EDGE elems;         /* KorbArray of unique members */
    uint8_t by_identity;             /* compare_by_identity: members compared by object identity */
} KorbSet;

typedef struct KorbRegexp {
    AroObjectHeader head;            /* KORB_OBJ_REGEXP */
    VALUE ARO_GC_EDGE source;        /* the pattern as a String */
    uint8_t ci;                      /* case-insensitive flag (== flags & 4) */
    uint32_t flags;                  /* prism regex flags (IGNORE_CASE=4/EXTENDED=8/MULTI_LINE=16) */
} KorbRegexp;

/* MatchData: the subject, the source Regexp, and per-group byte spans.
 * `offsets` is a KorbArray of Integers [b0,e0,b1,e1,...] (byte offsets into
 * `subject`, -1 for a group that did not participate). */
typedef struct KorbMatchData {
    AroObjectHeader head;            /* KORB_OBJ_MATCHDATA */
    VALUE ARO_GC_EDGE subject;       /* the string that was matched against */
    VALUE ARO_GC_EDGE regexp;        /* the source Regexp (or nil) */
    VALUE ARO_GC_EDGE offsets;       /* KorbArray of Integer byte offsets, 2*(n_groups+1) */
} KorbMatchData;

/* Binding: a captured local scope.  `env` is the frame's closure env (open →
 * live slots, or closed → heap vals after the frame returned), `self` the
 * captured receiver, `names` an immortal NODE carrying the (name_sym, index)
 * table for that scope.  KORB_OBJ_BINDING. */
typedef struct KorbBinding {
    AroObjectHeader head;            /* KORB_OBJ_BINDING */
    VALUE ARO_GC_EDGE env;           /* KorbEnv (open → live slots, closed → heap vals) */
    VALUE ARO_GC_EDGE self;          /* captured self */
    VALUE ARO_GC_EDGE extra;         /* Hash {sym=>val} of locals added after capture (nil until used) */
    VALUE ARO_GC_EDGE cref;          /* the lexical class/module the binding was taken in (nil at top level).
                                      * `eval(str)` lowers to eval(str, <hidden binding>), so this is what
                                      * makes `def` / `class` / a constant inside the eval'd string land in
                                      * the enclosing class instead of at top level. */
    const uint32_t *name_syms;       /* immortal: name_syms[i] = sym of local at env index i */
    uint32_t name_cnt;
    const struct Node *src_node;     /* immortal: the `binding` call node (for #source_location); NULL if unknown */
} KorbBinding;

/* bound Method object (obj.method(:sym)): receiver + interned method id. */
typedef struct KorbMethod {
    AroObjectHeader head;            /* KORB_OBJ_METHOD */
    VALUE ARO_GC_EDGE recv;          /* bound: the receiver; unbound: the owner class */
    VALUE ARO_GC_EDGE owner;         /* bound-from-unbound: the class to invoke from (fixed, not virtual); nil = virtual re-dispatch by mid */
    uint32_t mid;                    /* interned method name */
    uint8_t  unbound;                /* 1 = UnboundMethod (recv holds the owner class) */
    uint8_t  missing;                /* 1 = name has no definition; respond_to_missing? vouched for it,
                                      * so #call always routes through method_missing (CRuby mnew_missing) */
} KorbMethod;

/* Closure env (one captured scope activation), materialized when a Proc escapes
 * (docs/v2_blocks_design.md).  open: the defining frame is still live; `loc`
 * points at the frame's slots locals base and outer reads/writes go straight to
 * the live slots (shared with the frame's own direct writes — correct mutation).
 * closed: the frame has returned; values were copied into `vals` (a separate
 * KORB_OBJ_VALUE_ARRAY, the codebase pattern — no self-pointer to fix up) and
 * access goes through it.  `loc` is not a GC edge (open=slots root; unused when
 * closed); `vals` is the only forwarded edge (closed only). */
typedef struct KorbEnv {
    AroObjectHeader head;            /* KORB_OBJ_ENV */
    VALUE ARO_GC_EDGE prev;          /* parent env handle (tagged KorbEnv* or tagged slots*, 0=top) */
    VALUE ARO_GC_EDGE vals;          /* closed: KORB_OBJ_VALUE_ARRAY of the captured locals; 0 when open */
    VALUE *loc;                      /* open: frame slots locals base; unused when closed */
    uint32_t n;                      /* number of locals captured */
    uint8_t  closed;                 /* 0 = open (use loc), 1 = closed (use vals) */
} KorbEnv;

/* Proc / lambda: an immortal body descriptor (node_entry) + the captured env +
 * the captured self.  KORB_OBJ_PROC. */
typedef struct KorbProc {
    AroObjectHeader head;            /* KORB_OBJ_PROC */
    struct Node *iseq;               /* block node_entry (immortal; not scanned); NULL for a symbol proc */
    VALUE ARO_GC_EDGE env;           /* captured env handle (KorbEnv* tagged) or 0 */
    VALUE ARO_GC_EDGE self;          /* captured lexical self */
    uint32_t sym_mid;                /* Symbol#to_proc: send this mid to arg0 (iseq==NULL) */
    uint8_t  is_lambda;              /* lambda semantics (strict arity, return-from-proc) */
} KorbProc;

/* Fiber: a stackful coroutine.  The moving GC object is a thin handle; all
 * mutable state + GC roots live in a libc-malloc'd (stable) KorbFiberRep, so
 * the VM's C-side fiber pointers never dangle when the handle moves. */
typedef struct KorbFiberRep {
    VALUE transfer;                  /* value passed across resume/yield (root) */
    VALUE captured_self;             /* the block's lexical self (root) */
    VALUE fibobj;                    /* the Fiber object owning this rep (root) — Fiber.current */
    VALUE storage;                   /* Fiber#storage: fiber-local Hash (root; nil until used) */
    VALUE tls;                       /* Thread#[] storage — CRuby's "thread locals" are FIBER-local (root; nil until used) */
    VALUE *vslots;                   /* fiber value-stack base (fixed mmap) */
    VALUE *vslots_top;               /* saved scan top while suspended */
    VALUE *vslots_limit;
    VALUE *vslots_hw;                /* saved high-water while suspended */
    VALUE *def_env;                  /* block's def_env (creator stack, non-moving) */
    struct Node *body;               /* block entry (node_entry, immortal) */
    void  *uctx;                     /* ucontext_t * (fiber's saved context) */
    void  *resume_uctx;              /* ucontext_t * to switch back to on yield */
    void  *cstack;                   /* malloc'd native stack for the fiber */
    uint32_t errinfo_n;              /* the fiber's own `$!` stack depth (fiber-local) */
    uint8_t fstate;                  /* 0 created, 1 running, 2 suspended, 3 done */
    uint8_t raised;                  /* block raised → resume re-raises transfer */
    uint8_t pending_raise;           /* Fiber#raise: yield raises transfer instead of returning it */
    uint8_t transferred;             /* entered via #transfer — #resume is then a FiberError */
    uint8_t killing;                 /* Fiber#kill: unwind the fiber at its suspension point */
    uint8_t blocking;                /* Fiber.new(blocking: true) — #blocking? and Fiber.blocking? */
    struct KorbFiberRep *link;       /* vm fiber list (stable ptrs) */
    struct korb_thread *owner;       /* creating green thread — resume from another is a FiberError */
} KorbFiberRep;

typedef struct KorbFiber {
    AroObjectHeader head;            /* KORB_OBJ_FIBER (no GC edges; rep is libc) */
    struct KorbFiberRep *rep;
} KorbFiber;

/* korb_thread — a Ruby Thread (green thread; docs/io_design.md).  Phase 1:
 * native 1 本 + M green threads、切替は blocking 点のみ (協調)。全 mutable 状態と
 * GC roots はこの libc-stable な rep に置き、vm->thread_list 経由で scan する。
 * KorbThread heap object は薄い可動 handle。 */
struct korb_blop;                    /* blop 層 (M2) — fwd */
enum korb_thread_state { KORB_TH_READY = 0, KORB_TH_RUNNING, KORB_TH_PENDED, KORB_TH_DEAD };
struct korb_thread {
    /* GC roots (AROH_VISIT_ROOTS が thread_list 経由で visit) */
    VALUE thval;                     /* KorbThread handle (movable) */
    VALUE args;                      /* Thread.new の引数 Array (起動後 nil) */
    VALUE blk;                       /* body の Proc (escape-safe に env を close 済) —
                                        生 def_env 保持は作成フレームが thread 実行前に
                                        死ぬパターン (n.times { Thread.new{…} }) で壊れる */
    VALUE captured_self;             /* (未使用予約; blk が self を持つ) */
    VALUE result;                    /* body の戻り値 (#join / #value) */
    VALUE exc;                       /* thread を殺した例外 (raised=1 のとき) */
    VALUE tls;                       /* fiber-local storage Hash (#[] / #[]=) */
    VALUE tvars;                     /* thread_variable_get/set の Hash (tls とは別空間; CRuby 準拠) */
    VALUE name;                      /* Thread#name */
    VALUE pending_ints;              /* pending interrupt queue (M3) */
    VALUE tgroup;                    /* ThreadGroup (NIL = Default; 生成時に親から継承) */
    /* stacks / context */
    VALUE *vslots;                   /* 自分の value stack base (main: main slots) */
    VALUE *vslots_limit;
    VALUE *saved_base, *saved_top, *saved_hw;   /* suspend 中の c->slots tuple */
    const char *saved_cstack_limit;
    uint32_t saved_errinfo_n;        /* $! stack depth (thread-local, like a fiber's) */
    void  *uctx;                     /* ucontext_t* */
    void  *cstack;                   /* malloc native stack (NULL = main/process stack) */
    /* scheduling */
    uint8_t state;                   /* enum korb_thread_state */
    uint8_t started;                 /* trampoline が走るまで 0 */
    uint8_t raised;                  /* body が例外で終了 */
    uint8_t roe;                     /* Thread#report_on_exception (default 1) */
    uint8_t aoe;                     /* Thread#abort_on_exception (死時に main へ例外転送) */
    uint8_t defer_ints;              /* >0 = handle_interrupt(:never) 区間 (配送延期) */
    const char *waiting_feature;     /* require 待ち: 対象 feature の abspath (待機側の
                                        stack 上バッファ; PENDED の間だけ有効)。 */
    const char *blocked_in;          /* C-level cooperative wait の label ("require" 等)。
                                        NULL 以外の間は #stop? が true になり、#backtrace に
                                        `in '<label>'` フレームが 1 枚見える (busy-yield でも
                                        観測上は blocked として振る舞う)。 */
    int     priority;                /* Thread#priority (保持のみ; scheduler は無視) */
    struct korb_thread *rq_next;     /* run queue link (READY FIFO) */
    struct korb_thread *next;        /* vm->thread_list link (全 rep; 解放しない) */
    struct korb_thread *joiners;     /* 自分の死を #join で待つ thread 群 */
    struct korb_thread *join_next;
    struct korb_blop *blop;          /* PENDED 中に待っている blop (M2)、他は NULL */
};

typedef struct KorbThread {
    AroObjectHeader head;            /* KORB_OBJ_THREAD (no GC edges; rep is libc) */
    struct korb_thread *rep;
} KorbThread;

/* Mutex / ConditionVariable — 純 green-thread プリミティブ (OS lock 不要)。
 * owner / waiters は libc-stable な korb_thread rep へのポインタなので GC edge
 * なし (payload が動いてもポインタ値ごと写るだけ)。待ち行列のリンクは
 * korb_thread.join_next を流用 (thread は同時に 1 つしか待てない)。 */
typedef struct KorbMutex {
    AroObjectHeader head;            /* KORB_OBJ_MUTEX */
    struct korb_thread *owner;       /* NULL = unlocked */
    struct korb_thread *wq_head, *wq_tail;
} KorbMutex;

typedef struct KorbCondVar {
    AroObjectHeader head;            /* KORB_OBJ_CONDVAR */
    struct korb_thread *wq_head, *wq_tail;
} KorbCondVar;

typedef struct KorbException {
    AroObjectHeader head;
    uint32_t etype;          /* enum korb_etype (korb_runtime.c) */
    uint32_t line;           /* current unwind line (raise site, then each
                              * call site as the unwind passes it) */
    VALUE ARO_GC_EDGE msg;   /* KorbString | nil */
    VALUE ARO_GC_EDGE exc_class;  /* user exception Class (raise MyError) | nil → builtin etype class */
    VALUE ARO_GC_EDGE cause; /* the in-flight exception ($!) at raise time, or nil */
    VALUE ARO_GC_EDGE ivars; /* instance variables side-hash {sym→val} | nil (custom Exception subclass data) */
    VALUE ARO_GC_EDGE backtrace; /* Array<String> captured when first rescued/set, or nil (never raised) */
} KorbException;

/* Array: a header + a separately-allocated growable VALUE[] payload, so push
 * can grow the buffer without moving the KorbArray (moving-GC safe). */
typedef struct KorbArrayItems {
    AroObjectHeader head;            /* KORB_OBJ_VALUE_ARRAY */
    VALUE ARO_GC_EDGE data_priv[];   /* private: reach only via korb_items_data() */
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
    VALUE ARO_GC_EDGE default_proc;  /* Hash.new { |h,k| } block (nil unless set); called on [] miss */
    /* O(1) lookup index for large hashes: open-addressing table of pair indices
     * (slot = pair_idx + 1; 0 = empty), a raw KORB_OBJ_STR_BUF of uint32.  NULL
     * for small hashes (linear scan is faster) or when KORB_FL_HASH_NOINDEX is
     * set (a key whose value-equality is ambiguous to hash, e.g. Float/heap). */
    KorbStrBuf *ARO_GC_EDGE index;
    uint32_t idx_mask;               /* index capacity - 1 (0 = no index) */
} KorbHash;
/* Drop the lookup index — call after any structural mutation that removes or
 * reorders pairs (delete/shift/clear/reject!) so pair indices can't go stale.
 * korb_hash_find falls back to linear; the next insert past the threshold
 * rebuilds.  (NOINDEX, if set, stays set.) */
#define KORB_HASH_DROP_INDEX(h) do { ARO_GC_RAW_STORE(&(h)->index, NULL); (h)->idx_mask = 0; } while (0)

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
    uint32_t shape_id;               /* ivar layout (vm->shapes index; 1 = root) */
    uint32_t ivar_capa;              /* values-array capacity (VALUE count) */
    VALUE ARO_GC_EDGE klass;         /* KorbClass | nil */
    KorbArrayItems *ARO_GC_EDGE ivars;   /* ivar_capa VALUEs (values only), or NULL */
} KorbObject;

/* Object shape: a node in the ivar-layout transition tree (VM-side, libc-stable;
 * objects hold an integer shape_id so the GC can't invalidate it).  Adding an
 * ivar transitions shape→child(edge_sym).  ivar index = depth-1 at the shape
 * whose edge_sym matches; the inline cache keys on shape_id. */
struct korb_shape {
    uint32_t parent;                 /* parent shape id (0 = none, root's parent) */
    uint32_t edge_sym;               /* ivar id added vs parent (UINT32_MAX at root) */
    uint32_t ivar_count;             /* = depth = number of ivars */
    struct korb_shape_edge { uint32_t sym, child; } *edges;   /* sym → child shape */
    uint32_t edge_cnt, edge_capa;
};

/* User class.  The instance-method table is a libc side-array (method bodies
 * are immortal NODEs, names interned ints → no GC edges), so `methods` is a
 * raw non-edge pointer; only `superclass` and `name` are GC-visible. */
struct korb_method;
typedef struct KorbClass {
    AroObjectHeader head;            /* KORB_OBJ_CLASS */
    uint32_t name_sym;               /* class name (interned), 0 = anonymous */
    uint32_t temp_name_sym;          /* Module#set_temporary_name, 0 = none.  Kept apart from
                                      * name_sym so it can be discarded once the module becomes
                                      * reachable through a permanent constant path. */
    int32_t  exc_etype;              /* builtin exception class → its etype, else -1 */
    uint32_t method_cnt, method_capa;
    uint8_t  is_module;              /* 1 = module (mixin, not instantiable) */
    uint8_t  is_singleton;           /* 1 = per-object singleton class (transparent to .class) */
    uint8_t  new_kind;               /* .new dispatch cache: 0=unknown, 1=plain user class, 2=special (Fiber/Struct/builtin/module) */
    uint8_t  struct_kwinit;          /* Struct.new(..., keyword_init: true) → .new takes a kwargs hash */
    uint8_t  is_data;                /* 1 = Data.define class (immutable; .new accepts positional OR keyword) */
    uint8_t  cur_visibility;         /* default visibility for `def` in this class body: 0 pub / 1 priv / 2 prot */
    uint32_t serial;                 /* monotonic per-class id (GC-stable identity for #hash of anonymous Struct/Data classes) */
    struct korb_method **methods;    /* libc array of immortal entry ptrs (owner edge GC-forwarded) */
    VALUE ARO_GC_EDGE superclass;    /* KorbClass | nil (nil ⇒ Object) */
    VALUE ARO_GC_EDGE included;      /* KorbArray of included modules | nil */
    VALUE ARO_GC_EDGE prepended;     /* KorbArray of prepended modules | nil (searched before own methods) */
    VALUE ARO_GC_EDGE members;       /* Struct member-name Array (symbols), or nil */
    VALUE ARO_GC_EDGE cvars;         /* class variables: KorbHash sym→value, or nil */
    VALUE ARO_GC_EDGE class_ivars;   /* the class object's own instance variables: KorbHash sym→value, or nil */
    VALUE ARO_GC_EDGE enclosing;     /* lexically-enclosing module/class (for M::C names), or nil (top-level / anonymous) */
    VALUE ARO_GC_EDGE subclasses;    /* KorbArray of direct subclass class-objects (for Class#subclasses), or nil */
} KorbClass;

#define KORB_OBJ_TYPE(v)   (((AroObjectHeader *)(uintptr_t)(v))->flags & KORB_OBJ_TYPE_MASK)
#define KORB_STRING_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_STRING)
#define KORB_EXC_P(v)      (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_EXCEPTION)
#define KORB_ARRAY_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_ARRAY)
#define KORB_HASH_P(v)     (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_HASH)
#define KORB_RANGE_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_RANGE)
#define KORB_OBJECT_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_OBJECT)
#define KORB_CLASS_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_CLASS)
#define KORB_FLOAT_P(v)    (FLONUM_P(v) || (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_FLOAT))
#define KORB_HEAP_FLOAT_P(v) (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_FLOAT)
#define VAL2STR(v)         ((KorbString *)(uintptr_t)(v))
#define VAL2EXC(v)         ((KorbException *)(uintptr_t)(v))
#define VAL2ARY(v)         ((KorbArray *)(uintptr_t)(v))
#define VAL2HASH(v)        ((KorbHash *)(uintptr_t)(v))

/* ARO_BORROW: the sanctioned accessor for a String's movable byte buffer.  Only
 * ARO_BORROW-marked functions may reach into the raw layout, so the internal
 * representation can change by editing accessors alone (docs/c_ext_api_design.md
 * §4.1).  The returned pointer is valid only until the next allocation. */
/* ARO_BORROW: the ONLY sanctioned reach into the raw movable payload buffers.
 * The `data_priv` fields are named to force all other access through these
 * accessors (docs/c_ext_api_design.md §4.1).  Result valid until next alloc. */
static inline ARO_BORROW char  *korb_strbuf_data(KorbStrBuf *b)      { return b->data_priv; }
static inline ARO_BORROW VALUE *korb_items_data (KorbArrayItems *it) { return it->data_priv; }
static inline ARO_BORROW char  *korb_str_data   (VALUE v)           { return korb_strbuf_data(VAL2STR(v)->buf); }
#define VAL2RANGE(v)       ((KorbRange *)(uintptr_t)(v))
#define VAL2OBJ(v)         ((KorbObject *)(uintptr_t)(v))
#define VAL2CLASS(v)       ((KorbClass *)(uintptr_t)(v))
#define VAL2FLT(v)         ((KorbFloat *)(uintptr_t)(v))
/* the double value of any Float (flonum immediate or heap KorbFloat). */
#define korb_float_val(v)  (FLONUM_P(v) ? korb_flo2d(v) : VAL2FLT(v)->val)
#define KORB_RATIONAL_P(v) (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_RATIONAL)
#define VAL2RAT(v)         ((KorbRational *)(uintptr_t)(v))
#define KORB_COMPLEX_P(v)  (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_COMPLEX)
#define VAL2CPX(v)         ((KorbComplex *)(uintptr_t)(v))
#define KORB_ENUM_P(v)     (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_ENUMERATOR)
#define VAL2ENUM(v)        ((KorbEnumerator *)(uintptr_t)(v))
#define KORB_SET_P(v)      (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_SET)
#define VAL2SET(v)         ((KorbSet *)(uintptr_t)(v))
#define KORB_REGEXP_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_REGEXP)
#define VAL2RE(v)          ((KorbRegexp *)(uintptr_t)(v))
#define KORB_METHOD_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_METHOD)
#define VAL2METH(v)        ((KorbMethod *)(uintptr_t)(v))
#define KORB_BINDING_P(v)  (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_BINDING)
#define VAL2BIND(v)        ((KorbBinding *)(uintptr_t)(v))
#define KORB_FIBER_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_FIBER)
#define VAL2FIBER(v)       ((KorbFiber *)(uintptr_t)(v))
#define KORB_THREAD_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_THREAD)
#define VAL2THREAD(v)      ((KorbThread *)(uintptr_t)(v))
#define KORB_MUTEX_P(v)    (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_MUTEX)
#define VAL2MUTEX(v)       ((KorbMutex *)(uintptr_t)(v))
#define KORB_CONDVAR_P(v)  (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_CONDVAR)
#define VAL2CONDVAR(v)     ((KorbCondVar *)(uintptr_t)(v))
#define KORB_ARITHSEQ_P(v) (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_ARITHSEQ)
#define VAL2ASEQ(v)        ((KorbArithSeq *)(uintptr_t)(v))
#define KORB_MATCHDATA_P(v) (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_MATCHDATA)
#define VAL2MD(v)          ((KorbMatchData *)(uintptr_t)(v))
#define KORB_BIGNUM_P(v)   (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_BIGNUM)
#define VAL2BIG(v)         ((KorbBignum *)(uintptr_t)(v))
#define KORB_ENV_P(v)      (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_ENV)
#define VAL2ENV(v)         ((KorbEnv *)(uintptr_t)(v))
#define KORB_PROC_P(v)     (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_PROC)
#define VAL2PROC(v)        ((KorbProc *)(uintptr_t)(v))
/* any Integer: immediate Fixnum or heap Bignum */
#define KORB_INTEGER_P(v)  (FIXNUM_P(v) || KORB_BIGNUM_P(v))

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
    KORB_METHOD_BUILTIN = 1,  /* global C fn (puts/p/...): bfn(c, slots, args), no self */
    KORB_METHOD_ATTR_R = 2,   /* attr reader: return @ivar */
    KORB_METHOD_ATTR_W = 3,   /* attr writer: @ivar = arg0 */
    KORB_METHOD_CFUNC = 4,    /* receiver-dispatch C method (Array#push, ...): rfn/rbfn with self ref */
    KORB_METHOD_DM = 5,       /* define_method: body is a Proc (dm_proc) run with self = receiver */
    KORB_METHOD_UNDEF = 6,    /* undef_method: a tombstone that STOPS the MRO walk (unlike remove_method) */
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
    KORB_C_ENUMERATOR, KORB_C_SET, KORB_C_REGEXP, KORB_C_METHOD, KORB_C_FIBER,
    KORB_C_ARITHSEQ, KORB_C_PROC, KORB_C_MATCHDATA, KORB_C_BINDING,
    KORB_C_RANDOM, KORB_C_UNBOUND_METHOD, KORB_C_THREAD, KORB_C_MUTEX, KORB_C_CONDVAR,
    KORB_NCLASS
};
typedef RESULT (*korb_method_fn)(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE args);

/* Block-taking built-in (Array#each, Integer#times, ...).  `block` is a
 * node_entry NODE (NULL = no block given); `def_env` is the caller frame base
 * holding the block's captured outer vars.  The method drives the block via
 * korb_block_yield. */
typedef RESULT (*korb_method_blk_fn)(CTX *c, VALUE *slots, VALUE_REF self,
                                     VALUE_SLICE args, struct Node *block, VALUE *def_env,
                                     VALUE *captured_self);

struct korb_method {
    uint32_t mid;            /* interned name (current; changes on alias) */
    uint32_t orig_mid;       /* name at original definition; survives alias (for #original_name) */
    uint8_t  visibility;     /* 0 public / 1 private / 2 protected */
    uint8_t  kind;           /* enum korb_method_kind */
    uint8_t  uses_block;     /* ISEQ: reserves 2 frame-top cells for yield/block_given? */
    uint8_t  is_simple;      /* ISEQ: fixed positional arity, no rest/opt/post/kw/block —
                              * eligible for the streamlined korb_invoke_simple path. */
    int32_t  params_cnt;     /* total positional (req+opt); -1 = variadic (builtins only) */
    uint32_t req_cnt;        /* ISEQ: required positional count (== params_cnt if no opts) */
    uint32_t post_cnt;       /* ISEQ: post (after-rest) required params; slots at rest_slot+1.. */
    int32_t  rest_slot;      /* ISEQ: *rest local slot (collects surplus positionals), -1 none */
    uint32_t locals_cnt;     /* ISEQ: frame size (params first, +2 if uses_block) */
    uint32_t attr_ivar;      /* ATTR_R/W: the @ivar symbol id */
    struct Node *body;       /* ISEQ */
    VALUE owner;             /* defining class/module (super's def_class), nil for a global fn.
                              * Manually GC-forwarded (entry is immortal libc, so the class
                              * visitor + roots forward this field — like const_vals).  owner/mid
                              * never change, so a frame-held entry yields a live owner + stable mid. */
    VALUE super_owner;       /* where `super` resumes from, when that is NOT `owner`: an alias
                              * keeps the ORIGINAL defining class here (CRuby's defined_class),
                              * so `super` from Alias3#name3 continues after Alias2, not after
                              * Alias3.  nil = use owner.  Manually GC-forwarded next to owner. */
    VALUE dm_proc;           /* KORB_METHOD_DM: the define_method Proc (env pre-closed). nil otherwise.
                              * Manually GC-forwarded next to owner (entry is immortal libc). */
    VALUE refine_set;        /* refinements active where this method was defined (CTX.refinements
                              * snapshot), nil for the usual case.  Installed on the callee by
                              * korb_invoke_method.  Manually GC-forwarded next to owner. */
    struct Node **opt_defaults;  /* ISEQ: default-value exprs for optionals (len = params_cnt-req_cnt), NULL if none */
    void *kw_info;           /* ISEQ: struct korb_kw_info * (keyword params), NULL if none */
    void *param_info;        /* ISEQ: struct korb_param_info * (full param list for #parameters), NULL if none — cold-read only */
    korb_builtin_fn bfn;     /* BUILTIN (global C fn) */
    /* CFUNC (receiver-dispatch builtin): arity in params_cnt (-1 = variadic),
     * takes_block in uses_block.  rbfn used when uses_block, else rfn. */
    korb_method_fn     rfn;
    korb_method_blk_fn rbfn;
};

struct korb_bt_entry {       /* one unwind frame for the uncaught-exception report */
    uint32_t line;
    const char *name;        /* method name, or "<main>" */
};

/* CRuby-compatible MT19937 state (624-word vector + cursor); defined here so the
 * vm can embed the Kernel#rand default generator.  Operations: builtins/random.c. */
typedef struct KorbMT { uint32_t mt[624]; uint32_t mti; } KorbMT;

struct korb_vm {
    /* symbol intern table: id -> name (libc strings, never freed).  `sym_lens`
     * caches each name's length; `sym_hash` is an open-addressing index
     * (slot -> id+1, 0 = empty) for O(1) intern instead of a linear strlen scan. */
    const char **sym_names;
    uint32_t *sym_lens;
    /* Per-symbol encoding (a KORB_ENC_* index).  A Symbol keeps the encoding of
     * the String it came from, and the intern key is (bytes, encoding) — the same
     * bytes tagged ISO-8859-1 and BINARY are two distinct Symbols.  An ASCII-only
     * name is always US-ASCII whatever its source String was tagged (CRuby), so
     * every name interned from C lands on a single canonical id. */
    uint8_t *sym_encs;
    uint32_t *sym_hash;
    uint32_t sym_cnt, sym_capa, sym_hash_cap;

    /* IO fd table: an IO/File object holds its index in the @__io_fp ivar; the
     * FILE* lives here (raw C pointers, not VALUEs → no GC scan/forward). */
    struct KorbIORep **io_reps;   /* open streams (libc-stable reps; the IO object holds the index) */
    uint32_t io_cnt, io_capa;

    /* global function table (no-receiver calls: puts, p, user `def foo`) */
    struct korb_method **methods;
    uint32_t method_cnt, method_capa;
    uint64_t method_serial;  /* bumped by def — invalidates call caches */
    /* bumped whenever constant *resolution* could change: a new (name, owner)
     * entry, a remove_const, or an include/prepend/extend that alters an
     * ancestry.  Reassigning an existing constant does not move its index, so it
     * does not need one. */
    uint64_t const_serial;
    VALUE super_new_skip;    /* transient: the singleton whose `def self.new` is
                              * super-ing right now — korb_send_impl skips that
                              * override once so the default allocator runs.
                              * Set/cleared around one send; never live across GC
                              * user code (GC-visited as a root regardless). */
    uint32_t class_serial;   /* monotonic id handed to each new class (see KorbClass.serial) */
    /* set when a user redefines a node-fastpathed basic op (+,-,*,/,%,<,<=,>,>=)
     * on Integer/Float; the arithmetic/compare nodes then deopt to a real send
     * so the redefinition is honored (CRuby basic-op-redefined semantics). */
    bool basic_op_redefined;
    /* `==` redefined on a builtin OTHER than Integer/Float (String, Complex, …):
     * node_eq's korb_value_eq shortcut has to give way to a real send.  Separate
     * from basic_op_redefined so it costs one cold branch, not the arithmetic
     * fast paths. */
    bool value_eq_redefined;
    /* set the first time a `using` activates a refinement (docs/refinements.md).
     * Same deopt-flag pattern as basic_op_redefined: while false the dispatch
     * paths are untouched; once true the inline/call caches stop being filled so
     * every site re-resolves and can consult the active refinements. */
    bool refinements_active;

    /* constants (class names): parallel name→value arrays.  `const_vals` holds
     * GC objects (classes) and is root-scanned by AROH_VISIT_ROOTS.
     * `const_owners` is the lexically-defining module/class (nil = top-level),
     * for Module#constants; also root-scanned (owners are classes). */
    uint32_t *const_names;
    VALUE    *const_vals;
    VALUE    *const_owners;
    uint32_t  const_cnt, const_capa;
    /* boxed Float-literal pool: non-flonum literals (2.0/-2.0/out-of-range) are
     * boxed once and reused (deduped by bit value) instead of heap-boxed on every
     * eval.  Root-scanned by AROH_VISIT_ROOTS so the boxes are GC-forwarded. */
    VALUE    *flit_vals;
    uint32_t  flit_cnt, flit_capa;
    /* frozen String-literal pool (--enable-frozen-string-literal / the magic
     * comment): equal literals share one frozen object, as CRuby's fstring
     * table does.  Root-scanned alongside flit_vals. */
    VALUE    *fstr_vals;
    uint32_t  fstr_cnt, fstr_capa;

    /* per-instance class override table (subclass instances / extended objects /
     * singleton methods).  sklass_obj[i] is the heap object, sklass_cls[i] its
     * override class.  BOTH columns are forwarded as roots in AROH_VISIT_ROOTS,
     * so the moving GC rewrites them in lockstep and `self == sklass_obj[i]`
     * stays valid across compaction (no GC-core hook needed).  Only objects with
     * KORB_FL_HAS_KLASS appear here, so the common path never touches this.
     * NB: the obj column is a strong root → an overridden object is pinned for
     * the program's life (acceptable: such objects are rare; weak-ref later). */
    VALUE    *sklass_obj;
    VALUE    *sklass_cls;
    uint32_t  sklass_cnt, sklass_capa;
    /* generic-ivar side table (same lockstep-forwarded pattern as sklass): lets a
     * String/Array/Hash/Proc/... carry @ivars it has no struct slot for.  objivar_hash[i]
     * is a Hash {sym=>val} for objivar_obj[i].  Keeps such objects alive (weak-ref later). */
    VALUE    *objivar_obj;
    VALUE    *objivar_hash;
    uint32_t  objivar_cnt, objivar_capa;
    /* cached frozen result of nil/true/false #to_s (CRuby returns the same frozen
     * object each call).  KORB_NIL until first use; GC roots (AROH_VISIT_ROOTS). */
    VALUE     str_nil_to_s, str_true_to_s, str_false_to_s;

    /* B3 escape: a frame's open KorbEnv (if any) lives in its EP cell base[-1]
     * (clean even pointer, GC-rooted via the slot scan); closed (slots->vals
     * copied) by that frame's return.  No global registry. */

    /* Regexp engine: lazily dlopen'd koruby_regex.so (→ libastrogre.so).  re_fn is
     * the koruby_re_exec entry, or (void*)-1 if the .so failed to load; the named/
     * valid helpers share the same handle. */
    void     *re_fn;         /* koruby_re_exec */
    void     *re_named_fn;   /* koruby_re_named */
    void     *re_valid_fn;   /* koruby_re_valid */
    void     *re_err_fn;     /* koruby_re_error — why the last compile failed */
    void     *re_floor_fn;   /* koruby_re_set_stack_floor */

    /* exception etype → constant name (class looked up via the const table, so
     * no separate GC root needed).  Index by enum korb_etype. */
    uint32_t  exc_name[24];
    /* korb_class enum → builtin class constant name (Integer/String/...). */
    uint32_t  class_name[KORB_NCLASS];
    /* korb_class enum → const-table index of its class object (append-only table,
     * so the index is stable; const_vals[idx] is forwarded by GC).  O(1) access
     * to a builtin's class object for receiver dispatch. */
    uint32_t  class_obj_idx[KORB_NCLASS];

    /* Enumerator::Yielder class (block arg of Enumerator.new); KORB_NIL until
     * the enum init runs.  A GC root (forwarded in AROH_VISIT_ROOTS). */
    VALUE     yielder_class;
    /* Active streaming-each sink for a (possibly infinite) generator enumerator.
     * When non-NULL, Enumerator::Yielder#<< feeds each value straight to a user
     * block and honours break/StopIteration instead of materializing into an
     * array.  Points at a C struct on the driving frame's stack (save/restore
     * for nesting); the pointer is not GC-visited — the sink's break value is
     * written to a rooted slot the struct references. */
    struct korb_gen_sink *gen_sink;
    /* Enumerator::Lazy class (reported as the class of lazy-mode enumerators);
     * set once at init.  A GC root (forwarded in AROH_VISIT_ROOTS). */
    VALUE     lazy_class;

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

    /* Fiber support: list of all live fibers (suspended ones' value-stacks are
     * GC roots), the currently-running fiber (NULL = main), the main stack's
     * saved scan bounds while a fiber runs, and the trampoline hand-off. */
    struct KorbFiberRep *fiber_list;
    struct KorbFiberRep *root_fiber;   /* stand-in rep for the main stack (Fiber.current) */
    struct KorbFiberRep *running_fiber;
    struct KorbFiberRep *starting_fiber;
    VALUE *main_slots, *main_slots_top;

    /* green threads (Thread) — docs/io_design.md。cur_thread == NULL は
     * thread サブシステム未起動 (最初の Thread API 使用で boot): それまで
     * fast path はゼロコスト。rep は libc-stable、解放しない。 */
    struct korb_thread *thread_list;   /* 全 rep (GC scan 用) */
    struct korb_thread *cur_thread;    /* RUNNING (NULL = single-threaded) */
    struct korb_thread *main_thread;
    struct korb_thread *runq_head, *runq_tail;   /* READY FIFO */
    uint32_t name_thread;              /* interned "Thread" */
    /* blop 層 (blocking operations; docs/io_design.md)。pending は engine に
     * 登録済みで完了待ちの blop の連結リスト (rep 同様 C スタック上の実体)。 */
    struct korb_blop *blop_pending;
    uint32_t blop_npending;
    VALUE thread_kill_exc;             /* Thread#kill 用内部例外 class (遅延生成; GC root) */
    uint8_t thread_aoe_global;         /* Thread.abort_on_exception (class-level) */

    /* direct-mapped user-object method cache (klass,mid)→method.  Valid while
     * serial == method_serial (bumped on def AND on GC, so a moved/reused class
     * pointer can never produce a false hit). */
    struct korb_mcache_ent {
        uint64_t serial; VALUE klass; uint32_t mid;
        struct korb_method *m; VALUE def_class;
    } *mcache;

    /* object shape table (ivar-layout tree).  shapes[0]=unused, shapes[1]=root. */
    struct korb_shape *shapes;
    uint32_t shape_cnt, shape_capa;

    /* interned ids of dispatch-hot method names, resolved once at init so the
     * send path tests them with integer compares instead of korb_sym_name+strcmp
     * on every call (send/__send__/public_send check ran for every arg call). */
    uint32_t mid_send, mid___send__, mid_public_send, mid_new, mid_yield, mid_initialize, mid_eqq;
    uint32_t mid_method_missing;
    uint32_t mid_dm_super;   /* sentinel name a `super` with no lexical `def` is baked with:
                              * a define_method body IS a method, but only at run time. */
    uint32_t mid_band, mid_bor, mid_bxor, mid_shl, mid_shr;   /* bit-op dispatch fallbacks (avoid per-call korb_intern) */
    uint32_t mid_eq;                                          /* "==" — node_eq/node_neq user-object dispatch (avoid per-call korb_intern) */
    uint32_t mid_cmp;                                         /* "<=>" — Array#sort of user/Comparable objects */
    uint32_t name_fiber;   /* class name_sym of Fiber (class-receiver fast check) */
    uint32_t name_struct;  /* class name_sym of Struct (class-receiver fast check) */
    uint32_t name_module;  /* class name_sym of Module (Module.new anonymous module) */
    uint32_t mid_aref, mid_aset;   /* "[]" / "[]=" — node_aref/node_aset deopt target */
    /* set when Array#[] / Array#[]= is redefined: node_aref/node_aset then deopt
     * to a real send so the redefinition is honored (CRuby compat). */
    bool aref_redefined;
    /* set when Array#<< is redefined: node_shl's Array fast path then deopts. */
    bool arr_shl_redefined;
    /* set when any class defines `!`: node_not then dispatches instead of
     * negating in place (BasicObject proxies such as Delegator rely on it). */
    bool bang_redefined;
    /* set when Hash#[] is redefined: node_aref's Hash fast path then deopts. */
    bool hash_aref_redefined;
    /* set when Method#[] is redefined: node_aref's Method fast path then deopts.
     * optcarrot's memory dispatch is `@fetch[addr][addr]` = Method#[] (= .call),
     * megamorphic and ~7M/run; the fast path collapses the [] + meth_call double
     * dispatch into a single recv.mid send. */
    bool method_aref_redefined;
    /* set when Integer#[] is redefined: node_aref's Integer bit-test fast path
     * then deopts.  optcarrot does ~5M Fixnum n[bit] tests/run; the fast path
     * computes (n>>i)&1 inline, skipping the send + dispatch + builtin. */
    bool int_aref_redefined;

    /* Kernel#rand / srand default PRNG (CRuby-compatible MT19937; no GC edges so
     * it lives inline in the vm).  Random instances keep their own state in a
     * binary String ivar — see builtins/random.c. */
    KorbMT default_rng;
    bool   default_rng_seeded;
    VALUE  default_rng_seed;   /* last srand seed (for srand's return), FIX 0 initially */

    /* String#pack("P"/"p") side table.  CRuby packs a raw pointer to the
     * string's bytes; under our moving collector a real pointer would dangle,
     * so pack copies the bytes here (malloc'd, never VALUEs → needs no GC root
     * scanning) and embeds a 1-based index in the packed 8 bytes (0 = nil).
     * unpack("P"/"p") recovers the bytes by that index.  Bounded leak, mirrors
     * CRuby pinning the pointed memory; freed only at process exit. */
    char     **pack_ptr_bufs;
    uint32_t  *pack_ptr_lens;
    uint32_t   pack_ptr_count;
    uint32_t   pack_ptr_cap;

    /* Reusable open_memstream for String#% / format / sprintf.  Opening a fresh
     * memstream per call mallocs + zeroes a stdio buffer every time (~35% of a
     * format-heavy loop).  Cache one stream and rewind it instead.  `fmt_busy`
     * guards against re-entry (a nested format via a user #to_s/#inspect) — the
     * nested call falls back to its own open_memstream.  Not a GC edge (libc
     * buffer, no VALUEs); freed only at process exit. */
    FILE   *fmt_stream;
    char   *fmt_buf;
    size_t  fmt_sz;
    bool    fmt_busy;

    const char *script_name; /* for error messages */
    /* require/require_relative: the file currently being loaded (for
     * require_relative's base dir) and the set of already-loaded absolute paths
     * (libc-side strings, no GC). */
    const char *cur_load_file;
    char **loaded_files; uint32_t loaded_cnt, loaded_capa;
    /* Features currently being loaded, with the green thread that owns each
     * load: a concurrent require of the same feature from another thread
     * WAITS (yielding the scheduler) instead of seeing the pre-eval
     * $LOADED_FEATURES mark and returning false (CRuby's per-feature lock). */
    struct korb_load_claim { const char *path; struct korb_thread *owner; }
        *loading; uint32_t loading_cnt, loading_capa;
    /* "other" string encodings (index 3..7 in the header enc field): the interned
     * encoding-name symbol per index (0 = free).  Character-level ops on these
     * raise NotImplementedError; #encoding still round-trips via the name. */
    const char *last_syntax_msg;     /* parse-time SyntaxError detail (static string), NULL if none */
    /* Module#const_source_location: where each (name, owner) constant was assigned. */
    /* owner is keyed by the class's GC-stable `serial` (0 = top-level), not by a
     * VALUE: this table is libc memory the collector does not scan, so a moving
     * GC would otherwise turn a class-owned location into "no location". */
    struct korb_constloc { uint32_t name; uint32_t owner_serial; uint32_t file_sym; uint32_t line; } *constlocs;
    uint32_t constloc_cnt, constloc_capa;
    /* private_constant: (owner, name) pairs unreachable through an explicit
     * `Owner::NAME`.  Small and rarely populated — a linear scan is enough. */
    struct korb_privconst { uint32_t name; VALUE ARO_GC_EDGE owner; } *privconsts;
    uint32_t privconst_cnt, privconst_capa;
    /* deprecate_constant: same shape — reading such a constant warns. */
    struct korb_privconst *deprconsts;
    uint32_t deprconst_cnt, deprconst_capa;
    uint32_t str_enc_names[KORB_STR_ENC_MAX];
    uint32_t str_enc_sb_mask;        /* bit i: index i is a single-byte encoding (byte == character) */
    /* source_location: def/block body NODE → (file symbol, line), populated at
     * parse time.  Node ptrs are immortal (AST); no GC. */
    struct korb_srcloc { struct Node *node; uint32_t file_sym; uint32_t line; } *srclocs;
    uint32_t srcloc_cnt, srcloc_capa;
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
    /* Non-local-return target: the home method frame base a block's `return`
     * should unwind to (NULL = a plain method-level return, consumed by the
     * nearest method).  A method boundary consumes KORB_RETURN only when this is
     * NULL or equals its own frame base, else it propagates — so a block's
     * `return` skips intermediate USER methods (e.g. Enumerable#find via each).
     * It is a transient stack pointer: set the instant a return is raised and
     * cleared when consumed; never read across a GC. */
    VALUE *return_target;
    /* The method entry whose define_method body is running, so a `super` inside
     * that body can find the name and owner it was defined under (a block frame
     * carries no method entry).  Saved/restored around the body call. */
    const struct korb_method *dm_entry;
    /* Break target: a `break` in a block body belongs to the call that was
     * *given* that block, not to whatever intermediate call happens to be
     * running it (`m { break }` where m does `arr.each { b.call(x) }` must
     * return from m, not from each).  The innermost block yield a KORB_BREAK
     * passes through is the one whose body raised it, so that yield claims the
     * break by recording which block entry it came from.  A call site swallows
     * the break only when it handed over that same entry *as a literal block*
     * (a forwarded `&b` never owns it); NULL means unclaimed, which the
     * pre-identity C paths still rely on, so it is treated as "mine".
     * Transient like return_target: set when a break is raised, cleared when
     * consumed, never read across a GC. */
    struct Node *break_blk;
    /* `$!` stack (per-CTX): the chain of exceptions currently being handled, one
     * pushed per active rescue body (top == `$!`).  GC-visited as a root by
     * AROH_VISIT_ROOTS, so parked exceptions survive the body's GC; nesting and
     * restore-on-exit fall out of push/pop. */
    /* `def` inside instance_eval/instance_exec attaches to the receiver's
     * SINGLETON class, not to self's class — the "default definee" CRuby tracks
     * per frame.  KORB_NIL means "use self", which is every other case. */
    VALUE     def_definee;
    /* The cref an eval'd STRING runs under (instance_eval → the receiver's
     * singleton class, class_eval/module_eval → the module itself).  KORB_NIL
     * outside such an eval; a constant assignment with no parse-time cref uses
     * it as the owner, which is how `mod.module_eval("X = 1")` lands in mod. */
    VALUE     eval_cref;
    /* instance_eval/instance_exec rebind self to a non-class receiver, which
     * loses the class scope @@vars resolve against.  CRuby keeps the caller's
     * (block definition's) cref for them; this is that cref, KORB_NIL outside. */
    VALUE     cvar_cref;
    /* refinements active in the running lexical scope: a flat Array
     * [target, refinement, owner_module, ...] (later entries win), KORB_NIL when
     * none.  Saved/restored by every scope-ish construct — see docs/refinements.md. */
    VALUE     refinements;
    /* Scope save stack for `refinements`.  A C local must not hold a VALUE across
     * a GC (the moving collector would leave it stale), so a scope parks the
     * enclosing set here, where the root visitor forwards it. */
    VALUE    *refine_saved;
    uint32_t  refine_n, refine_cap;
    VALUE    *errinfo;
    uint32_t  errinfo_n, errinfo_cap;
    uint32_t  errinfo_live;          /* GC-scan depth: max over the running + all suspended threads */
    VALUE     throw_tag;     /* active `throw` tag while a KORB_THROW unwinds (GC-visited) */
    VALUE    *catch_tags;    /* stack of tags of the active `catch` blocks (GC-visited) */
    uint32_t  catch_n, catch_cap;
    /* Symbol id of the key a hash pattern could not find (0 = none), so the
     * NoMatchingPatternError raised at the end of a `case/in` chain can be
     * upgraded to NoMatchingPatternKeyError.  A plain id, not a VALUE, so it
     * needs no GC root.  Cleared on every top-level korb_pat_match. */
    uint32_t  pat_key_mid;
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
    /* start two cells early: bottom-header frames keep EP at base[-2] and the    \
     * receiver/self at base[-1]; the toplevel frame sits at c->slots so these    \
     * are c->slots[-2]/c->slots[-1].  (Per-frame magic at base[-3] is zeroed on  \
     * the reserve paths and, for non-toplevel frames, lies inside this range.) */ \
    for (VALUE *_p = (c)->slots - 2; _p < _aro_top; _p++) {                  \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, _p);                            \
    }                                                                        \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->super_new_skip);         \
    /* constants (class values) + their defining-module owners are roots too */ \
    for (uint32_t _ci = 0; _ci < (c)->vm->const_cnt; _ci++) {                \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->const_vals[_ci]);     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->const_owners[_ci]);   \
    }                                                                        \
    /* boxed Float-literal pool: forward each box so cached entries stay live */ \
    for (uint32_t _fi = 0; _fi < (c)->vm->flit_cnt; _fi++) {                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->flit_vals[_fi]);      \
    }                                                                        \
    /* frozen String-literal pool: same rule */                              \
    for (uint32_t _si = 0; _si < (c)->vm->fstr_cnt; _si++) {                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->fstr_vals[_si]);      \
    }                                                                        \
    for (uint32_t _pi = 0; _pi < (c)->vm->privconst_cnt; _pi++) {            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->privconsts[_pi].owner); \
    }                                                                        \
    for (uint32_t _di = 0; _di < (c)->vm->deprconst_cnt; _di++) {            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->deprconsts[_di].owner); \
    }                                                                        \
    /* `$!` stack: exceptions being handled.  Scan to errinfo_live, not the
     * current depth — suspended threads keep their own (deeper) depths and
     * their entries must stay rooted while they are away. */                \
    for (uint32_t _xi = 0; _xi < (c)->errinfo_live; _xi++) {                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->errinfo[_xi]);            \
    }                                                                        \
    if ((c)->throw_tag != KORB_NIL)                                          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->throw_tag);              \
    if ((c)->def_definee != KORB_NIL)                                        \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->def_definee);            \
    if ((c)->eval_cref != KORB_NIL)                                          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->eval_cref);              \
    if ((c)->cvar_cref != KORB_NIL)                                          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->cvar_cref);              \
    if ((c)->refinements != KORB_NIL)                                        \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->refinements);            \
    for (uint32_t _ri = 0; _ri < (c)->refine_n; _ri++) {                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->refine_saved[_ri]);       \
    }                                                                        \
    for (uint32_t _ti = 0; _ti < (c)->catch_n; _ti++) {                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->catch_tags[_ti]);         \
    }                                                                        \
    /* Kernel#srand's remembered last seed (may be a Bignum). */             \
    if ((c)->vm->default_rng_seeded)                                         \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->default_rng_seed);    \
    /* global fn entries (immortal libc): forward each entry's owner edge. */ \
    for (uint32_t _mi = 0; _mi < (c)->vm->method_cnt; _mi++) {               \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->methods[_mi]->owner); \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->methods[_mi]->super_owner); \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->methods[_mi]->dm_proc); \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->methods[_mi]->refine_set); \
    }                                                                        \
    /* per-instance class override table: forward both columns in lockstep   \
     * so the (object, class) pairing survives compaction. */                \
    for (uint32_t _si = 0; _si < (c)->vm->sklass_cnt; _si++) {               \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->sklass_obj[_si]);     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->sklass_cls[_si]);     \
    }                                                                        \
    /* generic-ivar side table: forward object + its ivar-hash in lockstep    \
     * so identity keys survive compaction (same rule as sklass). */          \
    for (uint32_t _oi = 0; _oi < (c)->vm->objivar_cnt; _oi++) {              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->objivar_obj[_oi]);    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->objivar_hash[_oi]);   \
    }                                                                        \
    /* open closure envs now live in each frame's EP cell (base[-2]), scanned    \
     * as part of the slot range above — no separate registry. */               \
    /* Enumerator::Yielder class object (KORB_NIL before enum init). */        \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->yielder_class);            \
    /* Enumerator::Lazy class object. */                                       \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->lazy_class);               \
    /* Thread#kill 内部例外 class (KORB_NIL まで未生成)。 */                    \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->thread_kill_exc);          \
    /* cached frozen nil/true/false #to_s strings. */                          \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->str_nil_to_s);             \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->str_true_to_s);            \
    ARO_GC_VISIT_EDGE((ctx), edge_visit, &(c)->vm->str_false_to_s);           \
    /* class pointers may move/reuse this GC → invalidate method caches      \
     * (mcache + node callcaches all validate against method_serial). */     \
    (c)->vm->method_serial++;                                                \
    /* main value-stack, suspended while a fiber runs (active stack scanned   \
     * above as c->slots..slots_top). */                                      \
    if ((c)->vm->running_fiber != NULL && (c)->vm->main_slots != NULL) {      \
        for (VALUE *_p = (c)->vm->main_slots - 2; _p < (c)->vm->main_slots_top; _p++) \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, _p);                         \
    }                                                                        \
    /* every live fiber's transfer/captured_self roots + (suspended) value    \
     * stack.  The rep is libc-stable; the active fiber's stack is the         \
     * c->slots scan above. */                                                \
    for (struct KorbFiberRep *_fr = (c)->vm->fiber_list; _fr; _fr = _fr->link) { \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_fr->transfer);                \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_fr->captured_self);           \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_fr->fibobj);                  \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_fr->storage);                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_fr->tls);                     \
        if (_fr != (c)->vm->running_fiber && _fr->fstate == 2) {              \
            for (VALUE *_p = _fr->vslots - 2; _p < _fr->vslots_top; _p++)         \
                ARO_GC_VISIT_EDGE((ctx), edge_visit, _p);                     \
        }                                                                    \
    }                                                                        \
    /* every green thread rep's roots + (suspended) stack range.  The running \
     * thread's stack is the c->slots scan above; an unstarted thread has no  \
     * stack yet (args root covers its inputs); a DEAD thread's stack is      \
     * freed (result/exc stay as roots). */                                   \
    for (struct korb_thread *_t = (c)->vm->thread_list; _t; _t = _t->next) {  \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->thval);                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->args);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->blk);                       \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->captured_self);             \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->result);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->exc);                       \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->tls);                       \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->tvars);                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->name);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->pending_ints);              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_t->tgroup);                     \
        if (_t != (c)->vm->cur_thread && _t->started && _t->state != KORB_TH_DEAD) { \
            for (VALUE *_p = _t->saved_base - 2; _p < _t->saved_top; _p++)    \
                ARO_GC_VISIT_EDGE((ctx), edge_visit, _p);                     \
        }                                                                    \
    }                                                                        \
} while (0)

#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {         \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);                      \
    switch (_h->flags & KORB_OBJ_TYPE_MASK) {                                \
      case KORB_OBJ_FLOAT:                                                    \
      case KORB_OBJ_STR_BUF:                                                  \
        /* raw double / char[] — no edges */                                 \
        (void)(payload_size);                                               \
        break;                                                               \
      case KORB_OBJ_RATIONAL: {                                              \
        KorbRational *_rt = (KorbRational *)(payload);                       \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_rt->num);                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_rt->den);                     \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_STRING: {                                                \
        KorbString *_s = (KorbString *)(payload);                            \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_s->buf);                  \
        (void)(payload_size);                                                \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_EXCEPTION: {                                             \
        KorbException *_e = (KorbException *)(payload);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->msg);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->exc_class);               \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->cause);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->ivars);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_e->backtrace);                \
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
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &korb_items_data(_ai)[_i]); \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_HASH: {                                                  \
        KorbHash *_hh = (KorbHash *)(payload);                              \
        ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_hh->items);              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_hh->default_val);            \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_hh->default_proc);           \
        if (_hh->index) ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, &_hh->index); /* raw uint32 table */ \
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
      case KORB_OBJ_ENUMERATOR: {                                            \
        KorbEnumerator *_en = (KorbEnumerator *)(payload);                   \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_en->values);                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_en->desc);                   \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_en->source);                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_en->ops);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_en->size_proc);              \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_SET: {                                                   \
        KorbSet *_st = (KorbSet *)(payload);                                 \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_st->elems);                  \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_REGEXP: {                                                \
        KorbRegexp *_re = (KorbRegexp *)(payload);                          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_re->source);                 \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_MATCHDATA: {                                             \
        KorbMatchData *_md = (KorbMatchData *)(payload);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_md->subject);               \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_md->regexp);                \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_md->offsets);              \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_METHOD: {                                                \
        KorbMethod *_m = (KorbMethod *)(payload);                           \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_m->recv);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_m->owner);                   \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_BINDING: {                                              \
        KorbBinding *_b = (KorbBinding *)(payload);                         \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_b->env);                     \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_b->self);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_b->extra);                   \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_b->cref);                    \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_FIBER:                                                   \
      case KORB_OBJ_THREAD:                                                  \
      case KORB_OBJ_MUTEX:                                                   \
      case KORB_OBJ_CONDVAR:                                                 \
        /* handle / libc-ptr のみ (fiber・thread rep は vm list 経由で scan) */ \
        (void)(payload_size);                                               \
        break;                                                               \
      case KORB_OBJ_ARITHSEQ: {                                              \
        KorbArithSeq *_as = (KorbArithSeq *)(payload);                       \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_as->recv);                    \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_as->a0);                      \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_as->a1);                      \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_BIGNUM:                                                  \
        /* no VALUE edges; mpz limbs are external malloc (copied as a raw    \
         * pointer when this object moves). */                               \
        (void)(payload_size);                                                \
        break;                                                               \
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
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->prepended);           \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->members);             \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->cvars);              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->class_ivars);        \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->enclosing);          \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->subclasses);         \
        /* method entries are immortal libc; forward each entry's owner edge. */ \
        for (uint32_t _mi = 0; _mi < _cl->method_cnt; _mi++) {              \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->methods[_mi]->owner); \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->methods[_mi]->super_owner); \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->methods[_mi]->dm_proc); \
            ARO_GC_VISIT_EDGE((ctx), edge_visit, &_cl->methods[_mi]->refine_set); \
        }                                                                  \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_ENV: {                                                   \
        KorbEnv *_ev = (KorbEnv *)(payload);                                \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_ev->prev);   /* odd slots-ptr skipped, KorbEnv* fwd */ \
        if (_ev->closed) ARO_GC_VISIT_EDGE((ctx), edge_visit, &_ev->vals);  /* open: loc->slots root */ \
        (void)(payload_size);                                               \
        break;                                                               \
      }                                                                      \
      case KORB_OBJ_PROC: {                                                  \
        KorbProc *_pr = (KorbProc *)(payload);                              \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_pr->env);    /* odd slots-ptr skipped, KorbEnv* fwd */ \
        ARO_GC_VISIT_EDGE((ctx), edge_visit, &_pr->self);                   \
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

/* Finalizer: free a collected Bignum's external GMP limbs.  Only KORB_OBJ_BIGNUM
 * is registered (aro_gc_finalize_register in korb_big_from_mpz), so the walk only
 * ever hands this macro a dead bignum — the type check is belt-and-suspenders.
 *
 * Also drops the limb bytes from the GC's external-pressure counter (the malloc-
 * increase analogue) so it tracks live external memory.  `c` here is the CTX
 * parameter of aro_gc_finalize_walk — the macro's ONLY expansion site (gc_common.c)
 * — so no GMP allocator hook / process-global is needed (the create site,
 * korb_big_from_mpz, accounts the matching +delta with its own c). */
/* `c` is the CTX parameter of aro_gc_finalize_walk — the macro's ONLY expansion
 * site (gc_common.c) — so finalizers reach the context without a process-global.
 * Each case frees only memory the object OWNS; shared/immortal data (a method
 * entry's body / opt_defaults / kw_info live in the immortal AST; both copies of
 * a define_method'd entry share them) is NOT freed here. */
#define KORB_FINALIZE_BIGNUM_CASE                                              \
      case KORB_OBJ_BIGNUM: {                                                  \
        KorbBignum *_aro_bz = (KorbBignum *)_aro_h;   /* _aro_h = the payload */ \
        aro_gc_account_external(c, -(ssize_t)korb_mp_extbytes(_aro_bz->z));     \
        korb_mp_free(_aro_bz->z);                                              \
        break;                                                                 \
      }
#define AROH_FINALIZE(payload) do {                                            \
    AroObjectHeader *_aro_h = (AroObjectHeader *)(payload);                    \
    switch (_aro_h->flags & KORB_OBJ_TYPE_MASK) {                              \
      case KORB_OBJ_CLASS: {                                                   \
        KorbClass *_aro_cl = (KorbClass *)_aro_h;                             \
        for (uint32_t _mi = 0; _mi < _aro_cl->method_cnt; _mi++)              \
            free(_aro_cl->methods[_mi]);   /* per-class calloc'd entries */    \
        free(_aro_cl->methods);            /* the libc method-ptr array */     \
        break;                                                                 \
      }                                                                        \
      KORB_FINALIZE_BIGNUM_CASE                                                \
      default: break;                                                          \
    }                                                                          \
  } while (0)

/* -----------------------------------------------------------------------------
 * Options
 * --------------------------------------------------------------------------- */
/* $RUBY_DESCRIPTION — the `-v` banner and the RUBY_DESCRIPTION constant must
 * agree (command_line/rubyopt_spec compares them). */
#define KORUBY_RUBY_DESCRIPTION "ruby 4.0.2 (koruby/ASTro) [x86_64-linux]"

struct koruby_option {
    bool plain;          /* --plain: ignore the code store */
    bool compiled_only;  /* --compiled-only: run only baked SDs; poison unswapped bodies (compile-miss detect) */
    bool aot_compile;    /* --aot-compile: run + bake at exit */
    bool pg_compile;     /* --pg-compile: M0 = same bake as AOT */
    bool clear_store;    /* --ccs */
    bool dump_ast;       /* --dump-ast */
    bool quiet;
    bool verbose;
    int  verbose_warn;   /* -w/-W2 → 1 ($VERBOSE=true), -W0 → -1 (nil), 0 = default */
    bool debug;          /* -d / --debug: $DEBUG = true */
    bool frozen_literals;/* --enable-frozen-string-literal */
    bool no_rubyopt;     /* --disable-rubyopt / --disable-all: ignore $RUBYOPT */
    bool switch_args;    /* -s: leading -name[=value] args become globals */
    bool loop_gets;      /* -n / -p: wrap the program in a `while gets` loop */
    bool loop_print;     /* -p: also print $_ each iteration */
    bool auto_split;     /* -a: $F = $_.split each iteration */
    bool chomp_lines;    /* -l: chomp $_ and set $\ = $/ */
    bool skip_to_ruby;   /* -x: ignore everything before the first #!...ruby line */
    bool search_path;    /* -S: look the script up in $RUBYPATH, then $PATH */
    bool syntax_check;   /* -c: parse only, print "Syntax OK" */
    bool rec_sep_given;  /* -0[octal] was given: rec_sep/rec_sep_len are $/ (NULL = nil) */
    const char *rec_sep;
    uint32_t rec_sep_len;
    bool no_deprecated;  /* -W:no-deprecated */
    bool no_experimental;/* -W:no-experimental */
    const char *kcode;   /* -K<letter>: the default external encoding name, or NULL */
    const char *extenc;  /* -E ext[:int] external encoding name, or NULL */
    const char *intenc;  /* -E's internal encoding name, or NULL */

    /* referenced by framework-generated ALLOC_ helpers */
    bool record_all;
};

/* libastrogre.so has its own global `OPTION` (struct astrogre_option), and the
 * interpreter is linked -rdynamic, so ours preempted the library's: astrogre
 * then read koruby's fields.  `-w` / `-W0` landed on its cs_verbose and spammed
 * "cs_miss: …" onto stderr from every regex.  Give ours a private ABI name; the
 * source (main.c, generated ALLOC_ helpers) keeps writing OPTION. */
#define OPTION korb_option
extern struct koruby_option OPTION;

#endif /* KORUBY_CONTEXT_H */
