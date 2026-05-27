// ascheme — R5RS Scheme on ASTro.
//
// Layout: this single TU bundles the runtime (heap, primitives,
// reader, error handler) and the front-end (compiler from s-expr →
// AST + main driver).  Generated dispatchers live in node_*.c (built
// from node.def by ASTroGen) and are pulled in through node.c.

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include "context.h"
#include "precise_gc/gc.h"
#include "node.h"
#include "parse.h"
#include "astro_code_store.h"
#include "astro_build.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct ascheme_option OPTION;

// ---------------------------------------------------------------------------
// Singleton heap objects (immediates).
// ---------------------------------------------------------------------------

// Singletons: type tag lives in head.flags low 5 bits (see SCM_TYPE / SCM_SET_TYPE).
struct sobj S_NIL_OBJ    = { .head = { .flags = OBJ_NIL } };
struct sobj S_TRUE_OBJ   = { .head = { .flags = OBJ_BOOL }, .b = true };
struct sobj S_FALSE_OBJ  = { .head = { .flags = OBJ_BOOL }, .b = false };
struct sobj S_UNSPEC_OBJ = { .head = { .flags = OBJ_UNSPEC } };
struct sobj S_EOF_OBJ    = { .head = { .flags = OBJ_EOF } };

// ---------------------------------------------------------------------------
// Heap.  Phase 1 of the libgc → ASTro precise GC migration (docs/migration.md):
// all allocations now go through `aro_gc_alloc_raw(c, sz)` / `aro_gc_alloc_byte_raw(c,
// sz)`.  The default GC backend is `none` (libc malloc, no collection), so
// behaviour matches the original libgc-with-no-pressure build.  Switching to
// a precise backend later requires Phase 2 work (sp/env tracking, SCAN_EDGES).
//
// GMP's allocator hooks have a fixed C signature that can't carry a CTX;
// `gmp_g_ctx` is the one and only sample-global "g_ctx" — a necessary evil
// for GMP's API, set once at main_init and never re-read elsewhere.
// ---------------------------------------------------------------------------

static CTX *gmp_g_ctx = NULL;

/* GMP allocator hooks.  GMP holds raw pointers into the buffers it
 * returns (= mpz->_mp_d), and the framework can't safely track those:
 *
 *   - Non-moving GC + GMP buffer on GC heap: OBJ_BIGNUM is reachable
 *     but the buffer has no SCAN_EDGES edge → sweep frees the buffer →
 *     next mpz access SEGVs.
 *   - Moving GC + GMP buffer on GC heap: forwarded buffer addr is not
 *     known to mpz_t._mp_d → stale pointer.
 *
 * Resolution: route GMP through libc malloc.  Buffers are fixed in
 * place and outside the framework's view.  When OBJ_BIGNUM becomes
 * unreachable, the framework's finalizer (AROH_FINALIZE in
 * context.h) calls mpz_clear which calls gmp_free → free(3).  No
 * leak, no stale pointers, no SCAN_EDGES bookkeeping. */
/* Account for external (= libc) bytes through the framework so the GC
 * threshold sees memory pressure from GMP buffers.  Without this hook,
 * bignum-heavy code (= matmul's LCG seed, expt loops) allocates GB-scale
 * GMP limbs with zero framework pressure, GC never fires, the finalize
 * pass never runs, and libc heap blows up. */
static void *
gmp_alloc(size_t sz)
{
    void *p = malloc(sz);
    if (gmp_g_ctx) aro_gc_account_external(gmp_g_ctx, (ssize_t)sz);
    return p;
}

static void *
gmp_realloc(void *p, size_t old, size_t nw)
{
    void *np = realloc(p, nw);
    if (gmp_g_ctx) aro_gc_account_external(gmp_g_ctx, (ssize_t)nw - (ssize_t)old);
    return np;
}

static void
gmp_free(void *p, size_t sz)
{
    if (gmp_g_ctx) aro_gc_account_external(gmp_g_ctx, -(ssize_t)sz);
    free(p);
}

struct sobj *
scm_alloc(CTX *c, int type)
{
    struct sobj *o = (struct sobj *)aro_gc_alloc_raw(c, sizeof(struct sobj));
    SCM_SET_TYPE(o, type);
    return o;
}

/* Compile-time host-side pointer arrays (= char ** / NODE ** / small
 * VALUE *) used to live on the GC heap, but the lex_scope / call-args
 * arrays they back have no live root for the compile()'s recursive
 * scm_cons / scm_intern allocs to keep them anchored.  Move them to libc
 * malloc — leaked at process exit, but compile-time churn is bounded by
 * source size, so the leak is acceptable. */
static inline void *
scm_alloc_min(CTX *c, size_t size)
{
    (void)c;
    void *p = calloc(1, size);
    if (!p) { perror("calloc scm_alloc_min"); abort(); }
    return p;
}

