#ifndef ASCHEME_CONTEXT_H
#define ASCHEME_CONTEXT_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "astro_debug.h"
#include <setjmp.h>
#include <alloca.h>
#include <gmp.h>

// ASTro precise GC framework.  Migration plan: docs/migration.md
#include "precise_gc/gc_types.h"   /* AroObjectHeader, AroGcCommonState */

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
    /* OBJ_FRAME — struct sframe; allocated via aro_gc_alloc.  Used so the
     * framework's SCAN_EDGES dispatch can identify a frame payload and
     * walk its parent + slots.  sframe layout starts with AroObjectHeader
     * at offset 0 (same as struct sobj), so the framework can read head.flags
     * via either cast. */
    OBJ_FRAME,

    /* OBJ_VEC_BACKING — backing payload buffer for OBJ_VECTOR / OBJ_MVALUES.
     * Layout: AroObjectHeader head; VALUE items[N] inline.  N is derived
     * from head.gc_size at scan time, so this type lets the framework scan
     * the items array WITHOUT needing the parent vector sobj (= which is
     * essential for mark_compact: in step 3 the parent's vec.items is
     * being updated to the post-slide location, but the items DATA stays
     * at the OLD location until slide step 5; scanning items[i] via the
     * backing buffer's own header keeps reader and data co-located).  */
    OBJ_VEC_BACKING,
};

struct sobj;
struct sframe;
struct CTX_struct;
struct Node;
struct ASTroGC;

typedef VALUE (*scm_prim_fn)(struct CTX_struct *c, int argc, VALUE *argv);

// Continuation state lives behind a pointer rather than inside `sobj`'s
// union — its `jmp_buf` is ~200 B on Linux x86_64 and would otherwise
// inflate every cons cell, vector, and closure to that size.  After this
// split, sizeof(struct sobj) is ~40 B (dominated by the GMP `mpq_t`).
struct scont {
    /* head: AroObjectHeader at offset 0 — required by the framework for
     * any aro_gc_alloc'd payload (size + flags + fwd live in this header).
     * Without it, moving GCs would read random bytes from jmp_buf as a
     * header and corrupt their bookkeeping.  flags low bits stay zero;
     * we don't dispatch on scont via head.flags (the owning sobj OBJ_CONT
     * case walks it directly). */
    AroObjectHeader head;
    jmp_buf buf;
    VALUE   result;
    /* Saved CTX state captured at call/cc entry.  These fields hold C-local
     * temporaries that would otherwise be invisible to the precise root
     * scanner across the `scm_apply(c, fn, ...)` call below — moving GCs
     * would leave the local copies stale.  Stashed in the scont (which
     * IS a scanned root via the OBJ_CONT SCAN_EDGES case) so they survive
     * arbitrary inner GC activity. */
    struct sframe *saved_env;
    VALUE   k_val;          /* the continuation VALUE itself */
    VALUE   fn_val;          /* the user-supplied procedure */
    int     saved_tcp;
    int     active;
    int     tag;
};

/* struct sobj head field: AroObjectHeader at offset 0 (iter 75 contract).
 * head.flags low 5 bits hold ascheme's obj_type tag (= ~20 types fit in 5
 * bits).  Higher head.flags bits are sample-reserved for future use.
 * head.gc_flags / gc_size are framework-controlled. */
#define SCM_TYPE_MASK  0x1Fu
#define SCM_TYPE(o)    ((int)((o)->head.flags & SCM_TYPE_MASK))
#define SCM_SET_TYPE(o, t)  ((o)->head.flags = (uint16_t)((o)->head.flags & ~SCM_TYPE_MASK) | (uint16_t)(t))

struct sobj {
    AroObjectHeader head;
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
            bool leaf;        // body has no inner `lambda` — safe to reuse frame on self-tail-call
            bool no_capture;  // body has no lref/lset crossing this lambda boundary — sframe alloc skippable, locals on sp[]
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
        struct scont *cont;
    };
};