VALUE
scm_cons(CTX *c, VALUE a, VALUE d)
{
    // Allocate exactly the bytes a pair needs (head + offset + car +
    // cdr) rather than the full sizeof(struct sobj).  Smaller heap
    // pressure when consing tight (list / sieve / nbody benches).  The
    // cast is sound because we never access fields past `pair` for
    // objects of type OBJ_PAIR.
    static const size_t pair_size = offsetof(struct sobj, pair) +
                                    sizeof(((struct sobj *)0)->pair);
    /* Park a / d on c->sp across the alloc.  Caller passes the args by
     * value (bits = heap addresses for ptr VALUEs); a moving GC triggered
     * by aro_gc_alloc would relocate the referents and the bits in C-local
     * `a` / `d` would go stale.  By writing them into sp slots *before*
     * the alloc, the root visitor sees them as live and rewrites the
     * slots in place; we then read the up-to-date bits back. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = a;
    sp[1] = d;
    struct sobj *o = (struct sobj *)aro_gc_alloc_raw(c, pair_size);
    SCM_SET_TYPE(o, OBJ_PAIR);
    /* o is freshly allocated (young) — WB is a no-op fast path but kept
     * uniform so all heap-slot writes route through ARO_STORE. */
    ARO_STORE(c, o, &o->pair.car, sp[0]);
    ARO_STORE(c, o, &o->pair.cdr, sp[1]);
    SP_POP(c, sp);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_string(CTX *c, const char *s, size_t len)
{
    /* The source `s` is a C-local pointer that a moving GC may leave
     * dangling: common callers (substring / string-append /
     * symbol->string / etc.) pass a heap-interior pointer into another
     * sobj's payload, and the inner allocs below relocate that source.
     *
     * Fix: copy `s` into a libc-malloc'd staging buffer up-front (= no
     * GC concern), then alloc the sobj + byte payload, then memcpy from
     * the staging buffer into the heap byte payload.  Small strings use
     * stack alloca to avoid the malloc round-trip on the hot path. */
    enum { STAGE_STACK = 256 };
    char stage_stack[STAGE_STACK];
    char *stage = (len + 1 <= STAGE_STACK) ? stage_stack : (char *)malloc(len + 1);
    if (!stage) { perror("malloc scm_make_string stage"); abort(); }
    memcpy(stage, s, len);
    stage[len] = '\0';

    /* Park the in-flight str sobj across the byte-payload alloc. */
    VALUE *sp = c->sp;
    struct sobj *o = scm_alloc(c, OBJ_STRING);
    /* str.chars/len are zero-init by scm_alloc — explicit clearing via
     * ARO_STORE for audit (= chars is ARO_GC_EDGE-qualified). */
    ARO_STORE(c, o, &o->str.chars, (VALUE)NULL);
    o->str.len = 0;
    sp[0] = SCM_OBJ_VAL(o);
    c->sp = sp + 1;
    char *raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + len + 1);
    o = SCM_PTR(sp[0]);   /* reload after potential GC move */
    /* o may be OLD now if alloc above promoted it.  Use WB so the byte
     * payload (= young) is remembered via o's dirty bit. */
    ARO_STORE(c, o, &o->str.chars, (VALUE)(raw + sizeof(AroObjectHeader)));
    memcpy(o->str.chars, stage, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    c->sp = sp;
    if (stage != stage_stack) free(stage);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_string_n(CTX *c, size_t len, char fill)
{
    /* No source heap pointer; only the in-flight sobj needs parking
     * across the byte-payload alloc. */
    VALUE *sp = c->sp;
    struct sobj *o = scm_alloc(c, OBJ_STRING);
    ARO_STORE(c, o, &o->str.chars, (VALUE)NULL);
    o->str.len = 0;
    sp[0] = SCM_OBJ_VAL(o);
    c->sp = sp + 1;
    char *raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + len + 1);
    o = SCM_PTR(sp[0]);
    ARO_STORE(c, o, &o->str.chars, (VALUE)(raw + sizeof(AroObjectHeader)));
    memset(o->str.chars, fill, len);
    o->str.chars[len] = '\0';
    o->str.len = len;
    c->sp = sp;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_char(CTX *c, uint32_t cp)
{
    struct sobj *o = scm_alloc(c, OBJ_CHAR);
    o->ch = cp;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_vector(CTX *c, size_t len, VALUE fill)
{
    /* Park the in-flight vector sobj + the fill VALUE across the inner
     * allocs.  Both the sobj alloc and the items payload alloc may
     * trigger GC — without parking, a moving backend leaves `o` and the
     * C-local `fill` bits pointing to stale (relocated) heap objects. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);    /* sp[0]=o sobj, sp[1]=fill */
    sp[1] = fill;
    struct sobj *o = scm_alloc(c, OBJ_VECTOR);
    ARO_STORE(c, o, &o->vec.items, (VALUE)NULL);
    o->vec.len = 0;
    sp[0] = SCM_OBJ_VAL(o);
    /* Allocate `header + N * VALUE`; the items pointer skips past the header
     * so writes to items[0..N-1] don't clobber gc_size — a mark_compact /
     * mark_compact_gen backend walks the region linearly using gc_size and
     * loops forever on a 0-size object. */
    size_t alloc_sz = sizeof(AroObjectHeader) + sizeof(VALUE) * (len ? len : 1);
    char *raw = (char *)aro_gc_alloc_raw(c, alloc_sz);
    /* Tag the backing buffer so SCAN_EDGES iterates items[] via the
     * buffer's own header — keeps reader and data co-located across moving
     * backends (= mark_compact slide-after-update, copying memcpy-on-promote).
     * See OBJ_VEC_BACKING in context.h. */
    SCM_SET_TYPE((struct sobj *)raw, OBJ_VEC_BACKING);
    o = SCM_PTR(sp[0]);
    /* WB on the items typed-ptr write: o may have been promoted by the
     * raw alloc above, raw is freshly young.  WB ensures o is remembered. */
    ARO_STORE(c, o, &o->vec.items, (VALUE)(raw + sizeof(AroObjectHeader)));
    o->vec.len = len;
    /* items[] backing payload is itself a heap object (= `raw`).  Holder
     * for the items slots is `raw` (the payload base).  raw is freshly
     * young so WB fast-path returns immediately, but we keep the routing
     * uniform. */
    for (size_t i = 0; i < len; i++) {
        ARO_STORE(c, raw, &o->vec.items[i], sp[1]);
    }
    SP_POP(c, sp);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_double(CTX *c, double d)
{
    // Try Ruby's inline flonum encoding first; falls through to a heap
    // OBJ_DOUBLE only for 0.0 / NaN / ±inf / |d| outside ~[1e-77, 1e+77].
    VALUE v = scm_try_flonum(d);
    if (LIKELY(v != 0)) return v;
    struct sobj *o = scm_alloc(c, OBJ_DOUBLE);
    o->dbl = d;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_bignum_z(CTX *c, mpz_srcptr z)
{
    struct sobj *o = scm_alloc(c, OBJ_BIGNUM);
    mpz_init_set(o->mpz, z);
    /* Register for finalize: the mpz limbs are libc-malloc'd via GMP's
     * allocator and invisible to the GC framework.  Without finalize the
     * limbs leak when GC reclaims `o`.  See AROH_FINALIZE in context.h. */
    aro_gc_finalize_register(c, o);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_normalize_int(CTX *c, mpz_srcptr z)
{
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (v >= SCM_FIXNUM_MIN && v <= SCM_FIXNUM_MAX) return SCM_FIX(v);
    }
    return scm_make_bignum_z(c, z);
}

VALUE
scm_make_rational_q(CTX *c, mpq_srcptr q)
{
    // If denominator is 1, return integer.
    if (mpz_cmp_ui(mpq_denref(q), 1) == 0) {
        return scm_normalize_int(c, mpq_numref(q));
    }
    struct sobj *o = scm_alloc(c, OBJ_RATIONAL);
    mpq_init(o->mpq);
    mpq_set(o->mpq, q);
    /* Register for finalize — see scm_make_bignum_z. */
    aro_gc_finalize_register(c, o);
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_rational_zz(CTX *c, mpz_srcptr num, mpz_srcptr den)
{
    mpq_t q;
    mpq_init(q);
    mpz_set(mpq_numref(q), num);
    mpz_set(mpq_denref(q), den);
    mpq_canonicalize(q);
    VALUE r = scm_make_rational_q(c, q);
    mpq_clear(q);
    return r;
}

VALUE
scm_normalize_rat(CTX *c, mpq_t q)
{
    mpq_canonicalize(q);
    return scm_make_rational_q(c, q);
}

VALUE
scm_make_complex(CTX *c, double re, double im)
{
    struct sobj *o = scm_alloc(c, OBJ_COMPLEX);
    o->cpx.re = re;
    o->cpx.im = im;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_simplify_complex(CTX *c, double re, double im)
{
    if (im == 0.0) return scm_make_double(c, re);
    return scm_make_complex(c, re, im);
}

VALUE
scm_make_mvalues(CTX *c, int count, VALUE *items)
{
    /* Header-prefixed items[] alloc — same rationale as scm_make_vector.
     * `items[]` is a C-local VALUE array supplied by the caller (typically
     * scm_apply argv).  Park each slot on c->sp before scm_alloc so the
     * root visitor can rewrite stale heap pointers across the two inner
     * allocs (the o sobj + the items payload). */
    VALUE *sp_base = c->sp;
    ASTRO_ASSERT(sp_base + 1 + count <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
    /* sp_base[0] = o sobj, sp_base[1 .. 1+count-1] = parked items copies */
    sp_base[0] = 0;
    for (int i = 0; i < count; i++) sp_base[1 + i] = items[i];
    c->sp = sp_base + 1 + count;

    struct sobj *o = scm_alloc(c, OBJ_MVALUES);
    ARO_STORE(c, o, &o->mv.items, (VALUE)NULL);
    o->mv.len = 0;
    sp_base[0] = SCM_OBJ_VAL(o);
    size_t alloc_sz = sizeof(AroObjectHeader) + sizeof(VALUE) * (count ? count : 1);
    char *raw = (char *)aro_gc_alloc_raw(c, alloc_sz);
    SCM_SET_TYPE((struct sobj *)raw, OBJ_VEC_BACKING);
    o = SCM_PTR(sp_base[0]);
    ARO_STORE(c, o, &o->mv.items, (VALUE)(raw + sizeof(AroObjectHeader)));
    o->mv.len = (size_t)count;
    for (int i = 0; i < count; i++) {
        ARO_STORE(c, raw, &o->mv.items[i], sp_base[1 + i]);
    }
    c->sp = sp_base;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_int(CTX *c, int64_t v)
{
    if (v >= SCM_FIXNUM_MIN && v <= SCM_FIXNUM_MAX) return SCM_FIX(v);
    mpz_t z; mpz_init(z);
    // mpz_set_si only takes long; for full int64_t, build via string or two halves.
#if LONG_MAX >= INT64_MAX
    mpz_set_si(z, (long)v);
#else
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    mpz_set_str(z, buf, 10);
#endif
    VALUE r = scm_make_bignum_z(c, z);
    mpz_clear(z);
    return r;
}

VALUE
scm_make_closure(CTX *c, NODE *body, struct sframe *env, int nparams, int has_rest)
{
    /* Park `env` across scm_alloc — a moving GC triggered there would
     * relocate the sframe and the C-local pointer would go stale.
     * body is libc-malloc'd (host-side NODE), unaffected. */
    VALUE *sp_base = c->sp;
    ASTRO_ASSERT(sp_base + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
    sp_base[0] = (VALUE)env;
    c->sp = sp_base + 1;
    struct sobj *o = scm_alloc(c, OBJ_CLOSURE);
    env = (struct sframe *)sp_base[0];   /* reload after GC */
    c->sp = sp_base;
    o->closure.body = body;
    /* WB on the env slot — env may have been promoted (OLD) by a prior
     * minor; o is freshly young so the env→o reverse edge would not need
     * remembering, but the framework's WB only triggers on holder OLD
     * (= o here). Fast path returns immediately. */
    ARO_STORE(c, o, &o->closure.env, (VALUE)env);
    o->closure.nparams = nparams;
    o->closure.has_rest = has_rest;
    o->closure.name = NULL;
    return SCM_OBJ_VAL(o);
}

VALUE
scm_make_prim(CTX *c, const char *name, scm_prim_fn fn, int min_argc, int max_argc)
{
    struct sobj *o = scm_alloc(c, OBJ_PRIM);
    o->prim.name = name;
    o->prim.fn = fn;
    o->prim.min_argc = min_argc;
    o->prim.max_argc = max_argc;
    return SCM_OBJ_VAL(o);
}

double
scm_get_double(VALUE v)
{
    if (SCM_IS_FIXNUM(v))    return (double)SCM_FIXVAL(v);
    if (SCM_IS_FLONUM(v))    return scm_flonum_to_double(v);
    if (scm_is_heap_double(v)) return SCM_PTR(v)->dbl;
    if (scm_is_bignum(v))    return mpz_get_d(SCM_PTR(v)->mpz);
    if (scm_is_rational(v))  return mpq_get_d(SCM_PTR(v)->mpq);
    if (scm_is_complex(v))   return SCM_PTR(v)->cpx.re;
    return 0.0;
}

bool
scm_is_integer_value(VALUE v)
{
    if (SCM_IS_FIXNUM(v) || scm_is_bignum(v)) return true;
    if (scm_is_double(v)) {
        double d = scm_get_double(v);
        return d == (double)(int64_t)d;
    }
    if (scm_is_rational(v)) {
        return mpz_cmp_ui(mpq_denref(SCM_PTR(v)->mpq), 1) == 0;
    }
    return false;
}

struct sframe *
scm_new_frame(CTX *c, struct sframe *parent, int nslots)
{
    /* Park `parent` across aro_gc_alloc — a moving GC triggered inside
     * may relocate the parent sframe, leaving the C-local ptr stale. */
    VALUE *sp_base = c->sp;
    ASTRO_ASSERT(sp_base + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
    sp_base[0] = (VALUE)parent;
    c->sp = sp_base + 1;
    struct sframe *f = (struct sframe *)aro_gc_alloc_raw(c,
        sizeof(struct sframe) + sizeof(VALUE) * (nslots ? nslots : 1));
    parent = (struct sframe *)sp_base[0];
    c->sp = sp_base;
    /* Tag the frame so SCAN_EDGES can identify it.  sframe shares the
     * head-at-offset-0 layout with struct sobj, so the framework-stored
     * gc_size / gc_flags survive; we only set the sample type bits. */
    SCM_SET_TYPE((struct sobj *)f, OBJ_FRAME);
    ARO_STORE(c, f, &f->parent, (VALUE)parent);
    f->nslots = nslots;
    /* Initialize slots to SCM_UNSPEC.  Frame is freshly young so WB
     * fast-path returns; the per-iter cost is negligible for typical
     * lambda arities. */
    for (int i = 0; i < nslots; i++) {
        ARO_STORE(c, f, &f->slots[i], SCM_UNSPEC);
    }
    return f;
}

// ---------------------------------------------------------------------------
// Symbol interning.  Linear table — fine for the source sizes we care
// about (tens of thousands of unique symbols at most).
// ---------------------------------------------------------------------------

/* Symbol table is referenced from aro_scheme_visit_roots below; expose
 * it file-wide rather than static so the root visitor can see it. */
struct sobj **SYMBOL_TABLE = NULL;
size_t SYMBOL_TABLE_LEN = 0;
size_t SYMBOL_TABLE_CAP = 0;
size_t aro_finalize_calls = 0;

VALUE
scm_intern(CTX *c, const char *name)
{
    for (size_t i = 0; i < SYMBOL_TABLE_LEN; i++) {
        if (strcmp(SYMBOL_TABLE[i]->sym.name, name) == 0) {
            return SCM_OBJ_VAL(SYMBOL_TABLE[i]);
        }
    }
    if (SYMBOL_TABLE_LEN == SYMBOL_TABLE_CAP) {
        SYMBOL_TABLE_CAP = SYMBOL_TABLE_CAP ? SYMBOL_TABLE_CAP * 2 : 64;
        /* Symbol table is C-owned (top-level static), use plain realloc. */
        SYMBOL_TABLE = (struct sobj **)realloc(SYMBOL_TABLE,
                                                sizeof(struct sobj *) * SYMBOL_TABLE_CAP);
    }
    /* `name` may point into a heap-allocated string payload (= caller
     * does scm_intern(c, SCM_PTR(arg)->str.chars)).  The inner allocs
     * below (sobj + byte payload) trigger GC and relocate that source.
     * Copy `name` into a libc-staged buffer up-front; cheap for symbols
     * (typically <= 32 chars). */
    size_t nlen = strlen(name);
    enum { STAGE_STACK = 256 };
    char stage_stack[STAGE_STACK];
    char *stage = (nlen + 1 <= STAGE_STACK) ? stage_stack : (char *)malloc(nlen + 1);
    if (!stage) { perror("malloc scm_intern stage"); abort(); }
    memcpy(stage, name, nlen + 1);
    /* Park the freshly allocated sym sobj in the symbol table slot BEFORE
     * the byte-payload alloc.  Moving GCs may trigger during that alloc,
     * and SYMBOL_TABLE is a scanned root.  Set sym.name=NULL so SCAN_EDGES
     * on this partially-init'd sobj is a no-op (interior char-slot visit
     * skips NULL). */
    struct sobj *o = scm_alloc(c, OBJ_SYMBOL);
    ARO_STORE(c, o, &o->sym.name, (VALUE)NULL);
    size_t reserved_idx = SYMBOL_TABLE_LEN;
    SYMBOL_TABLE[reserved_idx] = o;
    SYMBOL_TABLE_LEN++;
    char *raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + nlen + 1);
    /* Reload o after the alloc — moving GCs may have relocated it. */
    o = SYMBOL_TABLE[reserved_idx];
    /* WB on the sym.name typed-ptr write — o may be OLD if the byte
     * payload alloc above promoted it; raw payload is young. */
    ARO_STORE(c, o, &o->sym.name, (VALUE)(raw + sizeof(AroObjectHeader)));
    memcpy(o->sym.name, stage, nlen + 1);
    if (stage != stage_stack) free(stage);
    return SCM_OBJ_VAL(o);
}

/* ---------------------------------------------------------------------------
 * Permanent (libc-malloc'd) name pool — used by NODE allocators that
 * need a stable cstr pointer.  Symbol names on the GC heap can be
 * relocated by moving collectors, leaving any NODE that stored a
 * GC-interior cstr with a stale pointer.  Permanent names solve this
 * by handing out a malloc'd-once cstr per unique string.  Linear-scan
 * intern table — the size of the program's symbol table is bounded
 * and parse-time isn't latency-sensitive.
 * ------------------------------------------------------------------------- */
static char **PERM_NAMES = NULL;
static size_t PERM_NAMES_LEN = 0;
static size_t PERM_NAMES_CAP = 0;

const char *
scm_perm_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < PERM_NAMES_LEN; i++) {
        if (strcmp(PERM_NAMES[i], name) == 0) return PERM_NAMES[i];
    }
    if (PERM_NAMES_LEN == PERM_NAMES_CAP) {
        PERM_NAMES_CAP = PERM_NAMES_CAP ? PERM_NAMES_CAP * 2 : 64;
        PERM_NAMES = (char **)realloc(PERM_NAMES, sizeof(char *) * PERM_NAMES_CAP);
    }
    char *dup = strdup(name);
    PERM_NAMES[PERM_NAMES_LEN++] = dup;
    return dup;
}

/* ---------------------------------------------------------------------------
 * Quote roots — VALUEs embedded in NODE_QUOTE.  Under a moving GC, these
 * heap pointers stored inside NODEs would go stale without a live root.
 * `aro_scheme_visit_roots` walks this array and forwards each slot so
 * the embedded ptr in the NODE remains current (the NODE itself is libc-
 * malloc'd, so we update the slot in the array and the NODE's stored
 * bit-pattern stays in sync via the SAME slot — we hand out a pointer
 * to the slot, not a copy of the value, by storing the SAME address in
 * both places).  Simplest contract: bake the ptr into the NODE, AND
 * keep a parallel slot here for the GC to forward; we resync the NODE
 * before each dispatch (cheap because NODEs run-time read just stores
 * the bits).  Implementation:
 *   - scm_register_quote(v) → returns same v (after first run), but
 *     stores v in QUOTE_ROOTS.
 *   - When GC forwards QUOTE_ROOTS[i], the slot is updated; the NODE
 *     still has the OLD bits.  To keep them in sync, store the SLOT
 *     INDEX in the NODE instead of the value.  Use a different ALLOC
 *     ... too invasive.
 *
 * Instead: keep QUOTE_NODES[i] = NODE pointer, and on each
 * aro_scheme_visit_roots iteration:
 *   1. Read NODE's stored v (=ptr bits).
 *   2. Visit-edge it via the visitor, which forwards if needed.
 *   3. Write back the new bits into the NODE.
 * This fixes up every quote NODE on every GC.  Bounded by # of quotes
 * in the program — typically small.
 * ------------------------------------------------------------------------- */
static NODE **QUOTE_NODES = NULL;
static size_t QUOTE_NODES_LEN = 0;
static size_t QUOTE_NODES_CAP = 0;

void
scm_register_quote_node(NODE *n)
{
    if (QUOTE_NODES_LEN == QUOTE_NODES_CAP) {
        QUOTE_NODES_CAP = QUOTE_NODES_CAP ? QUOTE_NODES_CAP * 2 : 64;
        QUOTE_NODES = (NODE **)realloc(QUOTE_NODES, sizeof(NODE *) * QUOTE_NODES_CAP);
    }
    QUOTE_NODES[QUOTE_NODES_LEN++] = n;
}

/* Wrapper for ALLOC_node_quote that also registers the resulting NODE
 * with QUOTE_NODES so its embedded VALUE survives moving-GC cycles. */
static inline NODE *
scm_alloc_quote(uint64_t v)
{
    NODE *n = ALLOC_node_quote(v);
    scm_register_quote_node(n);
    return n;
}

/* scm_cons(c, scm_intern(name), d) helper.  Two C-local hazards to
 * sidestep:
 *   (a) C arg eval order is unspecified: a compiler may capture `d`
 *       (= bits read from a memory slot) BEFORE scm_intern triggers GC,
 *       leaving the cons call writing a stale heap pointer.  Park `d`
 *       on c->sp so the pre-GC bits are forwarded by the root visitor.
 *   (b) scm_intern itself may allocate and trigger GC, relocating the
 *       symbol it returns; sym's bits as a function-return register are
 *       fresh by the time scm_cons runs, but `d` (above) is stale unless
 *       we route it through sp.
 * We use c->sp directly (NOT the SP_PUSH macro) because this helper is
 * called from many busy paths and the macro version's assert/zero-init
 * adds non-trivial overhead. */
static inline VALUE
scm_cons_sym(CTX *c, const char *name, VALUE d)
{
    VALUE *sp_base = c->sp;
    ASTRO_ASSERT(sp_base + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
    sp_base[0] = d;
    c->sp = sp_base + 1;
    VALUE sym = scm_intern(c, name);
    VALUE r = scm_cons(c, sym, sp_base[0]);
    c->sp = sp_base;
    return r;
}

// ---------------------------------------------------------------------------
// Global definitions.  Linear array; lookups are linear but cheap given
// typical R5RS workloads.  Symbols are interned strings (`name` is the
// pointer returned by `scm_intern(c, ...)->sym.name`), so we compare by
// strcmp here for safety with literal C-string lookups.
// ---------------------------------------------------------------------------

void
scm_global_define(CTX *c, const char *name, VALUE v)
{
    c->globals_serial++;
    for (size_t i = 0; i < c->globals_size; i++) {
        /* Skip entries whose name is still NULL — a re-entrant define
         * triggered mid-alloc could see a half-initialized slot. */
        const char *gname = GENTRY_NAME(c->globals[i]);
        if (gname == NULL) continue;
        if (strcmp(gname, name) == 0) {
            c->globals[i].value = v;
            c->globals[i].defined = true;
            return;
        }
    }
    if (c->globals_size == c->globals_capa) {
        c->globals_capa = c->globals_capa ? c->globals_capa * 2 : 256;
        /* gentry table is host-owned metadata, not on the GC heap. */
        c->globals = (struct gentry *)realloc(c->globals,
                                              sizeof(struct gentry) * c->globals_capa);
    }
    /* Park v + bump globals_size BEFORE the name alloc — aro_gc_alloc_byte
     * can trigger GC, and v is a C-local VALUE that the root scanner would
     * otherwise miss.  Set name=NULL so the scanner skips the name slot
     * during this brief window.  defined=true keeps lookup semantics if
     * (somehow) a re-entrant define occurs mid-alloc. */
    c->globals[c->globals_size].name_payload = NULL;
    c->globals[c->globals_size].value = v;
    c->globals[c->globals_size].defined = true;
    c->globals_size++;
    size_t nlen = strlen(name);
    char *raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + nlen + 1);
    /* Store the byte-payload BASE (= raw, not raw + header).  Moving GCs
     * forward via the header bits; an interior pointer would break that.
     * Readers go through GENTRY_NAME() to skip past the header. */
    c->globals[c->globals_size - 1].name_payload = raw;
    memcpy(raw + sizeof(AroObjectHeader), name, nlen + 1);
}

VALUE
scm_global_ref(CTX *c, const char *name)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        const char *gname = GENTRY_NAME(c->globals[i]);
        if (gname == NULL) continue;
        if (strcmp(gname, name) == 0) {
            if (!c->globals[i].defined) {
                scm_error(c, "unbound variable: %s", name);
            }
            return c->globals[i].value;
        }
    }
    scm_error(c, "unbound variable: %s", name);
}

void
scm_global_set(CTX *c, const char *name, VALUE v)
{
    for (size_t i = 0; i < c->globals_size; i++) {
        const char *gname = GENTRY_NAME(c->globals[i]);
        if (gname == NULL) continue;
        if (strcmp(gname, name) == 0) {
            c->globals[i].value = v;
            c->globals[i].defined = true;
            c->globals_serial++;
            return;
        }
    }
    scm_error(c, "set! on unbound variable: %s", name);
}

// ---------------------------------------------------------------------------
// Errors — longjmp back to the REPL/script driver if an error handler
// is installed; otherwise print and exit.
// ---------------------------------------------------------------------------

static char SCM_ERR_MSG[1024];

void
scm_error(CTX *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(SCM_ERR_MSG, sizeof(SCM_ERR_MSG), fmt, ap);
    va_end(ap);
    if (c && c->err_jmp_active) {
        longjmp(c->err_jmp, 1);
    }
    fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
    exit(1);
}

// ---------------------------------------------------------------------------
// Display / write.
// ---------------------------------------------------------------------------

static void scm_display1(FILE *fp, VALUE v, bool readable);

static void
write_string(FILE *fp, const char *s, size_t len, bool readable)
{
    if (!readable) {
        fwrite(s, 1, len, fp);
        return;
    }
    fputc('"', fp);
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        switch (ch) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\t': fputs("\\t", fp); break;
        case '\r': fputs("\\r", fp); break;
        default:   fputc(ch, fp);
        }
    }
    fputc('"', fp);
}

static void
write_char(FILE *fp, uint32_t cp, bool readable)
{
    if (!readable) {
        if (cp < 128) fputc((int)cp, fp);
        else fprintf(fp, "?");   // simplified: no UTF-8 encoding
        return;
    }
    switch (cp) {
    case ' ':  fputs("#\\space", fp);   return;
    case '\n': fputs("#\\newline", fp); return;
    case '\t': fputs("#\\tab", fp);     return;
    case 0:    fputs("#\\nul", fp);     return;
    }
    if (cp >= 32 && cp < 127) fprintf(fp, "#\\%c", (int)cp);
    else fprintf(fp, "#\\x%x", cp);
}

static void
write_pair(FILE *fp, VALUE v, bool readable)
{
    fputc('(', fp);
    bool first = true;
    while (scm_is_pair(v)) {
        if (!first) fputc(' ', fp);
        first = false;
        scm_display1(fp, SCM_PTR(v)->pair.car, readable);
        v = SCM_PTR(v)->pair.cdr;
    }
    if (v != SCM_NIL) {
        fputs(" . ", fp);
        scm_display1(fp, v, readable);
    }
    fputc(')', fp);
}

static void
scm_display1(FILE *fp, VALUE v, bool readable)
{
    if (SCM_IS_FIXNUM(v)) { fprintf(fp, "%lld", (long long)SCM_FIXVAL(v)); return; }
    if (SCM_IS_FLONUM(v)) {
        double d = scm_flonum_to_double(v);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", d);
        fputs(buf, fp);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
            fputs(".0", fp);
        }
        return;
    }
    struct sobj *o = SCM_PTR(v);
    if (!o) { fputs("#<NULL>", fp); return; }
    switch (SCM_TYPE(o)) {
    case OBJ_NIL:    fputs("()", fp);             break;
    case OBJ_BOOL:   fputs(o->b ? "#t" : "#f", fp); break;
    case OBJ_UNSPEC: fputs("", fp);               break;
    case OBJ_EOF:    fputs("#<eof>", fp);         break;
    case OBJ_PAIR:   write_pair(fp, v, readable); break;
    case OBJ_SYMBOL: fputs(o->sym.name, fp);      break;
    case OBJ_STRING: write_string(fp, o->str.chars, o->str.len, readable); break;
    case OBJ_CHAR:   write_char(fp, o->ch, readable); break;
    case OBJ_VECTOR: {
        fputs("#(", fp);
        for (size_t i = 0; i < o->vec.len; i++) {
            if (i) fputc(' ', fp);
            scm_display1(fp, o->vec.items[i], readable);
        }
        fputc(')', fp);
        break;
    }
    case OBJ_DOUBLE: {
        double d = o->dbl;
        if (isnan(d)) { fputs("+nan.0", fp); break; }
        if (isinf(d)) { fputs(d > 0 ? "+inf.0" : "-inf.0", fp); break; }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", d);
        fputs(buf, fp);
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
            fputs(".0", fp);
        }
        break;
    }
    case OBJ_BIGNUM: {
        char *s = mpz_get_str(NULL, 10, o->mpz);
        fputs(s, fp);
        // s allocated via GMP's allocator (routed to aro_gc_alloc_byte).
        break;
    }
    case OBJ_RATIONAL: {
        char *s = mpq_get_str(NULL, 10, o->mpq);
        fputs(s, fp);
        break;
    }
    case OBJ_COMPLEX: {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", o->cpx.re); fputs(buf, fp);
        if (o->cpx.im >= 0) fputc('+', fp);
        snprintf(buf, sizeof(buf), "%.15g", o->cpx.im); fputs(buf, fp);
        fputc('i', fp);
        break;
    }
    case OBJ_MVALUES: {
        // R5RS leaves the printed form unspecified; we emit each value
        // separated by newlines, matching Gauche's REPL convention.
        for (size_t i = 0; i < o->mv.len; i++) {
            if (i) fputc('\n', fp);
            scm_display1(fp, o->mv.items[i], readable);
        }
        break;
    }
    case OBJ_CLOSURE: fprintf(fp, "#<procedure %s>", o->closure.name ? o->closure.name : "anon"); break;
    case OBJ_PRIM:    fprintf(fp, "#<primitive %s>", o->prim.name); break;
    case OBJ_CONT:    fputs("#<continuation>", fp); break;
    case OBJ_PORT:    fputs("#<port>", fp); break;
    default:          fputs("#<?>", fp); break;
    }
}

void
scm_display(FILE *fp, VALUE v, bool readable)
{
    scm_display1(fp, v, readable);
}

// ---------------------------------------------------------------------------
// Compiler — s-expression → AST.  Threads a lex_scope chain so symbols
// resolve to (depth, idx) lref nodes when bound, falling back to gref.
// ---------------------------------------------------------------------------

/* lex_scope is allocated via aro_gc_alloc (= must start with AroObjectHeader
 * so moving GCs can read size / forwarding bits at offset 0).  flags=0 means
 * SCAN_EDGES dispatches to the default no-op; we treat lex_scope as a
 * scanned root via the host C parser/compiler holding stack-local pointers
 * to it.  Under a moving GC those pointers would be stale — but lex_scope
 * is only used during the synchronous compile() recursion, which doesn't
 * allocate GC heap memory itself (NODE allocations are host malloc'd via
 * ALLOC_node_*), so no GC trigger can reach it during the dangerous window. */
struct lex_scope {
    /* libc-allocated (compile-time only).  Putting lex_scope on the GC
     * heap would require rooting the whole chain — compile recurses
     * arbitrarily deep through scm_cons / scm_intern, and the scopes
     * passed by value have no live anchor.  Leaking is fine because
     * compile-time scope churn is bounded by program size. */
    struct lex_scope *parent;
    int nslots;
    char **names;   // libc-permanent cstrs (= scm_perm_name)
    /* is_lambda_boundary: true for scopes pushed by compile_lambda
     * (= a closure body), false for scopes pushed by compile_let.  Used
     * to detect when an lref crosses a closure boundary. */
    bool is_lambda_boundary;
    /* has_outer_ref: when this scope is a lambda boundary, true means at
     * least one lref/lset inside the body refers across this boundary
     * (= the closure captures from an enclosing scope).  Used to mark
     * the closure "no_capture" when false, enabling sframe-alloc skip. */
    bool has_outer_ref;
    /* pending_lrefs: emitted lref/lset NODE pointers within this lambda's
     * body.  Only meaningful on lambda-boundary scopes.  compile_lambda
     * iterates these after body compile and patches dispatchers to the
     * _sp variants when !has_outer_ref (= no_capture closure body uses
     * sp[] frame). */
    NODE **pending_lrefs;
    size_t pending_n, pending_cap;
};

static struct lex_scope *
push_scope(CTX *c, struct lex_scope *parent, int nslots, char **names)
{
    (void)c;
    struct lex_scope *s = (struct lex_scope *)calloc(1, sizeof(*s));
    if (!s) { perror("calloc lex_scope"); abort(); }
    s->parent = parent;
    s->nslots = nslots;
    s->names = names;
    /* default: not a lambda boundary (= compile_let path).  compile_lambda
     * sets is_lambda_boundary = true after push_scope. */
    s->is_lambda_boundary = false;
    s->has_outer_ref = false;
    return s;
}

/* Walk `depth` scopes upward from `current` and mark each lambda boundary
 * along the path with has_outer_ref=true.  Called at lref/lset emit time
 * when depth>=1 — every lambda crossed gains an "outer capture" record. */
static void
mark_outer_capture_path(struct lex_scope *current, uint32_t depth)
{
    struct lex_scope *s = current;
    for (uint32_t d = 0; d < depth && s; d++) {
        if (s->is_lambda_boundary) s->has_outer_ref = true;
        s = s->parent;
    }
}

/* Find the nearest enclosing lambda boundary, including `s` itself. */
static struct lex_scope *
enclosing_lambda(struct lex_scope *s)
{
    for (; s; s = s->parent) if (s->is_lambda_boundary) return s;
    return NULL;
}

/* Record an emitted lref/lset NODE in the enclosing lambda's pending list,
 * so compile_lambda can later patch its dispatcher to the _sp variant if the
 * lambda turns out to be no_capture. */
static void
record_pending_lref(struct lex_scope *current, NODE *n)
{
    struct lex_scope *lam = enclosing_lambda(current);
    if (!lam) return; /* top-level — never a closure body */
    if (lam->pending_n == lam->pending_cap) {
        size_t newcap = lam->pending_cap ? lam->pending_cap * 2 : 16;
        lam->pending_lrefs = (NODE **)realloc(lam->pending_lrefs, newcap * sizeof(NODE *));
        if (!lam->pending_lrefs) { perror("realloc pending_lrefs"); abort(); }
        lam->pending_cap = newcap;
    }
    lam->pending_lrefs[lam->pending_n++] = n;
}

// Look up `name` in the lex chain.  Returns true and writes (depth,idx)
// if found.
static bool
lex_lookup(struct lex_scope *s, const char *name, uint32_t *depth, uint32_t *idx)
{
    uint32_t d = 0;
    for (; s; s = s->parent) {
        for (int i = 0; i < s->nslots; i++) {
            if (s->names[i] && strcmp(s->names[i], name) == 0) {
                *depth = d; *idx = (uint32_t)i;
                return true;
            }
        }
        d++;
    }
    return false;
}

// Variable-arg call args pool — referenced from node_call_n.
NODE  **ASCHEME_CALL_ARGS = NULL;
uint32_t ASCHEME_CALL_ARGS_CNT = 0;
static uint32_t ASCHEME_CALL_ARGS_CAP = 0;

// AOT-mode entry list: every node_lambda body and every top-level form is
// registered here so `--compile` can specialize the lot in one pass.
static NODE **AOT_ENTRIES = NULL;
static size_t AOT_ENTRIES_LEN = 0;
static size_t AOT_ENTRIES_CAP = 0;

// Profiling switch — flipped on by `--pg-compile` so scm_apply's hot
// loop can skip its `body->head.dispatch_cnt++` write when it isn't
// going to be read.  Removing the counter increment shaves ~5 % off
// tight tail-call loops because the read-modify-write across millions
// of iterations is non-trivial.  Also referenced from the inline
// scm_apply_tail in node.h.
bool ASCHEME_PROFILING = false;

static void
aot_add_entry(NODE *n)
{
    // @noinline roots emit an empty SPECIALIZE body, so we'd just create an
    // .c file with no SD function in it.  Skip them — the body's children
    // (which usually are inlinable) are reached when their parent's SD is
    // generated and recurses via SPECIALIZE().
    if (!n || n->head.flags.no_inline) return;
    if (AOT_ENTRIES_LEN == AOT_ENTRIES_CAP) {
        AOT_ENTRIES_CAP = AOT_ENTRIES_CAP ? AOT_ENTRIES_CAP * 2 : 64;
        /* AOT_ENTRIES is host metadata, not GC-managed. */
        AOT_ENTRIES = (NODE **)realloc(AOT_ENTRIES, sizeof(NODE *) * AOT_ENTRIES_CAP);
    }
    AOT_ENTRIES[AOT_ENTRIES_LEN++] = n;
}

static uint32_t
register_call_args(NODE **args, uint32_t cnt)
{
    if (ASCHEME_CALL_ARGS_CNT + cnt > ASCHEME_CALL_ARGS_CAP) {
        uint32_t need = ASCHEME_CALL_ARGS_CNT + cnt;
        uint32_t capa = ASCHEME_CALL_ARGS_CAP ? ASCHEME_CALL_ARGS_CAP : 64;
        while (capa < need) capa *= 2;
        /* host-owned call-arg pool: plain realloc. */
        ASCHEME_CALL_ARGS = (NODE **)realloc(ASCHEME_CALL_ARGS,
                                             sizeof(NODE *) * capa);
        ASCHEME_CALL_ARGS_CAP = capa;
    }
    uint32_t base = ASCHEME_CALL_ARGS_CNT;
    for (uint32_t i = 0; i < cnt; i++) ASCHEME_CALL_ARGS[base + i] = args[i];
    ASCHEME_CALL_ARGS_CNT += cnt;
    return base;
}

// Helpers for examining s-exprs.
static bool
is_symbol(VALUE v, const char *name)
{
    if (!scm_is_symbol(v)) return false;
    return strcmp(SCM_PTR(v)->sym.name, name) == 0;
}

static int
list_length(VALUE v)
{
    int n = 0;
    while (scm_is_pair(v)) { n++; v = SCM_PTR(v)->pair.cdr; }
    return n;
}

static VALUE car(VALUE v)  { return SCM_PTR(v)->pair.car; }
static VALUE cdr(VALUE v)  { return SCM_PTR(v)->pair.cdr; }
static VALUE cadr(VALUE v) { return car(cdr(v)); }
static VALUE caddr(VALUE v){ return car(cdr(cdr(v))); }
static VALUE cadddr(VALUE v){return car(cdr(cdr(cdr(v)))); }

static NODE *compile(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail);
static NODE *compile_body(CTX *c, VALUE body, struct lex_scope *scope, bool is_tail);

// Expand `(quasiquote form)` into a constructive expression — `cons`,
// `list`, and `append` calls — that builds the same s-expr at runtime
// while interpolating `(unquote …)` and `(unquote-splicing …)`.  R5RS
// §4.2.6.  Nested quasiquotes increase `depth`; only `(unquote x)` at
// depth 1 evaluates `x`, deeper nests just rebuild the syntactic form.
static VALUE
expand_quasiquote(CTX *c, VALUE form, int depth)
{
    if (!scm_is_pair(form)) {
        if (SCM_IS_FIXNUM(form) || scm_is_bool(form) || scm_is_null(form) ||
            scm_is_double(form) || scm_is_string(form) || scm_is_char(form) ||
            scm_is_bignum(form) || scm_is_rational(form))
            return form;
        /* (quote form) */
        VALUE * restrict sp_q = c->sp;
        SP_PUSH(c, sp_q, 1);
        sp_q[0] = form;
        VALUE tail_cell = scm_cons(c, sp_q[0], SCM_NIL);
        VALUE r = scm_cons_sym(c, "quote", tail_cell);
        SP_POP(c, sp_q);
        return r;
    }
    /* form is a pair.  sp[0]=form, sp[1]=head, sp[2]=tail, sp[3..]=staging. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 6);
    sp[0] = form;
    sp[1] = SCM_PTR(form)->pair.car;
    sp[2] = SCM_PTR(form)->pair.cdr;
    if (scm_is_symbol(sp[1])) {
        const char *name = SCM_PTR(sp[1])->sym.name;
        if (strcmp(name, "unquote") == 0) {
            VALUE inner = scm_is_pair(sp[2]) ? SCM_PTR(sp[2])->pair.car : SCM_NIL;
            if (depth == 1) { SP_POP(c, sp); return inner; }
            /* (list (quote head) <recur>) */
            sp[3] = inner;
            sp[4] = expand_quasiquote(c, sp[3], depth - 1);
            sp[5] = scm_cons(c, sp[4], SCM_NIL);
            VALUE quote_tail = scm_cons(c, sp[1], SCM_NIL);
            sp[3] = quote_tail;
            sp[3] = scm_cons_sym(c, "quote", sp[3]);
            sp[3] = scm_cons(c, sp[3], sp[5]);
            sp[3] = scm_cons_sym(c, "list", sp[3]);
            VALUE r = sp[3];
            SP_POP(c, sp);
            return r;
        }
        if (strcmp(name, "quasiquote") == 0) {
            VALUE inner = scm_is_pair(sp[2]) ? SCM_PTR(sp[2])->pair.car : SCM_NIL;
            sp[3] = inner;
            sp[4] = expand_quasiquote(c, sp[3], depth + 1);
            sp[5] = scm_cons(c, sp[4], SCM_NIL);
            VALUE quote_tail = scm_cons(c, sp[1], SCM_NIL);
            sp[3] = quote_tail;
            sp[3] = scm_cons_sym(c, "quote", sp[3]);
            sp[3] = scm_cons(c, sp[3], sp[5]);
            sp[3] = scm_cons_sym(c, "list", sp[3]);
            VALUE r = sp[3];
            SP_POP(c, sp);
            return r;
        }
    }
    if (depth == 1 && scm_is_pair(sp[1])) {
        VALUE hh = SCM_PTR(sp[1])->pair.car;
        if (scm_is_symbol(hh) && strcmp(SCM_PTR(hh)->sym.name, "unquote-splicing") == 0) {
            VALUE inner = SCM_PTR(SCM_PTR(sp[1])->pair.cdr)->pair.car;
            sp[3] = inner;
            sp[4] = expand_quasiquote(c, sp[2], depth);
            sp[5] = scm_cons(c, sp[4], SCM_NIL);
            sp[3] = scm_cons(c, sp[3], sp[5]);
            sp[3] = scm_cons_sym(c, "append", sp[3]);
            VALUE r = sp[3];
            SP_POP(c, sp);
            return r;
        }
    }
    /* (cons <head-recur> <tail-recur>) */
    sp[3] = expand_quasiquote(c, sp[1], depth);
    sp[4] = expand_quasiquote(c, sp[2], depth);
    sp[5] = scm_cons(c, sp[4], SCM_NIL);
    sp[3] = scm_cons(c, sp[3], sp[5]);
    sp[3] = scm_cons_sym(c, "cons", sp[3]);
    VALUE r = sp[3];
    SP_POP(c, sp);
    return r;
}

// Compile-time helpers used by several special-form lowerings.
static VALUE
gensym_at(CTX *c, const char *base)
{
    static int seq = 0;
    char buf[64]; snprintf(buf, sizeof(buf), "|%s-%d|", base, ++seq);
    return scm_intern(c, buf);
}

static VALUE
list_append1(CTX *c, VALUE list, VALUE elt)
{
    /* sp[0]=list, sp[1]=elt, sp[2]=result head, sp[3]=last cell,
     * sp[4]=iter. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 5);
    sp[0] = list;
    sp[1] = elt;
    sp[2] = SCM_NIL;
    sp[3] = SCM_NIL;
    for (sp[4] = sp[0]; scm_is_pair(sp[4]); sp[4] = SCM_PTR(sp[4])->pair.cdr) {
        VALUE cell = scm_cons(c, SCM_PTR(sp[4])->pair.car, SCM_NIL);
        if (sp[2] == SCM_NIL) { sp[2] = cell; }
        else {
            struct sobj *last_cell = SCM_PTR(sp[3]);
            ARO_STORE(c, last_cell, &last_cell->pair.cdr, cell);
        }
        sp[3] = cell;
    }
    VALUE last = scm_cons(c, sp[1], SCM_NIL);
    if (sp[2] == SCM_NIL) { sp[2] = last; }
    else {
        struct sobj *last_cell = SCM_PTR(sp[3]);
        ARO_STORE(c, last_cell, &last_cell->pair.cdr, last);
    }
    VALUE r = sp[2];
    SP_POP(c, sp);
    return r;
}

// (begin a b ... last) → seq-chain.  The last form inherits is_tail.
static NODE *
compile_seq(CTX *c, VALUE forms, struct lex_scope *scope, bool is_tail)
{
    if (!scm_is_pair(forms)) return ALLOC_node_const_unspec();
    if (cdr(forms) == SCM_NIL) {
        return compile(c, car(forms), scope, is_tail);
    }
    /* Park `forms` across the two recursive compile calls. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 1);
    sp[0] = forms;
    NODE *head = compile(c, car(sp[0]), scope, false);
    NODE *rest = compile_seq(c, cdr(sp[0]), scope, is_tail);
    NODE *r = ALLOC_node_seq(head, rest);
    SP_POP(c, sp);
    return r;
}

// Try to fold `(<op> ...)` into a specialized node.  Returns NULL when no
// specialization applies (parser falls back to a generic call_K).  The
// specialization is safe under R5RS rebinding because each emitted node
// carries an arith_cache that tracks the global at its install_prims-time
// snapshot; see node.def for the runtime check.
static NODE *
try_specialize_arith(CTX *c, VALUE fn_form, VALUE args, struct lex_scope *scope)
{
    if (!scm_is_symbol(fn_form)) return NULL;
    int argc = list_length(args);
    /* Use scm_perm_name so `name` survives any subsequent GC trigger.
     * The lex_scope->names are also perm cstrs so the strcmp below stays
     * valid. */
    const char *name = scm_perm_name(SCM_PTR(fn_form)->sym.name);
    uint32_t depth, idx;
    if (lex_lookup(scope, name, &depth, &idx)) return NULL;

    /* Park `args` so the per-arg compile() doesn't lose it under moving GC.
     * `name` (= sym.name) — keep `fn_form` parked too so the underlying
     * symbol payload isn't reclaimed, and re-derive `name` if we ever
     * use it after a GC trigger (here strcmp is done BEFORE each compile,
     * so `name` stays fresh-enough at each branch entry). */
    // Match the name BEFORE compiling args.  Earlier we compiled the args
    // unconditionally and then picked a node based on the name — but for
    // calls like `(display X)` whose name doesn't match, we'd return NULL
    // and compile_call would re-compile the args.  The first (discarded)
    // compile registered AOT entries whose dispatchers got patched by
    // astro_cs_load, while the second (live) compile produced fresh NODE
    // pointers whose dispatchers stayed on the slow DISPATCH_node_*
    // host fallbacks — defeating AOT for any program that calls a global
    // 1/2/3-arg function whose name isn't in the specialized set.
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = fn_form;
    sp[1] = args;
    NODE *result = NULL;
    if (argc == 1) {
        if (strcmp(name, "null?") == 0)
            { result = ALLOC_node_pred_null(compile(c, car(sp[1]), scope, false)); goto done; }
        if (strcmp(name, "pair?") == 0)
            { result = ALLOC_node_pred_pair(compile(c, car(sp[1]), scope, false)); goto done; }
        if (strcmp(name, "car") == 0)
            { result = ALLOC_node_pred_car(compile(c, car(sp[1]), scope, false)); goto done; }
        if (strcmp(name, "cdr") == 0)
            { result = ALLOC_node_pred_cdr(compile(c, car(sp[1]), scope, false)); goto done; }
        if (strcmp(name, "not") == 0)
            { result = ALLOC_node_pred_not(compile(c, car(sp[1]), scope, false)); goto done; }
        goto done;
    }
    if (argc == 2) {
        bool match = (strcmp(name, "+")  == 0 || strcmp(name, "-")  == 0 ||
                      strcmp(name, "*")  == 0 || strcmp(name, "<")  == 0 ||
                      strcmp(name, "<=") == 0 || strcmp(name, ">")  == 0 ||
                      strcmp(name, ">=") == 0 || strcmp(name, "=")  == 0 ||
                      strcmp(name, "vector-ref") == 0 ||
                      strcmp(name, "cons") == 0 ||
                      strcmp(name, "eq?")  == 0 ||
                      strcmp(name, "eqv?") == 0);
        if (!match) goto done;
        NODE *a = compile(c, car(sp[1]),  scope, false);
        NODE *b = compile(c, cadr(sp[1]), scope, false);
        /* Re-derive `name` since the symbol payload may have moved during
         * the two compile() calls above.  fn_form is parked in sp[0]. */
        const char *nm = SCM_PTR(sp[0])->sym.name;
        if (strcmp(nm, "+")  == 0) { result = ALLOC_node_arith_add(a, b); goto done; }
        if (strcmp(nm, "-")  == 0) { result = ALLOC_node_arith_sub(a, b); goto done; }
        if (strcmp(nm, "*")  == 0) { result = ALLOC_node_arith_mul(a, b); goto done; }
        if (strcmp(nm, "<")  == 0) { result = ALLOC_node_arith_lt(a, b); goto done; }
        if (strcmp(nm, "<=") == 0) { result = ALLOC_node_arith_le(a, b); goto done; }
        if (strcmp(nm, ">")  == 0) { result = ALLOC_node_arith_gt(a, b); goto done; }
        if (strcmp(nm, ">=") == 0) { result = ALLOC_node_arith_ge(a, b); goto done; }
        if (strcmp(nm, "=")  == 0) { result = ALLOC_node_arith_eq(a, b); goto done; }
        if (strcmp(nm, "vector-ref") == 0) { result = ALLOC_node_vec_ref(a, b); goto done; }
        if (strcmp(nm, "cons") == 0)   { result = ALLOC_node_cons_op(a, b); goto done; }
        if (strcmp(nm, "eq?") == 0)    { result = ALLOC_node_eq_op(a, b); goto done; }
        if (strcmp(nm, "eqv?") == 0)   { result = ALLOC_node_eqv_op(a, b); goto done; }
        goto done;
    }
    if (argc == 3) {
        if (strcmp(name, "vector-set!") != 0) goto done;
        NODE *a = compile(c, car(sp[1]),  scope, false);
        NODE *b = compile(c, cadr(sp[1]), scope, false);
        NODE *d = compile(c, caddr(sp[1]), scope, false);
        result = ALLOC_node_vec_set(a, b, d);
        goto done;
    }
done:
    SP_POP(c, sp);
    return result;
}

// When the parser is compiling a named-let body (or single-binding letrec
// with a lambda init), CURRENT_SELF_CALL holds the loop name + arity.
// compile_call uses this to recognize tail-position self-recursive calls
// and lower them to node_self_tail_call_K — which together with the
// node_loop wrapping the body collapses the trampoline tail loop to a
// tight C for(;;).  Cleared on entry to any nested compile_lambda so
// that escaped closures don't accidentally take the fast path.
struct self_call_ctx {
    const char         *name;          // loop name being self-called
    uint32_t            nparams;       // expected arity
    struct lex_scope   *target_scope;  // NULL = global mode (top-level
                                       // define).  Else lex mode: the
                                       // scope that owns NAME's slot
                                       // (used to detect inner shadowing
                                       //  — `(let ((NAME ...)) ...)`
                                       //  inside the body changes the
                                       //  meaning of NAME).
    bool                used;          // any patch happened?
};
static struct self_call_ctx *CURRENT_SELF_CALL = NULL;
// Set by compile_define just before invoking compile() on the lambda
// form: gives compile_lambda the ctx to install for the IMMEDIATE
// lambda's body (and consumed there, so nested lambdas inside the
// body still get the usual save+clear).  NULL outside that brief
// window.  compile_let_named uses a different path (manual AST
// construction) and doesn't need this.
static struct self_call_ctx *SELF_CALL_FOR_NEXT_LAMBDA = NULL;

// lex_lookup variant that also reports the scope that owns the slot,
// so compile_call can compare it against CURRENT_SELF_CALL->target_scope.
static bool
lex_lookup_full(struct lex_scope *s, const char *name,
                struct lex_scope **scope_out, uint32_t *depth, uint32_t *idx)
{
    uint32_t d = 0;
    for (; s; s = s->parent) {
        for (int i = 0; i < s->nslots; i++) {
            if (s->names[i] && strcmp(s->names[i], name) == 0) {
                *scope_out = s; *depth = d; *idx = (uint32_t)i;
                return true;
            }
        }
        d++;
    }
    return false;
}

// Build call node for given fn + args.
static NODE *
compile_call(CTX *c, VALUE fn_form, VALUE args, struct lex_scope *scope, bool is_tail)
{
    /* Park fn_form + args across recursive compile()/try_specialize/etc.
     * — every nested step may trigger arbitrarily many GCs and a C-local
     * VALUE here would go stale on a moving backend.  sp[0]=fn_form,
     * sp[1]=args (the head of the arg list), sp[2]=iter `p` (so the for
     * loop's traversal cursor stays live as well). */
    VALUE * restrict sp_top = c->sp;
    SP_PUSH(c, sp_top, 3);
    sp_top[0] = fn_form;
    sp_top[1] = args;
    // Self-tail-call recognition.  Two modes selected by
    // CURRENT_SELF_CALL->target_scope:
    //   non-NULL → lex mode (named-let / single-binding letrec).  Match
    //              when fn name is a lex var resolving to that exact
    //              scope (so inner `(let ((NAME …)) …)` shadow doesn't
    //              hijack the fast path) and arity matches.  Emits
    //              node_self_tail_call_K.
    //   NULL     → global mode (top-level `(define (f …) …)`).  Match
    //              when fn name is a non-shadowed global symbol equal
    //              to the function being defined.  Emits
    //              node_self_tail_call_global_K — gref_cache gates the
    //              fast path on globals_serial, so a later
    //              `(set! f g)` (which bumps the serial) drops to slow
    //              path and dispatches via scm_apply_tail.
    NODE *result;
    if (is_tail && CURRENT_SELF_CALL && scm_is_symbol(sp_top[0])) {
        /* perm_name → survives the recursive compile() GCs below. */
        const char *fn_name = scm_perm_name(SCM_PTR(sp_top[0])->sym.name);
        if (strcmp(fn_name, CURRENT_SELF_CALL->name) == 0) {
            int argc = list_length(sp_top[1]);
            bool match = false;
            if (CURRENT_SELF_CALL->target_scope) {
                // Lex mode: must resolve to the registered scope.
                struct lex_scope *resolved_scope;
                uint32_t depth, idx;
                match = lex_lookup_full(scope, fn_name, &resolved_scope, &depth, &idx) &&
                        resolved_scope == CURRENT_SELF_CALL->target_scope;
            } else {
                // Global mode: name must NOT be a lex var (no shadowing).
                uint32_t depth, idx;
                match = !lex_lookup(scope, fn_name, &depth, &idx);
            }
            if (match && argc == (int)CURRENT_SELF_CALL->nparams && argc <= 4) {
                NODE *aN[ASCHEME_LOOP_MAX_PARAMS];
                int i = 0;
                /* Use sp_top[2] as live iter cursor through GC-triggering
                 * recursive compile() calls. */
                for (sp_top[2] = sp_top[1]; scm_is_pair(sp_top[2]); sp_top[2] = cdr(sp_top[2]), i++) {
                    aN[i] = compile(c, car(sp_top[2]), scope, false);
                }
                CURRENT_SELF_CALL->used = true;
                if (CURRENT_SELF_CALL->target_scope) {
                    switch (argc) {
                    case 0: result = ALLOC_node_self_tail_call_0(); goto done_call;
                    case 1: result = ALLOC_node_self_tail_call_1(aN[0]); goto done_call;
                    case 2: result = ALLOC_node_self_tail_call_2(aN[0], aN[1]); goto done_call;
                    case 3: result = ALLOC_node_self_tail_call_3(aN[0], aN[1], aN[2]); goto done_call;
                    case 4: result = ALLOC_node_self_tail_call_4(aN[0], aN[1], aN[2], aN[3]); goto done_call;
                    }
                } else {
                    /* Permanent-name cstr: the symbol byte payload moves
                     * under moving GC, so the NODE must reference a libc
                     * pool name to stay live across collections. */
                    const char *pn = scm_perm_name(SCM_PTR(sp_top[0])->sym.name);
                    switch (argc) {
                    case 0: result = ALLOC_node_self_tail_call_global_0(pn); goto done_call;
                    case 1: result = ALLOC_node_self_tail_call_global_1(pn, aN[0]); goto done_call;
                    case 2: result = ALLOC_node_self_tail_call_global_2(pn, aN[0], aN[1]); goto done_call;
                    case 3: result = ALLOC_node_self_tail_call_global_3(pn, aN[0], aN[1], aN[2]); goto done_call;
                    case 4: result = ALLOC_node_self_tail_call_global_4(pn, aN[0], aN[1], aN[2], aN[3]); goto done_call;
                    }
                }
            }
        }
    }

    {
        NODE *spec = try_specialize_arith(c, sp_top[0], sp_top[1], scope);
        if (spec) { result = spec; goto done_call; }
    }

    {
        NODE *fn = compile(c, sp_top[0], scope, false);
        int argc = list_length(sp_top[1]);
        NODE *aN[8];
        if (argc <= 4) {
            int i = 0;
            for (sp_top[2] = sp_top[1]; scm_is_pair(sp_top[2]); sp_top[2] = cdr(sp_top[2]), i++) {
                aN[i] = compile(c, car(sp_top[2]), scope, false);
            }
            switch (argc) {
            case 0: result = ALLOC_node_call_0((uint32_t)is_tail, fn); goto done_call;
            case 1: result = ALLOC_node_call_1((uint32_t)is_tail, fn, aN[0]); goto done_call;
            case 2: result = ALLOC_node_call_2((uint32_t)is_tail, fn, aN[0], aN[1]); goto done_call;
            case 3: result = ALLOC_node_call_3((uint32_t)is_tail, fn, aN[0], aN[1], aN[2]); goto done_call;
            case 4: result = ALLOC_node_call_4((uint32_t)is_tail, fn, aN[0], aN[1], aN[2], aN[3]); goto done_call;
            }
        }
        NODE **abuf = (NODE **)scm_alloc_min(c, sizeof(NODE *) * argc);
        int i = 0;
        for (sp_top[2] = sp_top[1]; scm_is_pair(sp_top[2]); sp_top[2] = cdr(sp_top[2]), i++) {
            abuf[i] = compile(c, car(sp_top[2]), scope, false);
        }
        uint32_t base = register_call_args(abuf, (uint32_t)argc);
        result = ALLOC_node_call_n((uint32_t)is_tail, fn, base, (uint32_t)argc);
    }
done_call:
    SP_POP(c, sp_top);
    return result;
}