/* struct sframe head: AroObjectHeader at offset 0 — same layout contract
 * as struct sobj.  Type tag = OBJ_FRAME (in head.flags low 5 bits).  This
 * lets SCAN_EDGES dispatch uniformly over `void *payload` regardless of
 * whether the payload is a sobj or sframe. */
struct sframe {
    AroObjectHeader head;
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

/* `name_payload` points to the BASE of a heap-allocated byte payload
 * (= aro_gc_alloc_byte result, i.e. the AroObjectHeader).  The actual
 * C string starts at name_payload + sizeof(AroObjectHeader).  Storing
 * the base (not an interior pointer past the header) is essential for
 * moving GCs: the forward callback expects a payload-base slot so it
 * can read the header to compute size / lookup forwarding addresses. */
struct gentry {
    char  *name_payload;             /* byte-payload base, NULL = empty slot */
    VALUE  value;
    bool   defined;
};
#define GENTRY_NAME(ge) \
    ((const char *)((ge).name_payload \
                    ? ((ge).name_payload + sizeof(AroObjectHeader)) \
                    : NULL))

// Inline cache stamped at every node_gref call site.  Stored as `@ref`
// (embedded in the NODE union, not on the structural hash).  `cached`
// goes from 0 → 1 once we've resolved the name; the index is stable
// across the lifetime of `c->globals` (we never remove globals, and
// realloc keeps numeric positions intact even when the buffer moves).
// Global rebinding generation counter — bumped by `scm_global_define` /
// `scm_global_set` on any name.  Inline caches that snapshot the value
// only need to refresh when their stored serial is out of date, which
// in benchmark code is approximately never (most set!s happen at init,
// then the loop runs).
struct gref_cache {
    uint64_t serial;
    VALUE    value;
};

// Inline cache for the specialized arithmetic / comparison nodes.  Each
// such node also baked an "expected" prim sobj at install_prims time
// (e.g. PRIM_PLUS_VAL); the EVAL body confirms `c->globals[index].value
// == PRIM_<op>_VAL` before taking the fast path.  When the user does
// `(set! + my-add)` the global value at this index changes, the check
// fails, and we fall back to a regular `scm_apply` against whatever the
// global now points to — preserving R5RS semantics.
// Arith-cache: store the resolved global VALUE directly, gated on the
// same `globals_serial`.  The fast path is just two 8-byte loads + a
// pointer compare — no array indirection through `c->globals`.
struct arith_cache {
    uint64_t serial;
    VALUE    value;
};

// The original primitive sobj for each specialized operator.  Set by
// install_prims; checked by node_arith_* / node_pred_* / node_vec_* on
// every call to detect user redefinition.  Definitions live in builtin.c.
extern VALUE PRIM_PLUS_VAL, PRIM_MINUS_VAL, PRIM_MUL_VAL;
extern VALUE PRIM_NUM_LT_VAL, PRIM_NUM_LE_VAL, PRIM_NUM_GT_VAL, PRIM_NUM_GE_VAL, PRIM_NUM_EQ_VAL;
extern VALUE PRIM_NULL_P_VAL, PRIM_PAIR_P_VAL, PRIM_CAR_VAL, PRIM_CDR_VAL, PRIM_NOT_VAL;
extern VALUE PRIM_VECTOR_REF_VAL, PRIM_VECTOR_SET_VAL;
extern VALUE PRIM_CONS_VAL, PRIM_EQ_P_VAL, PRIM_EQV_P_VAL;

// Lazy parent-chain cache used by node_lref / node_lset for depth >= 1.
// `env_chain[0] = env, [1] = env->parent, ..., env_chain[env_chain_filled]`
// is the highest already-resolved level.
//
// Cache key is `env_serial` — a counter bumped on every assignment to
// `c->env`.  Pointer-equality on `c->env` would NOT be safe: alloca'd
// frames (leaf-closure path) get their stack memory recycled when the
// containing C call returns, so the same address can host a different
// frame later, with a different parent chain.  The serial sidesteps that.
// Self-tail-call frame-reuse (which keeps c->env the same and merely
// overwrites slots) skips the bump, so the cache stays warm across the
// hot tail-call loops it's meant to accelerate.
#define ASCHEME_LREF_CACHE_SIZE 8

// Max arity for parser-recognized self-tail-call to a named-let / single-
// binding letrec.  Higher arities fall back to the generic call_K +
// scm_apply_tail trampoline.
#define ASCHEME_LOOP_MAX_PARAMS 8

typedef struct CTX_struct {
    // ASTro precise GC framework: process-scope GC instance.  Backend
    // defines `struct ASTroGC` internally (must start with `AroGcCommonState
    // common` for ARO_GC_COMMON() cast to work).  Allocated by
    // aro_gc_init() and freed by aro_gc_fini().
    struct ASTroGC *astro_gc;

    // Framework-required spill-stack top.  ascheme uses lexical envs (=
    // c->env) rather than a flat sp-style VALUE stack, so this field is
    // never written by the sample, only by the GC's collect/alloc paths
    // (which read/write c->sp as part of the alloc contract).  Kept NULL.
    VALUE *sp;

    // Current lexical environment chain (closures + call frames).
    struct sframe *env;

    // Global definitions: linear array (small N for now).
    struct gentry *globals;
    size_t globals_size;
    size_t globals_capa;
    // Bumped by every define/set! so inline caches keyed off `globals`
    // know when to re-validate.  Starts at 1 so a memset-zero cache is
    // unambiguously "uninitialised".
    uint64_t globals_serial;

    // Tail-call trampoline state (set by tail-position call nodes).
    struct Node *next_body;
    struct sframe *next_env;
    int tail_call_pending;

    // Lazy parent-chain cache for lref/lset depth >= 1.  See block comment
    // above the typedef.  env_serial bumps on every env switch.
    uint64_t       env_serial;
    uint64_t       env_cache_serial;            // env_serial when cache was built
    struct sframe *env_chain[ASCHEME_LREF_CACHE_SIZE];
    uint32_t       env_chain_filled;            // highest index already valid

    // Self-tail-call loop signaling.  node_self_tail_call_K (parser-
    // emitted for named-let / single-binding letrec self-recursive tail
    // calls) evaluates its new args into loop_args[] and sets
    // loop_continue=1; the enclosing node_loop catches the flag, copies
    // the args back into the current frame's slots, and re-enters its
    // body — turning the trampoline tail loop into a tight C for(;;)
    // with no scm_apply re-entry.  loop_continue stays 0 outside this
    // protocol.
    uint32_t loop_continue;
    VALUE    loop_args[ASCHEME_LOOP_MAX_PARAMS];

    // call/cc tag generator + active continuations stack.
    int cont_tag_seq;

    // Top-level error escape.
    jmp_buf err_jmp;
    int err_jmp_active;
} CTX;

// ---------------------------------------------------------------------------
// Sample-side root-spill stack.  Per-function precise rooting for VALUE
// temporaries that must survive an inner allocation.  Pattern:
//
//   SP_PUSH(c, sp, n)        — reserve `n` slots starting at sp[0..n-1],
//                              zero-init them so the root scanner doesn't
//                              see uninit'd ptr bits.  Pushes c->sp by n.
//   sp[i] = ...              — park / read VALUEs across allocations.
//   SP_POP(c, sp)            — restore c->sp.
//
// The scratch range walked by `aro_scheme_visit_roots` is
// [g_sp_scratch, c->sp), so any slot inside that range is a precise root.
// Backend independent — non-moving GCs just mark, moving GCs forward and
// rewrite the slot.  Singletons / immediates / NULL are filtered by the
// AROH_IS_GC_OBJECT macro in the visitor.
extern VALUE g_sp_scratch[];
#define ASCHEME_SP_SCRATCH_SIZE 4096
// 3-arg dispatcher: `sp` is a function parameter in every NODE_DEF body.
// SP_PUSH zero-fills [sp..sp+n) and sets c->sp = sp + n so GC root scan
// covers the slots.  No local declaration — the caller already has `sp`
// in scope (= dispatcher parameter, or `VALUE *sp = c->sp;` at the entry
// of non-NODE_DEF functions like scm_apply / scm_callcc).
#define SP_PUSH(c, name, n) \
    do { \
        ASTRO_ASSERT((name) + (n) <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE); \
        for (int _spi = 0; _spi < (n); _spi++) (name)[_spi] = 0; \
        (c)->sp = (name) + (n); \
    } while (0)
#define SP_POP(c, name)    do { (c)->sp = (name); } while (0)

// Always switch env through this macro so env_serial gets bumped (which
// invalidates the lref level cache).  The frame-reuse path in
// scm_apply_tail intentionally bypasses this — it overwrites slots in
// place, c->env doesn't change, and the cache stays valid.
#define CTX_SET_ENV(c, new_env) do { \
    (c)->env = (new_env); \
    (c)->env_serial++; \
} while (0)

// Singleton scheme values, initialized at INIT().
extern struct sobj S_NIL_OBJ, S_TRUE_OBJ, S_FALSE_OBJ, S_UNSPEC_OBJ, S_EOF_OBJ;
#define SCM_NIL      SCM_OBJ_VAL(&S_NIL_OBJ)
#define SCM_TRUE     SCM_OBJ_VAL(&S_TRUE_OBJ)
#define SCM_FALSE    SCM_OBJ_VAL(&S_FALSE_OBJ)
#define SCM_UNSPEC   SCM_OBJ_VAL(&S_UNSPEC_OBJ)
#define SCM_EOFV     SCM_OBJ_VAL(&S_EOF_OBJ)

// Type predicates (work for both immediates and heap objects).
static inline bool scm_is_pair(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_PAIR; }
static inline bool scm_is_null(VALUE v) { return v == SCM_NIL; }
static inline bool scm_is_true(VALUE v) { return v != SCM_FALSE; }   // R5RS: only #f is false
static inline bool scm_is_false(VALUE v) { return v == SCM_FALSE; }
static inline bool scm_is_bool(VALUE v) { return v == SCM_TRUE || v == SCM_FALSE; }
static inline bool scm_is_symbol(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_SYMBOL; }
static inline bool scm_is_string(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_STRING; }
static inline bool scm_is_char(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_CHAR; }
static inline bool scm_is_vector(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_VECTOR; }
static inline bool scm_is_closure(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_CLOSURE; }
static inline bool scm_is_prim(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_PRIM; }
// "double" covers both inline flonums and the heap-allocated OBJ_DOUBLE.
// scm_is_heap_double matches only the latter — used by `scm_get_double`
// and friends to decide whether to dereference.
static inline bool scm_is_heap_double(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_DOUBLE; }
static inline bool scm_is_double(VALUE v) { return SCM_IS_FLONUM(v) || scm_is_heap_double(v); }
static inline bool scm_is_bignum(VALUE v)  { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_BIGNUM; }
static inline bool scm_is_rational(VALUE v){ return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_RATIONAL; }
static inline bool scm_is_complex(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_COMPLEX; }
static inline bool scm_is_mvalues(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_MVALUES; }
static inline bool scm_is_promise(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_PROMISE; }
static inline bool scm_is_port(VALUE v)    { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_PORT; }
static inline bool scm_is_cont(VALUE v) { return SCM_IS_PTR(v) && SCM_TYPE(SCM_PTR(v)) == OBJ_CONT; }
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