// Build a quoted constant from a (read-only) scheme value.  Symbols/lists/
// vectors get embedded as a `node_quote(uint64_t)` referencing the value's
// pointer; immediates use the typed const nodes for clarity.
static NODE *
compile_quote(VALUE v)
{
    if (SCM_IS_FIXNUM(v)) {
        int64_t n = SCM_FIXVAL(v);
        if (n >= INT32_MIN && n <= INT32_MAX) return ALLOC_node_const_int((int32_t)n);
        return ALLOC_node_const_int64((uint64_t)v);
    }
    if (v == SCM_NIL)    return ALLOC_node_const_nil();
    if (v == SCM_TRUE)   return ALLOC_node_const_bool(1);
    if (v == SCM_FALSE)  return ALLOC_node_const_bool(0);
    if (v == SCM_UNSPEC) return ALLOC_node_const_unspec();
    if (scm_is_symbol(v)) return ALLOC_node_const_sym(scm_perm_name(SCM_PTR(v)->sym.name));
    if (scm_is_string(v)) return ALLOC_node_const_str(scm_perm_name(SCM_PTR(v)->str.chars));
    if (scm_is_char(v))   return ALLOC_node_const_char(SCM_PTR(v)->ch);
    if (scm_is_double(v)) return ALLOC_node_const_double(scm_get_double(v));
    return scm_alloc_quote((uint64_t)v);
}

// Detect leading internal-define forms in a body and rewrite to letrec
// (modeled with `let` over uninitialized slots + `set!` initializers).
// Returns the transformed body list.
static VALUE
hoist_internal_defines(CTX *c, VALUE body)
{
    if (!scm_is_pair(body)) return body;
    /* sp[0] = body (advances across iterations),
     * sp[1] = bindings head,
     * sp[2] = bindings last cell,
     * sp[3..] = per-iter staging (init/lambda + pair). */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 6);
    sp[0] = body;
    sp[1] = SCM_NIL;
    sp[2] = SCM_NIL;
    while (scm_is_pair(sp[0]) && scm_is_pair(car(sp[0])) && is_symbol(car(car(sp[0])), "define")) {
        VALUE def = car(sp[0]);
        /* Park name + init in sp slots BEFORE any scm_cons (which can
         * GC and move the symbol/payload).  A C-local `name` here goes
         * stale across the lambda-form construction below — the
         * subsequent (name init) cons then bakes the dead pointer into
         * the synthesized letrec's binding pair's car. */
        if (scm_is_pair(cadr(def))) {
            // (define (f params) body...) — synthesize lambda form
            sp[4] = car(cadr(def));               /* name (= symbol VALUE) */
            sp[3] = cdr(cadr(def));               /* params */
            /* sp[5] = bodydefs.  re-derive def via sp[0] (def may have
             * moved across the cons cascade below). */
            sp[5] = cdr(cdr(car(sp[0])));
            sp[5] = scm_cons(c, sp[3], sp[5]);    /* (params . body) */
            sp[5] = scm_cons_sym(c, "lambda", sp[5]); /* (lambda params . body) */
            sp[3] = sp[5];                        /* init = lambda form */
        } else {
            /* re-derive def via sp[0] in case prior iteration GC'd */
            VALUE d2 = car(sp[0]);
            sp[4] = cadr(d2);                     /* name */
            sp[3] = caddr(d2);                    /* init */
        }
        /* pair = (name init) */
        VALUE init_cell = scm_cons(c, sp[3], SCM_NIL);
        sp[5] = init_cell;
        sp[5] = scm_cons(c, sp[4], sp[5]);
        VALUE pcell = scm_cons(c, sp[5], SCM_NIL);
        if (sp[1] == SCM_NIL) { sp[1] = pcell; }
        else {
            struct sobj *last_cell = SCM_PTR(sp[2]);
            ARO_STORE(c, last_cell, &last_cell->pair.cdr, pcell);
        }
        sp[2] = pcell;
        sp[0] = SCM_PTR(sp[0])->pair.cdr;
    }
    if (sp[1] == SCM_NIL) {
        VALUE r = sp[0];
        SP_POP(c, sp);
        return r;
    }
    /* letrec = (letrec bindings body) */
    sp[5] = scm_cons(c, sp[1], sp[0]);
    sp[5] = scm_cons_sym(c, "letrec", sp[5]);
    sp[5] = scm_cons(c, sp[5], SCM_NIL);
    VALUE r = sp[5];
    SP_POP(c, sp);
    return r;
}