// Object-construction helpers (defined in main.c).  All take CTX so the
// precise GC framework knows which heap instance to allocate from.
struct sobj *scm_alloc(CTX *c, int type);
VALUE scm_cons(CTX *c, VALUE a, VALUE d);
VALUE scm_intern(CTX *c, const char *name);
VALUE scm_make_string(CTX *c, const char *s, size_t len);
VALUE scm_make_string_n(CTX *c, size_t len, char fill);
VALUE scm_make_char(CTX *c, uint32_t cp);
VALUE scm_make_vector(CTX *c, size_t len, VALUE fill);
VALUE scm_make_double(CTX *c, double d);
VALUE scm_make_int(CTX *c, int64_t v);          // fixnum or bignum if overflows
VALUE scm_make_bignum_z(CTX *c, mpz_srcptr z);  // copy mpz_t into a fresh bignum sobj
VALUE scm_make_rational_q(CTX *c, mpq_srcptr q);
VALUE scm_make_rational_zz(CTX *c, mpz_srcptr num, mpz_srcptr den);
VALUE scm_make_complex(CTX *c, double re, double im);
VALUE scm_make_mvalues(CTX *c, int count, VALUE *items);
VALUE scm_make_closure(CTX *c, struct Node *body, struct sframe *env, int nparams, int has_rest);
VALUE scm_make_prim(CTX *c, const char *name, scm_prim_fn fn, int min_argc, int max_argc);
double scm_get_double(VALUE v);          // converts any numeric to C double
VALUE scm_normalize_int(CTX *c, mpz_srcptr z);   // → fixnum if fits, bignum otherwise
VALUE scm_normalize_rat(CTX *c, mpq_t q);        // → fixnum / bignum / rational
VALUE scm_simplify_complex(CTX *c, double re, double im);  // im=0 ⇒ real