static NODE *
compile_body(CTX *c, VALUE body, struct lex_scope *scope, bool is_tail)
{
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 1);
    sp[0] = hoist_internal_defines(c, body);
    NODE *r = compile_seq(c, sp[0], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// COMPILE_INNER_LAMBDA_SEEN bubbles outward from any `compile_lambda` —
// each enclosing compile_lambda observes whether its (transitive) body
// contained a nested `lambda`.  The flag feeds the closure's `leaf` bit,
// which scm_apply_tail consults to decide whether a self-tail-call can
// reuse the existing frame in place.  Without this gating, a tail call
// that overwrote a frame captured by an inner closure would silently
// corrupt that closure's lexical view.
static bool COMPILE_INNER_LAMBDA_SEEN = false;

// Build a (lambda (params) body...) node.  Handles fixed-arity, dotted
// rest, and the trivial `(lambda x body)` rest-only form.
static NODE *
compile_lambda(CTX *c, VALUE params, VALUE body, struct lex_scope *scope)
{
    int nparams = 0;
    int has_rest = 0;
    /* names_buf holds libc-permanent cstrs — stable across moving GC. */
    const char *names_buf[64];
    int nslots = 0;

    /* Park params + body across the scm_alloc_min / push_scope / compile
     * cascade.  Walk params using sp[0]. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);
    sp[0] = params;
    sp[1] = body;
    if (scm_is_symbol(sp[0])) {
        names_buf[nslots++] = scm_perm_name(SCM_PTR(sp[0])->sym.name);
        nparams = 0;
        has_rest = 1;
    } else {
        VALUE p = sp[0];
        while (scm_is_pair(p)) {
            if (nslots >= (int)(sizeof(names_buf)/sizeof(names_buf[0])))
                scm_error(c, "too many lambda parameters");
            names_buf[nslots++] = scm_perm_name(SCM_PTR(car(p))->sym.name);
            nparams++;
            p = cdr(p);
        }
        if (scm_is_symbol(p)) {
            names_buf[nslots++] = scm_perm_name(SCM_PTR(p)->sym.name);
            has_rest = 1;
        }
    }
    char **names = (char **)scm_alloc_min(c, sizeof(char *) * (nslots ? nslots : 1));
    for (int i = 0; i < nslots; i++) names[i] = (char *)(uintptr_t)names_buf[i];

    struct lex_scope *new_scope = push_scope(c, scope, nslots, names);
    new_scope->is_lambda_boundary = true;

    bool saved = COMPILE_INNER_LAMBDA_SEEN;
    COMPILE_INNER_LAMBDA_SEEN = false;
    // Nested lambdas don't share the enclosing named-let's self-call
    // identity — even if they syntactically reference NAME, the call
    // happens in a different env when the closure escapes.  But
    // compile_define for `(define (f params) body)` arms
    // SELF_CALL_FOR_NEXT_LAMBDA so that the IMMEDIATE compile_lambda
    // for f's lambda gets a global-mode ctx for body compilation.
    // Consume the token here (so nested compile_lambda calls inside
    // body still get the standard save+clear).
    struct self_call_ctx *saved_self_call = CURRENT_SELF_CALL;
    if (SELF_CALL_FOR_NEXT_LAMBDA) {
        CURRENT_SELF_CALL = SELF_CALL_FOR_NEXT_LAMBDA;
        SELF_CALL_FOR_NEXT_LAMBDA = NULL;
    } else {
        CURRENT_SELF_CALL = NULL;
    }
    NODE *body_node = compile_body(c, sp[1], new_scope, true);   // body is in tail position
    bool body_has_inner_lambda = COMPILE_INNER_LAMBDA_SEEN;
    // Bubble the "we are a lambda" flag to the enclosing scope.
    COMPILE_INNER_LAMBDA_SEEN = saved || true;
    CURRENT_SELF_CALL = saved_self_call;

    aot_add_entry(body_node);
    /* Conservative: a body containing an inner lambda might create a
     * closure that captures THIS frame.  If we skip sframe alloc (= fast
     * path), inner_closure.env would point to the caller's frame instead
     * of ours, so inner's lref(d>=1) reads wrong slot.  Force has_outer_ref
     * to disable fast path whenever inner lambdas are present. */
    if (body_has_inner_lambda) new_scope->has_outer_ref = true;
    /* has_rest closures pack the trailing arguments into a list at
     * params[nparams].  The fast-path frame_sp layout doesn't accommodate
     * that, so disable patching for has_rest. */
    if (has_rest) new_scope->has_outer_ref = true;
    uint32_t no_capture = (!new_scope->has_outer_ref) ? 1u : 0u;
    /* If body is no_capture, patch every pending lref/lset NODE to its
     * _sp variant (= reads sp[sp_offset] directly).  Patching only
     * `head.kind` + `head.dispatcher` works because the two variants
     * share operand layout (depth, idx, sp_offset).  Body invoked via
     * node_call_K's no_capture fast path with body_sp = c->sp = past args
     * → sp[idx - nparams] = arg[idx]. */
    if (no_capture) {
        extern const struct NodeKind kind_node_lref_sp;
        extern const struct NodeKind kind_node_lset_sp;
        extern const struct NodeKind kind_node_lref;
        extern const struct NodeKind kind_node_lset;
        for (size_t i = 0; i < new_scope->pending_n; i++) {
            NODE *nd = new_scope->pending_lrefs[i];
            bool patched = false;
            if (nd->head.kind == &kind_node_lref) {
                nd->head.dispatcher = kind_node_lref_sp.default_dispatcher;
                nd->head.dispatcher_name = kind_node_lref_sp.default_dispatcher_name;
                nd->head.kind = &kind_node_lref_sp;
                patched = true;
            } else if (nd->head.kind == &kind_node_lset) {
                nd->head.dispatcher = kind_node_lset_sp.default_dispatcher;
                nd->head.dispatcher_name = kind_node_lset_sp.default_dispatcher_name;
                nd->head.kind = &kind_node_lset_sp;
                patched = true;
            }
            if (patched) {
                /* Patching changed kind: re-run OPTIMIZE so astro_cs_load
                 * looks up the SD by the post-patch hash.  HASH itself
                 * doesn't cache (see runtime/astro_node.c), so it always
                 * reflects the current kind. */
                OPTIMIZE(nd);
            }
        }
        /* Re-OPTIMIZE the lambda body so later runs bind the right SD
         * for it (its hash now derives from patched children). */
        if (body_node) {
            OPTIMIZE(body_node);
        }
    }
    free(new_scope->pending_lrefs);
    new_scope->pending_lrefs = NULL;
    new_scope->pending_n = new_scope->pending_cap = 0;
    NODE *r = ALLOC_node_lambda((uint32_t)nparams, (uint32_t)has_rest, (uint32_t)nslots,
                                body_has_inner_lambda ? 0 : 1,
                                no_capture,
                                body_node);
    SP_POP(c, sp);
    return r;
}

// (let ((a v) (b w)) body) → ((lambda (a b) body) v w)
static NODE *
compile_let(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE second = cadr(form);
    if (scm_is_symbol(second)) {
        // Named let.  Build the AST directly so we can install
        // CURRENT_SELF_CALL around the inner-lambda body compile and let
        // compile_call lower self-recursive tail calls to
        // node_self_tail_call_K (collapsing the trampoline tail loop into
        // a tight C for(;;) inside node_loop).  The general fallback path
        // — letrec → let → lambda call — still works for higher arities
        // and any case the optimization can't handle.
        VALUE name = second;
        VALUE bindings = caddr(form);
        VALUE body = cdr(cdr(cdr(form)));

        int nparams = 0;
        const char *param_names[ASCHEME_LOOP_MAX_PARAMS];
        for (VALUE b = bindings; scm_is_pair(b); b = cdr(b)) {
            if (nparams >= ASCHEME_LOOP_MAX_PARAMS) goto named_let_fallback;
            VALUE bn = car(car(b));
            param_names[nparams++] = scm_perm_name(SCM_PTR(bn)->sym.name);
        }

        /* Park bindings, body, name across the upcoming scm_alloc_min /
         * compile cascade — all of which can trigger GC. */
        VALUE * restrict sp_nlh = c->sp;
        SP_PUSH(c, sp_nlh, 3);
        sp_nlh[0] = name;
        sp_nlh[1] = bindings;
        sp_nlh[2] = body;
        // Outer scope: just the loop binding.
        char **outer_names = (char **)scm_alloc_min(c, sizeof(char *));
        outer_names[0] = (char *)(uintptr_t)scm_perm_name(SCM_PTR(sp_nlh[0])->sym.name);
        struct lex_scope *outer_scope = push_scope(c, scope, 1, outer_names);

        // Inner scope: the loop's params, with parent = outer scope.
        char **inner_names = (char **)scm_alloc_min(c, sizeof(char *) * (nparams ? nparams : 1));
        for (int i = 0; i < nparams; i++) inner_names[i] = (char *)(uintptr_t)param_names[i];
        struct lex_scope *inner_scope = push_scope(c, outer_scope, nparams, inner_names);

        // Compile inner body with self-call recognition active.
        struct self_call_ctx ctx = { outer_names[0], (uint32_t)nparams, outer_scope, false };
        struct self_call_ctx *saved_sc = CURRENT_SELF_CALL;
        CURRENT_SELF_CALL = &ctx;
        bool saved_inner = COMPILE_INNER_LAMBDA_SEEN;
        COMPILE_INNER_LAMBDA_SEEN = false;

        NODE *inner_body = compile_body(c, sp_nlh[2], inner_scope, /*is_tail=*/true);

        bool body_has_inner_lambda = COMPILE_INNER_LAMBDA_SEEN;
        COMPILE_INNER_LAMBDA_SEEN = saved_inner || true;
        CURRENT_SELF_CALL = saved_sc;

        // If at least one self-tail-call was patched, wrap the body in
        // node_loop so the patched call's c->loop_continue=1 gets caught
        // and routed back to the body.  Otherwise leave it bare — the
        // wrapper would only add overhead for nothing.
        /* Named-let inner lambda is forced no_capture=0 below.  leaf is
         * computed from body_has_inner_lambda: non-leaf means an inner
         * lambda may capture the loop frame, so node_loop must allocate
         * a fresh frame per iteration to preserve R5RS binding semantics. */
        NODE *inner_body_final = ctx.used
            ? ALLOC_node_loop(inner_body, (uint32_t)nparams, /*no_capture=*/0u,
                              body_has_inner_lambda ? 0u : 1u)
            : inner_body;
        aot_add_entry(inner_body_final);

        // Named-let inner lambda: body references outer's `name` binding via
        // lref(0, 0) — wait, actually our compile path makes it depth=0 in
        // the inner_scope (= the call's frame in named-let).  Conservatively
        // assume capture (= no_capture=0) for named-let inner lambdas; full
        // analysis would require tracking inner_scope's outer ref flag.
        NODE *inner_lambda = ALLOC_node_lambda((uint32_t)nparams, 0,
                                               (uint32_t)nparams,
                                               body_has_inner_lambda ? 0 : 1,
                                               0u, // conservative: assume capture
                                               inner_body_final);

        // Outer lambda body:
        //   (seq (lset 0 0 inner_lambda)
        //        (call_K (lref 0 0) inits...))
        // Outer scope has 1 slot (= the inner closure binding) → sp_offset = 0 - 1 = -1.
        NODE *lset = ALLOC_node_lset(0, 0, -1, inner_lambda);
        NODE *fn_lref = ALLOC_node_lref(0, 0, -1);

        NODE *init_nodes[ASCHEME_LOOP_MAX_PARAMS];
        /* Walk bindings via sp slot to keep cursor alive across the inner
         * compile() calls. */
        VALUE * restrict sp_iter = c->sp;
        SP_PUSH(c, sp_iter, 1);
        int ii = 0;
        for (sp_iter[0] = sp_nlh[1]; scm_is_pair(sp_iter[0]); sp_iter[0] = cdr(sp_iter[0]), ii++) {
            init_nodes[ii] = compile(c, cadr(car(sp_iter[0])), outer_scope, false);
        }
        SP_POP(c, sp_iter);

        NODE *call_inner;
        switch (nparams) {
        case 0:
            call_inner = ALLOC_node_call_0(/*is_tail=*/1, fn_lref);
            break;
        case 1:
            call_inner = ALLOC_node_call_1(1, fn_lref, init_nodes[0]);
            break;
        case 2:
            call_inner = ALLOC_node_call_2(1, fn_lref, init_nodes[0], init_nodes[1]);
            break;
        case 3:
            call_inner = ALLOC_node_call_3(1, fn_lref, init_nodes[0], init_nodes[1], init_nodes[2]);
            break;
        case 4:
            call_inner = ALLOC_node_call_4(1, fn_lref, init_nodes[0], init_nodes[1], init_nodes[2], init_nodes[3]);
            break;
        default: {
            // call_n for higher arities (>=5).
            NODE **abuf = (NODE **)scm_alloc_min(c, sizeof(NODE *) * nparams);
            for (int i = 0; i < nparams; i++) abuf[i] = init_nodes[i];
            uint32_t base = register_call_args(abuf, (uint32_t)nparams);
            call_inner = ALLOC_node_call_n(1, fn_lref, base, (uint32_t)nparams);
            break;
        }
        }

        NODE *outer_body = ALLOC_node_seq(lset, call_inner);
        aot_add_entry(outer_body);

        NODE *outer_lambda = ALLOC_node_lambda(1, 0, 1,
                                               /*leaf=*/0, // contains inner lambda
                                               /*no_capture=*/0u, // wraps inner that captures outer
                                               outer_body);

        NODE *unspec = ALLOC_node_const_unspec();
        NODE *r_named = ALLOC_node_call_1((uint32_t)is_tail, outer_lambda, unspec);
        SP_POP(c, sp_nlh);
        return r_named;

      named_let_fallback:
        ; // fall through to the original letrec desugaring
      {
        /* Precise rooting: park name + bindings + body, then build params /
         * inits via head + last-cell pattern with the iter cursor in sp.
         *   sp[0]=name,  sp[1]=bindings,  sp[2]=body,
         *   sp[3]=params head, sp[4]=params last,
         *   sp[5]=inits head,  sp[6]=inits last,
         *   sp[7]=iter cursor.
         *   sp[8] = staging slot for intermediate cons chains. */
        VALUE * restrict sp_nl = c->sp;
        SP_PUSH(c, sp_nl, 9);
        sp_nl[0] = name;
        sp_nl[1] = bindings;
        sp_nl[2] = body;
        sp_nl[3] = SCM_NIL;
        sp_nl[4] = SCM_NIL;
        sp_nl[5] = SCM_NIL;
        sp_nl[6] = SCM_NIL;
        for (sp_nl[7] = sp_nl[1]; scm_is_pair(sp_nl[7]);
             sp_nl[7] = SCM_PTR(sp_nl[7])->pair.cdr) {
            VALUE bn = car(car(sp_nl[7]));
            VALUE pcell = scm_cons(c, bn, SCM_NIL);
            if (sp_nl[3] == SCM_NIL) { sp_nl[3] = pcell; }
            else {
                struct sobj *last = SCM_PTR(sp_nl[4]);
                ARO_STORE(c, last, &last->pair.cdr, pcell);
            }
            sp_nl[4] = pcell;
            VALUE bv = cadr(car(sp_nl[7]));
            VALUE icell = scm_cons(c, bv, SCM_NIL);
            if (sp_nl[5] == SCM_NIL) { sp_nl[5] = icell; }
            else {
                struct sobj *last = SCM_PTR(sp_nl[6]);
                ARO_STORE(c, last, &last->pair.cdr, icell);
            }
            sp_nl[6] = icell;
        }
        /* lambda = (lambda params body...) */
        sp_nl[8] = scm_cons(c, sp_nl[3], sp_nl[2]);
        sp_nl[8] = scm_cons_sym(c, "lambda", sp_nl[8]);
        /* letrec_binding = (name lambda) */
        VALUE lr_lambda = scm_cons(c, sp_nl[8], SCM_NIL);
        sp_nl[8] = lr_lambda;
        sp_nl[8] = scm_cons(c, sp_nl[0], sp_nl[8]);
        /* letrec_bindings = (letrec_binding) */
        sp_nl[8] = scm_cons(c, sp_nl[8], SCM_NIL);
        /* call = (name inits...) */
        sp_nl[4] = scm_cons(c, sp_nl[0], sp_nl[5]);    /* reuse sp_nl[4] */
        /* call_form = (call) */
        sp_nl[4] = scm_cons(c, sp_nl[4], SCM_NIL);
        /* letrec = (letrec letrec_bindings call_form...) — last arg is body
         * shaped: (letrec letrec_bindings . call_form).  Original code: */
        VALUE letrec_kw = scm_intern(c, "letrec");
        sp_nl[4] = scm_cons(c, sp_nl[8], sp_nl[4]);    /* (letrec_bindings . call_form) */
        sp_nl[4] = scm_cons(c, letrec_kw, sp_nl[4]);
        NODE *r = compile(c, sp_nl[4], scope, is_tail);
        SP_POP(c, sp_nl);
        return r;
      }
    }
    /* Standard let: (let ((a v) (b w)) body) → ((lambda (a b) body) v w).
     *   sp[0]=form,   sp[1]=bindings head,   sp[2]=body,
     *   sp[3]=params head,   sp[4]=params last cell,
     *   sp[5]=inits  head,   sp[6]=inits  last cell,
     *   sp[7]=iter cursor (b) for safe traversal across cons allocations.
     */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 8);
    sp[0] = form;
    sp[1] = cadr(form);
    sp[2] = cdr(cdr(form));
    sp[3] = SCM_NIL;
    sp[4] = SCM_NIL;
    sp[5] = SCM_NIL;
    sp[6] = SCM_NIL;
    for (sp[7] = sp[1]; scm_is_pair(sp[7]); sp[7] = SCM_PTR(sp[7])->pair.cdr) {
        VALUE bn = car(car(sp[7]));
        VALUE bv = cadr(car(sp[7]));
        VALUE pcell = scm_cons(c, bn, SCM_NIL);
        if (sp[3] == SCM_NIL) { sp[3] = pcell; }
        else {
            struct sobj *last = SCM_PTR(sp[4]);
            ARO_STORE(c, last, &last->pair.cdr, pcell);
        }
        sp[4] = pcell;
        /* sp[7] still valid (= rooted), but bv (= cadr(car(sp[7]))) was
         * captured before scm_cons; that VALUE could have moved.  Re-read.
         */
        bv = cadr(car(sp[7]));
        VALUE icell = scm_cons(c, bv, SCM_NIL);
        if (sp[5] == SCM_NIL) { sp[5] = icell; }
        else {
            struct sobj *last = SCM_PTR(sp[6]);
            ARO_STORE(c, last, &last->pair.cdr, icell);
        }
        sp[6] = icell;
    }
    /* Build (lambda params body) → call w/ inits.  Stage via sp[4]. */
    sp[4] = scm_cons(c, sp[3], sp[2]);      /* (params . body) */
    {
        VALUE lambda_kw = scm_intern(c, "lambda");
        sp[4] = scm_cons(c, lambda_kw, sp[4]);   /* (lambda params . body) */
    }
    sp[4] = scm_cons(c, sp[4], sp[5]);       /* ((lambda ...) inits...) */
    NODE *r = compile(c, sp[4], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// (let* ((a v) (b w)) body) → (let ((a v)) (let ((b w)) body))
static NODE *
compile_letstar(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 5);
    sp[0] = form;
    sp[1] = cadr(form);            /* bindings */
    sp[2] = cdr(cdr(form));        /* body */
    if (sp[1] == SCM_NIL) {
        sp[3] = scm_cons(c, SCM_NIL, sp[2]);
        sp[3] = scm_cons_sym(c, "let", sp[3]);
        NODE *r = compile(c, sp[3], scope, is_tail);
        SP_POP(c, sp);
        return r;
    }
    VALUE first = car(sp[1]);
    VALUE rest = cdr(sp[1]);
    sp[3] = first;
    sp[4] = rest;
    /* inner = (let* rest body...) */
    VALUE inner = scm_cons(c, sp[4], sp[2]);
    sp[4] = inner;
    sp[4] = scm_cons_sym(c, "let*", sp[4]);
    /* outer = (let ((first)) inner) */
    VALUE first_cell = scm_cons(c, sp[3], SCM_NIL);
    sp[3] = first_cell;
    VALUE inner_cell = scm_cons(c, sp[4], SCM_NIL);
    sp[4] = inner_cell;
    sp[3] = scm_cons(c, sp[3], sp[4]);   /* ((first) inner) */
    sp[3] = scm_cons_sym(c, "let", sp[3]);
    NODE *r = compile(c, sp[3], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// (letrec ((a v) ...) body) →
//   (let ((a <unspec>) ...) (set! a v) ... body)
static NODE *
compile_letrec(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    /* Precise rooting: park form + bindings + body + accumulators across the
     * many scm_cons / scm_intern calls below.
     *   sp[0]=form, sp[1]=bindings, sp[2]=body,
     *   sp[3]=pairs head,  sp[4]=pairs last cell,
     *   sp[5]=assigns head, sp[6]=assigns last,
     *   sp[7]=iter cursor, sp[8]=staging. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 9);
    sp[0] = form;
    sp[1] = cadr(form);
    sp[2] = cdr(cdr(form));
    sp[3] = SCM_NIL;
    sp[4] = SCM_NIL;
    sp[5] = SCM_NIL;
    sp[6] = SCM_NIL;
    for (sp[7] = sp[1]; scm_is_pair(sp[7]); sp[7] = SCM_PTR(sp[7])->pair.cdr) {
        /* `name` is a heap-typed symbol VALUE; never cache it across an
         * alloc — re-read from the rooted sp[7] / sp[8] chain each time
         * (= every scm_cons call below may relocate the symbol object). */
        /* undef = (name SCM_UNSPEC) */
        sp[8] = scm_cons(c, SCM_UNSPEC, SCM_NIL);
        sp[8] = scm_cons(c, car(car(sp[7])), sp[8]);   /* re-read name */
        VALUE pcell = scm_cons(c, sp[8], SCM_NIL);
        if (sp[3] == SCM_NIL) { sp[3] = pcell; }
        else {
            struct sobj *last = SCM_PTR(sp[4]);
            ARO_STORE(c, last, &last->pair.cdr, pcell);
        }
        sp[4] = pcell;
        /* setform = (set! name init) */
        sp[8] = scm_cons(c, cadr(car(sp[7])), SCM_NIL);   /* (init) */
        sp[8] = scm_cons(c, car(car(sp[7])), sp[8]);      /* (name init) — re-read */
        sp[8] = scm_cons_sym(c, "set!", sp[8]);
        VALUE acell = scm_cons(c, sp[8], SCM_NIL);
        if (sp[5] == SCM_NIL) { sp[5] = acell; }
        else {
            struct sobj *last = SCM_PTR(sp[6]);
            ARO_STORE(c, last, &last->pair.cdr, acell);
        }
        sp[6] = acell;
    }
    /* inner_body = append(assigns, body) */
    if (sp[5] == SCM_NIL) {
        sp[5] = sp[2];
    } else {
        struct sobj *last = SCM_PTR(sp[6]);
        ARO_STORE(c, last, &last->pair.cdr, sp[2]);
    }
    /* letform = (let pairs inner_body...) */
    sp[8] = scm_cons(c, sp[3], sp[5]);
    sp[8] = scm_cons_sym(c, "let", sp[8]);
    NODE *r = compile(c, sp[8], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// (cond (test e...) ... (else e...))
static NODE *
compile_cond(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    if (cdr(form) == SCM_NIL) return ALLOC_node_const_unspec();
    /* Park form + (first clause, rest of clauses, test, body, staging). */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 6);
    sp[0] = form;
    VALUE clauses = cdr(form);
    sp[1] = car(clauses);              /* first clause */
    sp[2] = cdr(clauses);              /* rest clauses */
    sp[3] = car(sp[1]);                /* test */
    sp[4] = cdr(sp[1]);                /* body */
    sp[5] = SCM_NIL;                   /* staging */
    bool is_else = is_symbol(sp[3], "else");
    NODE *thn;
    if (sp[4] == SCM_NIL) {
        thn = compile(c, sp[3], scope, is_tail);
    } else if (scm_is_pair(sp[4]) && is_symbol(car(sp[4]), "=>")) {
        /* (cond (test => fn) ...) → (fn test) */
        VALUE fn = cadr(sp[4]);
        VALUE test_cell = scm_cons(c, sp[3], SCM_NIL);
        sp[5] = test_cell;
        sp[5] = scm_cons(c, fn, sp[5]);
        thn = compile(c, sp[5], scope, is_tail);
    } else {
        VALUE begin_sym = scm_intern(c, "begin");
        sp[5] = scm_cons(c, begin_sym, sp[4]);
        thn = compile(c, sp[5], scope, is_tail);
    }
    NODE *r;
    if (is_else) { r = thn; goto done; }
    NODE *cnd = compile(c, sp[3], scope, false);
    NODE *els;
    if (sp[2] == SCM_NIL) {
        els = ALLOC_node_const_unspec();
    } else {
        VALUE cond_sym = scm_intern(c, "cond");
        sp[5] = scm_cons(c, cond_sym, sp[2]);
        els = compile_cond(c, sp[5], scope, is_tail);
    }
    r = ALLOC_node_if(cnd, thn, els);
done:
    SP_POP(c, sp);
    return r;
}

// (case key (vals body...) ... (else body...))
static NODE *
compile_case(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    /* sp[0]=form, sp[1]=key, sp[2]=clauses head, sp[3]=k_sym, sp[4]=bindings,
     * sp[5]=cond_clauses head, sp[6]=cond_clauses last, sp[7]=iter cl,
     * sp[8]=staging */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 9);
    sp[0] = form;
    sp[1] = cadr(form);
    sp[2] = cdr(cdr(form));
    sp[3] = gensym_at(c, "case-key");
    /* bindings = ((k_sym key)) */
    sp[8] = scm_cons(c, sp[1], SCM_NIL);
    sp[8] = scm_cons(c, sp[3], sp[8]);
    sp[4] = scm_cons(c, sp[8], SCM_NIL);
    sp[5] = SCM_NIL;
    sp[6] = SCM_NIL;
    for (sp[7] = sp[2]; scm_is_pair(sp[7]); sp[7] = SCM_PTR(sp[7])->pair.cdr) {
        VALUE vals = car(car(sp[7]));
        if (is_symbol(vals, "else")) {
            /* (else body...) — wrap as (else . body) directly. */
            VALUE test = scm_intern(c, "else");
            /* Re-read body_clause AFTER scm_intern (= GC trigger). */
            VALUE body_clause = cdr(car(sp[7]));
            sp[8] = scm_cons(c, test, body_clause);
        } else {
            /* quoted_vals = (quote vals); test = (memv k_sym quoted_vals) */
            VALUE qv_tail = scm_cons(c, vals, SCM_NIL);
            sp[8] = qv_tail;
            sp[8] = scm_cons_sym(c, "quote", sp[8]);
            /* test = (memv k_sym sp[8]) */
            VALUE t_tail = scm_cons(c, sp[8], SCM_NIL);
            sp[8] = t_tail;
            sp[8] = scm_cons(c, sp[3], sp[8]);
            sp[8] = scm_cons_sym(c, "memv", sp[8]);
            /* re-read clause body since multiple allocs above moved it */
            VALUE body_clause = cdr(car(sp[7]));
            sp[8] = scm_cons(c, sp[8], body_clause);
        }
        VALUE cell = scm_cons(c, sp[8], SCM_NIL);
        if (sp[5] == SCM_NIL) { sp[5] = cell; }
        else {
            struct sobj *last = SCM_PTR(sp[6]);
            ARO_STORE(c, last, &last->pair.cdr, cell);
        }
        sp[6] = cell;
    }
    /* cnd = (cond cond_clauses...) */
    sp[8] = scm_cons_sym(c, "cond", sp[5]);
    /* letform = (let bindings cnd) */
    VALUE cnd_cell = scm_cons(c, sp[8], SCM_NIL);
    sp[8] = cnd_cell;
    sp[8] = scm_cons(c, sp[4], sp[8]);
    sp[8] = scm_cons_sym(c, "let", sp[8]);
    NODE *r = compile(c, sp[8], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// (and a b c) → (if a (if b c #f) #f).  (and) → #t.  (and a) → a.
static NODE *
compile_and(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    if (cdr(form) == SCM_NIL) return ALLOC_node_const_bool(1);
    if (cdr(cdr(form)) == SCM_NIL) return compile(c, car(cdr(form)), scope, is_tail);
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = form;
    sp[1] = cdr(form);                       /* args */
    sp[2] = SCM_NIL;
    NODE *cnd = compile(c, car(sp[1]), scope, false);
    sp[2] = scm_cons_sym(c, "and", cdr(sp[1]));
    NODE *thn = compile(c, sp[2], scope, is_tail);
    NODE *els = ALLOC_node_const_bool(0);
    NODE *r = ALLOC_node_if(cnd, thn, els);
    SP_POP(c, sp);
    return r;
}

// (or a b c) — returns first truthy or #f.  Implemented as
// (let ((t a)) (if t t (or b c))) using temp slot at depth 0 of an
// inserted scope.
static NODE *
compile_or(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    if (cdr(form) == SCM_NIL) return ALLOC_node_const_bool(0);
    if (cdr(cdr(form)) == SCM_NIL) return compile(c, car(cdr(form)), scope, is_tail);
    /* sp[0]=form, sp[1]=args, sp[2]=tmp sym, sp[3]=binding, sp[4]=rest,
     * sp[5]=staging iff, sp[6]=letform */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 7);
    sp[0] = form;
    sp[1] = cdr(form);
    sp[2] = gensym_at(c, "or-tmp");
    /* binding = ((tmp args[0])) */
    sp[3] = scm_cons(c, car(sp[1]), SCM_NIL);
    sp[3] = scm_cons(c, sp[2], sp[3]);
    sp[3] = scm_cons(c, sp[3], SCM_NIL);
    /* rest = (or args[1..]) */
    sp[4] = scm_cons_sym(c, "or", cdr(sp[1]));
    /* iff = (if tmp tmp rest) */
    sp[5] = scm_cons(c, sp[4], SCM_NIL);
    sp[5] = scm_cons(c, sp[2], sp[5]);
    sp[5] = scm_cons(c, sp[2], sp[5]);
    sp[5] = scm_cons_sym(c, "if", sp[5]);
    /* letform = (let binding iff) */
    sp[6] = scm_cons(c, sp[5], SCM_NIL);
    sp[6] = scm_cons(c, sp[3], sp[6]);
    sp[6] = scm_cons_sym(c, "let", sp[6]);
    NODE *r = compile(c, sp[6], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

// (when test body) → (if test (begin body) <unspec>).  (unless test body)
// inverts.
static NODE *
compile_when(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = form;
    sp[1] = cadr(form);                  /* test */
    sp[2] = cdr(cdr(form));              /* body */
    NODE *cnd = compile(c, sp[1], scope, false);
    sp[2] = scm_cons_sym(c, "begin", sp[2]);
    NODE *thn = compile(c, sp[2], scope, is_tail);
    NODE *els = ALLOC_node_const_unspec();
    NODE *r = ALLOC_node_if(cnd, thn, els);
    SP_POP(c, sp);
    return r;
}

static NODE *
compile_unless(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 3);
    sp[0] = form;
    sp[1] = cadr(form);
    sp[2] = cdr(cdr(form));
    NODE *cnd = compile(c, sp[1], scope, false);
    NODE *thn = ALLOC_node_const_unspec();
    sp[2] = scm_cons_sym(c, "begin", sp[2]);
    NODE *els = compile(c, sp[2], scope, is_tail);
    NODE *r = ALLOC_node_if(cnd, thn, els);
    SP_POP(c, sp);
    return r;
}

// (do ((var init step) ...) (test result...) body...) →
//   (letrec ((loop (lambda (var ...)
//                    (if test
//                        (begin result...)
//                        (begin body... (loop step...))))))
//     (loop init ...))
static NODE *
compile_do(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    /* sp[0]=form, sp[1]=specs, sp[2]=test_clause, sp[3]=body, sp[4]=test,
     * sp[5]=result, sp[6]=vars head, sp[7]=vars last,
     * sp[8]=inits head, sp[9]=inits last,
     * sp[10]=steps head, sp[11]=steps last,
     * sp[12]=iter cursor, sp[13]=loop_sym,
     * sp[14..]=staging */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 18);
    sp[0] = form;
    sp[1] = cadr(form);
    sp[2] = caddr(form);
    sp[3] = cdr(cdr(cdr(form)));
    sp[4] = car(sp[2]);
    sp[5] = cdr(sp[2]);
    sp[6] = sp[7] = SCM_NIL;
    sp[8] = sp[9] = SCM_NIL;
    sp[10] = sp[11] = SCM_NIL;
    for (sp[12] = sp[1]; scm_is_pair(sp[12]); sp[12] = SCM_PTR(sp[12])->pair.cdr) {
        VALUE spec = car(sp[12]);
        VALUE var = car(spec);
        VALUE vcell = scm_cons(c, var, SCM_NIL);
        if (sp[6] == SCM_NIL) { sp[6] = vcell; }
        else {
            struct sobj *last = SCM_PTR(sp[7]);
            ARO_STORE(c, last, &last->pair.cdr, vcell);
        }
        sp[7] = vcell;
        VALUE init = cadr(car(sp[12]));
        VALUE icell = scm_cons(c, init, SCM_NIL);
        if (sp[8] == SCM_NIL) { sp[8] = icell; }
        else {
            struct sobj *last = SCM_PTR(sp[9]);
            ARO_STORE(c, last, &last->pair.cdr, icell);
        }
        sp[9] = icell;
        VALUE step;
        if (cdr(cdr(car(sp[12]))) != SCM_NIL) step = caddr(car(sp[12]));
        else step = car(car(sp[12]));   /* default to var */
        VALUE scell = scm_cons(c, step, SCM_NIL);
        if (sp[10] == SCM_NIL) { sp[10] = scell; }
        else {
            struct sobj *last = SCM_PTR(sp[11]);
            ARO_STORE(c, last, &last->pair.cdr, scell);
        }
        sp[11] = scell;
    }
    sp[13] = gensym_at(c, "do-loop");
    /* recur = (loop_sym steps...) → sp[14] */
    sp[14] = scm_cons(c, sp[13], sp[10]);
    /* result_branch = (begin result...) or (if #t unspec) → sp[15] */
    if (sp[5] == SCM_NIL) {
        sp[15] = scm_cons(c, SCM_UNSPEC, SCM_NIL);
        sp[15] = scm_cons(c, SCM_TRUE, sp[15]);
        sp[15] = scm_cons_sym(c, "if", sp[15]);
    } else {
        sp[15] = scm_cons_sym(c, "begin", sp[5]);
    }
    /* body_then_recur = list_append1(c, body, recur) */
    sp[16] = list_append1(c, sp[3], sp[14]);
    /* (begin body_then_recur) → sp[16] */
    sp[16] = scm_cons_sym(c, "begin", sp[16]);
    /* iff = (if test result_branch (begin ...))
     *      = (if . (test . (result_branch . ((begin ...) . NIL)))) */
    sp[17] = scm_cons(c, sp[16], SCM_NIL);
    sp[17] = scm_cons(c, sp[15], sp[17]);
    sp[17] = scm_cons(c, sp[4], sp[17]);
    sp[17] = scm_cons_sym(c, "if", sp[17]);
    /* lambda = (lambda vars iff) */
    VALUE iff_cell = scm_cons(c, sp[17], SCM_NIL);
    sp[17] = iff_cell;
    sp[17] = scm_cons(c, sp[6], sp[17]);
    sp[17] = scm_cons_sym(c, "lambda", sp[17]);
    /* binding = (loop_sym lambda) */
    VALUE lambda_cell = scm_cons(c, sp[17], SCM_NIL);
    sp[17] = lambda_cell;
    sp[17] = scm_cons(c, sp[13], sp[17]);
    /* binding_list = (binding) */
    sp[17] = scm_cons(c, sp[17], SCM_NIL);
    /* call_inner = (loop_sym inits...) */
    sp[15] = scm_cons(c, sp[13], sp[8]);   /* reuse sp[15] */
    /* call_form = (call_inner) */
    sp[15] = scm_cons(c, sp[15], SCM_NIL);
    /* letrec = (letrec binding_list call_form...)
     *        = (letrec . (binding_list . call_form)) */
    sp[16] = scm_cons(c, sp[17], sp[15]);
    sp[16] = scm_cons_sym(c, "letrec", sp[16]);
    NODE *r = compile(c, sp[16], scope, is_tail);
    SP_POP(c, sp);
    return r;
}

static NODE *
compile_define(CTX *c, VALUE form, struct lex_scope *scope)
{
    VALUE second = cadr(form);
    if (scm_is_pair(second)) {
        VALUE name = car(second);
        VALUE params = cdr(second);
        VALUE body = cdr(cdr(form));
        /* name_str must be a libc-stable cstr — moving GC may relocate the
         * symbol's byte payload underneath us, leaving the NODE's stored
         * cstr dangling.  scm_perm_name interns into a libc-malloc'd pool. */
        const char *name_str = scm_perm_name(SCM_PTR(name)->sym.name);

        // For top-level `(define (f params) body)` at the outermost
        // scope, push a global-mode self-call context so compile_call
        // can rewrite `(f X)` tail calls inside body to
        // node_self_tail_call_global_K — converting global recursion
        // (loop / ack / tak / tail-recursive helpers) into a tight
        // C for(;;) inside the lambda's SD, just like §18 does for
        // named-let.  An inner `(define (f …) …)` (nested under
        // another lambda) is handled by hoist_internal_defines lifting
        // it to a letrec; that path goes through compile_letrec /
        // compile_let which doesn't push global mode (correct, since
        // those defines are lex bindings, not globals).
        bool top_level = (scope == NULL);
        if (top_level && scm_is_symbol(params) == false) {
            // Count fixed-arity params (skip rest-bearing forms).
            int nparams = 0;
            bool has_rest = false;
            for (VALUE p = params; scm_is_pair(p); p = cdr(p)) {
                nparams++;
            }
            if (params != SCM_NIL && !scm_is_pair(params)) {
                // bare symbol ⇒ rest only, already false above
            }
            // Detect dotted-tail (rest after fixed): the chain ended
            // on a non-pair, non-nil cell, meaning the symbol there
            // is a rest param.
            VALUE p = params;
            while (scm_is_pair(p)) p = cdr(p);
            if (scm_is_symbol(p)) has_rest = true;

            if (!has_rest && nparams <= ASCHEME_LOOP_MAX_PARAMS && nparams <= 4) {
                /* sp[0]=params, sp[1]=body, sp[2]=lambda */
                VALUE * restrict sp_d = c->sp;
                SP_PUSH(c, sp_d, 3);
                sp_d[0] = params;
                sp_d[1] = body;
                sp_d[2] = scm_cons(c, sp_d[0], sp_d[1]);
                sp_d[2] = scm_cons_sym(c, "lambda", sp_d[2]);
                struct self_call_ctx ctx = { name_str, (uint32_t)nparams, NULL, false };
                SELF_CALL_FOR_NEXT_LAMBDA = &ctx;
                NODE *val = compile(c, sp_d[2], scope, false);
                SELF_CALL_FOR_NEXT_LAMBDA = NULL;
                if (ctx.used) {
                    NODE *lambda_body = val->u.node_lambda.body;
                    NODE *wrapped = ALLOC_node_loop(lambda_body, (uint32_t)nparams,
                                                   val->u.node_lambda.no_capture,
                                                   val->u.node_lambda.leaf);
                    aot_add_entry(wrapped);
                    val->u.node_lambda.body = wrapped;
                }
                SP_POP(c, sp_d);
                return ALLOC_node_gdef(name_str, val);
            }
        }

        {
            VALUE * restrict sp_d = c->sp;
            SP_PUSH(c, sp_d, 3);
            sp_d[0] = params;
            sp_d[1] = body;
            sp_d[2] = scm_cons(c, sp_d[0], sp_d[1]);
            sp_d[2] = scm_cons_sym(c, "lambda", sp_d[2]);
            NODE *val = compile(c, sp_d[2], scope, false);
            SP_POP(c, sp_d);
            return ALLOC_node_gdef(name_str, val);
        }
    }
    VALUE name = second;
    VALUE val_form = caddr(form);
    /* Park `name` so we can re-derive name_str via scm_perm_name after the
     * compile() call may have moved the symbol's byte payload. */
    VALUE * restrict sp_def = c->sp;
    SP_PUSH(c, sp_def, 1);
    sp_def[0] = name;
    NODE *val = compile(c, val_form, scope, false);
    const char *nm = scm_perm_name(SCM_PTR(sp_def[0])->sym.name);
    SP_POP(c, sp_def);
    return ALLOC_node_gdef(nm, val);
}

static NODE *
compile(CTX *c, VALUE form, struct lex_scope *scope, bool is_tail)
{
    /* Park `form` at the top of compile so it survives any GC triggered
     * inside recursive scm_cons / scm_intern / compile_* calls.  We read
     * the live form via sp[0] every time we need to access fields. */
    if (SCM_IS_FIXNUM(form)) {
        int64_t n = SCM_FIXVAL(form);
        if (n >= INT32_MIN && n <= INT32_MAX) return ALLOC_node_const_int((int32_t)n);
        return ALLOC_node_const_int64((uint64_t)form);
    }
    if (form == SCM_NIL)    return ALLOC_node_const_nil();
    if (form == SCM_TRUE)   return ALLOC_node_const_bool(1);
    if (form == SCM_FALSE)  return ALLOC_node_const_bool(0);
    if (form == SCM_UNSPEC) return ALLOC_node_const_unspec();
    if (scm_is_double(form))return ALLOC_node_const_double(scm_get_double(form));
    if (scm_is_string(form))return ALLOC_node_const_str(scm_perm_name(SCM_PTR(form)->str.chars));
    if (scm_is_char(form))  return ALLOC_node_const_char(SCM_PTR(form)->ch);
    if (scm_is_vector(form))return scm_alloc_quote((uint64_t)form);
    if (scm_is_bignum(form) || scm_is_rational(form) || scm_is_complex(form))
        return scm_alloc_quote((uint64_t)form);
    if (scm_is_symbol(form)) {
        const char *name = scm_perm_name(SCM_PTR(form)->sym.name);
        uint32_t depth, idx;
        struct lex_scope *resolved;
        if (lex_lookup_full(scope, name, &resolved, &depth, &idx)) {
            // sp_offset: parse-time baked for depth=0 (= var lives in current
            // frame at sp[idx - locals_cnt]).  For depth>=1, sp_offset is
            // unused (= env-chain walk path).  Also track outer ref on
            // current scope for later "no_capture" analysis.
            int32_t sp_offset = (depth == 0) ? ((int32_t)idx - resolved->nslots) : 0;
            if (depth >= 1) mark_outer_capture_path(scope, depth);
            NODE *r = ALLOC_node_lref(depth, idx, sp_offset);
            if (depth == 0) record_pending_lref(scope, r);
            return r;
        }
        return ALLOC_node_gref(name);
    }
    if (!scm_is_pair(form)) {
        scm_error(c, "compile: unexpected form");
    }
    /* form is a pair — recursive paths below allocate, so park it on sp. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 1);
    sp[0] = form;
    VALUE head = car(form);   /* head is a VALUE; will reload from sp[0] below if needed */
    NODE *result;
    if (scm_is_symbol(head)) {
        const char *h = SCM_PTR(head)->sym.name;
        if (strcmp(h, "quote") == 0)    { result = compile_quote(cadr(sp[0])); goto done; }
        if (strcmp(h, "quasiquote") == 0) {
            VALUE expanded = expand_quasiquote(c, cadr(sp[0]), 1);
            result = compile(c, expanded, scope, is_tail);
            goto done;
        }
        if (strcmp(h, "delay") == 0) {
            // (delay E) → (|make-promise| (lambda () E))
            /* Stage cons chain through sp to avoid stale arg bits across
             * the per-cons GC triggers. */
            VALUE * restrict sp2 = c->sp;
            SP_PUSH(c, sp2, 2);  /* sp2[0]=thunk, sp2[1]=call */
            sp2[0] = scm_cons(c, cadr(sp[0]), SCM_NIL);
            sp2[0] = scm_cons(c, SCM_NIL, sp2[0]);
            sp2[0] = scm_cons_sym(c, "lambda", sp2[0]);
            sp2[1] = scm_cons(c, sp2[0], SCM_NIL);
            sp2[1] = scm_cons_sym(c, "|make-promise|", sp2[1]);
            result = compile(c, sp2[1], scope, is_tail);
            SP_POP(c, sp2);
            goto done;
        }
        if (strcmp(h, "if") == 0) {
            NODE *cnd = compile(c, cadr(sp[0]), scope, false);
            NODE *thn = compile(c, caddr(sp[0]), scope, is_tail);
            NODE *els = (cdr(cdr(cdr(sp[0]))) == SCM_NIL)
                ? ALLOC_node_const_unspec()
                : compile(c, cadddr(sp[0]), scope, is_tail);
            result = ALLOC_node_if(cnd, thn, els);
            goto done;
        }
        if (strcmp(h, "begin") == 0) {
            result = compile_seq(c, cdr(sp[0]), scope, is_tail);
            goto done;
        }
        if (strcmp(h, "lambda") == 0) {
            result = compile_lambda(c, cadr(sp[0]), cdr(cdr(sp[0])), scope);
            goto done;
        }
        if (strcmp(h, "set!") == 0) {
            VALUE name = cadr(sp[0]);
            VALUE val_form = caddr(sp[0]);
            /* Snapshot the name into the permanent-name pool before the
             * recursive compile() — the symbol byte payload can move. */
            const char *nm = scm_perm_name(SCM_PTR(name)->sym.name);
            uint32_t depth, idx;
            struct lex_scope *resolved;
            if (lex_lookup_full(scope, nm, &resolved, &depth, &idx)) {
                NODE *val = compile(c, val_form, scope, false);
                int32_t sp_offset = (depth == 0) ? ((int32_t)idx - resolved->nslots) : 0;
                if (depth >= 1) mark_outer_capture_path(scope, depth);
                result = ALLOC_node_lset(depth, idx, sp_offset, val);
                if (depth == 0) record_pending_lref(scope, result);
                goto done;
            }
            NODE *val = compile(c, val_form, scope, false);
            result = ALLOC_node_gset(nm, val);
            goto done;
        }
        if (strcmp(h, "define") == 0) {
            result = compile_define(c, sp[0], scope);
            goto done;
        }
        if (strcmp(h, "let") == 0)    { result = compile_let(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "let*") == 0)   { result = compile_letstar(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "letrec") == 0) { result = compile_letrec(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "cond") == 0)   { result = compile_cond(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "case") == 0)   { result = compile_case(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "and") == 0)    { result = compile_and(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "or") == 0)     { result = compile_or(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "when") == 0)   { result = compile_when(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "unless") == 0) { result = compile_unless(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "do") == 0)     { result = compile_do(c, sp[0], scope, is_tail); goto done; }
        if (strcmp(h, "call/cc") == 0 || strcmp(h, "call-with-current-continuation") == 0) {
            NODE *fn = compile(c, cadr(sp[0]), scope, false);
            result = ALLOC_node_callcc(fn);
            goto done;
        }
    }
    result = compile_call(c, car(sp[0]), cdr(sp[0]), scope, is_tail);
done:
    SP_POP(c, sp);
    return result;
}

// ---------------------------------------------------------------------------
// Apply / tail-call trampoline.
// ---------------------------------------------------------------------------

// Bind argv into a fresh frame for `cl`.  Handles dotted-rest by collecting
// excess args into a list bound to the last slot.
static struct sframe *
build_frame_for(CTX *c, struct sobj *cl, int argc, VALUE *argv)
{
    int nparams = cl->closure.nparams;
    int has_rest = cl->closure.has_rest;
    int total = nparams + (has_rest ? 1 : 0);
    if (has_rest) {
        if (argc < nparams) scm_error(c, "too few arguments");
    } else {
        if (argc != nparams) scm_error(c, "wrong number of arguments (got %d, expected %d)", argc, nparams);
    }
    /* Precise rooting plan:
     *   sp_base[0]    = closure sobj VALUE              (cl may move)
     *   sp_base[1]    = rest list (built before frame)  (may move per cons)
     *   sp_base[2..]  = argv copies                     (may move)
     *
     * We build the rest list FIRST (before any frame alloc), then allocate
     * the frame and copy parked argv + parked rest into slots.  This way
     * `f` (which can also move during the cons loop) only needs to live
     * across the trivial slot-copy loop, where no allocs happen. */
    VALUE *sp_base = c->sp;
    ASTRO_ASSERT(sp_base + 2 + argc <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
    sp_base[0] = SCM_OBJ_VAL(cl);
    sp_base[1] = SCM_NIL;
    for (int i = 0; i < argc; i++) sp_base[2 + i] = argv[i];
    c->sp = sp_base + 2 + argc;

    /* Build rest list first (no frame yet → nothing else to root). */
    if (has_rest) {
        for (int i = argc - 1; i >= nparams; i--) {
            sp_base[1] = scm_cons(c, sp_base[2 + i], sp_base[1]);
        }
    }
    /* Now allocate the frame.  cl was kept fresh via sp_base[0]; reload
     * its env via the parked pointer. */
    struct sframe *f = scm_new_frame(c, SCM_PTR(sp_base[0])->closure.env, total);
    /* No more allocs from here on — slot copies are pure memory writes.
     * f is freshly allocated (young) — WB fast-path returns immediately,
     * but kept uniform for any future code path that might promote f
     * before reaching here. */
    for (int i = 0; i < nparams; i++) {
        ARO_STORE(c, f, &f->slots[i], sp_base[2 + i]);
    }
    if (has_rest) {
        ARO_STORE(c, f, &f->slots[nparams], sp_base[1]);
    }
    c->sp = sp_base;
    return f;
}

VALUE
scm_apply(CTX *c, VALUE fn, int argc, VALUE *argv)
{
    if (UNLIKELY(scm_is_prim(fn))) {
        struct sobj *p = SCM_PTR(fn);
        if (argc < p->prim.min_argc) {
            scm_error(c, "%s: too few arguments", p->prim.name);
        }
        if (p->prim.max_argc >= 0 && argc > p->prim.max_argc) {
            scm_error(c, "%s: too many arguments", p->prim.name);
        }
        return p->prim.fn(c, argc, argv);
    }
    if (UNLIKELY(scm_is_cont(fn))) {
        struct sobj *k = SCM_PTR(fn);
        if (!k->cont->active) scm_error(c, "continuation already invoked / expired");
        if (argc != 1) scm_error(c, "continuation expects exactly 1 argument");
        /* cont was allocated earlier (= possibly OLD by now) and argv[0]
         * is freshly captured.  WB is mandatory. */
        ARO_STORE(c, k->cont, &k->cont->result, argv[0]);
        longjmp(k->cont->buf, 1);
    }
    if (LIKELY(scm_is_closure(fn))) {
        struct sobj *cl = SCM_PTR(fn);
        struct sframe *new_env;
        // Leaf-closure stack frame.  When the closure's body has no
        // nested `lambda`, no escaped sub-closure can capture this
        // frame, so we can park it on the C stack via `alloca` and
        // avoid the heap alloc entirely.  Lifetime = the rest of this
        // scm_apply call, which is exactly what the body needs.
        //
        // Disabled under a precise GC backend (BARUBY_GC != NONE):
        // the GC root visitor traverses c->env, which would write to
        // stack memory (= corrupt frame header) or call bitmap_set on
        // an off-heap address (= SEGV).  Heap-allocated frames are
        // tagged OBJ_FRAME so SCAN_EDGES dispatches correctly.
#if BARUBY_GC == BARUBY_GC_NONE && !defined(ARO_GC_WB_AUDIT)
        if (LIKELY(cl->closure.leaf)) {
            int total = cl->closure.nparams + (cl->closure.has_rest ? 1 : 0);
            if (cl->closure.has_rest) {
                if (argc < cl->closure.nparams) scm_error(c, "too few arguments");
            } else {
                if (argc != cl->closure.nparams) scm_error(c, "wrong number of arguments (got %d, expected %d)", argc, cl->closure.nparams);
            }
            new_env = (struct sframe *)alloca(sizeof(struct sframe) +
                                              sizeof(VALUE) * (total ? total : 1));
            new_env->parent = cl->closure.env;
            new_env->nslots = total;
            for (int i = 0; i < cl->closure.nparams; i++) new_env->slots[i] = argv[i];
            if (cl->closure.has_rest) {
                VALUE rest = SCM_NIL;
                for (int i = argc - 1; i >= cl->closure.nparams; i--)
                    rest = scm_cons(c, argv[i], rest);
                new_env->slots[cl->closure.nparams] = rest;
            }
        } else
#endif
        {
            /* Build heap frame.  For moving GC this is the only path. */
            VALUE *sp_inner = c->sp;
            ASTRO_ASSERT(sp_inner + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
            sp_inner[0] = fn;
            c->sp = sp_inner + 1;
            new_env = build_frame_for(c, cl, argc, argv);
            cl = SCM_PTR(sp_inner[0]);
            c->sp = sp_inner;
        }
        /* Park saved env (= caller's frame) on c->sp so it survives GC
         * triggered inside the EVAL(body) trampoline below — `saved` is
         * a C local sframe* and a moving GC would relocate it. */
        VALUE *sp_base = c->sp;
        ASTRO_ASSERT(sp_base + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
        sp_base[0] = (VALUE)c->env;      /* saved env */
        c->sp = sp_base + 1;
        NODE *body = cl->closure.body;
        CTX_SET_ENV(c, new_env);
        /* For no_capture closures, body uses lref_sp (= c->frame_sp[sp_offset]).
         * Set c->frame_sp to a position where frame_sp[idx - nparams] = arg[idx].
         * Place args at sp_base + 1 .. sp_base + nparams (= already at sp_base[0]+1
         * if from node_call_K, otherwise copy).  frame_sp = sp_base + 1 + nparams. */
        VALUE *saved_frame_sp = c->frame_sp;
        if (cl->closure.no_capture && !cl->closure.has_rest) {
            int nparams = cl->closure.nparams;
            VALUE *args_base = sp_base + 1;
            /* Read from new_env->slots (= the heap frame build_frame_for
             * just populated) rather than argv — argv may be an interior
             * pointer into a heap object (e.g., mvalues.items from
             * call-with-values) that moved during build_frame_for's
             * alloc.  new_env is a live root via the GC visitor's
             * traversal of `c->env` set just above, so its slots stay
             * valid after subsequent allocations. */
            for (int i = 0; i < nparams; i++) args_base[i] = new_env->slots[i];
            c->sp = args_base + nparams;
            c->frame_sp = c->sp;
        }
        // Trampoline: re-enter while tail_call_pending is set.  Bumps
        // the body's dispatch counter — used by `--pg-compile` to decide
        // which entries are worth AOT-compiling on the next run.  The
        // counter is gated on ASCHEME_PROFILING because the read-modify-
        // write per call is measurable (~5%) on tight tail-call loops.
        if (UNLIKELY(ASCHEME_PROFILING)) body->head.dispatch_cnt++;
        for (;;) {
            VALUE v = EVAL(c, body, c->sp);
            if (!c->tail_call_pending) {
                CTX_SET_ENV(c, (struct sframe *)sp_base[0]);  /* reload saved */
                c->sp = sp_base;
                c->frame_sp = saved_frame_sp;
                return v;
            }
            c->tail_call_pending = 0;
            body = c->next_body;
            // Frame-reuse path leaves next_env == current env; skip the
            // CTX_SET_ENV bump in that case so the lref level cache stays
            // warm across tight tail-call loops.  Real env switches go
            // through the bump.
            if (c->next_env != c->env) CTX_SET_ENV(c, c->next_env);
            /* No_capture next body: refresh sp-based args + frame_sp from
             * the new env's slots.  Without this, patched lref_sp/lset_sp
             * in the new body would read a stale frame_sp (m-even/m-odd
             * style cross-closure tail calls). */
            if (c->next_no_capture) {
                uint16_t np = c->next_nparams;
                VALUE *args_base = sp_base + 1;
                ASTRO_ASSERT(args_base + np <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
                for (uint16_t i = 0; i < np; i++) args_base[i] = c->env->slots[i];
                c->sp = args_base + np;
                c->frame_sp = c->sp;
            }
            if (UNLIKELY(ASCHEME_PROFILING)) body->head.dispatch_cnt++;
        }
    }
    scm_error(c, "not a procedure");
}

// Slow-path complement to the inline `scm_apply_tail` in node.h.
// Handles non-tail calls, non-closure targets, has_rest closures, and
// the "shape mismatch" cases where the existing frame can't be reused.
VALUE
scm_apply_tail_slow(CTX *c, VALUE fn, int argc, VALUE *argv, uint32_t is_tail)
{
    if (is_tail && scm_is_closure(fn)) {
        struct sobj *cl = SCM_PTR(fn);
        int total = cl->closure.nparams + (cl->closure.has_rest ? 1 : 0);

        // Self-tail-call frame reuse.  When the new closure shares the
        // current frame's parent + slot count *and* its body has no
        // nested lambda (so no escaped closure can hold a reference),
        // we overwrite the live frame in place and skip an aro_gc_alloc.
        // For tight tail loops (`loop` / `sum` benches) this removes
        // ~30 ns of allocation work per iteration.
        if (LIKELY(cl->closure.leaf &&
                    c->env != NULL &&
                    c->env->parent == cl->closure.env &&
                    c->env->nslots == total)) {
            if (cl->closure.has_rest) {
                if (argc < cl->closure.nparams) scm_error(c, "too few arguments");
            } else {
                if (argc != cl->closure.nparams) scm_error(c, "wrong number of arguments");
            }
            int np = cl->closure.nparams;
            bool hr = cl->closure.has_rest;
            if (!hr) {
                /* Pure-slot path — no alloc, argv stays valid.  c->env is
                 * the live frame, may be OLD; argv[i] values are young or
                 * existing references.  WB is required so a minor GC after
                 * tail-call pending sees the new edges. */
                for (int i = 0; i < np; i++) {
                    ARO_STORE(c, c->env, &c->env->slots[i], argv[i]);
                }
                c->next_body = cl->closure.body;
                c->next_env = c->env;
                c->next_no_capture = cl->closure.no_capture ? 1u : 0u;
                c->next_nparams = (uint16_t)np;
                c->tail_call_pending = 1;
                return SCM_UNSPEC;
            }
            /* has_rest path: scm_cons can move c->env, argv values, cl.
             * Park argv copies + rest list + fn on c->sp, build rest first,
             * then copy parked fixed args + rest into slots — both writes
             * happen after the last alloc. */
            VALUE *sp_base = c->sp;
            ASTRO_ASSERT(sp_base + 2 + argc <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
            sp_base[0] = fn;             /* root the closure VALUE */
            sp_base[1] = SCM_NIL;        /* rest list  */
            for (int i = 0; i < argc; i++) sp_base[2 + i] = argv[i];
            c->sp = sp_base + 2 + argc;
            for (int i = argc - 1; i >= np; i--) {
                sp_base[1] = scm_cons(c, sp_base[2 + i], sp_base[1]);
            }
            /* No more allocs from here — slot writes are pure memory.
             * c->env may be OLD; sp_base values may include young objects. */
            for (int i = 0; i < np; i++) {
                ARO_STORE(c, c->env, &c->env->slots[i], sp_base[2 + i]);
            }
            ARO_STORE(c, c->env, &c->env->slots[np], sp_base[1]);
            c->next_body = SCM_PTR(sp_base[0])->closure.body;
            c->next_env = c->env;
            /* has_rest closures are never no_capture (compile_lambda forces
             * has_outer_ref=true), but be explicit anyway. */
            c->next_no_capture = 0u;
            c->next_nparams = (uint16_t)np;
            c->tail_call_pending = 1;
            c->sp = sp_base;
            return SCM_UNSPEC;
        }

        /* Generic build_frame_for — already precise (parks cl + argv). */
        /* Save body via fn-park so we read it after the alloc. */
        VALUE *sp_base = c->sp;
        ASTRO_ASSERT(sp_base + 1 <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE);
        sp_base[0] = fn;
        c->sp = sp_base + 1;
        struct sframe *new_env = build_frame_for(c, cl, argc, argv);
        struct sobj *cl_reloaded = SCM_PTR(sp_base[0]);
        c->next_body = cl_reloaded->closure.body;
        c->next_env = new_env;
        c->next_no_capture = (cl_reloaded->closure.no_capture && !cl_reloaded->closure.has_rest) ? 1u : 0u;
        c->next_nparams = (uint16_t)cl_reloaded->closure.nparams;
        c->tail_call_pending = 1;
        c->sp = sp_base;
        return SCM_UNSPEC;
    }
    return scm_apply(c, fn, argc, argv);
}

// Escape continuation via setjmp/longjmp.  Save/restore CTX state so a
// longjmp out of an inner call frame doesn't leave c->env / tail-call
// pending in an inconsistent state.  Calling the captured continuation
// after the original call/cc has returned raises a clean error rather
// than triggering UB.
//
// Precise-rooting contract: under a moving GC, C-local pointers (kobj,
// saved_env, k, fn) become stale across the inner `scm_apply` call.  We
// therefore stash everything that must survive in the scont body, which
// IS a scanned root (see OBJ_CONT case in AROH_SCAN_EDGES).  After
// each potential GC trigger we reload kobj from its parked sp slot.
VALUE
scm_callcc(CTX *c, VALUE fn)
{
    if (!scm_is_proc(fn)) scm_error(c, "call/cc: not a procedure");
    /* Park both `fn` (caller-supplied procedure VALUE) AND the in-flight
     * cont sobj in sp[] across the two inner allocs so a moving GC can
     * relocate them.  Without the fn slot the C-local `fn` parameter
     * goes stale across scm_alloc(OBJ_CONT) + aro_gc_alloc_raw(scont),
     * and the subsequent scm_apply(c, ..., fn_val, ...) sees stale bits
     * → "not a procedure". */
    VALUE *sp = c->sp;
    sp[0] = 0;        /* kobj VALUE — set after scm_alloc */
    sp[1] = fn;       /* parked procedure VALUE */
    c->sp = sp + 2;
    struct sobj *kobj = scm_alloc(c, OBJ_CONT);
    /* SCAN_EDGES skips NULL cont slot; route via ARO_STORE for audit. */
    ARO_STORE(c, kobj, &kobj->cont, (VALUE)NULL);
    sp[0] = SCM_OBJ_VAL(kobj);
    struct scont *cnt = (struct scont *)aro_gc_alloc_raw(c, sizeof(struct scont));
    /* Reload kobj — it may have moved during the alloc above. */
    kobj = SCM_PTR(sp[0]);
    /* kobj is freshly alloc'd young — WB fast-path returns. */
    ARO_STORE(c, kobj, &kobj->cont, (VALUE)cnt);
    cnt->active = 1;
    cnt->tag = ++c->cont_tag_seq;
    /* cnt is also freshly young — same fast path. */
    ARO_STORE(c, cnt, &cnt->result, SCM_UNSPEC);
    ARO_STORE(c, cnt, &cnt->saved_env, (VALUE)c->env);
    cnt->saved_tcp = c->tail_call_pending;
    cnt->saved_sp = sp;                  /* pre-call/cc sp */
    cnt->saved_frame_sp = c->frame_sp;   /* pre-call/cc frame_sp */
    ARO_STORE(c, cnt, &cnt->k_val, SCM_OBJ_VAL(kobj));
    ARO_STORE(c, cnt, &cnt->fn_val, sp[1]);    /* use parked, post-relocate procedure VALUE */
    if (setjmp(cnt->buf) != 0) {
        /* longjmp path — reload kobj from sp, then read saved state from
         * the (now possibly moved) scont via the owning kobj. */
        kobj = SCM_PTR(sp[0]);
        cnt  = kobj->cont;
        CTX_SET_ENV(c, cnt->saved_env);
        c->tail_call_pending = cnt->saved_tcp;
        c->sp = cnt->saved_sp;        /* restore sp before reading frame_sp */
        c->frame_sp = cnt->saved_frame_sp;
        cnt->active = 0;
        VALUE r = cnt->result;
        return r;
    }
    /* Reload kobj before scm_apply (paranoid, since cnt fields may have
     * been written into a now-stale kobj address — but kobj is in sp[0]
     * so it's still tracked).  The k VALUE and the fn VALUE both live
     * in the scont so scm_apply's GC moves stay coherent. */
    kobj = SCM_PTR(sp[0]);
    VALUE arg[1] = { kobj->cont->k_val };
    /* arg[] is a C-local but scm_apply doesn't keep it past param-binding,
     * and the underlying VALUE (k_val) is held in the rooted scont. */
    VALUE r = scm_apply(c, kobj->cont->fn_val, 1, arg);
    kobj = SCM_PTR(sp[0]);
    kobj->cont->active = 0;
    c->sp = sp;
    return r;
}


static void
install_prims(CTX *c)
{
    for (struct prim_entry *p = PRIM_TABLE; p->name; p++) {
        VALUE v = scm_make_prim(c, p->name, p->fn, p->min_argc, p->max_argc);
        scm_global_define(c, p->name, v);
    }
    // Snapshot the original prim sobj for every specialized operator.
    // node_arith_<op>'s fast path compares the live global value at its
    // cached index against the snapshot; a mismatch means the user has
    // rebound the operator and we must fall through to general dispatch.
    PRIM_PLUS_VAL   = scm_global_ref(c, "+");
    PRIM_MINUS_VAL  = scm_global_ref(c, "-");
    PRIM_MUL_VAL    = scm_global_ref(c, "*");
    PRIM_NUM_LT_VAL = scm_global_ref(c, "<");
    PRIM_NUM_LE_VAL = scm_global_ref(c, "<=");
    PRIM_NUM_GT_VAL = scm_global_ref(c, ">");
    PRIM_NUM_GE_VAL = scm_global_ref(c, ">=");
    PRIM_NUM_EQ_VAL = scm_global_ref(c, "=");

    // Standard ports.  These are wrappers around the libc FILE* and not
    // owned (close-input-port / close-output-port leaves stdin/out/err
    // alone).
    PORT_STDIN  = port_make(c, stdin,  true,  false);
    PORT_STDOUT = port_make(c, stdout, false, false);
    PORT_STDERR = port_make(c, stderr, false, false);

    PRIM_NULL_P_VAL     = scm_global_ref(c, "null?");
    PRIM_PAIR_P_VAL     = scm_global_ref(c, "pair?");
    PRIM_CAR_VAL        = scm_global_ref(c, "car");
    PRIM_CDR_VAL        = scm_global_ref(c, "cdr");
    PRIM_NOT_VAL        = scm_global_ref(c, "not");
    PRIM_VECTOR_REF_VAL = scm_global_ref(c, "vector-ref");
    PRIM_VECTOR_SET_VAL = scm_global_ref(c, "vector-set!");
    PRIM_CONS_VAL       = scm_global_ref(c, "cons");
    PRIM_EQ_P_VAL       = scm_global_ref(c, "eq?");
    PRIM_EQV_P_VAL      = scm_global_ref(c, "eqv?");
}

// Slow-path helpers for the specialized arith / pred / vec nodes.  The
// hot path inside each EVAL_node_* checks `cache->serial == globals_serial`
// only — the value-vs-expected-prim check is encoded INTO that serial:
// `arith_refresh` sets `cache->serial = globals_serial` only if the freshly-
// resolved global is still the original primitive.  When the user rebinds
// the operator (`(set! + my+)`), `cache->value` gets the new closure and
// `cache->serial` is left at its prior (stale) value, so the hot path's
// single equality check fails and slow-path dispatch through the new
// closure runs instead.  This collapses the previous dual check
// (`serial && value == PRIM_*`) into one — saving a GOT load + compare
// per arith call.

static VALUE
arith_refresh(CTX *c, struct arith_cache *cache, const char *opname, VALUE expected)
{
    VALUE v = scm_global_ref(c, opname);
    cache->value = v;
    if (v == expected) cache->serial = c->globals_serial;
    // else: leave cache->serial stale; the hot path won't fire while the
    //       binding stays rebound, but cache->value is fresh so the slow
    //       path uses it directly without re-resolving every call.
    return v;
}

// Slow-path entry from EVAL_node_*.  Returns the function to apply,
// trusting cache->value when it's fresh-but-rebound (cache->serial was
// just set by a previous refresh that didn't bump it because the value
// wasn't the expected prim — but that caller still left cache->value
// pointing at the rebound closure, so we can use it directly).
static inline VALUE
arith_resolve(CTX *c, struct arith_cache *cache, const char *opname, VALUE expected)
{
    // We arrive here with cache->serial != globals_serial.  Two reasons:
    //  (1) globals just bumped (some define/set!); cache may be stale.
    //  (2) operator was rebound; cache->serial was deliberately left
    //      stale by a prior arith_refresh.
    // Either way, re-resolving via scm_global_ref is correct.  In
    // case (2) it just returns the same rebound closure that's already
    // in cache->value, but the cost is bounded — scm_global_ref is a
    // small table lookup.
    return arith_refresh(c, cache, opname, expected);
}

VALUE
arith_dispatch1(CTX *c, struct arith_cache *cache, const char *opname, VALUE expected, VALUE av)
{
    VALUE fn = arith_resolve(c, cache, opname, expected);
    return scm_apply(c, fn, 1, &av);
}

VALUE
arith_dispatch(CTX *c, struct arith_cache *cache, const char *opname, VALUE expected, VALUE av, VALUE bv)
{
    VALUE fn = arith_resolve(c, cache, opname, expected);
    VALUE args[2] = { av, bv };
    return scm_apply(c, fn, 2, args);
}

VALUE
arith_dispatch3(CTX *c, struct arith_cache *cache, const char *opname, VALUE expected, VALUE a, VALUE b, VALUE d)
{
    VALUE fn = arith_resolve(c, cache, opname, expected);
    VALUE args[3] = { a, b, d };
    return scm_apply(c, fn, 3, args);
}

// ---------------------------------------------------------------------------
// Precise GC root visitor.  Called by every backend's collect path to mark
// (or forward) every VALUE / heap-ptr slot that lives OUTSIDE the GC heap.
//
// ascheme keeps roots in a small set of places:
//   - c->env, c->next_env       — typed-ptr to sframe (struct sframe *)
//   - c->globals[i].value       — VALUE
//   - c->globals[i].name        — raw byte payload (= heap-allocated cstr)
//   - c->loop_args[]            — VALUE temp slots used by self-tail-call
//   - c->env_chain[]            — sframe parent-chain cache; invalidated
//                                  via env_serial++ below to keep things
//                                  simple
//   - SYMBOL_TABLE[i]           — interned OBJ_SYMBOL sobj
//   - PORT_STDIN/STDOUT/STDERR  — heap-allocated port sobj's, kept as
//                                  C globals (not via c->globals)
//   - PRIM_*_VAL                — aliases for c->globals[].value, so they
//                                  are kept alive transitively.  We do NOT
//                                  visit them explicitly here.
//
// Cache invalidation: we bump c->globals_serial and c->env_serial each
// time so all gref / arith / env-chain caches re-resolve through the
// rooted globals[] and env on next access.  This keeps the cache contents
// (= VALUEs / sframe* not visible to root visit) from being treated as
// independent roots.  Under non-moving GC the cached values would in fact
// remain valid as pointers, but we still want to avoid the framework
// touching off-heap addresses they sometimes hold.
// ---------------------------------------------------------------------------

extern struct sobj **SYMBOL_TABLE;
extern size_t SYMBOL_TABLE_LEN;
extern VALUE PORT_STDIN, PORT_STDOUT, PORT_STDERR;

/* Base of the scratch sp range — sample-side spill region for the
 * realloc helpers below.  Visit the range so the parked old-payload
 * pointer survives a GC trigger nested inside the alloc.  declared in
 * context.h (= shared with parse.c). */

/* iter 76: framework は CTX-opaque 化したため、 c->sp に park する realloc
 * helper は sample-side で実装する。 g_sp_scratch[0] に park し、 c->sp を
 * 一時的に進めて inner alloc 中 root scan に含める。 */
void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    if (!old) return aro_gc_alloc_raw(c, new_size);

    void *in_place = aro_gc_realloc_in_place(c, old, new_size);
    if (in_place) return in_place;

    size_t old_size = aro_gc_size_of(old);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    VALUE *sp_top = c->sp;
    sp_top[0] = (VALUE)old;
    c->sp = sp_top + 1;
    void *newp = aro_gc_alloc_raw(c, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    aro_gc_reset_payload_header(newp, new_size);
    return newp;
}

void *
aro_gc_realloc_byte_payload(CTX *c, void *old, size_t new_size)
{
    if (!old) return aro_gc_alloc_byte_raw(c, new_size);

    void *in_place = aro_gc_realloc_in_place(c, old, new_size);
    if (in_place) return in_place;

    size_t old_size = aro_gc_size_of(old);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    VALUE *sp_top = c->sp;
    sp_top[0] = (VALUE)old;
    c->sp = sp_top + 1;
    void *newp = aro_gc_alloc_byte_raw(c, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    aro_gc_reset_payload_header(newp, new_size);
    return newp;
}

void
aro_scheme_visit_roots(CTX *c, void *gc, void (*edge_visit)(void *, void **))
{
    /* env / next_env: typed-ptr to sframe (= raw heap pointer). */
    if (c->env)      ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &c->env);
    if (c->next_env) ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &c->next_env);

    /* Framework-managed spill range (= aro_gc_realloc_byte_payload etc.
     * stash the old payload here across an inner alloc).  Walks any
     * occupied slot — raw typed-ptr semantics suffice because the only
     * thing parked is a raw void *. */
    for (VALUE *p = g_sp_scratch; p < c->sp; p++) {
        ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, p);
    }

    /* Globals: name_payload (= byte-payload BASE, i.e. AroObjectHeader at
     * offset 0; safe to forward as a typed-ptr) + value (VALUE).  Filter
     * singletons from VALUE-slot visits so the framework doesn't touch
     * their off-heap headers (= bitmap_set on a non-page address would
     * SEGV).
     *
     * ascheme stores VALUEs as RAW heap pointers, so VALUE slots are
     * forwarded via ARO_GC_VISIT_EDGE_PTR (= raw typed-ptr semantics,
     * no encoding involved). */
    for (size_t i = 0; i < c->globals_size; i++) {
        if (c->globals[i].name_payload) {
            ARO_GC_VISIT_EDGE_PTR(gc, edge_visit,
                                     (void **)&c->globals[i].name_payload);
        }
        VALUE v = c->globals[i].value;
        if (v != 0 && SCM_IS_PTR(v) && !scm_is_singleton(v)) {
            ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &c->globals[i].value);
        }
    }

    /* Self-tail-call temp args (= live across the call_K → loop body
     * trampoline, before frame-slot writeback). */
    for (int i = 0; i < ASCHEME_LOOP_MAX_PARAMS; i++) {
        VALUE v = c->loop_args[i];
        if (v != 0 && SCM_IS_PTR(v) && !scm_is_singleton(v)) {
            ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &c->loop_args[i]);
        }
    }

    /* Process-static roots stored as C globals. */
    if (SYMBOL_TABLE) {
        for (size_t i = 0; i < SYMBOL_TABLE_LEN; i++) {
            if (SYMBOL_TABLE[i]) {
                ARO_GC_VISIT_EDGE_PTR(gc, edge_visit,
                                         (void **)&SYMBOL_TABLE[i]);
            }
        }
    }
    if (SCM_IS_PTR(PORT_STDIN)  && !scm_is_singleton(PORT_STDIN))
        ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &PORT_STDIN);
    if (SCM_IS_PTR(PORT_STDOUT) && !scm_is_singleton(PORT_STDOUT))
        ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &PORT_STDOUT);
    if (SCM_IS_PTR(PORT_STDERR) && !scm_is_singleton(PORT_STDERR))
        ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &PORT_STDERR);

    /* Cached prim VALUEs used by specialized arith / pred / vec nodes —
     * these heap-pointer VALUEs are C globals; under a moving GC they
     * need explicit forwarding (sample-owned roots). */
    #define VISIT_PRIM(var) do { \
        if (SCM_IS_PTR(var) && !scm_is_singleton(var)) \
            ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &(var)); \
    } while (0)
    VISIT_PRIM(PRIM_PLUS_VAL);    VISIT_PRIM(PRIM_MINUS_VAL);   VISIT_PRIM(PRIM_MUL_VAL);
    VISIT_PRIM(PRIM_NUM_LT_VAL);  VISIT_PRIM(PRIM_NUM_LE_VAL);  VISIT_PRIM(PRIM_NUM_GT_VAL);
    VISIT_PRIM(PRIM_NUM_GE_VAL);  VISIT_PRIM(PRIM_NUM_EQ_VAL);
    VISIT_PRIM(PRIM_NULL_P_VAL);  VISIT_PRIM(PRIM_PAIR_P_VAL);  VISIT_PRIM(PRIM_CAR_VAL);
    VISIT_PRIM(PRIM_CDR_VAL);     VISIT_PRIM(PRIM_NOT_VAL);
    VISIT_PRIM(PRIM_VECTOR_REF_VAL); VISIT_PRIM(PRIM_VECTOR_SET_VAL);
    VISIT_PRIM(PRIM_CONS_VAL);    VISIT_PRIM(PRIM_EQ_P_VAL);    VISIT_PRIM(PRIM_EQV_P_VAL);
    #undef VISIT_PRIM

    /* Quote literals embedded in NODE_QUOTE — visit each so the stored
     * VALUE bits get forwarded under a moving backend. */
    for (size_t i = 0; i < QUOTE_NODES_LEN; i++) {
        NODE *n = QUOTE_NODES[i];
        VALUE v = (VALUE)n->u.node_quote.v;
        if (SCM_IS_PTR(v) && !scm_is_singleton(v) && v != 0) {
            ARO_GC_VISIT_EDGE_PTR(gc, edge_visit, &n->u.node_quote.v);
        }
    }

    /* Invalidate parent-chain + gref / arith caches so any stored VALUE
     * / sframe* outside the explicit root set is forced to re-resolve
     * through rooted state on next access.  Cheap (uint64 increment). */
    c->globals_serial++;
    c->env_serial++;
    c->env_cache_serial = c->env_serial - 1;  /* force rebuild */
    c->env_chain_filled = 0;
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

/* Scratch slots for c->sp.  ascheme has no per-call VALUE stack, but
 * the sample-provided realloc helpers (aro_gc_realloc_byte_payload /
 * aro_gc_realloc_payload, defined above) park the old payload into
 * sp[0] across an inner alloc so the root scanner keeps it alive.
 * Many sample-side helpers (parser desugaring, list builders, scm_apply
 * argv parking) also push 2–6 slots here while iterating across allocs,
 * and the recursion may nest dozens of frames deep.  Visible to
 * aro_scheme_visit_roots above (= forward extern), hence non-static.
 * Size symbol ASCHEME_SP_SCRATCH_SIZE is defined in context.h. */
VALUE g_sp_scratch[ASCHEME_SP_SCRATCH_SIZE];

static CTX *
create_context(void)
{
    /* CTX itself is host-owned (it's the input to aro_gc_init), not on the
     * GC heap.  Plain calloc gives us the zeroed start state. */
    CTX *c = (CTX *)calloc(1, sizeof(CTX));
    if (!c) { perror("calloc CTX"); abort(); }
    c->env = NULL;
    c->sp  = g_sp_scratch;   /* framework writes 1 slot, then restores */
    c->globals_serial = 1;   // any cache with serial==0 is uninitialised
    aro_gc_init(c);
    /* GMP allocators must route through aro_gc heap so bignum chunks live
     * in the same arena as the rest of the values.  Signature is fixed
     * (no CTX parameter), hence `gmp_g_ctx` — the one and only sample-wide
     * "g_ctx" pattern, justified by the external API constraint. */
    gmp_g_ctx = c;
    mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
    install_prims(c);
    return c;
}