// Frame helpers.
struct sframe *scm_new_frame(CTX *c, struct sframe *parent, int nslots);

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

// ---------------------------------------------------------------------------
// builtin.c-side exports (= primitive table + standard-port globals).
// install_prims (= main.c, startup) iterates PRIM_TABLE and wires the three
// standard ports via port_make.  aro_scheme_visit_roots (= main.c, root
// visit) walks PORT_STDIN/STDOUT/STDERR so the libc-FILE port sobj's stay
// alive across GC.
// ---------------------------------------------------------------------------

// Primitive table — populated in builtin.c, scanned by install_prims (main.c)
// at startup.  Terminated by a sentinel entry with `name == NULL`.
struct prim_entry {
    const char *name;
    scm_prim_fn fn;
    int min_argc, max_argc;
};
extern struct prim_entry PRIM_TABLE[];

// Wrap a libc FILE* in an OBJ_PORT sobj.  Defined in builtin.c; called by
// install_prims (= main.c) to set up the three standard ports.
VALUE port_make(CTX *c, FILE *fp, bool input, bool owned);

// Standard ports — heap-allocated port sobj's, populated by install_prims.
// Kept as program-globals (not via c->globals) and walked by
// aro_scheme_visit_roots so a moving GC can relocate them.
extern VALUE PORT_STDIN, PORT_STDOUT, PORT_STDERR;

// ---------------------------------------------------------------------------
// Precise-GC integration: SCAN_EDGES + AROH_IS_GC_OBJECT for sample-side filtering.
// ---------------------------------------------------------------------------

/* Accessor used by every backend (= ASTroGC *) — points at CTX's astro_gc
 * field set up at aro_gc_init time.  Each backend typedefs `ASTroGC` as
 * its own concrete struct, matching the forward-decl `struct ASTroGC`
 * stored in CTX. */
#define ARO_GC_INSTANCE(c)  ((c)->astro_gc)

/* Root visitor contract — sample が framework に提供する。 ascheme は
 * roots が散在する (sframe chain, globals, loop_args, symbol table,
 * stdports, ...) ので macro 1 行に押し込まず、 sample-local function
 * `aro_scheme_visit_roots` (main.c 実装) を呼ぶ形にする。 詳細は
 * runtime/precise_gc/gc.h の AROH_VISIT_ROOTS 説明を参照。 */
void aro_scheme_visit_roots(CTX *c, void *gc,
                            void (*edge_visit)(void *, void **));
#define AROH_VISIT_ROOTS(c, ctx, edge_visit) \
    aro_scheme_visit_roots((c), (ctx), (edge_visit))


/* Statically-allocated singleton sobj's live in the program's data segment,
 * not on the GC heap.  Their addresses are valid `struct sobj *` values but
 * the GC framework must NOT attempt to mark / set bitmap bits / forward
 * them (= would write into pages they don't belong to).  We expose the
 * test as inline so it folds away when v is a known-tagged immediate. */