static VALUE
eval_top(CTX *c, NODE *body)
{
    c->tail_call_pending = 0;
    return EVAL(c, body, c->sp);
}

// ---------------------------------------------------------------------------
// Profile-guided entry selection.
//
// Modeled on abruby's PGO machinery (sample/abruby/abruby_gen.rb registers
// HOPT + PROFILE tasks; we implement a stripped-down version).  The flow:
//
//   1. `--profile run.scm` runs interpretively; scm_apply increments
//      `body->head.dispatch_cnt` on each closure entry.  At exit we walk
//      AOT_ENTRIES and dump (Horg, count) tuples to code_store/profile.txt.
//
//   2. `--use-profile --aot-compile run.scm` loads the profile and, during
//      `aot_compile_and_load`, skips entries whose recorded count is below
//      AOT_PROFILE_THRESHOLD.  Cold entries keep their default dispatcher,
//      so make/gcc only burns time on the hot ones — typically 10× fewer
//      entries means 10× faster cold AOT and a smaller all.so.
//
// abruby goes further by emitting a separate Hopt-keyed PGSD_<Hopt>
// variant that bakes profile-derived constants (method prologues, etc.)
// into the generated C; ascheme's specialized nodes already inline their
// hot-path constants via PRIM_*_VAL, so we get most of the same benefit
// without a parallel hash.