static inline bool
scm_is_singleton(VALUE v)
{
    return v == SCM_OBJ_VAL(&S_NIL_OBJ)
        || v == SCM_OBJ_VAL(&S_TRUE_OBJ)
        || v == SCM_OBJ_VAL(&S_FALSE_OBJ)
        || v == SCM_OBJ_VAL(&S_UNSPEC_OBJ)
        || v == SCM_OBJ_VAL(&S_EOF_OBJ);
}

/* AROH_IS_GC_OBJECT — framework-facing predicate.  Called by every backend's
 * mark_value / forward_value to skip values that aren't GC-managed heap
 * pointers.  In ascheme that means fixnums, inline flonums, AND the
 * five process-static singletons (which look like real pointers but
 * aren't in any GC page).  NULL (= 0) also passes SCM_IS_PTR so we
 * filter it explicitly — uninit'd loop_args slots / sp scratch slots
 * are all zero and must not be visited. */
#define AROH_IS_GC_OBJECT(v)   ((v) != 0 && SCM_IS_PTR(v) && !scm_is_singleton((VALUE)(v)))

/* Helper: visit one VALUE slot, but skip framework-side dispatch for
 * singletons (they look like ptrs to SCM_IS_PTR but the GC framework
 * doesn't manage them).  Used by both SCAN_EDGES and root visit.
 *
 * ascheme stores VALUE bits as RAW heap pointers (= aro_gc_alloc_raw +
 * SCM_OBJ_VAL).  ARO_GC_VISIT_EDGE assumes scrambled storage and XORs
 * with scramble_R; using it here would corrupt the slot value on
 * scramble backends AND would SEGV on non-scramble backends that
 * legitimately pass ctx=NULL into SCAN_EDGES (= mark_compact's
 * update_pointers).  Route through ARO_GC_VISIT_EDGE_PTR (= raw slot,
 * forward via fn directly) and filter singletons / immediates here. */
#define ASCHEME_VISIT_VAL_SLOT(ctx, fn, slot_ptr) do {                       \
    VALUE *_avs = (VALUE *)(slot_ptr);                                       \
    VALUE  _av  = *_avs;                                                     \
    if (SCM_IS_PTR(_av) && _av != 0 && !scm_is_singleton(_av)) {             \
        ARO_GC_VISIT_EDGE_PTR((ctx), (fn), (void **)_avs);                   \
    }                                                                         \
} while (0)

/* For symbols / strings, the C-string pointer is INTERIOR (= one header
 * past the byte payload base).  Moving GCs forward a typed-ptr by reading
 * its header at offset 0; an interior pointer would corrupt the dispatch.
 * Workaround: compute the base, visit it (= framework forwards if needed),
 * then re-derive the interior pointer. */
#define ASCHEME_VISIT_INTERIOR_CHAR_SLOT(ctx, fn, slot_ptr) do {              \
    char **__s = (char **)(slot_ptr);                                         \
    if (*__s) {                                                               \
        char *__base = *__s - sizeof(AroObjectHeader);                      \
        ARO_GC_VISIT_EDGE_PTR((ctx), (fn), (void **)&__base);               \
        *__s = __base + sizeof(AroObjectHeader);                            \
    }                                                                          \
} while (0)

/* SCAN_EDGES — invoked by the framework on every live heap object's
 * payload.  Dispatch on head.flags & SCM_TYPE_MASK and walk inner
 * VALUE / typed-ptr slots.  Scalar-only types (numbers, chars, etc.)
 * fall through to default and have nothing to do. */
#define AROH_SCAN_EDGES(payload, payload_size, ctx, edge_visit) do {     \
    AroObjectHeader *_h = (AroObjectHeader *)(payload);                  \
    (void)(payload_size);                                                     \
    switch (_h->flags & SCM_TYPE_MASK) {                                      \
      case OBJ_PAIR: {                                                        \
          struct sobj *_o = (struct sobj *)(payload);                         \
          ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->pair.car);           \
          ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->pair.cdr);           \
          break;                                                              \
      }                                                                       \
      case OBJ_SYMBOL: {                                                      \
          struct sobj *_o = (struct sobj *)(payload);                         \
          ASCHEME_VISIT_INTERIOR_CHAR_SLOT((ctx), edge_visit, &_o->sym.name);  \
          break;                                                              \
      }                                                                       \
      case OBJ_STRING: {                                                      \
          struct sobj *_o = (struct sobj *)(payload);                         \
          ASCHEME_VISIT_INTERIOR_CHAR_SLOT((ctx), edge_visit, &_o->str.chars); \
          break;                                                              \
      }                                                                       \
      case OBJ_VECTOR: {                                                      \
          struct sobj *_o = (struct sobj *)(payload);                         \
          /* items[] payload base is `items - sizeof(header)`.  We ONLY      \
           * forward the items_base reference here; the items[i] iteration  \
           * happens inside OBJ_VEC_BACKING's own SCAN_EDGES (= called when  \
           * the framework visits the items_base object directly).  This    \
           * decoupling matters for mark_compact: in step 3 (update_pointers)\
           * the parent's vec.items is updated to the POST-slide location,  \
           * but slide doesn't happen until step 5 — so the post-slide      \
           * location has uninitialized memory.  By walking items[i] via    \
           * the backing buffer's own SCAN_EDGES, reader and data stay      \
           * co-located: mark_compact reads OLD location (data still there),\
           * copy backends read NEW (post-memcpy) location.                  */\
          if (_o->vec.items) {                                                \
              char *__base = (char *)_o->vec.items                             \
                             - sizeof(AroObjectHeader);                     \
              ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, (void **)&__base);   \
              _o->vec.items = (VALUE *)(__base + sizeof(AroObjectHeader));  \
          }                                                                   \
          break;                                                              \
      }                                                                       \
      case OBJ_VEC_BACKING: {                                                 \
          /* Backing payload for OBJ_VECTOR / OBJ_MVALUES.  Length derived  \
           * from gc_size (= sizeof(header) + N * sizeof(VALUE)).            \
           * Reader is co-located with data, so this works for both         \
           * mark_compact (data at OLD until slide) and copying backends    \
           * (data at NEW post-memcpy).                                       */\
          AroObjectHeader *__bh = (AroObjectHeader *)(payload);               \
          size_t __n = ((__bh)->gc_size - sizeof(AroObjectHeader))           \
                       / sizeof(VALUE);                                        \
          VALUE *__items = (VALUE *)((char *)(payload)                         \
                                     + sizeof(AroObjectHeader));             \
          for (size_t _i = 0; _i < __n; _i++) {                               \
              ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &__items[_i]);        \
          }                                                                   \
          break;                                                              \
      }                                                                       \
      case OBJ_CLOSURE: {                                                     \
          struct sobj *_o = (struct sobj *)(payload);                         \
          ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit,                          \
                                   (void **)&_o->closure.env);                \
          /* body is a host-side NODE *, not GC-managed. */                  \
          break;                                                              \
      }                                                                       \
      case OBJ_PROMISE: {                                                     \
          struct sobj *_o = (struct sobj *)(payload);                         \
          ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->promise.thunk);      \
          ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->promise.value);      \
          break;                                                              \
      }                                                                       \
      case OBJ_MVALUES: {                                                     \
          /* Same pattern as OBJ_VECTOR: only forward the items_base ref;    \
           * the items[i] iteration is done by OBJ_VEC_BACKING.               */\
          struct sobj *_o = (struct sobj *)(payload);                         \
          if (_o->mv.items) {                                                 \
              char *__mb = (char *)_o->mv.items                                \
                           - sizeof(AroObjectHeader);                       \
              ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, (void **)&__mb);     \
              _o->mv.items = (VALUE *)(__mb + sizeof(AroObjectHeader));     \
          }                                                                   \
          break;                                                              \
      }                                                                       \
      case OBJ_CONT: {                                                        \
          struct sobj *_o = (struct sobj *)(payload);                         \
          /* `cont` itself is a separately-allocated heap obj (aro_gc_alloc); \
           * forward the typed-ptr so a moving GC relocates the scont body.  \
           * Then walk the scanned fields inside it. */                      \
          ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit, (void **)&_o->cont);     \
          if (_o->cont) {                                                     \
              ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->cont->result);   \
              ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->cont->k_val);    \
              ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_o->cont->fn_val);   \
              if (_o->cont->saved_env) {                                      \
                  ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit,                  \
                                           (void **)&_o->cont->saved_env);    \
              }                                                               \
          }                                                                   \
          break;                                                              \
      }                                                                       \
      case OBJ_FRAME: {                                                       \
          struct sframe *_f = (struct sframe *)(payload);                     \
          ARO_GC_VISIT_EDGE_PTR((ctx), edge_visit,                          \
                                   (void **)&_f->parent);                     \
          for (int _i = 0; _i < _f->nslots; _i++) {                           \
              ASCHEME_VISIT_VAL_SLOT((ctx), edge_visit, &_f->slots[_i]);      \
          }                                                                   \
          break;                                                              \
      }                                                                       \
      /* OBJ_STRING / OBJ_SYMBOL / OBJ_CHAR / OBJ_DOUBLE / OBJ_BIGNUM /        \
       * OBJ_RATIONAL / OBJ_COMPLEX / OBJ_PORT / OBJ_PRIM — no scannable       \
       * VALUE / sframe slots (mpz/mpq bytes / char buffer are byte payloads  \
       * which the framework's BYTE / opaque categories handle separately).   \
       * OBJ_NIL / OBJ_BOOL / OBJ_UNSPEC / OBJ_EOF are singletons that should  \
       * not reach SCAN_EDGES anyway (AROH_IS_GC_OBJECT filters them at root entry). */  \
      default: break;                                                          \
    }                                                                          \
} while (0)