#define AOT_PROFILE_THRESHOLD 10

struct profile_entry {
    node_hash_t horg;
    uint32_t    count;
};

static struct profile_entry *PROFILE_DATA = NULL;
static size_t PROFILE_LEN = 0;
static size_t PROFILE_CAPA = 0;
static bool   PROFILE_LOADED = false;

static void
profile_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "code_store/profile.txt");
}

static void
profile_dump(void)
{
    char path[256]; profile_path(path, sizeof(path));
    (void)!system("mkdir -p code_store");
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "# ascheme profile: <Horg-hex> <count>\n");
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        NODE *n = AOT_ENTRIES[i];
        if (n->head.dispatch_cnt == 0) continue;
        fprintf(fp, "%lx %u\n",
                (unsigned long)HASH(n), n->head.dispatch_cnt);
    }
    fclose(fp);
}

static void
profile_load(void)
{
    char path[256]; profile_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    PROFILE_LOADED = true;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long horg; unsigned int count;
        if (sscanf(line, "%lx %u", &horg, &count) != 2) continue;
        if (PROFILE_LEN == PROFILE_CAPA) {
            PROFILE_CAPA = PROFILE_CAPA ? PROFILE_CAPA * 2 : 64;
            /* PROFILE_DATA is host-side metadata; plain realloc. */
            PROFILE_DATA = (struct profile_entry *)realloc(PROFILE_DATA,
                                sizeof(struct profile_entry) * PROFILE_CAPA);
        }
        PROFILE_DATA[PROFILE_LEN].horg = (node_hash_t)horg;
        PROFILE_DATA[PROFILE_LEN].count = count;
        PROFILE_LEN++;
    }
    fclose(fp);
}

static uint32_t
profile_lookup(node_hash_t h)
{
    for (size_t i = 0; i < PROFILE_LEN; i++)
        if (PROFILE_DATA[i].horg == h) return PROFILE_DATA[i].count;
    return 0;
}

// AOT compile each registered entry, build all.so, reload, then patch every
// entry's dispatcher from the freshly-loaded shared object.  Returns the
// number of entries that successfully loaded a specialized SD_<hash>.
static size_t
aot_compile_and_load(CTX *c, bool verbose)
{
    size_t skipped = 0;
    if (verbose) {
        if (PROFILE_LOADED)
            fprintf(stderr, "ascheme: AOT compiling (profile-guided, threshold=%d, %zu entries)...\n",
                    AOT_PROFILE_THRESHOLD, AOT_ENTRIES_LEN);
        else
            fprintf(stderr, "ascheme: AOT compiling %zu entries...\n", AOT_ENTRIES_LEN);
    }
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        if (PROFILE_LOADED) {
            uint32_t count = profile_lookup(HASH(AOT_ENTRIES[i]));
            if (count < AOT_PROFILE_THRESHOLD) { skipped++; continue; }
        }
        astro_cs_compile(AOT_ENTRIES[i], NULL);
    }
    if (verbose && skipped > 0)
        fprintf(stderr, "ascheme: skipped %zu cold entries\n", skipped);
    if (verbose) fprintf(stderr, "ascheme: building all.so (-O3 -lgc -lgmp)...\n");
    // SD_*.c needs gc.h-free build; we link gc/gmp via the host (-rdynamic).
    // Disable ccache: it tries to write to its cache dir which may be on a
    // read-only / sandboxed FS.  Setting CCACHE_DISABLE turns ccache into a
    // pass-through to the underlying gcc.
    setenv("CCACHE_DISABLE", "1", 1);
    // SD_*.c は #include "node.h" / "precise_gc/gc_types.h" を引くので、
    // ascheme_precise の build 時に -DASCHEME_PRECISE_DIR / -DASTRO_RUNTIME_DIR
    // で baked された絶対 path を -I として cc に渡す。
    // BARUBY_GC も必須: 未定義だと gc_types.h が default の BARUBY_GC_COPY
    // を選び、 host (= 別 backend) と AroObjectHeader レイアウトが
    // 食い違って sweep 等が無限 loop / SEGV する。
    char extra_cflags[1024];
    snprintf(extra_cflags, sizeof(extra_cflags),
             " -I" ASCHEME_PRECISE_DIR
             " -I" ASTRO_RUNTIME_DIR
             " -DBARUBY_GC=%d",
             BARUBY_GC);
    astro_cs_build(extra_cflags);
    astro_cs_reload();
    OPTION.no_compiled_code = false;        // OPTIMIZE will load via cs_load
    size_t loaded = 0, unique = 0;
    // Patch every entry's dispatcher.  Two entries can share a hash (the
    // compiler sometimes builds the same shape via different paths), and
    // each NODE instance has its own head.dispatcher slot to fix up — the
    // SD_<hash> function in all.so is shared, but the patch must hit each
    // pointer or the un-patched ones keep dispatching through the slow
    // host DISPATCH_node_* fallback.  Track unique hashes for the verbose
    // report only.
    /* `seen` は hash 重複カウント用の一時 buffer。 GC heap 上に取ると
     * 先頭 8 byte が AroObjectHeader を上書きして region walk が崩れる
     * (= mark_freelist の sweep が gc_size=hash 値を読んで size_class_for
     * が -1 → size_class_bytes[-1] で `p += garbage` 無限 loop)。
     * heap pointer を保持しないので普通の calloc で十分。 */
    node_hash_t *seen = (node_hash_t *)calloc(AOT_ENTRIES_LEN, sizeof(node_hash_t));
    size_t seen_n = 0;
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        node_hash_t h = HASH(AOT_ENTRIES[i]);
        bool already = false;
        for (size_t j = 0; j < seen_n; j++) if (seen[j] == h) { already = true; break; }
        if (!already) { seen[seen_n++] = h; unique++; }
        if (astro_cs_load(AOT_ENTRIES[i], NULL)) loaded++;
    }
    free(seen);
    if (verbose) fprintf(stderr, "ascheme: loaded %zu / %zu entries (%zu unique SDs)\n",
                         loaded, AOT_ENTRIES_LEN, unique);
    return loaded;
}

static int
run_string(CTX *c, const char *src, size_t len, bool print_results)
{
    // Per-form error recovery: a script that mis-types one expression
    // shouldn't abort the rest.  Each iteration installs a fresh setjmp
    // landing pad; on error we print and move on, accumulating an error
    // count for the exit status.
    /* Park `forms` (and the iterator `p`) on c->sp so they survive across
     * compile() / eval_top(), each of which may trigger arbitrarily many
     * GC cycles under a moving backend. */
    VALUE * restrict sp = c->sp;
    SP_PUSH(c, sp, 2);     /* sp[0]=forms, sp[1]=iter p */
    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        SP_POP(c, sp);
        return 1;
    }
    c->err_jmp_active = 1;
    sp[0] = scm_read_all_string(c, src, len);
    int errors = 0;
    sp[1] = sp[0];
    while (scm_is_pair(sp[1])) {
        VALUE form = SCM_PTR(sp[1])->pair.car;
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            sp[1] = SCM_PTR(sp[1])->pair.cdr;
            continue;
        }
        c->err_jmp_active = 1;
        NODE *ast = compile(c, form, NULL, false);
        VALUE r = eval_top(c, ast);
        if (print_results && r != SCM_UNSPEC) {
            scm_display(stdout, r, true);
            putchar('\n');
        }
        sp[1] = SCM_PTR(sp[1])->pair.cdr;
    }
    c->err_jmp_active = 0;
    SP_POP(c, sp);
    return errors > 0 ? 1 : 0;
}

// One-shot profile-guided compilation, modeled on abruby's --pg-compile.
// We run the program interpretively (no AOT applied during the run) so
// `body->head.dispatch_cnt` accumulates true execution counts; on exit we
// AOT-compile entries above the threshold and persist the resulting
// code_store/ for the *next* invocation to consume via `--aot-compile`.
static int
run_file_pg_compile(CTX *c, const char *path, bool verbose)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* file contents buffer; treat as scratch byte payload. */ char *buf_raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + sz + 1); char *buf = buf_raw + sizeof(AroObjectHeader);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);

    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        return 1;
    }
    c->err_jmp_active = 1;

    // Parse + compile to collect entries (no AOT yet).
    VALUE forms = scm_read_all_string(c, buf, (size_t)sz);
    NODE **asts = (NODE **)scm_alloc_min(c, sizeof(NODE *) * (list_length(forms) + 1));
    int nasts = 0;
    for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        asts[nasts] = compile(c, SCM_PTR(p)->pair.car, NULL, false);
        aot_add_entry(asts[nasts]);
        nasts++;
    }

    // Run interpretively — this populates dispatch_cnt on every body.
    int errors = 0;
    for (int i = 0; i < nasts; i++) {
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            continue;
        }
        c->err_jmp_active = 1;
        eval_top(c, asts[i]);
    }
    c->err_jmp_active = 0;

    // Synthesize a profile from the live counters.  We reuse the
    // file-loading path so aot_compile_and_load filters cold entries.
    PROFILE_LOADED = true;
    for (size_t i = 0; i < AOT_ENTRIES_LEN; i++) {
        NODE *n = AOT_ENTRIES[i];
        if (n->head.dispatch_cnt == 0) continue;
        if (PROFILE_LEN == PROFILE_CAPA) {
            PROFILE_CAPA = PROFILE_CAPA ? PROFILE_CAPA * 2 : 64;
            /* PROFILE_DATA is host-side metadata; plain realloc. */
            PROFILE_DATA = (struct profile_entry *)realloc(PROFILE_DATA,
                                sizeof(struct profile_entry) * PROFILE_CAPA);
        }
        PROFILE_DATA[PROFILE_LEN].horg = HASH(n);
        PROFILE_DATA[PROFILE_LEN].count = n->head.dispatch_cnt;
        PROFILE_LEN++;
    }
    if (verbose) fprintf(stderr, "ascheme: --pg-compile: synthesized profile (%zu entries with count > 0)\n", PROFILE_LEN);

    // Compile hot entries; cache them on disk via astro_cs_build.  The
    // freshly-loaded SDs go unused here (we already finished the run),
    // but the next `ascheme --aot-compile` invocation picks them up from
    // code_store/all.so.
    aot_compile_and_load(c, verbose);

    // Persist a textual copy too — useful for inspection / sharing
    // profiles across machines without a binary code store.
    profile_dump();

    return errors > 0 ? 1 : 0;
}

// Two-pass execution for AOT mode: first parse + compile every form,
// register entries, AOT-compile, build, reload, load — then evaluate.
static int
run_file_aot(CTX *c, const char *path, bool verbose)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* file contents buffer; treat as scratch byte payload. */ char *buf_raw = (char *)aro_gc_alloc_byte_raw(c, sizeof(AroObjectHeader) + sz + 1); char *buf = buf_raw + sizeof(AroObjectHeader);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);

    // If a profile from a prior `--pg-compile` exists in the code store,
    // pick it up automatically.  This makes `--aot-compile` after `--pg-compile`
    // behave as a pure cache-load for hot entries — cold entries are
    // skipped (left running on the default dispatcher) instead of being
    // compiled on the spot.
    profile_load();

    // Pass 1 (parse+compile) runs under one error envelope — mid-parse
    // errors abort the whole pass.  Pass 3 re-arms setjmp per form so a
    // runtime error in form N doesn't squash the rest of the script.
    if (setjmp(c->err_jmp) != 0) {
        fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
        c->err_jmp_active = 0;
        return 1;
    }
    c->err_jmp_active = 1;

    VALUE forms = scm_read_all_string(c, buf, (size_t)sz);
    NODE **asts = (NODE **)scm_alloc_min(c, sizeof(NODE *) * (list_length(forms) + 1));
    int nasts = 0;
    for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
        asts[nasts] = compile(c, SCM_PTR(p)->pair.car, NULL, false);
        aot_add_entry(asts[nasts]);
        nasts++;
    }

    aot_compile_and_load(c, verbose);

    int errors = 0;
    for (int i = 0; i < nasts; i++) {
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            errors++;
            continue;
        }
        c->err_jmp_active = 1;
        eval_top(c, asts[i]);
    }
    c->err_jmp_active = 0;
    return errors > 0 ? 1 : 0;
}

static int
run_file(CTX *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* Source buffer must outlive every potential GC during parse + run
     * (= parser stores substrings into OBJ_SYMBOL.sym.name etc.).  GC
     * heap allocation needs an explicit root which the source-buffer
     * path doesn't have; libc malloc lives forever and is invisible to
     * the GC, so it's safe.  Leak on exit is acceptable (= one alloc
     * per program). */
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { perror("malloc source buffer"); fclose(fp); return 1; }
    if (fread(buf, 1, sz, fp) != (size_t)sz) { perror(path); free(buf); fclose(fp); return 1; }
    buf[sz] = '\0';
    fclose(fp);
    int r = run_string(c, buf, (size_t)sz, false);
    return r;
}

static char *
read_line(const char *prompt)
{
#ifdef USE_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;
#else
    fputs(prompt, stdout);
    fflush(stdout);
    static char buf[8192];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
#endif
}

static int
repl(CTX *c)
{
    if (!OPTION.quiet) {
        printf("ascheme — R5RS Scheme on ASTro.  Type (exit) to quit.\n");
    }
    char *line;
    while ((line = read_line("ascheme> ")) != NULL) {
        if (!*line) continue;
        if (setjmp(c->err_jmp) != 0) {
            fprintf(stderr, "ascheme: error: %s\n", SCM_ERR_MSG);
            c->err_jmp_active = 0;
            continue;
        }
        c->err_jmp_active = 1;
        VALUE forms = scm_read_all_string(c, line, strlen(line));
        for (VALUE p = forms; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) {
            VALUE form = SCM_PTR(p)->pair.car;
            NODE *ast = compile(c, form, NULL, false);
            VALUE r = eval_top(c, ast);
            if (r != SCM_UNSPEC) {
                scm_display(stdout, r, true);
                putchar('\n');
            }
        }
        c->err_jmp_active = 0;
#ifdef USE_READLINE
        free(line);
#endif
    }
    putchar('\n');
    return 0;
}

static void
usage(void)
{
    fprintf(stderr,
        "usage: ascheme_precise [options] [file.scm | -e <expr> | -]\n"
        "\n"
        "ascheme_precise-specific options:\n"
        "  -e <expr>            evaluate expression and print result\n"
        "  -                    read program from stdin\n"
        "      --clear-cs       delete code_store/ before starting\n"
        "\n");
    astro_print_build_help(stderr);
}

int
main(int argc, char *argv[])
{
    /* Phase 1 migration: GC initialisation is per-CTX (aro_gc_init), done
     * inside create_context().  There's no longer a process-global GC init. */
    OPTION.no_compiled_code = true;        // plain interpreter is the default

    // Pre-scan argv for framework-owned build flags (--plain, --aot-compile,
    // --pg-compile, --run, --build OUT, -q/--quiet, -v/--verbose, -h/--help,
    // --version).  Order-free within the flag block (before the source file).
    struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
    if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

    if (bcfg.help_requested)    { usage(); return 0; }
    if (bcfg.version_requested) { printf("ascheme_precise (ASTro %s)\n", ASTRO_VERSION); return 0; }

    // Translate framework flags into ascheme's OPTION.  ascheme follows the
    // koruby pattern: it must run to discover entries (compile() registers
    // each top-level form before eval_top), so bake-only is meaningless.
    // --aot-compile (with or without --run) => always run+bake.
    if (bcfg.quiet)   OPTION.quiet = true;
    bool aot        = bcfg.aot_compile;
    bool pg_compile = bcfg.pg_compile;
    bool verbose    = bcfg.verbose;

    // Sample-specific flag pass: -e (delayed), --clear-cs.  The framework
    // already consumed -q/-v/-h/--version/--plain/--aot-compile/--pg-compile.
    int ai = 1;
    bool clear_cs = false;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        if (!strcmp(argv[ai], "--clear-cs")) clear_cs = true;
        else if (!strcmp(argv[ai], "-e")) break;        // delayed
        else if (!strcmp(argv[ai], "--")) { ai++; break; }
        else { fprintf(stderr, "ascheme: unknown option %s\n", argv[ai]); usage(); return 2; }
        ai++;
    }
    if (clear_cs) (void)!system("rm -rf code_store");
    INIT();
    /* Enable OPTIMIZE → astro_cs_load on plain runs.  astro_cs_load
     * silently returns false if no SD was cached for the node's hash,
     * so this is a no-op when code_store/all.so doesn't exist.  When
     * `--aot-compile` previously baked SDs, plain runs now pick them
     * up automatically. */
    if (!bcfg.plain) {
        OPTION.no_compiled_code = false;
    }
    CTX *c = create_context();
    if (ai >= argc) return repl(c);
    if (!strcmp(argv[ai], "-e")) {
        if (ai + 1 >= argc) { fprintf(stderr, "ascheme: -e requires an argument\n"); return 2; }
        return run_string(c, argv[ai + 1], strlen(argv[ai + 1]), true);
    }
    if (!strcmp(argv[ai], "-")) {
        char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
        buf[n] = '\0';
        return run_string(c, buf, n, false);
    }
    int _rc;
    if (pg_compile) { ASCHEME_PROFILING = true; _rc = run_file_pg_compile(c, argv[ai], verbose); }
    else if (aot) _rc = run_file_aot(c, argv[ai], verbose);
    else _rc = run_file(c, argv[ai]);
    if (getenv("ASCHEME_GC_STATS")) {
        fprintf(stderr, "[gc_stats] finalize_calls=%zu gc_count=%zu total_bytes=%zu heap_bytes=%zu\n",
                aro_finalize_calls, aro_gc_count(c),
                aro_gc_total_bytes(c), aro_gc_heap_bytes(c));
    }
    return _rc;
}