/* Scan-safe init: aro_gc_alloc zero-fills the payload so a GC scan
 * triggered immediately after alloc sees no stale ptr bits.  We zero
 * everything AFTER the head — head's gc_size/gc_flags were set by the
 * backend's alloc and must survive. */
#define AROH_INIT_PAYLOAD(payload, size_bytes)                            \
    memset((char *)(payload) + sizeof(AroObjectHeader), 0,                  \
           (size_bytes) - sizeof(AroObjectHeader))

/* Byte payload init: GC never scans these, so skip memset.  Caller fills
 * the bytes before any further alloc. */
#define AROH_INIT_BYTE_PAYLOAD(payload, size_bytes) ((void)0)

/* Finalize hook — invoked by the framework's aro_gc_finalize_walk on a
 * payload that the backend's aro_gc_finalize_check reported as dead.
 *
 * We register OBJ_BIGNUM / OBJ_RATIONAL payloads at alloc time (see
 * scm_make_bignum_z / scm_make_rational_q in main.c).  The matching
 * finalize releases GMP's libc-malloc'd internal limb buffers — without
 * this, every collected bignum/rational leaks its mpz/mpq backing store.
 *
 * Other GC-managed payloads (closure env / vec / string chars / etc.)
 * are tracked by the framework directly and need no finalize.  OBJ_PORT
 * is intentionally not finalized here (its FILE * lifecycle is managed
 * explicitly via port-close); revisit if we add unowned-port semantics. */
extern size_t aro_finalize_calls;   /* debug counter */
#define AROH_FINALIZE(payload) do {                                       \
    AroObjectHeader *_aro_h = (AroObjectHeader *)(payload);               \
    struct sobj      *_aro_o = (struct sobj *)(payload);                      \
    aro_finalize_calls++;                                                      \
    switch ((int)(_aro_h->flags & SCM_TYPE_MASK)) {                           \
      case OBJ_BIGNUM:   mpz_clear(_aro_o->mpz); break;                       \
      case OBJ_RATIONAL: mpq_clear(_aro_o->mpq); break;                       \
      default: break;                                                          \
    }                                                                          \
} while (0)

/* Header layout accessors (framework default for non-moving backends). */
#define AROH_HEADER_SIZE(h)         ((h)->gc_size)
#define AROH_HEADER_SET_SIZE(h, s)  ((h)->gc_size = (uint32_t)(s))
#define AROH_HEADER_GET_FWD(h)      ((h)->gc_fwd)
#define AROH_HEADER_SET_FWD(h, p)   ((h)->gc_fwd = (p))

#endif
