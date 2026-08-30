/* koruby_precise v2 — korb_runtime.c
 *
 * Runtime core on the slots ABI (docs/v2_design.md):
 *   - korb_alloc: the ONLY c->slots_top publish point
 *   - strings / exceptions (moving heap, korb_alloc only — no libc objects)
 *   - symbol intern table + global method table (per-CTX VM, no globals)
 *   - korb_call: frame push (params window = staged args), RETURN catch,
 *     unwind backtrace accumulation
 *   - builtins: puts / p / print
 */

#define _GNU_SOURCE 1   /* pthread_getattr_np */
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <ucontext.h>
#include <poll.h>      /* blop 層の pump (builtins/thread.c) */
#include <dlfcn.h>

#include "node.h"
#include "korb_runtime.h"
#include "precise_gc/gc.h"

/* Frame-push headroom: covers in-frame expression staging without a per-node
 * check.  Deeper-than-slack staging without an intervening call lands on the
 * guard page (the designed last-resort backstop, v2_design §3.5). */

/* ---------------------------------------------------------------------------
 * Allocation — publish + alloc (v2_design §3.3).
 * ------------------------------------------------------------------------- */

void *
korb_alloc(CTX *c, VALUE *slots, size_t size, unsigned int type)
{
    ASTRO_ASSERT(slots >= c->slots && slots <= c->slots_limit);
    c->slots_top = slots;                 /* publish: live values are below */
    /* The copy GC overlays each forwarded object's new address at payload
     * offset 0 (= object offset 8, sizeof(AroObjectHeader)).  An object smaller
     * than header+8 = 16 bytes therefore has its forward overlay spill into the
     * NEXT object's header, corrupting it (hit by a closed n=0 env's empty
     * KORB_OBJ_VALUE_ARRAY, whose sizeof is just the 8-byte header).  Pad to 16
     * and zero the tail so a VALUE_ARRAY's gc_size-derived edge scan reads a nil
     * phantom slot rather than garbage. */
    const size_t asize = size < 16 ? 16 : size;
    VALUE v = aro_gc_alloc(c, asize);     /* may collect; scans [slots, slots_top) */
    AroObjectHeader *h = (AroObjectHeader *)(uintptr_t)v;
    h->flags = (uint16_t)type;
    if (asize > size) memset((char *)h + size, 0, asize - size);
    return h;
}

/* ---------------------------------------------------------------------------
 * Strings.  Bytes are inline (single allocation, copied whole on move).
 * `bytes` source must be C memory (literal operand / stack buffer) — it is
 * read AFTER the allocation.  Heap-sourced constructors take VALUE_REFs.
 * ------------------------------------------------------------------------- */

/* Allocate a KorbString with an uninitialized buffer of capacity `len` (len set,
 * NUL-terminated). Buffer is separate (header never moves on later grow). Caller
 * fills s->buf->data with no intervening GC. */
static KorbString *
korb_str_alloc(CTX *c, VALUE *slots, uint32_t len)
{
    KorbStrBuf *b = korb_alloc(c, slots, sizeof(KorbStrBuf) + len + 1, KORB_OBJ_STR_BUF);
    VALUE_REF bref = SLOTS_PUSH(slots, (VALUE)b);        /* root buf while header allocs */
    KorbString *s = korb_alloc(c, slots, sizeof(KorbString), KORB_OBJ_STRING);
    b = (KorbStrBuf *)(uintptr_t)VALUE_REF_GET(bref);    /* re-read after GC */
    s->len = len; s->capa = len;
    ARO_STORE(c, s, (VALUE *)(uintptr_t)&s->buf, (VALUE)(uintptr_t)b);
    korb_strbuf_data(b)[len] = '\0';
    return s;
}

RESULT
korb_str_new(CTX *c, VALUE *slots, const char *bytes, uint32_t len)
{
    KorbString *s = korb_str_alloc(c, slots, len);
    memcpy(korb_strbuf_data(s->buf), bytes, len);
    return RESULT_OK((VALUE)s);
}

/* Ensure `s` (rooted via sref) has capacity for at least `need` bytes; grows the
 * buffer (header stays put). Returns the (possibly relocated) KorbString*. */
static KorbString *
korb_str_ensure(CTX *c, VALUE *slots, VALUE_REF sref, uint32_t need)
{
    KorbString *s = VAL2STR(VALUE_REF_GET(sref));
    if (need <= s->capa) return s;
    uint32_t ncapa = s->capa ? s->capa * 2 : 16;
    while (ncapa < need) ncapa *= 2;
    KorbStrBuf *nb = korb_alloc(c, slots, sizeof(KorbStrBuf) + ncapa + 1, KORB_OBJ_STR_BUF);
    s = VAL2STR(VALUE_REF_GET(sref));                    /* re-read after GC */
    memcpy(korb_strbuf_data(nb), korb_strbuf_data(s->buf), s->len);
    korb_strbuf_data(nb)[s->len] = '\0';
    ARO_STORE(c, s, (VALUE *)(uintptr_t)&s->buf, (VALUE)(uintptr_t)nb);
    s->capa = ncapa;
    return s;
}

/* Append `n` bytes to string `s` (rooted via sref). src must NOT point into a
 * GC-movable buffer across the grow (copy it to a stable place first, or pass a
 * pointer re-read from a rooted slot — callers ensure this). */
static RESULT
korb_str_cat(CTX *c, VALUE *slots, VALUE_REF sref, const char *src, uint32_t n)
{
    KorbString *s = korb_str_ensure(c, slots, sref, VAL2STR(VALUE_REF_GET(sref))->len + n);
    memcpy(korb_strbuf_data(s->buf) + s->len, src, n);
    s->len += n;
    korb_strbuf_data(s->buf)[s->len] = '\0';
    return RESULT_OK(VALUE_REF_GET(sref));
}

/* ---------------------------------------------------------------------------
 * Float (heap-boxed double).
 * ------------------------------------------------------------------------- */

RESULT
korb_float_box(CTX *c, VALUE *slots, double d)
{
    KorbFloat *f = korb_alloc(c, slots, sizeof(KorbFloat), KORB_OBJ_FLOAT);
    f->val = d;
    return RESULT_OK((VALUE)f);
}

/* Float-literal pool: return a boxed Float for `d`, reusing an existing box with
 * the same bit pattern (so a non-flonum literal like 2.0 in a loop is boxed once,
 * not on every eval).  The pool is root-scanned, so cached boxes survive GC. */
VALUE korb_flit_get(CTX *c, VALUE *slots, double d) {
    struct korb_vm *const vm = c->vm;
    union { double d; uint64_t u; } key; key.d = d;
    for (uint32_t i = 0; i < vm->flit_cnt; i++) {            /* dedup by exact bits (handles -0.0 / NaN) */
        union { double d; uint64_t u; } e; e.d = VAL2FLT(vm->flit_vals[i])->val;
        if (e.u == key.u) return vm->flit_vals[i];
    }
    const VALUE boxed = korb_float_box(c, slots, d).value;     /* may GC (aborts on OOM); pool/const roots already forwarded */
    if (vm->flit_cnt == vm->flit_capa) {
        vm->flit_capa = vm->flit_capa ? vm->flit_capa * 2 : 8;
        vm->flit_vals = realloc(vm->flit_vals, sizeof(VALUE) * vm->flit_capa);
    }
    vm->flit_vals[vm->flit_cnt++] = boxed;
    return boxed;
}

/* Frozen String-literal pool: one shared frozen String per (bytes, encoding),
 * like CRuby's fstring table.  Linear scan — the pool only ever holds the
 * program's distinct frozen literals, and lookups happen once per literal
 * NODE (the node caches the result). */
VALUE korb_fstr_get(CTX *c, VALUE *slots, const char *bytes, uint32_t len, uint32_t enc) {
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->fstr_cnt; i++) {
        const KorbString *const s = VAL2STR(vm->fstr_vals[i]);
        if (s->len == len && KORB_STR_ENC(vm->fstr_vals[i]) == enc &&
            memcmp(korb_strbuf_data(s->buf), bytes, len) == 0)
            return vm->fstr_vals[i];
    }
    const VALUE v = korb_str_new(c, slots, bytes, len).value;   /* may GC; the pool is a root */
    KORB_STR_ENC_SET(v, enc);
    ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_FROZEN;
    if (vm->fstr_cnt == vm->fstr_capa) {
        vm->fstr_capa = vm->fstr_capa ? vm->fstr_capa * 2 : 16;
        vm->fstr_vals = realloc(vm->fstr_vals, sizeof(VALUE) * vm->fstr_capa);
        if (!vm->fstr_vals) abort();
    }
    vm->fstr_vals[vm->fstr_cnt++] = v;
    return v;
}

RESULT
korb_float_new(CTX *c, VALUE *slots, double d)
{
    VALUE imm = korb_d2flo(d);                 /* immediate flonum — no heap box */
    if (imm) return RESULT_OK(imm);
    return korb_float_box(c, slots, d);
}

static korb_sword_t korb_gcd_pos(korb_sword_t a, korb_sword_t b) {   /* gcd of |a|,|b| */
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { korb_sword_t t = a % b; a = b; b = t; }
    return a;
}
/* korb_to_mpz / korb_big_from_mpz live in builtins/bignum.c (included later);
 * forward-declare so the Rational core can reduce Bignum num/den. */
static void korb_to_mpz(VALUE v, korb_mp_t out);
static RESULT korb_big_from_mpz(CTX *c, VALUE *slots, const korb_mp_t src);
static RESULT korb_coerce_to_int(CTX *c, VALUE *slots, VALUE *v);   /* fwd (string.c) — #to_int coercion, usable from the main body */
static RESULT korb_coerce_to_ary(CTX *c, VALUE *slots, VALUE *v);   /* fwd (string.c) — #to_ary coercion */

/* Make a reduced Rational from VALUE num/den (Fixnum or Bignum); den != 0,
 * normalized den > 0.  Fixnum-fits fast path; Bignum path reduces via korb_mp_gcd. */
RESULT
korb_rat_new_v(CTX *c, VALUE *slots, VALUE num, VALUE den)
{
    if (LIKELY(FIXNUM_P(num) && FIXNUM_P(den))) {
        korb_sword_t n = FIX2LONG(num), d = FIX2LONG(den);
        if (UNLIKELY(d == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        if (d < 0) { n = -n; d = -d; }
        korb_sword_t g = korb_gcd_pos(n, d);
        if (g > 1) { n /= g; d /= g; }
        KorbRational *r = korb_alloc(c, slots, sizeof(KorbRational), KORB_OBJ_RATIONAL);
        ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->num, LONG2FIX(n));
        ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->den, LONG2FIX(d));
        return RESULT_OK((VALUE)r);
    }
    korb_mp_t zn, zd, zg;
    korb_to_mpz(num, zn); korb_to_mpz(den, zd);
    if (UNLIKELY(korb_mp_sgn(zd) == 0)) { korb_mp_clear(zn); korb_mp_clear(zd); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
    if (korb_mp_sgn(zd) < 0) { korb_mp_neg(zn, zn); korb_mp_neg(zd, zd); }
    korb_mp_init(zg); korb_mp_gcd(zg, zn, zd);
    if (korb_mp_cmp_ui(zg, 1) > 0) { korb_mp_divexact(zn, zn, zg); korb_mp_divexact(zd, zd, zg); }
    korb_mp_clear(zg);
    RESULT nr = korb_big_from_mpz(c, slots, zn); korb_mp_clear(zn);
    if (UNLIKELY(nr.state != KORB_NORMAL)) { korb_mp_clear(zd); return nr; }
    slots[0] = nr.value;                                       /* root num across den alloc */
    RESULT dr = korb_big_from_mpz(c, slots + 1, zd); korb_mp_clear(zd);
    if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
    slots[1] = dr.value;
    KorbRational *r = korb_alloc(c, slots + 2, sizeof(KorbRational), KORB_OBJ_RATIONAL);
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->num, slots[0]);
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->den, slots[1]);
    return RESULT_OK((VALUE)r);
}
/* word → VALUE (Fixnum if FIXABLE, else Bignum — e.g. float_to_rat's 1<<62). */
static RESULT korb_intptr_to_val(CTX *c, VALUE *slots, korb_sword_t n) {
    if (LIKELY(FIXABLE(n))) return RESULT_OK(LONG2FIX(n));
    korb_mp_t z; korb_mp_init_set_si(z, (long)n);
    RESULT r = korb_big_from_mpz(c, slots, z); korb_mp_clear(z); return r;
}
/* Legacy intptr entry (some callers pass values up to 1<<62, beyond Fixnum). */
RESULT korb_rat_new(CTX *c, VALUE *slots, korb_sword_t num, korb_sword_t den) {
    slots[0] = UNWRAP(korb_intptr_to_val(c, slots, num));     /* root num across den/rat allocs */
    slots[1] = UNWRAP(korb_intptr_to_val(c, slots + 1, den));
    return korb_rat_new_v(c, slots + 2, slots[0], slots[1]);
}
/* (num,den) VALUEs of an Int-or-Rational; false if neither. */
static bool korb_as_rat_v(VALUE v, VALUE *num, VALUE *den) {
    if (KORB_INTEGER_P(v))  { *num = v; *den = LONG2FIX(1); return true; }   /* Fixnum or Bignum */
    if (KORB_RATIONAL_P(v)) { *num = VAL2RAT(v)->num; *den = VAL2RAT(v)->den; return true; }
    return false;
}
/* Rational arithmetic (op 0+ 1- 2* 3/); Float involved → Float, else exact Rational.
 * Integer num/den products go through korb_int_arith (Fixnum → Bignum on overflow),
 * staged in slots[0..] so each Bignum alloc keeps the operands rooted. */
RESULT korb_rat_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op) {
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(r)) return korb_num_arith(c, slots, l, r, op, 0);
    /* Rational ⊗ Complex is Complex arithmetic (korb_cpx_parts takes a Rational
     * component as-is) — decided before the Rational conversion, which a Complex
     * operand would otherwise fail with a coercion TypeError. */
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(r)) return korb_cpx_arith(c, slots, l, r, op);
    if (UNLIKELY(!korb_as_rat_v(l, &slots[0], &slots[1]) || !korb_as_rat_v(r, &slots[2], &slots[3]))) {
        if (KORB_RATIONAL_P(l) && KORB_OBJECT_P(r)) {     /* a, b = r.coerce(l); a OP b */
            static const char *const ratop[] = { "+", "-", "*", "/", "%" };
            bool h; RESULT cr = korb_try_coerce(c, slots, l, r, ratop[op], 0, &h); if (h) return cr;
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Rational", korb_type_name(KORB_RATIONAL_P(l) ? r : l));
    }
    /* Modulo is not a variant of division: it needs the floored quotient, which
     * korb_int_rat_divmod computes (it only ever calls back with ops 1/2/3). */
    if (op == 4) return korb_int_rat_divmod(c, slots, l, r, 1);
    /* slots[0..3] = ln, ld, rn, rd (rooted); compute num→slots[6], den→slots[7]. */
    if (op == 0 || op == 1) {                                  /* (ln*rd ± rn*ld) / (ld*rd) */
        slots[4] = UNWRAP(korb_int_arith(c, slots + 4, slots[0], slots[3], 2, 0));   /* ln*rd */
        slots[5] = UNWRAP(korb_int_arith(c, slots + 5, slots[2], slots[1], 2, 0));   /* rn*ld */
        slots[6] = UNWRAP(korb_int_arith(c, slots + 6, slots[4], slots[5], op, 0));  /* ± */
        slots[7] = UNWRAP(korb_int_arith(c, slots + 7, slots[1], slots[3], 2, 0));   /* ld*rd */
    } else if (op == 2) {                                      /* ln*rn / ld*rd */
        slots[6] = UNWRAP(korb_int_arith(c, slots + 6, slots[0], slots[2], 2, 0));
        slots[7] = UNWRAP(korb_int_arith(c, slots + 7, slots[1], slots[3], 2, 0));
    } else {                                                   /* / : ln*rd / ld*rn */
        slots[6] = UNWRAP(korb_int_arith(c, slots + 6, slots[0], slots[3], 2, 0));
        slots[7] = UNWRAP(korb_int_arith(c, slots + 7, slots[1], slots[2], 2, 0));
    }
    return korb_rat_new_v(c, slots + 8, slots[6], slots[7]);
}
/* Rational compare vs Int/Rational/Float → -1/0/1, or 2 if incomparable.
 * Cross-multiply via korb_int_cmp on the integer products (Bignum-safe). */
static int korb_rat_cmp(VALUE l, VALUE r) {
    VALUE ln, ld, rn, rd;
    if (korb_as_rat_v(l, &ln, &ld) && korb_as_rat_v(r, &rn, &rd)) {
        if (FIXNUM_P(ln) && FIXNUM_P(ld) && FIXNUM_P(rn) && FIXNUM_P(rd)) {
            __int128 a = (__int128)FIX2LONG(ln) * FIX2LONG(rd);   /* dens > 0 → sign preserved */
            __int128 b = (__int128)FIX2LONG(rn) * FIX2LONG(ld);
            return (a > b) - (a < b);
        }
        korb_mp_t a, b, t; korb_to_mpz(ln, a); korb_to_mpz(rd, t); korb_mp_mul(a, a, t);
        korb_to_mpz(rn, b); korb_to_mpz(ld, t); korb_mp_mul(b, b, t);
        int cmp = korb_mp_cmp(a, b); korb_mp_clear(a); korb_mp_clear(b); korb_mp_clear(t);
        return (cmp > 0) - (cmp < 0);
    }
    double x, y;
    if (korb_num_to_d(l, &x) && korb_num_to_d(r, &y)) return (x > y) - (x < y);
    return 2;
}
static RESULT korb_m_rat_num(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RAT->num); }
static RESULT korb_m_rat_den(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RAT->den); }
static RESULT korb_m_rat_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE num = SELF_RAT->num, den = SELF_RAT->den;
    if (LIKELY(FIXNUM_P(num) && FIXNUM_P(den)))
        return korb_float_new(c, slots, (double)FIX2LONG(num) / (double)FIX2LONG(den));
    /* Bignum num/den: naive num.to_d / den.to_d overflows each to ±Inf → NaN.
     * korb_mq_get_d computes the ratio exactly-rounded regardless of magnitude. */
    korb_mp_t zn, zd; korb_to_mpz(num, zn); korb_to_mpz(den, zd);
    korb_mq_t q; korb_mq_init(q); korb_mq_set_num(q, zn); korb_mq_set_den(q, zd); korb_mq_canonicalize(q);
    const double r = korb_mq_get_d(q);
    korb_mq_clear(q); korb_mp_clear(zn); korb_mp_clear(zd);
    return korb_float_new(c, slots, r);
}
static RESULT korb_m_rat_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* integer floor-div of rational num/den (mode: 0 floor, 1 ceil, 2 trunc,
 * 3 round-half-away (:up), 4 round-half-even, 5 round-half-down (toward zero)). */
static RESULT korb_rat_intdiv(CTX *c, VALUE *slots, VALUE num, VALUE den, int mode) {
    if (LIKELY(FIXNUM_P(num) && FIXNUM_P(den))) {
        const korb_sword_t n = FIX2LONG(num), d = FIX2LONG(den);   /* d > 0 (normalized) */
        korb_sword_t q = n / d, rem = n % d;
        if (mode == 0)      { if (rem != 0 && n < 0) q--; }
        else if (mode == 1) { if (rem != 0 && n > 0) q++; }
        else if (mode == 5) { const korb_sword_t ar = rem < 0 ? -rem : rem; if (ar * 2 > d) q += (n < 0 ? -1 : 1); }   /* ties toward zero */
        else if (mode == 3) { const korb_sword_t ar = rem < 0 ? -rem : rem; if (ar * 2 >= d) q += (n < 0 ? -1 : 1); }
        else if (mode == 4) { const korb_sword_t ar = rem < 0 ? -rem : rem, t = ar * 2;   /* round half to even */
                              if (t > d) q += (n < 0 ? -1 : 1);
                              else if (t == d && (q & 1)) q += (n < 0 ? -1 : 1); }
        return RESULT_OK(LONG2FIX(q));   /* trunc (mode 2) = bare q */
    }
    korb_mp_t zn, zd, zq, zr; korb_to_mpz(num, zn); korb_to_mpz(den, zd); korb_mp_init(zq); korb_mp_init(zr);
    if (mode == 0)      korb_mp_fdiv_qr(zq, zr, zn, zd);
    else if (mode == 1) korb_mp_cdiv_qr(zq, zr, zn, zd);
    else                korb_mp_tdiv_qr(zq, zr, zn, zd);
    if (mode == 3 || mode == 5) { korb_mp_t two_ar; korb_mp_init(two_ar); korb_mp_abs(two_ar, zr); korb_mp_mul_ui(two_ar, two_ar, 2);
                     const int cmp = korb_mp_cmp(two_ar, zd);   /* :up rounds ties away (>=), :down keeps ties (>) */
                     if (cmp > 0 || (mode == 3 && cmp == 0)) { if (korb_mp_sgn(zn) < 0) korb_mp_sub_ui(zq, zq, 1); else korb_mp_add_ui(zq, zq, 1); }
                     korb_mp_clear(two_ar); }
    else if (mode == 4) { korb_mp_t two_ar; korb_mp_init(two_ar); korb_mp_abs(two_ar, zr); korb_mp_mul_ui(two_ar, two_ar, 2);
                     const int cmp = korb_mp_cmp(two_ar, zd);   /* round half to even */
                     if (cmp > 0 || (cmp == 0 && korb_mp_odd_p(zq))) { if (korb_mp_sgn(zn) < 0) korb_mp_sub_ui(zq, zq, 1); else korb_mp_add_ui(zq, zq, 1); }
                     korb_mp_clear(two_ar); }
    korb_mp_clear(zn); korb_mp_clear(zd); korb_mp_clear(zr);
    RESULT r = korb_big_from_mpz(c, slots, zq); korb_mp_clear(zq);
    return r;
}
/* Integer (self) combined with a Rational operand: op 0 div, 1 modulo,
 * 2 divmod, 3 remainder.  div = floor(self/rat); modulo/divmod use the floored
 * quotient, remainder the truncated one; rem = self - rat*q.  All operands are
 * parked in slots so the GMP/Rational allocs below can't strand a moved VALUE. */
RESULT korb_int_rat_divmod(CTX *c, VALUE *slots, VALUE s, VALUE rat, int op) {
    slots[0] = s; slots[1] = rat;                              /* rooted across allocs */
    RESULT qr = korb_rat_arith(c, slots + 2, slots[0], slots[1], 3);   /* self / rat */
    if (UNLIKELY(qr.state != KORB_NORMAL)) return qr;
    slots[2] = qr.value;                                       /* quotient (Rational or Integer) */
    if (KORB_RATIONAL_P(slots[2])) {                           /* → integer quotient (floor, or trunc for remainder) */
        RESULT ir = korb_rat_intdiv(c, slots + 3, VAL2RAT(slots[2])->num, VAL2RAT(slots[2])->den, op == 3 ? 2 : 0);
        if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
        slots[2] = ir.value;
    }
    if (op == 0) return RESULT_OK(slots[2]);                   /* div */
    RESULT pr = korb_rat_arith(c, slots + 3, slots[1], slots[2], 2);   /* rat * q */
    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
    slots[3] = pr.value;
    RESULT rr = korb_rat_arith(c, slots + 4, slots[0], slots[3], 1);   /* self - rat*q */
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (op != 2) return rr;                                    /* modulo / remainder */
    slots[3] = rr.value;                                       /* divmod: [q, rem]; slots[2]=q kept */
    RESULT ar = korb_ary_new(c, slots + 4, 2);
    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
    slots[4] = ar.value;
    CHECK(korb_ary_push_val(c, slots + 5, VALUE_REF_AT(&slots[4]), slots[2]));
    CHECK(korb_ary_push_val(c, slots + 5, VALUE_REF_AT(&slots[4]), slots[3]));
    return RESULT_OK(slots[4]);
}
static RESULT korb_rat_round_digits(CTX *c, VALUE *slots, VALUE num0, VALUE den0, korb_sword_t nd, int mode);   /* fwd */
/* floor/ceil/truncate take the same optional digit count as #round: > 0 keeps a
 * Rational, <= 0 gives an Integer. */
static RESULT korb_rat_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    korb_sword_t nd = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        slots[0] = VALUE_SLICE_GET(a, 0);
        CHECK(korb_coerce_to_int_pub(c, slots + 1, &slots[0]));
        if (!FIXNUM_P(slots[0]))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer",
                              korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        nd = FIX2LONG(slots[0]);
    }
    return korb_rat_round_digits(c, slots, SELF_RAT->num, SELF_RAT->den, nd, mode);
}
static RESULT korb_m_rat_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_intdiv(c, slots, SELF_RAT->num, SELF_RAT->den, 2); }
static RESULT korb_m_rat_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_dig(c, slots, self, a, 0); }
static RESULT korb_m_rat_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_dig(c, slots, self, a, 1); }
static RESULT korb_m_rat_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RAT->num == LONG2FIX(0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_rat_integerp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static void korb_class_qname_into(CTX *c, VALUE cls, char *out, size_t outsz);   /* defined below */
int32_t korb_hash_find(const KorbHash *h, VALUE key);   /* defined below; non-static so node_eval.c's Hash#[] fast path can call it (LTO still inlines) */
void korb_warn(CTX *c, VALUE *slots, const char *fmt, ...);                /* defined below; builtins emit rb_warn-style warnings */
void korb_warn_at(CTX *c, VALUE *slots, const char *file, uint32_t line, const char *fmt, ...);   /* with a source position */
static RESULT korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             NODE *block, VALUE *def_env, VALUE *captured_self);   /* defined below */
static RESULT korb_block_to_proc(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE *cself);   /* defined below */
static RESULT korb_m_ary_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);   /* array.c — for builtin Array subclass .new */
static RESULT korb_m_str_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* string.c — for String.new(non-String source) */
static RESULT korb_eval_run(CTX *c, VALUE *slots, NODE *ast, VALUE *cur, const char *fname, VALUE cref);   /* defined below */
static const char *korb_recv_desc(CTX *c, VALUE *slots, VALUE v, char *buf, size_t bufsz);   /* fwd: "an instance of Foo" */
static RESULT korb_eval_str_self(CTX *c, VALUE *slots, VALUE str, VALUE self_val, const char *fname, int32_t line, VALUE cref);   /* defined below — for instance/class_eval(String) in set.c */
static RESULT korb_eval_binding_core(CTX *c, VALUE *slots, VALUE *src_slot, VALUE *bind_slot,
                                     const char *fname, int32_t eline, VALUE *self_slot, VALUE cref);   /* eval with caller binding (set.c uses it too) */
/* SyntaxError from a parse: the parser leaves a detail message on the vm when it
 * has one (e.g. "Can't set variable $&"); otherwise the generic text is used. */
static RESULT korb_raise_syntax(CTX *c, VALUE *slots, const char *generic);
static RESULT korb_raise_syntax_at(CTX *c, VALUE *slots, const char *generic, const char *fname);   /* + SyntaxError#path */
static RESULT korb_alias_argsym(CTX *c, VALUE *slots, VALUE v, uint32_t *out);   /* name arg → mid: Symbol/String/#to_str (defined below) */
/* div(n) = (self / n).floor → Integer (any numeric n; via runtime dispatch). */
static RESULT korb_m_rat_divfloor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE arg = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(arg) && korb_float_val(arg) == 0.0)   /* Rational#div(0.0): ZeroDivisionError, not floor(Inf) */
        return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    slots[0] = VALUE_REF_GET(self);
    slots[1] = VALUE_SLICE_GET(a, 0);
    RESULT q = korb_send_impl(c, slots + 2, korb_intern(c->vm, "/", 1), 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(q.state != KORB_NORMAL)) return q;
    slots[0] = q.value;
    return korb_send_impl(c, slots + 1, korb_intern(c->vm, "floor", 5), 0, 0, NULL, NULL, NULL);
}
/* self % other == self - (self.div other) * other  (floored modulo).  Composed
 * via dispatch; `other` is re-read from the scanned args between sends (GC-safe)
 * and intermediate results are parked in slots before each send. */
static RESULT korb_m_rat_mod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(o)) {                                /* Rational % Float → Float (floored fmod) */
        const double of = korb_float_val(o);
        if (UNLIKELY(of == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        double sf; korb_num_to_d(VALUE_REF_GET(self), &sf);
        return korb_float_new(c, slots, korb_float_fmod(sf, of));
    }
    /* floored modulo via the shared GMP/Rational divmod (op 1 = modulo); it parks
     * operands so the allocs can't strand a moved VALUE. */
    return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), o, 1);
}
/* divmod → [self.div(other), self % other]. */
static RESULT korb_m_rat_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT d = korb_m_rat_divfloor(c, slots, self, a);
    if (UNLIKELY(d.state != KORB_NORMAL)) return d;
    slots[0] = d.value;
    RESULT m = korb_m_rat_mod(c, slots + 1, self, a);
    if (UNLIKELY(m.state != KORB_NORMAL)) return m;
    slots[1] = m.value;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* marshal_dump → [numerator, denominator]. */
static RESULT korb_m_rat_marshal_dump(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = SELF_RAT->num; slots[1] = SELF_RAT->den;      /* root before alloc */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF arr = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[1]));
    return RESULT_OK(slots[2]);
}
/* Shared digit-scaling for Rational#round/truncate: nd==0 → Integer; nd>0 →
 * Rational(intdiv(num*10^nd, den), 10^nd); nd<0 → Integer scaled.  `mode` selects
 * the rounding (3/4/5 round variants, 0 floor, 1 ceil, 2 trunc-toward-zero). */
static RESULT korb_rat_round_digits(CTX *c, VALUE *slots, VALUE num0, VALUE den0, korb_sword_t nd, int mode) {
    if (nd == 0) return korb_rat_intdiv(c, slots, num0, den0, mode);
    slots[0] = num0;
    slots[1] = den0;                                      /* root across allocs */
    {
        korb_mp_t p; korb_mp_init(p); korb_mp_ui_pow_ui(p, 10, (unsigned long)(nd > 0 ? nd : -nd));   /* 10^|nd| */
        RESULT pr = korb_big_from_mpz(c, slots + 2, p); korb_mp_clear(p);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        slots[2] = pr.value;
    }
    if (nd > 0) {                                         /* Rational(intdiv(num*10^nd, den), 10^nd) */
        RESULT sc = korb_int_arith(c, slots + 3, slots[0], slots[2], 2, 0);   /* num * 10^nd */
        if (UNLIKELY(sc.state != KORB_NORMAL)) return sc;
        slots[3] = sc.value;
        RESULT q = korb_rat_intdiv(c, slots + 4, slots[3], slots[1], mode);
        if (UNLIKELY(q.state != KORB_NORMAL)) return q;
        slots[4] = q.value;
        return korb_rat_new_v(c, slots + 5, slots[4], slots[2]);
    }
    /* nd < 0: intdiv(num, den*10^-nd) * 10^-nd → Integer */
    RESULT dsc = korb_int_arith(c, slots + 3, slots[1], slots[2], 2, 0);       /* den * 10^-nd */
    if (UNLIKELY(dsc.state != KORB_NORMAL)) return dsc;
    slots[3] = dsc.value;
    RESULT q = korb_rat_intdiv(c, slots + 4, slots[0], slots[3], mode);
    if (UNLIKELY(q.state != KORB_NORMAL)) return q;
    slots[4] = q.value;
    return korb_int_arith(c, slots + 5, slots[4], slots[2], 2, 0);             /* q * 10^-nd */
}
/* round([ndigits], half: :up|:even) — ndigits<=0 → Integer, ndigits>0 → Rational. */
static RESULT korb_m_rat_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    int mode = 3;                                          /* 3 = half up (default), 4 = half even, 5 = half down */
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {   /* trailing half: kwarg */
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "half", 4)));
        if (hx >= 0) {
            const VALUE hv = korb_items_data(h->items)[2 * hx + 1];
            const char *nm = SYMBOL_P(hv) ? korb_sym_name(c->vm, SYM2ID(hv))
                           : (KORB_STRING_P(hv) ? korb_strbuf_data(VAL2STR(hv)->buf) : NULL);
            if (nm && !strcmp(nm, "even")) mode = 4;
            else if (nm && !strcmp(nm, "down")) mode = 5;
            else if (nm && !strcmp(nm, "up")) mode = 3;
            else if (hv != KORB_NIL) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid rounding mode: %s",
                                                       nm ? nm : korb_type_name(hv));   /* unknown half: mode */
        }
        n--;
    }
    korb_sword_t nd = 0;
    if (n >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) nd = FIX2LONG(VALUE_SLICE_GET(a, 0));
    return korb_rat_round_digits(c, slots, SELF_RAT->num, SELF_RAT->den, nd, mode);
}
/* truncate([ndigits]) — toward zero (mode 2). ndigits<=0 → Integer, >0 → Rational. */
static RESULT korb_m_rat_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    korb_sword_t nd = 0;
    if (VALUE_SLICE_LEN(a) >= 1) {
        const VALUE p = VALUE_SLICE_GET(a, 0);
        if (FIXNUM_P(p)) nd = FIX2LONG(p);
        else if (UNLIKELY(!KORB_INTEGER_P(p)))       /* non-Integer precision → TypeError (no #to_int coercion) */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "not an integer");   /* Bignum precision keeps nd=0 (untested) */
    }
    return korb_rat_round_digits(c, slots, SELF_RAT->num, SELF_RAT->den, nd, 2);
}
static double korb_cospi(double x);   /* fwd (builtins/int_float_ext.c) */
static double korb_sinpi(double x);
RESULT korb_cpx_new(CTX *c, VALUE *slots, VALUE re, VALUE im);   /* fwd (defined below) */
static bool korb_obj_is_numeric(CTX *c, VALUE v);   /* fwd (builtins/time.c) */
/* Rational ** exp: Integer exp -> exact Rational; Float/Rational exp -> Float
 * (negative base + fractional exp -> Complex, as for Integer and Float power). */
static RESULT korb_m_rat_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const VALUE e = VALUE_SLICE_GET(a, 0);
    slots[0] = SELF_RAT->num; slots[1] = SELF_RAT->den;     /* root */
    if (KORB_INTEGER_P(e)) {                                /* exact Rational */
        korb_mp_t en; korb_to_mpz(e, en);
        if (!korb_mp_fits_slong_p(en)) {   /* exponent doesn't fit a long: only 0 and ±1 bases are representable */
            const bool exp_neg = korb_mp_sgn(en) < 0, exp_odd = korb_mp_odd_p(en) != 0;
            korb_mp_clear(en);
            korb_mp_t zn, zd; korb_to_mpz(slots[0], zn); korb_to_mpz(slots[1], zd);
            const int nsgn = korb_mp_sgn(zn);
            if (nsgn == 0) {                                /* base 0: 0**(-n) diverges, 0**n = 0 */
                korb_mp_clear(zn); korb_mp_clear(zd);
                if (exp_neg) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
                return korb_rat_new_v(c, slots, LONG2FIX(0), LONG2FIX(1));
            }
            korb_mp_t az; korb_mp_init(az); korb_mp_abs(az, zn);
            const bool base_is_one = korb_mp_cmp(az, zd) == 0;  /* |num| == den → |base| == 1 */
            korb_mp_clear(az); korb_mp_clear(zn); korb_mp_clear(zd);
            if (base_is_one) return korb_rat_new_v(c, slots, LONG2FIX((nsgn > 0 || !exp_odd) ? 1 : -1), LONG2FIX(1));
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "exponent is too large");
        }
        long n = korb_mp_get_si(en); korb_mp_clear(en);
        if (n == 0) return korb_rat_new_v(c, slots, LONG2FIX(1), LONG2FIX(1));
        korb_mp_t zn, zd, rn, rd; korb_to_mpz(slots[0], zn); korb_to_mpz(slots[1], zd); korb_mp_init(rn); korb_mp_init(rd);
        const unsigned long an = (unsigned long)(n < 0 ? -n : n);
        korb_mp_pow_ui(rn, zn, an); korb_mp_pow_ui(rd, zd, an);
        if (n < 0) { korb_mp_swap(rn, rd); if (korb_mp_sgn(rd) < 0) { korb_mp_neg(rn, rn); korb_mp_neg(rd, rd); } }
        slots[2] = UNWRAP(korb_big_from_mpz(c, slots + 2, rn));
        slots[3] = UNWRAP(korb_big_from_mpz(c, slots + 3, rd));
        korb_mp_clear(zn); korb_mp_clear(zd); korb_mp_clear(rn); korb_mp_clear(rd);
        return korb_rat_new_v(c, slots + 4, slots[2], slots[3]);
    }
    {   /* fractional / float exponent → Float (negative base + frac exp = NaN here; Complex out of scope) */
        korb_mp_t zn, zd; korb_to_mpz(slots[0], zn); korb_to_mpz(slots[1], zd);
        const double base = korb_mp_get_d(zn) / korb_mp_get_d(zd); korb_mp_clear(zn); korb_mp_clear(zd);
        double ex;
        if (KORB_FLOAT_P(e)) ex = korb_float_val(e);
        else if (KORB_RATIONAL_P(e)) { korb_mp_t a2, b2; korb_to_mpz(VAL2RAT(e)->num, a2); korb_to_mpz(VAL2RAT(e)->den, b2); ex = korb_mp_get_d(a2) / korb_mp_get_d(b2); korb_mp_clear(a2); korb_mp_clear(b2); }
        else if (KORB_OBJECT_P(e)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), e, "**", 0, &h); if (h) return cr; return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Rational", korb_type_name(e)); }
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Rational", korb_type_name(e));
        /* 0 ** negative: an exact (Rational) exponent diverges → ZeroDivisionError;
         * a Float exponent stays in the float domain → pow(0.0, neg) = Infinity. */
        if (base == 0.0 && ex < 0 && KORB_RATIONAL_P(e)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        if (base < 0 && ex != floor(ex)) {                 /* negative base, fractional exp → Complex */
            const double mag = pow(-base, ex);
            slots[2] = UNWRAP(korb_float_new(c, slots + 2, mag * korb_cospi(ex)));
            slots[3] = UNWRAP(korb_float_new(c, slots + 3, mag * korb_sinpi(ex)));
            return korb_cpx_new(c, slots + 4, slots[2], slots[3]);
        }
        return korb_flo(c, slots, pow(base, ex));
    }
}
static RESULT korb_m_rat_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = SELF_RAT->num; slots[1] = SELF_RAT->den;   /* root across arith */
    if (korb_int_cmp(slots[0], LONG2FIX(0)) >= 0) return korb_rat_new_v(c, slots + 2, slots[0], slots[1]);
    slots[2] = UNWRAP(korb_int_arith(c, slots + 2, LONG2FIX(0), slots[0], 1, 0));   /* 0 - num */
    return korb_rat_new_v(c, slots + 3, slots[2], slots[1]);
}
static RESULT korb_m_rat_neg(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; slots[0] = SELF_RAT->num; slots[1] = SELF_RAT->den;
    if (FIXNUM_P(slots[0])) return korb_rat_new_v(c, slots + 2, LONG2FIX(-FIX2LONG(slots[0])), slots[1]);
    slots[2] = UNWRAP(korb_int_arith(c, slots + 2, LONG2FIX(0), slots[0], 1, 0));   /* 0 - num */
    return korb_rat_new_v(c, slots + 3, slots[2], slots[1]);
}
static RESULT korb_m_rat_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_rat_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 1); }
static RESULT korb_m_rat_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 2); }
static RESULT korb_m_rat_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_rat_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 3); }
static RESULT korb_m_rat_cmp_m(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE o = VALUE_SLICE_GET(a, 0);
    int r = korb_rat_cmp(VALUE_REF_GET(self), o);
    if (r == 2) {                                          /* coercible object → a, b = o.coerce(self); a <=> b */
        if (KORB_OBJECT_P(o)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), o, "<=>", 0, &h); if (h) return cr; }
        return RESULT_OK(KORB_NIL);
    }
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_rat_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; int r = korb_rat_cmp(VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); return RESULT_OK(r == 0 ? KORB_TRUE : KORB_FALSE); }

static RESULT korb_num_binop(CTX *c, VALUE *slots, VALUE l, VALUE r, int op);

/* Complex re + im*i; re/im are numeric VALUEs (GC edges). */
RESULT
korb_cpx_new(CTX *c, VALUE *slots, VALUE re, VALUE im)
{
    slots[0] = re; slots[1] = im;                      /* root both across alloc */
    KorbComplex *x = korb_alloc(c, slots + 2, sizeof(KorbComplex), KORB_OBJ_COMPLEX);
    ARO_STORE(c, x, (VALUE *)(uintptr_t)&x->re, slots[0]);
    ARO_STORE(c, x, (VALUE *)(uintptr_t)&x->im, slots[1]);
    return RESULT_OK((VALUE)x);
}
static RESULT korb_m_cpx_real(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_CPX->re); }
static RESULT korb_m_cpx_imag(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_CPX->im); }
static RESULT korb_m_cpx_rect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = SELF_CPX->re;
    slots[1] = SELF_CPX->im;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF arr = VALUE_REF_AT(slots + 2);
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_m_cpx_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_cpx_conj(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {  /* conjugate: negate im */
    (void)a;
    slots[0] = SELF_CPX->re;
    RESULT nr = korb_num_binop(c, slots + 1, LONG2FIX(0), SELF_CPX->im, 1);   /* 0 - im */
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    slots[1] = nr.value;
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_cpx_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double re, im;
    if (!korb_num_to_d(SELF_CPX->re, &re) || !korb_num_to_d(SELF_CPX->im, &im))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Complex#abs with non-real components");
    if (re == 0.0 && im == 0.0) return RESULT_OK(LONG2FIX(0));   /* Complex(0,0).abs → Integer 0 (CRuby) */
    return korb_float_new(c, slots, sqrt(re * re + im * im));
}
/* Complex#arg / angle / phase → atan2(imaginary, real) — the polar angle. */
static RESULT korb_m_cpx_arg(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double re, im;
    if (!korb_num_to_d(SELF_CPX->re, &re) || !korb_num_to_d(SELF_CPX->im, &im))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Complex#arg with non-real components");
    return korb_float_new(c, slots, atan2(im, re));
}
/* Complex#polar (instance) → [abs, arg] (magnitude 0 at the origin is Integer 0, matching #abs). */
static RESULT korb_m_cpx_to_polar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double re, im;
    if (!korb_num_to_d(SELF_CPX->re, &re) || !korb_num_to_d(SELF_CPX->im, &im))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Complex#polar with non-real components");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    slots[1] = (re == 0.0 && im == 0.0) ? LONG2FIX(0) : UNWRAP(korb_float_new(c, slots + 1, sqrt(re * re + im * im)));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, atan2(im, re)));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}
static int korb_cmp_full(CTX *c, VALUE a, VALUE b);   /* fwd (defined below) */
/* Complex#<=>: comparable only when both are real (imaginary part 0) → compare
 * the real parts; otherwise nil (CRuby). */
static RESULT korb_m_cpx_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    const KorbComplex *x = SELF_CPX;
    double sim;
    if (!(korb_num_to_d(x->im, &sim) && sim == 0.0)) return RESULT_OK(KORB_NIL);
    const VALUE o = VALUE_SLICE_GET(a, 0);
    VALUE ore;
    if (KORB_COMPLEX_P(o)) {
        double oim;
        if (!(korb_num_to_d(VAL2CPX(o)->im, &oim) && oim == 0.0)) return RESULT_OK(KORB_NIL);
        ore = VAL2CPX(o)->re;
    } else if (FIXNUM_P(o) || KORB_FLOAT_P(o) || KORB_RATIONAL_P(o) || KORB_BIGNUM_P(o)) {
        ore = o;
    } else return RESULT_OK(KORB_NIL);
    const int r = korb_cmp_full(c, x->re, ore);
    return RESULT_OK(r == 2 ? KORB_NIL : LONG2FIX(r));
}
static RESULT korb_m_cpx_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* re² + im² (exact when components are) */
    (void)a;
    slots[0] = SELF_CPX->re; slots[1] = SELF_CPX->im;
    RESULT r2 = korb_num_binop(c, slots + 2, slots[0], slots[0], 2); if (UNLIKELY(r2.state != KORB_NORMAL)) return r2; slots[2] = r2.value;
    RESULT i2 = korb_num_binop(c, slots + 3, slots[1], slots[1], 2); if (UNLIKELY(i2.state != KORB_NORMAL)) return i2; slots[3] = i2.value;
    return korb_num_binop(c, slots + 4, slots[2], slots[3], 0);
}
static RESULT korb_m_cpx_infinite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* 1 if a component is ±Inf, else nil */
    (void)slots;(void)a; double re, im;
    bool inf = (korb_num_to_d(SELF_CPX->re, &re) && isinf(re)) || (korb_num_to_d(SELF_CPX->im, &im) && isinf(im));
    return RESULT_OK(inf ? LONG2FIX(1) : KORB_NIL);
}
/* num/den of a Complex component (Integer→(v,1), Rational→(num,den)); Fixnum-only. */
static void korb_cpx_nd(VALUE v, korb_sword_t *num, korb_sword_t *den) {
    if (KORB_RATIONAL_P(v) && FIXNUM_P(VAL2RAT(v)->num) && FIXNUM_P(VAL2RAT(v)->den)) {
        *num = FIX2LONG(VAL2RAT(v)->num); *den = FIX2LONG(VAL2RAT(v)->den);
    } else if (FIXNUM_P(v)) { *num = FIX2LONG(v); *den = 1; }
    else { *num = 0; *den = 1; }
}
static korb_sword_t korb_igcd(korb_sword_t a, korb_sword_t b) { a = a < 0 ? -a : a; b = b < 0 ? -b : b; while (b) { korb_sword_t t = a % b; a = b; b = t; } return a ? a : 1; }
static RESULT korb_m_cpx_denominator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* lcm of component denominators */
    (void)c;(void)slots;(void)a;
    korb_sword_t rn, rd, in, id; korb_cpx_nd(SELF_CPX->re, &rn, &rd); korb_cpx_nd(SELF_CPX->im, &in, &id);
    return RESULT_OK(LONG2FIX(rd / korb_igcd(rd, id) * id));
}
static RESULT korb_m_cpx_numerator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* Complex(re*d/rd, im*d/id) where d=lcm */
    (void)a;
    korb_sword_t rn, rd, in, id; korb_cpx_nd(SELF_CPX->re, &rn, &rd); korb_cpx_nd(SELF_CPX->im, &in, &id);
    const korb_sword_t d = rd / korb_igcd(rd, id) * id;
    slots[0] = LONG2FIX(rn * (d / rd)); slots[1] = LONG2FIX(in * (d / id));
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_cpx_to_rat_via(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, const char *meth, uint32_t mlen, bool strict) {   /* re.<meth> when imaginary is 0 */
    double im;
    /* strict (rationalize) rejects a Float imaginary part even when it is 0.0
     * (an inexact zero is not an exact zero); lenient (to_r) accepts 0.0. */
    if (UNLIKELY(!(korb_num_to_d(SELF_CPX->im, &im) && im == 0.0) || (strict && KORB_FLOAT_P(SELF_CPX->im))))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "can't convert %s into Rational", korb_type_name(VALUE_REF_GET(self)));
    const uint32_t argc = VALUE_SLICE_LEN(a);
    slots[0] = SELF_CPX->re;                                 /* recv below the staged args */
    for (uint32_t j = 0; j < argc; j++) slots[1 + j] = VALUE_SLICE_GET(a, j);
    return korb_send(c, slots + 1 + argc, korb_intern(c->vm, meth, mlen), 0, argc);
}
static RESULT korb_m_cpx_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cpx_to_rat_via(c, slots, self, a, "rationalize", 11, true); }
static RESULT korb_m_cpx_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cpx_to_rat_via(c, slots, self, a, "to_r", 4, false); }
static RESULT korb_m_cpx_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    const KorbComplex *x = SELF_CPX;
    if (KORB_COMPLEX_P(o))
        return RESULT_OK((korb_value_eq(x->re, VAL2CPX(o)->re) && korb_value_eq(x->im, VAL2CPX(o)->im)) ? KORB_TRUE : KORB_FALSE);
    /* complex == real iff im == 0 and re == real */
    double im;
    bool im_zero = korb_num_to_d(x->im, &im) && im == 0.0;
    return RESULT_OK((im_zero && korb_value_eq(x->re, o)) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_cpx_new(c, slots, LONG2FIX(0), VALUE_REF_GET(self)); }
static RESULT korb_m_num_conj_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static bool korb_cpx_parts(VALUE v, VALUE *re, VALUE *im) {
    if (KORB_COMPLEX_P(v)) { *re = VAL2CPX(v)->re; *im = VAL2CPX(v)->im; return true; }
    if (FIXNUM_P(v) || KORB_FLOAT_P(v) || KORB_RATIONAL_P(v) || KORB_BIGNUM_P(v)) { *re = v; *im = LONG2FIX(0); return true; }
    return false;
}
/* Complex division produces exact components, and CRuby reports a whole one as
 * an Integer (`Complex(4,2)/2` → `(2+1i)`) — note a directly built
 * `Complex(Rational(2,1), 0)` keeps its Rational, so this is a property of the
 * division result, not of Complex construction. */
static VALUE korb_cpx_whole(VALUE v) {
    return (KORB_RATIONAL_P(v) && VAL2RAT(v)->den == LONG2FIX(1)) ? VAL2RAT(v)->num : v;
}
/* Complex arithmetic (op 0+ 1- 2* 3/); returns a Complex. Components combined
 * via korb_num_binop (Int/Float/Rational-aware); division is exact. */
RESULT korb_cpx_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op) {
    VALUE lre, lim, rre, rim;
    if (UNLIKELY(op == 4))                                /* Complex defines no #% (CRuby) */
        return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method '%%' for %s",
                          korb_a_type_name(KORB_COMPLEX_P(l) ? l : r));
    if (UNLIKELY(!korb_cpx_parts(l, &lre, &lim) || !korb_cpx_parts(r, &rre, &rim))) {
        if (KORB_COMPLEX_P(l) && KORB_OBJECT_P(r) && op >= 0 && op <= 3) {
            static const char *const opn[] = { "+", "-", "*", "/" };
            /* A REAL Numeric applies per component (CRuby's f_add(dat->real, other)):
             * the component send then runs the coerce protocol itself. */
            const uint32_t real_p = korb_intern(c->vm, "real?", 5);
            if (korb_obj_is_numeric(c, r) && korb_responds_to(c, r, real_p)) {
                slots[0] = l; slots[1] = r;                 /* root across the dispatches */
                RESULT rp = korb_send(c, slots + 2, real_p, 0, 0);
                if (UNLIKELY(rp.state != KORB_NORMAL)) return rp;
                if (KORB_TRUTHY(rp.value)) {
                    const uint32_t mid = (op == 3) ? korb_intern(c->vm, "quo", 3)
                                                   : korb_intern(c->vm, opn[op], 1);
                    VALUE lre2, lim2;
                    (void)korb_cpx_parts(slots[0], &lre2, &lim2);
                    slots[2] = lre2; slots[3] = slots[1];
                    RESULT re = korb_send(c, slots + 4, mid, 0, 1);   /* real op other */
                    if (UNLIKELY(re.state != KORB_NORMAL)) return re;
                    slots[2] = re.value;                    /* park the new real part */
                    if (op == 0 || op == 1) {               /* +/- leave the imaginary part alone */
                        (void)korb_cpx_parts(slots[0], &lre2, &lim2);
                        slots[3] = lim2;
                    } else {                                /* * and / scale it too */
                        (void)korb_cpx_parts(slots[0], &lre2, &lim2);
                        slots[3] = lim2; slots[4] = slots[1];
                        RESULT im = korb_send(c, slots + 5, mid, 0, 1);
                        if (UNLIKELY(im.state != KORB_NORMAL)) return im;
                        slots[3] = im.value;
                    }
                    return korb_cpx_new(c, slots + 4, slots[2], slots[3]);
                }
            }
            bool h; RESULT cr = korb_try_coerce(c, slots, l, r, opn[op], 0, &h);
            if (h) return cr;
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Complex", korb_type_name(KORB_COMPLEX_P(l) ? r : l));
    }
    slots[0] = lre; slots[1] = lim; slots[2] = rre; slots[3] = rim;   /* root inputs */
    VALUE res_re, res_im;
    if (op == 0 || op == 1) {
        RESULT a = korb_num_binop(c, slots + 4, slots[0], slots[2], op); if (UNLIKELY(a.state != KORB_NORMAL)) return a; slots[4] = a.value;
        RESULT b = korb_num_binop(c, slots + 5, slots[1], slots[3], op); if (UNLIKELY(b.state != KORB_NORMAL)) return b; slots[5] = b.value;
        res_re = slots[4]; res_im = slots[5];
    } else if (op == 2) {   /* mul: (lre*rre - lim*rim) + (lre*rim + lim*rre)i */
        RESULT m1 = korb_num_binop(c, slots + 4, slots[0], slots[2], 2); if (UNLIKELY(m1.state != KORB_NORMAL)) return m1; slots[4] = m1.value;
        RESULT m2 = korb_num_binop(c, slots + 5, slots[1], slots[3], 2); if (UNLIKELY(m2.state != KORB_NORMAL)) return m2; slots[5] = m2.value;
        RESULT re = korb_num_binop(c, slots + 6, slots[4], slots[5], 1); if (UNLIKELY(re.state != KORB_NORMAL)) return re; slots[6] = re.value;
        RESULT m3 = korb_num_binop(c, slots + 7, slots[0], slots[3], 2); if (UNLIKELY(m3.state != KORB_NORMAL)) return m3; slots[7] = m3.value;
        RESULT m4 = korb_num_binop(c, slots + 8, slots[1], slots[2], 2); if (UNLIKELY(m4.state != KORB_NORMAL)) return m4; slots[8] = m4.value;
        RESULT im = korb_num_binop(c, slots + 9, slots[7], slots[8], 0); if (UNLIKELY(im.state != KORB_NORMAL)) return im; slots[9] = im.value;
        res_re = slots[6]; res_im = slots[9];
    } else if (!KORB_COMPLEX_P(r)) {   /* div by a real: exact (#quo) per component, not Integer floor division */
        RESULT re = korb_rat_arith(c, slots + 4, slots[0], slots[2], 3); if (UNLIKELY(re.state != KORB_NORMAL)) return re; slots[4] = korb_cpx_whole(re.value);
        RESULT im = korb_rat_arith(c, slots + 5, slots[1], slots[2], 3); if (UNLIKELY(im.state != KORB_NORMAL)) return im; slots[5] = korb_cpx_whole(im.value);
        res_re = slots[4]; res_im = slots[5];
    } else {   /* div by complex: ((lre*rre+lim*rim) + (lim*rre-lre*rim)i) / (rre²+rim²) */
        RESULT c2 = korb_num_binop(c, slots + 4, slots[2], slots[2], 2); if (UNLIKELY(c2.state != KORB_NORMAL)) return c2; slots[4] = c2.value;
        RESULT d2 = korb_num_binop(c, slots + 5, slots[3], slots[3], 2); if (UNLIKELY(d2.state != KORB_NORMAL)) return d2; slots[5] = d2.value;
        RESULT dn = korb_num_binop(c, slots + 6, slots[4], slots[5], 0); if (UNLIKELY(dn.state != KORB_NORMAL)) return dn; slots[6] = dn.value;  /* denom */
        RESULT ac = korb_num_binop(c, slots + 7, slots[0], slots[2], 2); if (UNLIKELY(ac.state != KORB_NORMAL)) return ac; slots[7] = ac.value;
        RESULT bd = korb_num_binop(c, slots + 8, slots[1], slots[3], 2); if (UNLIKELY(bd.state != KORB_NORMAL)) return bd; slots[8] = bd.value;
        RESULT rn = korb_num_binop(c, slots + 9, slots[7], slots[8], 0); if (UNLIKELY(rn.state != KORB_NORMAL)) return rn; slots[9] = rn.value;   /* re numerator */
        RESULT bc = korb_num_binop(c, slots + 10, slots[1], slots[2], 2); if (UNLIKELY(bc.state != KORB_NORMAL)) return bc; slots[10] = bc.value;
        RESULT ad = korb_num_binop(c, slots + 11, slots[0], slots[3], 2); if (UNLIKELY(ad.state != KORB_NORMAL)) return ad; slots[11] = ad.value;
        RESULT in = korb_num_binop(c, slots + 12, slots[10], slots[11], 1); if (UNLIKELY(in.state != KORB_NORMAL)) return in; slots[12] = in.value;  /* im numerator */
        RESULT re = korb_rat_arith(c, slots + 13, slots[9], slots[6], 3); if (UNLIKELY(re.state != KORB_NORMAL)) return re; slots[13] = korb_cpx_whole(re.value);
        RESULT im = korb_rat_arith(c, slots + 14, slots[12], slots[6], 3); if (UNLIKELY(im.state != KORB_NORMAL)) return im; slots[14] = korb_cpx_whole(im.value);
        res_re = slots[13]; res_im = slots[14];
    }
    slots[15] = res_re; slots[16] = res_im;
    return korb_cpx_new(c, slots + 17, slots[15], slots[16]);
}
static bool korb_value_eql(VALUE a, VALUE b);   /* fwd (defined below) */
/* Complex#eql? — both Complex and components eql? (type-strict: 1 ≠ 1.0). */
static RESULT korb_m_cpx_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_COMPLEX_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbComplex *x = SELF_CPX, *y = VAL2CPX(o);
    return RESULT_OK((korb_value_eql(x->re, y->re) && korb_value_eql(x->im, y->im)) ? KORB_TRUE : KORB_FALSE);
}
/* Complex#** — exact repeated squaring for an Integer exponent (negative →
 * reciprocal); otherwise the polar formula z^w = exp(w·ln z) (Float result). */
static RESULT korb_m_cpx_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE ev = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(ev)) {
        korb_sword_t n = FIX2LONG(ev);
        const bool neg = n < 0; uintptr_t k = neg ? (uintptr_t)(-n) : (uintptr_t)n;
        slots[0] = UNWRAP(korb_cpx_new(c, slots, LONG2FIX(1), LONG2FIX(0)));   /* result = 1+0i */
        slots[1] = VALUE_REF_GET(self);                                        /* base */
        while (k) {
            if (k & 1u) { RESULT r = korb_cpx_arith(c, slots + 2, slots[0], slots[1], 2); if (UNLIKELY(r.state != KORB_NORMAL)) return r; slots[0] = r.value; }
            k >>= 1;
            if (k)      { RESULT r = korb_cpx_arith(c, slots + 2, slots[1], slots[1], 2); if (UNLIKELY(r.state != KORB_NORMAL)) return r; slots[1] = r.value; }
        }
        if (!neg) return RESULT_OK(slots[0]);
        slots[1] = UNWRAP(korb_cpx_new(c, slots + 1, LONG2FIX(1), LONG2FIX(0)));   /* 1 / result */
        return korb_cpx_arith(c, slots + 2, slots[1], slots[0], 3);
    }
    /* float / complex exponent → polar */
    double zre, zim, wre = 0, wim = 0;
    if (!korb_num_to_d(SELF_CPX->re, &zre) || !korb_num_to_d(SELF_CPX->im, &zim))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "Complex#** with non-numeric components");
    if (KORB_COMPLEX_P(ev)) { korb_num_to_d(VAL2CPX(ev)->re, &wre); korb_num_to_d(VAL2CPX(ev)->im, &wim); }
    else if (!korb_num_to_d(ev, &wre)) {
        if (KORB_OBJECT_P(ev)) {                          /* a, b = ev.coerce(self); a ** b */
            bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ev, "**", 0, &h);
            if (h) return cr;
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Complex", korb_type_name(ev));
    }
    const double r = hypot(zre, zim), th = atan2(zim, zre), lnr = log(r);
    const double p = wre * lnr - wim * th, q = wre * th + wim * lnr, ep = exp(p);
    slots[0] = UNWRAP(korb_float_new(c, slots, ep * cos(q)));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, ep * sin(q)));
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
/* Coerce a Complex.polar argument to a double: a real, or a Complex whose
 * imaginary part is zero (CRuby treats (x+0i) as the real x here). */
static bool korb_polar_real_d(VALUE v, double *out) {
    if (korb_num_to_d(v, out)) return true;
    if (KORB_COMPLEX_P(v)) {
        double im; const KorbComplex *const cx = VAL2CPX(v);
        if (korb_num_to_d(cx->im, &im) && im == 0.0 && korb_num_to_d(cx->re, out)) return true;
    }
    return false;
}
/* Complex.polar(abs, arg=0) → abs·(cos arg + i·sin arg) (Float components). */
static RESULT korb_m_cpx_polar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    double ab, ar = 0;
    if (UNLIKELY(!korb_polar_real_d(VALUE_SLICE_GET(a, 0), &ab))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    if (VALUE_SLICE_LEN(a) < 2) {                                    /* no angle → real part keeps its type, imag 0.0 */
        const VALUE a0 = VALUE_SLICE_GET(a, 0);
        slots[0] = KORB_COMPLEX_P(a0) ? VAL2CPX(a0)->re : a0;        /* (x+0i) → x */
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, 0.0));
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    if (UNLIKELY(!korb_polar_real_d(VALUE_SLICE_GET(a, 1), &ar))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    if (ar == 0.0 && !KORB_COMPLEX_P(VALUE_SLICE_GET(a, 0))) {       /* angle 0 → real keeps the magnitude's type, imag 0.0 */
        slots[0] = VALUE_SLICE_GET(a, 0);
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, 0.0));
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    slots[0] = UNWRAP(korb_float_new(c, slots, ab * cos(ar)));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, ab * sin(ar)));
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
/* Complex.rect(real[, imag]) / .rectangular — build a Complex, preserving the
 * components' types.  Both must be real (non-real → TypeError). */
static RESULT korb_m_cpx_class_rect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    slots[0] = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : LONG2FIX(0);   /* re/im rooted across #real? dispatch */
    slots[1] = VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : LONG2FIX(0);
    const uint32_t realq = korb_intern(c->vm, "real?", 5);
    double tmp;
    /* accept a built-in real, a Complex with zero imaginary (→ its real part), or
     * any object whose #real? answers truthy — matches CRuby. */
    for (int k = 0; k < 2; k++) {
        const VALUE v = slots[k];
        if (korb_num_to_d(v, &tmp)) continue;
        if (KORB_COMPLEX_P(v)) {
            double vi;
            if (korb_num_to_d(VAL2CPX(v)->im, &vi) && vi == 0.0) { slots[k] = VAL2CPX(v)->re; continue; }
            return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
        }
        if (KORB_OBJECT_P(v) && korb_responds_to(c, v, realq)) {
            slots[2] = v;
            RESULT rr = korb_send_impl(c, slots + 3, realq, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
            if (KORB_TRUTHY(rr.value)) continue;   /* real? truthy → accept (slots[k] rooted/fresh) */
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    }
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_cpx_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_cpx_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 1); }
static RESULT korb_m_cpx_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 2); }
static RESULT korb_m_cpx_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 3); }
/* Complex#fdiv(n) → Complex(re.fdiv(n), im.fdiv(n)) with Float components. */
static RESULT korb_m_cpx_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double n, re, im;
    if (!korb_num_to_d(VALUE_SLICE_GET(a, 0), &n))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Complex", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (!korb_num_to_d(SELF_CPX->re, &re) || !korb_num_to_d(SELF_CPX->im, &im))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Complex#fdiv with non-real components");
    slots[0] = UNWRAP(korb_float_new(c, slots, re / n));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, im / n));
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}

bool
korb_num_to_d(VALUE v, double *out)
{
    if (FIXNUM_P(v))     { *out = (double)FIX2LONG(v); return true; }
    if (KORB_FLOAT_P(v)) { *out = korb_float_val(v);     return true; }
    if (KORB_RATIONAL_P(v)) { double n, d; korb_num_to_d(VAL2RAT(v)->num, &n); korb_num_to_d(VAL2RAT(v)->den, &d); *out = n / d; return true; }
    if (KORB_BIGNUM_P(v)) { *out = korb_big_to_d(v); return true; }
    return false;
}

/* Index coercion: Integer as-is, Float truncated via to_int (CRuby Array#[] etc). */
static inline bool
korb_to_index(VALUE v, korb_sword_t *out)
{
    if (FIXNUM_P(v))     { *out = FIX2LONG(v);          return true; }
    if (KORB_FLOAT_P(v)) {
        const double d = korb_float_val(v);             /* out of long range → not an index */
        if (isnan(d) || d >= 9.223372036854776e18 || d <= -9.223372036854776e18) return false;
        *out = (korb_sword_t)d; return true;
    }
    return false;
}

/* CRuby-style Float#to_s: shortest round-tripping decimal, always with a '.'
 * or exponent.  buf must be >= 32 bytes; returns the length. */
static uint32_t
korb_float_to_s(double d, char *buf)
{
    if (isnan(d)) { memcpy(buf, "NaN", 4); return 3; }
    if (isinf(d)) {
        if (d < 0) { memcpy(buf, "-Infinity", 10); return 9; }
        memcpy(buf, "Infinity", 9); return 8;
    }
    /* shortest #significant-digits that round-trips (via scientific form) */
    char tmp[48];
    int sig = 1;
    for (; sig < 17; sig++) {
        snprintf(tmp, sizeof tmp, "%.*e", sig - 1, d);
        if (strtod(tmp, NULL) == d) break;
    }
    int exp10 = atoi(strchr(tmp, 'e') + 1);
    if (exp10 >= -4 && exp10 < 15) {                 /* fixed notation (CRuby range: -4..14) */
        int frac = sig - 1 - exp10;
        if (frac < 1) frac = 1;                      /* Ruby always shows ≥1 fractional digit */
        snprintf(buf, 32, "%.*f", frac, d);
    } else {                                         /* scientific: d.dddde±XX */
        snprintf(buf, 32, "%.*e", sig - 1, d);
        char *e = strchr(buf, 'e');
        if (e && !memchr(buf, '.', (size_t)(e - buf))) {   /* "1e+20" → "1.0e+20" */
            char t2[48]; size_t ml = (size_t)(e - buf);
            memcpy(t2, buf, ml); memcpy(t2 + ml, ".0", 2); strcpy(t2 + ml + 2, e);
            strcpy(buf, t2);
        }
    }
    return (uint32_t)strlen(buf);
}

RESULT korb_try_coerce(CTX *c, VALUE *slots, VALUE l, VALUE rhs, const char *op, uint32_t line, bool *handled);   /* fwd; decl in node.h for node_eval.c */
/* numeric arithmetic with at least one Float operand.  op: 0+ 1- 2* 3/ 4% */
RESULT
korb_num_arith(CTX *c, VALUE *slots, VALUE l, VALUE rhs, int op, uint32_t line)
{
    static const char *const opn[] = { "+", "-", "*", "/", "%" };
    double a = 0.0, b = 0.0;
    if (UNLIKELY(!korb_num_to_d(l, &b)))     /* l not numeric → method missing on l */
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '%s' for %s", opn[op], korb_a_type_name(l));
    if (UNLIKELY(!korb_num_to_d(rhs, &b))) {  /* rhs not numeric → coerce protocol, else coercion error */
        bool h; RESULT cr = korb_try_coerce(c, slots, l, rhs, opn[op], line, &h);
        if (h) return cr;
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Float", korb_type_name(rhs));
    }
    (void)korb_num_to_d(l, &a);
    double r;
    switch (op) {
      case 0: r = a + b; break;
      case 1: r = a - b; break;
      case 2: r = a * b; break;
      case 3: r = a / b; break;                 /* float div: Inf on /0 */
      default: r = fmod(a, b); if (r != 0.0 && ((r < 0) != (b < 0))) r += b; break;
    }
    return korb_float_new(c, slots, r);
}

/* CRuby names every unbound required keyword at once: "missing keywords: :a, :b". */
static RESULT
korb_raise_missing_kw(CTX *c, VALUE *slots, uint32_t line, const struct korb_kw_info *kw, uint64_t present)
{
    char buf[512];
    uint32_t n = 0;
    for (uint32_t j = 0; j < kw->count; j++) {
        if ((j < 64 && (present & (1ull << j))) || kw->entries[j].deflt) continue;
        n++;
    }
    int off = snprintf(buf, sizeof buf, "missing keyword%s:", n > 1 ? "s" : "");
    bool first = true;
    for (uint32_t j = 0; j < kw->count && off > 0 && (size_t)off < sizeof buf; j++) {
        if ((j < 64 && (present & (1ull << j))) || kw->entries[j].deflt) continue;
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%s :%s", first ? "" : ",",
                        korb_sym_name(c->vm, kw->entries[j].mid));
        first = false;
    }
    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "%s", buf);
}

/* ---- string encoding negotiation ---------------------------------------- */

/* the canonical name of a header encoding index */
const char *
korb_enc_name_of(const struct korb_vm *vm, uint32_t idx)
{
    if (idx == KORB_ENC_UTF8) return "UTF-8";
    if (idx == KORB_ENC_USASCII) return "US-ASCII";
    if (idx == KORB_ENC_BINARY) return "ASCII-8BIT";
    return (idx < KORB_STR_ENC_MAX && vm->str_enc_names[idx]) ? korb_sym_name(vm, vm->str_enc_names[idx]) : "unknown";
}
/* the UTF-16/32, UTF-7 and stateful families are not ASCII-compatible */
bool
korb_enc_ascii_compat_idx(const struct korb_vm *vm, uint32_t idx)
{
    if (idx < KORB_ENC_OTHER_MIN || idx >= KORB_STR_ENC_MAX || vm->str_enc_names[idx] == 0) return true;
    const char *const nm = korb_sym_name(vm, vm->str_enc_names[idx]);
    return !(strncasecmp(nm, "UTF-16", 6) == 0 || strncasecmp(nm, "UTF-32", 6) == 0 ||
             strcasecmp(nm, "UTF-7") == 0 || strncasecmp(nm, "ISO-2022", 8) == 0 ||
             strncasecmp(nm, "CP502", 5) == 0);
}
/* "ASCII-only" the way CRuby means it: 7-bit *and* in an ASCII-compatible
 * encoding (a UTF-16 string of ASCII bytes is not ASCII-only). */
static bool korb_str_ascii_only_p(const struct korb_vm *vm, VALUE sv) {
    if (!korb_enc_ascii_compat_idx(vm, KORB_STR_ENC(sv))) return false;
    const KorbString *const s = VAL2STR(sv);
    for (uint32_t i = 0; i < s->len; i++)
        if ((unsigned char)korb_strbuf_data(s->buf)[i] >= 0x80) return false;
    return true;
}
/* The encoding two Strings can share (CRuby's rb_enc_compatible): same one, or
 * the side that is not plain 7-bit ASCII; false when there is none. */
bool
korb_str_enc_combine(const struct korb_vm *vm, VALUE av, VALUE bv, uint32_t *out)
{
    const uint32_t ea = KORB_STR_ENC(av), eb = KORB_STR_ENC(bv);
    if (ea == eb) { *out = ea; return true; }
    const bool ca = korb_enc_ascii_compat_idx(vm, ea), cb = korb_enc_ascii_compat_idx(vm, eb);
    const bool aa = korb_str_ascii_only_p(vm, av), ab = korb_str_ascii_only_p(vm, bv);
    if (VAL2STR(bv)->len == 0) { *out = ea; return true; }                 /* an empty side yields (CRuby) */
    if (VAL2STR(av)->len == 0) { *out = (ca && ab) ? ea : eb; return true; }   /* CRuby: enc1 only if str2 is 7-bit */
    if (!ca || !cb) return false;
    if (aa && ab) { *out = (ea == KORB_ENC_USASCII) ? eb : ea; return true; }
    if (ab) { *out = ea; return true; }
    if (aa) { *out = eb; return true; }
    return false;
}
/* Fold one more String into a running result encoding (gsub/join-style
 * accumulation): the same rules as korb_str_enc_combine, but the left side is
 * carried as (encoding, is-7-bit) so no intermediate String is needed.
 * false = the two have no common encoding. */
bool
korb_str_enc_fold_raw(const struct korb_vm *vm, uint32_t *renc, bool *rasc, uint32_t eb, bool ab)
{
    if (eb == *renc) { *rasc = *rasc && ab; return true; }
    if (!korb_enc_ascii_compat_idx(vm, *renc) || !korb_enc_ascii_compat_idx(vm, eb)) return false;
    if (*rasc && ab) { if (*renc == KORB_ENC_USASCII) *renc = eb; return true; }
    if (ab) return true;                                   /* the 7-bit side yields */
    if (*rasc) { *renc = eb; *rasc = false; return true; }
    return false;                                          /* both non-7-bit, different encodings */
}
bool
korb_str_enc_fold(const struct korb_vm *vm, uint32_t *renc, bool *rasc, VALUE bv)
{
    if (VAL2STR(bv)->len == 0) return true;                /* an empty side yields */
    return korb_str_enc_fold_raw(vm, renc, rasc, KORB_STR_ENC(bv), korb_str_ascii_only_p(vm, bv));
}
/* raise Owner::Name (a prelude-defined exception class) with a formatted msg */
RESULT
korb_raise_nested(CTX *c, VALUE *slots, const char *owner, const char *name, const char *msg)
{
    const VALUE om = korb_const_get(c->vm, korb_intern(c->vm, owner, (uint32_t)strlen(owner)));
    VALUE cls = KORB_NIL;
    if (KORB_CLASS_P(om)) {
        const uint32_t ix = korb_const_index_owned(c->vm, korb_intern(c->vm, name, (uint32_t)strlen(name)), om);
        if (ix != UINT32_MAX) cls = c->vm->const_vals[ix];
    }
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}
/* Encoding::CompatibilityError, which lives in the prelude (nested in Encoding). */
/* raise Encoding::CompatibilityError (a prelude class) with `msg` */
RESULT
korb_raise_enc_compat_msg(CTX *c, VALUE *slots, const char *msg)
{
    const VALUE encm = korb_const_get(c->vm, korb_intern(c->vm, "Encoding", 8));
    VALUE cls = KORB_NIL;
    if (KORB_CLASS_P(encm)) {
        const uint32_t ix = korb_const_index_owned(c->vm, korb_intern(c->vm, "CompatibilityError", 18), encm);
        if (ix != UINT32_MAX) cls = c->vm->const_vals[ix];
    }
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}
RESULT
korb_raise_enc_compat(CTX *c, VALUE *slots, uint32_t ea, uint32_t eb)
{
    char msg[160];
    snprintf(msg, sizeof msg, "incompatible character encodings: %s and %s",
             korb_enc_name_of(c->vm, ea), korb_enc_name_of(c->vm, eb));
    return korb_raise_enc_compat_msg(c, slots, msg);
}

/* a + b — alloc first, then copy through refs (fixup-safe; v2_design §4.3). */
static RESULT
korb_str_plus_ref(CTX *c, VALUE *slots, VALUE_REF a, VALUE_REF b)
{
    uint32_t alen = VAL2STR(VALUE_REF_GET(a))->len;
    uint32_t blen = VAL2STR(VALUE_REF_GET(b))->len;
    KorbString *s = korb_str_alloc(c, slots, alen + blen);
    const KorbString *as = VAL2STR(VALUE_REF_GET(a));   /* re-read: fixed up */
    const KorbString *bs = VAL2STR(VALUE_REF_GET(b));
    memcpy(korb_strbuf_data(s->buf), korb_strbuf_data(as->buf), alen);
    memcpy(korb_strbuf_data(s->buf) + alen, korb_strbuf_data(bs->buf), blen);
    /* result encoding: same enc → that; else the non-ASCII-only side's (US-ASCII
     * yields).  No shared encoding → Encoding::CompatibilityError. */
    uint32_t renc;
    if (!korb_str_enc_combine(c->vm, VALUE_REF_GET(a), VALUE_REF_GET(b), &renc))
        return korb_raise_enc_compat(c, slots, KORB_STR_ENC(VALUE_REF_GET(a)), KORB_STR_ENC(VALUE_REF_GET(b)));
    KORB_STR_ENC_SET((VALUE)s, renc);
    return RESULT_OK((VALUE)s);
}

static RESULT
korb_str_repeat_ref(CTX *c, VALUE *slots, VALUE_REF src, korb_sword_t cnt, uint32_t line)
{
    if (cnt < 0)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
    uint32_t len = VAL2STR(VALUE_REF_GET(src))->len;
    size_t total = (size_t)len * (size_t)cnt;
    if (total > (size_t)1 << 31)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "argument too big");
    KorbString *s = korb_str_alloc(c, slots, (uint32_t)total);
    const KorbString *ss = VAL2STR(VALUE_REF_GET(src));
    for (korb_sword_t i = 0; i < cnt; i++) {
        memcpy(korb_strbuf_data(s->buf) + (size_t)i * len, korb_strbuf_data(ss->buf), len);
    }
    return RESULT_OK((VALUE)s);
}

static uint32_t korb_fmt_int(korb_sword_t n, int base, char *buf);   /* defined below */

void korb_fprint_inspect_s(CTX *c, VALUE *slots, FILE *fp, VALUE v);   /* fwd: slots-aware inspect/to_s (container elements dispatch #inspect) */
static void korb_fprint_to_s_s(CTX *c, VALUE *slots, FILE *fp, VALUE v);
/* String interpolation step: acc (a String) + to_s(part).  String parts take
 * the direct concat path; other values render through korb_fprint_to_s_s (whose
 * container elements dispatch #inspect, which may GC — `part` is rooted). */
RESULT
korb_str_interp(CTX *c, VALUE *slots, VALUE_REF aref, VALUE part)
{
    VALUE_REF pref = SLOTS_PUSH(slots, part);            /* root part across GC */
    VALUE p = VALUE_REF_GET(pref);
    if (KORB_STRING_P(p))
        return korb_str_plus_ref(c, slots, aref, pref);
    /* Dispatch #to_s rather than using the C printer below for:
     *  - Regexp / MatchData, which the printer does not know at all (it would
     *    render them as "#<Object>") — and `/#{re}/` goes through here, so
     *    getting it wrong silently corrupts composed patterns;
     *  - Class / Module, whose #to_s a program may override (the printer would
     *    always print the real name). */
    if (KORB_OBJECT_P(p) || KORB_REGEXP_P(p) || KORB_MATCHDATA_P(p) || KORB_CLASS_P(p)) {
        RESULT tsr = korb_send(c, slots, korb_intern(c->vm, "to_s", 4), 0, 0);   /* recv = pref slot */
        if (UNLIKELY(tsr.state != KORB_NORMAL)) return tsr;
        VALUE_REF_SET(pref, tsr.value);                  /* reuse the rooted slot to hold the result */
        if (KORB_STRING_P(tsr.value))
            return korb_str_plus_ref(c, slots, aref, pref);
        p = tsr.value;                                   /* non-String to_s → render the returned value */
    }

    /* scalar fast path: format into a stack buffer, skipping the open_memstream
     * FILE+malloc that dominates interpolation of small values (#{int}, #{float}…).
     * The value is fully captured into `sb`/`src` before korb_str_new can GC. */
    {
        char sb[48];
        const char *src = NULL;
        uint32_t slen = 0;
        if (FIXNUM_P(p))          { slen = korb_fmt_int((korb_sword_t)FIX2LONG(p), 10, sb); src = sb; }
        else if (p == KORB_NIL)   { src = "";      slen = 0; }
        else if (p == KORB_TRUE)  { src = "true";  slen = 4; }
        else if (p == KORB_FALSE) { src = "false"; slen = 5; }
        else if (SYMBOL_P(p))     { src = korb_sym_name(c->vm, SYM2ID(p)); slen = (uint32_t)strlen(src); }  /* interned, GC-stable */
        else if (KORB_FLOAT_P(p)) { korb_float_to_s(korb_float_val(p), sb); slen = (uint32_t)strlen(sb); src = sb; }
        if (src) {
            const VALUE tmp = UNWRAP(korb_str_new(c, slots, src, slen));
            VALUE_REF tref = SLOTS_PUSH(slots, tmp);
            return korb_str_plus_ref(c, slots, aref, tref);
        }
    }

    char *buf = NULL;
    size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_to_s_s(c, slots, ms, p);                 /* slots is past pref/aref (SLOTS_PUSH advanced it); containers dispatch element #inspect */
    fclose(ms);
    RESULT sr = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    VALUE tmp = UNWRAP(sr);
    VALUE_REF tref = SLOTS_PUSH(slots, tmp);
    return korb_str_plus_ref(c, slots, aref, tref);
}

/* ---------------------------------------------------------------------------
 * Array — header + separately-allocated growable VALUE[] payload.
 * ------------------------------------------------------------------------- */

RESULT
korb_ary_new(CTX *c, VALUE *slots, uint32_t capa)
{
    if (capa < 4) capa = 4;
    /* items first, rooted while the header is allocated (both may GC). */
    KorbArrayItems *it = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)capa * sizeof(VALUE),
                                    KORB_OBJ_VALUE_ARRAY);
    VALUE_REF itref = SLOTS_PUSH(slots, (VALUE)it);
    KorbArray *a = korb_alloc(c, slots, sizeof(KorbArray), KORB_OBJ_ARRAY);
    it = (KorbArrayItems *)(uintptr_t)VALUE_REF_GET(itref);   /* re-read after GC */
    a->len = 0;
    a->capa = capa;
    ARO_STORE(c, a, (VALUE *)(uintptr_t)&a->items, (VALUE)(uintptr_t)it);
    return RESULT_OK((VALUE)a);
}

static RESULT
korb_ary_ensure(CTX *c, VALUE *slots, VALUE_REF aref, uint32_t need)
{
    KorbArray *a = VAL2ARY(VALUE_REF_GET(aref));
    if ((size_t)a->len + need <= a->capa) return RESULT_OK(VALUE_REF_GET(aref));
    uint32_t ncapa = a->capa ? a->capa * 2 : 4;
    while ((size_t)ncapa < (size_t)a->len + need) ncapa *= 2;
    KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * sizeof(VALUE),
                                     KORB_OBJ_VALUE_ARRAY);
    a = VAL2ARY(VALUE_REF_GET(aref));               /* re-read after GC */
    KorbArrayItems *oit = a->items;                 /* a fixed up → its items too */
    ARO_STORE_BULK(c, nit, korb_items_data(nit), korb_items_data(oit), (size_t)a->len);   /* new tail is zero-init = nil */
    ARO_STORE(c, a, (VALUE *)(uintptr_t)&a->items, (VALUE)(uintptr_t)nit);
    a->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(aref));
}

RESULT
korb_ary_push_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE elem)
{
    KorbArray *a = VAL2ARY(VALUE_REF_GET(aref));
    if (LIKELY(a->len < a->capa)) {                 /* room available: no grow, no GC → store directly (skip the ensure call + elem rooting) */
        KorbArrayItems *const it = a->items;
        ARO_STORE(c, it, &korb_items_data(it)[a->len], elem);
        a->len++;
        return RESULT_OK((VALUE)(uintptr_t)a);
    }
    VALUE_REF eref = SLOTS_PUSH(slots, elem);       /* grow path: root elem across the alloc/GC */
    CHECK(korb_ary_ensure(c, slots, aref, 1));
    a = VAL2ARY(VALUE_REF_GET(aref));
    KorbArrayItems *const it = a->items;
    ARO_STORE(c, it, &korb_items_data(it)[a->len], VALUE_REF_GET(eref));
    a->len++;
    return RESULT_OK(VALUE_REF_GET(aref));
}

/* In-place store ary[i] = val (write-barriered).  Caller guarantees i is in
 * range.  Exported for node_aset's fast path (node_eval.c lacks the GC macros). */
void
korb_ary_store_at(CTX *c, VALUE ary, uint32_t i, VALUE val)
{
    KorbArray *const a = VAL2ARY(ary);
    ARO_STORE(c, a->items, &korb_items_data(a->items)[i], val);
}

static bool korb_range_int_bounds(const KorbRange *r, korb_sword_t *lo, korb_sword_t *hi);
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* defined in builtins/range.c */
/* splat `*val` into aref: Array → its elements, Range → its ints, nil → nothing,
 * else → val itself (Ruby `[*x]` semantics). aref/srcref are slots-rooted. */
RESULT
korb_ary_concat_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE val)
{
    if (KORB_ARRAY_P(val)) {
        VALUE_REF sref = SLOTS_PUSH(slots, val);    /* root src across grow GCs */
        uint32_t n = VAL2ARY(VALUE_REF_GET(sref))->len;
        for (uint32_t i = 0; i < n; i++)
            CHECK(korb_ary_push_val(c, slots + 1, aref, korb_items_data(VAL2ARY(VALUE_REF_GET(sref))->items)[i]));
        return RESULT_OK(VALUE_REF_GET(aref));
    }
    if (KORB_RANGE_P(val)) {
        const KorbRange *r = VAL2RANGE(val);
        korb_sword_t lo, hi;
        if (korb_range_int_bounds(r, &lo, &hi)) {
            for (korb_sword_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots, aref, LONG2FIX(i)));
            return RESULT_OK(VALUE_REF_GET(aref));
        }
        slots[0] = val;                                 /* non-int (e.g. String) range → expand via to_a */
        RESULT ta = korb_m_range_to_a(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0));
        if (UNLIKELY(ta.state != KORB_NORMAL)) return ta;
        slots[0] = ta.value;
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++)
            CHECK(korb_ary_push_val(c, slots + 1, aref, korb_items_data(VAL2ARY(slots[0])->items)[i]));
        return RESULT_OK(VALUE_REF_GET(aref));
    }
    if (val == KORB_NIL) return RESULT_OK(VALUE_REF_GET(aref));
    /* `*obj` for an object with a to_a (e.g. Struct) spreads its elements
     * (Ruby `[*x]` / `a, b = *x` semantics); otherwise `*x` is just `[x]`. */
    if (KORB_OBJECT_P(val)) {
        const uint32_t to_a_mid = korb_intern(c->vm, "to_a", 4);
        VALUE_REF vr = SLOTS_PUSH(slots, val);           /* root recv across the respond_to? / to_a dispatch */
        if (korb_responds_to_coerce(c, slots, VALUE_REF_GET(vr), to_a_mid)) {   /* honors #respond_to? (proxies/mocks) */
            RESULT ta = korb_send(c, slots, to_a_mid, 0, 0);
            if (UNLIKELY(ta.state != KORB_NORMAL)) return ta;
            if (KORB_ARRAY_P(ta.value)) {
                VALUE_REF_SET(vr, ta.value);             /* reuse the rooted slot for the array */
                uint32_t n = VAL2ARY(VALUE_REF_GET(vr))->len;
                for (uint32_t i = 0; i < n; i++)
                    CHECK(korb_ary_push_val(c, slots, aref, korb_items_data(VAL2ARY(VALUE_REF_GET(vr))->items)[i]));
                return RESULT_OK(VALUE_REF_GET(aref));
            }
            if (ta.value == KORB_NIL)                    /* to_a → nil: `*x` is just `[x]` */
                return korb_ary_push_val(c, slots, aref, VALUE_REF_GET(vr));
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_a gives %s)",   /* to_a → non-Array */
                              korb_coerce_name(c, VALUE_REF_GET(vr)), korb_coerce_name(c, VALUE_REF_GET(vr)), korb_type_name(ta.value));
        }
        return korb_ary_push_val(c, slots, aref, VALUE_REF_GET(vr));   /* re-read: coerce may have moved val */
    }
    return korb_ary_push_val(c, slots, aref, val);
}

/* Multiple assignment `a, b = rhs`: coerce a non-Array rhs via #to_ary (an Array
 * result is spread; nil keeps it scalar → [rhs]; any other non-Array raises
 * TypeError).  rhs must be in slots[0]; slots[0] is updated to the Array on
 * success.  Non-static so node_eval.c can call it. */
RESULT
korb_massign_coerce(CTX *c, VALUE *slots)
{
    const VALUE v = slots[0];
    if (KORB_ARRAY_P(v) || !KORB_OBJECT_P(v)) return RESULT_OK(v);
    const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
    if (!korb_responds_to_coerce(c, slots + 1, slots[0], to_ary)) return RESULT_OK(slots[0]);   /* honors #respond_to? */
    RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (KORB_ARRAY_P(r.value)) { slots[0] = r.value; return RESULT_OK(r.value); }
    if (r.value == KORB_NIL) return RESULT_OK(slots[0]);   /* to_ary → nil: keep scalar */
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_ary gives %s)",
                      korb_coerce_name(c, slots[0]), korb_coerce_name(c, slots[0]), korb_type_name(r.value));
}

/* Concatenate two arrays into a fresh one (Array#+ / the `+` binop).  lref/rref
 * are rooted; the result is left on the slots cursor via push. */
static RESULT
korb_ary_plus_ref(CTX *c, VALUE *slots, VALUE_REF lref, VALUE_REF rref)
{
    uint32_t ln = VAL2ARY(VALUE_REF_GET(lref))->len;
    uint32_t rn = VAL2ARY(VALUE_REF_GET(rref))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, ln + rn)));
    for (uint32_t i = 0; i < ln; i++) {
        VALUE elem = korb_items_data(VAL2ARY(VALUE_REF_GET(lref))->items)[i];   /* push roots elem first */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    rn = VAL2ARY(VALUE_REF_GET(rref))->len;                          /* re-read (rooted) */
    for (uint32_t i = 0; i < rn; i++) {
        VALUE elem = korb_items_data(VAL2ARY(VALUE_REF_GET(rref))->items)[i];
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* ---------------------------------------------------------------------------
 * Hash — header + growable [k0,v0,k1,v1,...] payload (insertion-ordered, linear
 * lookup).  Same moving-GC discipline as Array (separate, relocatable items).
 * ------------------------------------------------------------------------- */

RESULT
korb_hash_new(CTX *c, VALUE *slots, uint32_t capa)
{
    if (capa < 4) capa = 4;
    KorbArrayItems *it = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)capa * 2 * sizeof(VALUE),
                                    KORB_OBJ_VALUE_ARRAY);
    VALUE_REF itref = SLOTS_PUSH(slots, (VALUE)it);
    KorbHash *h = korb_alloc(c, slots, sizeof(KorbHash), KORB_OBJ_HASH);
    it = (KorbArrayItems *)(uintptr_t)VALUE_REF_GET(itref);    /* re-read after GC */
    h->len = 0;
    h->capa = capa;                       /* default_val already nil (zero-init) */
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->items, (VALUE)(uintptr_t)it);
    return RESULT_OK((VALUE)h);
}

/* Keys whose hash is unambiguous w.r.t. korb_value_eq (no cross-type ==): only
 * these go in the O(1) index.  Float (1==1.0) and heap objects are excluded. */
static inline bool korb_key_indexable(VALUE v) {
    return FIXNUM_P(v) || SYMBOL_P(v) || KORB_STRING_P(v) || KORB_ARRAY_P(v) ||
           v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE;
}
static uint64_t korb_value_hash_d(VALUE v, int depth) {
    if (FIXNUM_P(v)) { uint64_t x = (uint64_t)v; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 29; return x; }
    if (SYMBOL_P(v)) { uint64_t x = (uint64_t)SYM2ID(v) + 1; x *= 0x9e3779b97f4a7c15ULL; return x ^ (x >> 32); }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        uint64_t h = 1469598103934665603ULL;            /* FNV-1a */
        for (uint32_t i = 0; i < s->len; i++) { h ^= (unsigned char)korb_strbuf_data(s->buf)[i]; h *= 1099511628211ULL; }
        return h;
    }
    if (v == KORB_NIL)  return 0x9e3779b97f4a7c15ULL;
    if (v == KORB_TRUE) return 0x100000001ULL;
    if (v == KORB_FALSE) return 0x200000002ULL;
    if (KORB_ARRAY_P(v)) {                              /* Array key: content hash (matches Array#== / #eql?) */
        if (depth > 3) return 0x345678ULL;             /* shallow cap: bounds cost on wide self-referential arrays (width^depth); deeper distinctions fall through to #eql? */
        const KorbArray *const a = VAL2ARY(v);
        uint64_t h = 0x345678ULL ^ ((uint64_t)a->len * 0x9e3779b97f4a7c15ULL);
        for (uint32_t i = 0; i < a->len; i++) { h ^= korb_value_hash_d(korb_items_data(a->items)[i], depth + 1); h *= 1099511628211ULL; }
        return h;
    }
    return 0x200000002ULL;                              /* other heap objects: single bucket (rare; value_eq confirms) */
}
static uint64_t korb_value_hash(VALUE v) { return korb_value_hash_d(v, 0); }

bool korb_value_eq(VALUE a, VALUE b);   /* defined below */

/* Inlinable eql? for hot Hash scans: the dominant cases (identity, and a
 * mismatched Symbol or Fixnum-pair — never eql? unless bit-equal) resolve
 * without a call; only heterogeneous/heap keys fall to the out-of-line
 * korb_value_eq (which the compiler won't inline — it carries the full
 * Integer/String/Array/Set type cascade). */
static inline bool korb_value_eq_fast(VALUE a, VALUE b)
{
    if (a == b) return true;
    if (SYMBOL_P(a) || (FIXNUM_P(a) && FIXNUM_P(b))) return false;
    if (KORB_ARRAY_P(a) && KORB_ARRAY_P(b)) {        /* Array hash-keys: inline element-wise eql? (this is the only caller — korb_hash_find — hash keys use eql? semantics; array keys are hot in optcarrot, so inline to skip the call) */
        const KorbArray *const x = VAL2ARY(a), *const y = VAL2ARY(b);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++) {
            const VALUE xi = korb_items_data(x->items)[i], yi = korb_items_data(y->items)[i];
            if (xi == yi) continue;                  /* identical element */
            if (FIXNUM_P(xi) && FIXNUM_P(yi)) return false;
            if (!korb_value_eql(xi, yi)) return false;  /* eql? (type-strict: 1.0 ≠ 1); recurses for nested arrays */
        }
        return true;
    }
    return korb_value_eql(a, b);   /* hash key compare = eql? (not ==): 1.0 and 1 are distinct keys */
}

/* eql? (numeric-type-strict equality) — defined below; Hash keys use it so 1
 * and 1.0 are distinct keys (CRuby Hash uses eql?, not ==). */
static bool korb_value_eql(VALUE a, VALUE b);

/* index of key in the pair array, or -1 */
int32_t
korb_hash_find(const KorbHash *h, VALUE key)
{
    const VALUE *const d = korb_items_data(h->items);
    if (LIKELY(h->idx_mask && korb_key_indexable(key))) {   /* O(1) open-addressing probe (CMP_BY_ID never indexes) */
        const uint32_t *const tab = (const uint32_t *)korb_strbuf_data(h->index);
        const uint32_t mask = h->idx_mask;
        uint32_t slot = (uint32_t)korb_value_hash(key) & mask;
        for (;;) {
            uint32_t e = tab[slot];
            if (e == 0) return -1;
            if (korb_value_eq_fast(d[2 * (e - 1)], key)) return (int32_t)(e - 1);
            slot = (slot + 1) & mask;
        }
    }
    if (h->head.flags & KORB_FL_CMP_BY_ID) {        /* compare_by_identity */
        for (uint32_t i = 0; i < h->len; i++)
            if (d[2 * i] == key) return (int32_t)i;
        return -1;
    }
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eql(d[2 * i], key)) return (int32_t)i;   /* eql?: 1 and 1.0 are distinct keys */
    return -1;
}

/* CTX-aware find: a user object key (which may define a custom eql?/hash) is
 * matched by dispatching `stored.eql?(searched)` over a linear scan; every other
 * key type uses the CTX-free fast find.  `href` is the rooted Hash (re-read each
 * step since eql? may GC).  On a dispatch error *out_err carries it (result -1). */
static int32_t
korb_hash_find_ctx(CTX *c, VALUE *slots, VALUE_REF href, VALUE key, RESULT *out_err)
{
    *out_err = RESULT_OK(KORB_NIL);
    if (LIKELY(!KORB_OBJECT_P(key)))
        return korb_hash_find(VAL2HASH(VALUE_REF_GET(href)), key);
    if (VAL2HASH(VALUE_REF_GET(href))->head.flags & KORB_FL_CMP_BY_ID) {   /* compare_by_identity: pointer only */
        const KorbHash *const h = VAL2HASH(VALUE_REF_GET(href));
        for (uint32_t i = 0; i < h->len; i++) if (korb_items_data(h->items)[2 * i] == key) return (int32_t)i;
        return -1;
    }
    slots[0] = key;                                       /* root searched key across dispatch */
    const uint32_t eqm = korb_intern(c->vm, "eql?", 4);
    const uint32_t hashm = korb_intern(c->vm, "hash", 4);
    /* CRuby buckets by #hash first, so a key whose #hash differs is never
     * compared with #eql? (mocks in the spec suite assert exactly that). */
    slots[2] = slots[0];
    RESULT khr = korb_send_impl(c, slots + 3, hashm, 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(khr.state != KORB_NORMAL)) { *out_err = khr; return -1; }
    slots[1] = khr.value;                                 /* searched key's #hash (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *const h = VAL2HASH(VALUE_REF_GET(href));
        if (i >= h->len) return -1;
        const VALUE existing = korb_items_data(h->items)[2 * i];
        if (existing == slots[0]) return (int32_t)i;      /* identity shortcut (no dispatch) */
        if (KORB_OBJECT_P(existing)) {                    /* both are objects → compare their #hash */
            slots[3] = existing;
            slots[4] = existing;
            const RESULT hr = korb_send_impl(c, slots + 5, hashm, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) { *out_err = hr; return -1; }
            if (!korb_value_eq(hr.value, slots[1])) continue;
            slots[3] = slots[0]; slots[4] = korb_items_data(VAL2HASH(VALUE_REF_GET(href))->items)[2 * i];
        } else {
            slots[3] = slots[0]; slots[4] = existing;
        }
        const RESULT r = korb_send_impl(c, slots + 5, eqm, 0, 1, NULL, NULL, NULL);   /* searched.eql?(stored) */
        if (UNLIKELY(r.state != KORB_NORMAL)) { *out_err = r; return -1; }
        if (KORB_TRUTHY(r.value)) return (int32_t)i;
    }
}

/* Insert pair `pi` into an existing index (no alloc; caller ensures capacity). */
static void korb_hash_index_put(KorbHash *h, uint32_t pi) {
    uint32_t *const tab = (uint32_t *)korb_strbuf_data(h->index);
    const uint32_t mask = h->idx_mask;
    uint32_t slot = (uint32_t)korb_value_hash(korb_items_data(h->items)[2 * pi]) & mask;
    while (tab[slot]) slot = (slot + 1) & mask;
    tab[slot] = pi + 1;
}
/* (Re)build the O(1) index for an all-indexable hash (load factor ~0.5). */
static RESULT korb_hash_index_build(CTX *c, VALUE *slots, VALUE_REF href) {
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    uint32_t cap = 16; while ((size_t)cap < (size_t)h->len * 2) cap <<= 1;
    KorbStrBuf *idx = korb_alloc(c, slots, sizeof(KorbStrBuf) + (size_t)cap * sizeof(uint32_t), KORB_OBJ_STR_BUF);
    memset(korb_strbuf_data(idx), 0, (size_t)cap * sizeof(uint32_t));
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after alloc/GC */
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->index, (VALUE)(uintptr_t)idx);
    h->idx_mask = cap - 1;
    for (uint32_t i = 0; i < h->len; i++) korb_hash_index_put(h, i);   /* no alloc in the loop */
    return RESULT_OK(KORB_NIL);
}
#define KORB_HASH_INDEX_THRESHOLD 16u

static RESULT
korb_hash_ensure(CTX *c, VALUE *slots, VALUE_REF href, uint32_t need)
{
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    if ((size_t)h->len + need <= h->capa) return RESULT_OK(VALUE_REF_GET(href));
    uint32_t ncapa = h->capa ? h->capa * 2 : 4;
    while ((size_t)ncapa < (size_t)h->len + need) ncapa *= 2;
    KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * 2 * sizeof(VALUE),
                                     KORB_OBJ_VALUE_ARRAY);
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after GC */
    KorbArrayItems *oit = h->items;
    ARO_STORE_BULK(c, nit, korb_items_data(nit), korb_items_data(oit), (size_t)h->len * 2);
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->items, (VALUE)(uintptr_t)nit);
    h->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(href));
}

/* merge `**src` into href: copy each src pair (Hash) into href.  nil → no-op. */
RESULT
korb_hash_merge_val(CTX *c, VALUE *slots, VALUE_REF href, VALUE src)
{
    if (src == KORB_NIL) return RESULT_OK(VALUE_REF_GET(href));
    if (UNLIKELY(!KORB_HASH_P(src))) {                 /* `**obj` → obj.to_hash */
        const char *const orig = korb_type_name(src);
        const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
        if (KORB_OBJECT_P(src) && korb_responds_to_coerce_p(c, slots, &src, to_hash)) {
            slots[0] = src;
            RESULT r = korb_send_impl(c, slots + 1, to_hash, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            src = r.value;
        }
        if (!KORB_HASH_P(src)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", orig);
    }
    VALUE_REF sref = SLOTS_PUSH(slots, src);          /* root src across grow GCs */
    uint32_t n = VAL2HASH(VALUE_REF_GET(sref))->len;
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = korb_items_data(VAL2HASH(VALUE_REF_GET(sref))->items)[2*i];      /* key */
        slots[2] = korb_items_data(VAL2HASH(VALUE_REF_GET(sref))->items)[2*i + 1];  /* val */
        CHECK(korb_hash_set(c, slots + 3, href, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(href));
}

RESULT
korb_hash_set(CTX *c, VALUE *slots, VALUE_REF href, VALUE_REF kref, VALUE val)
{
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val across grow / eql? GC */
    RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, href, VALUE_REF_GET(kref), &ferr);
    if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));      /* re-read after possible eql? dispatch GC */
    if (idx >= 0) {
        KorbArrayItems *it = h->items;
        ARO_STORE(c, it, &korb_items_data(it)[2 * idx + 1], VALUE_REF_GET(vref));
        return RESULT_OK(VALUE_REF_GET(href));
    }
    CHECK(korb_hash_ensure(c, slots, href, 1));
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after grow */
    KorbArrayItems *it = h->items;
    uint32_t i = h->len;
    ARO_STORE(c, it, &korb_items_data(it)[2 * i],     VALUE_REF_GET(kref));
    ARO_STORE(c, it, &korb_items_data(it)[2 * i + 1], VALUE_REF_GET(vref));
    h->len++;
    /* O(1) index maintenance.  An ambiguous key (Float/heap) permanently drops
     * the index; otherwise build at the threshold and incrementally fill,
     * rebuilding bigger when the load factor exceeds ~0.7. */
    if (!(h->head.flags & (KORB_FL_HASH_NOINDEX | KORB_FL_CMP_BY_ID))) {
        if (UNLIKELY(!korb_key_indexable(VALUE_REF_GET(kref)))) {
            h->head.flags |= KORB_FL_HASH_NOINDEX;
            ARO_GC_RAW_STORE(&h->index, NULL); h->idx_mask = 0;
        } else if (h->idx_mask) {
            if (UNLIKELY((size_t)h->len * 10 > (size_t)(h->idx_mask + 1) * 7))
                CHECK(korb_hash_index_build(c, slots, href));
            else
                korb_hash_index_put(h, i);
        } else if (h->len >= KORB_HASH_INDEX_THRESHOLD) {
            CHECK(korb_hash_index_build(c, slots, href));
        }
    }
    return RESULT_OK(VALUE_REF_GET(href));
}

/* ---------------------------------------------------------------------------
 * Object + instance variables.  ivars are a lazily-allocated [sym,val] pair
 * payload (same shape as Hash storage); names are interned symbols compared by
 * identity.
 * ------------------------------------------------------------------------- */

RESULT
korb_obj_new(CTX *c, VALUE *slots, VALUE klass)
{
    VALUE_REF kref = SLOTS_PUSH(slots, klass);        /* root klass across alloc */
    KorbObject *o = korb_alloc(c, slots, sizeof(KorbObject), KORB_OBJ_OBJECT);
    o->shape_id = 1;                                  /* root shape (no ivars) */
    /* zero-init: ivar_capa=0, klass=nil, ivars=NULL */
    if (klass != KORB_NIL) ARO_STORE(c, o, (VALUE *)(uintptr_t)&o->klass, VALUE_REF_GET(kref));
    return RESULT_OK((VALUE)o);
}

/* --- object shapes (ivar-layout transition tree; VM-side / libc-stable) --- */
static uint32_t
korb_shape_new(struct korb_vm *vm, uint32_t parent, uint32_t edge_sym)
{
    if (vm->shape_cnt == vm->shape_capa) {
        uint32_t nc = vm->shape_capa ? vm->shape_capa * 2 : 64;
        vm->shapes = realloc(vm->shapes, sizeof(*vm->shapes) * nc);
        if (!vm->shapes) { fprintf(stderr, "koruby_precise: oom (shapes)\n"); abort(); }
        vm->shape_capa = nc;
    }
    uint32_t id = vm->shape_cnt++;
    struct korb_shape *s = &vm->shapes[id];
    s->parent = parent;
    s->edge_sym = edge_sym;
    s->ivar_count = parent ? vm->shapes[parent].ivar_count + 1 : 0;
    s->edges = NULL; s->edge_cnt = s->edge_capa = 0;
    return id;
}

/* find-or-create the child shape reached by adding ivar `sym` to `shape`. */
static uint32_t
korb_shape_transition(struct korb_vm *vm, uint32_t shape, uint32_t sym)
{
    struct korb_shape *s = &vm->shapes[shape];
    for (uint32_t i = 0; i < s->edge_cnt; i++)
        if (s->edges[i].sym == sym) return s->edges[i].child;
    uint32_t child = korb_shape_new(vm, shape, sym);  /* may realloc vm->shapes */
    s = &vm->shapes[shape];                            /* re-read after possible realloc */
    if (s->edge_cnt == s->edge_capa) {
        uint32_t nc = s->edge_capa ? s->edge_capa * 2 : 4;
        s->edges = realloc(s->edges, sizeof(*s->edges) * nc);
        if (!s->edges) { fprintf(stderr, "koruby_precise: oom (shape edges)\n"); abort(); }
        s->edge_capa = nc;
    }
    s->edges[s->edge_cnt].sym = sym;
    s->edges[s->edge_cnt].child = child;
    s->edge_cnt++;
    return child;
}

/* korb_shape_index is now an inline in node.h (folds into the SDs). */

/* Direct-mapped (shape_id, sym) → slot cache for the *out-of-line* ivar
 * accessors (korb_ivar_get/set), which lack a per-node inline cache and would
 * otherwise walk the shape parent chain O(depth) every call — hot for ivar
 * multi-assign (node_massign_het) and attr writers on deep-ivar objects (e.g.
 * optcarrot's 79-ivar PPU).  A (shape_id, sym) pair maps to a permanent slot
 * (shapes are immutable), so entries never need invalidation. */
#define KORB_SHIDX_BITS 12
struct korb_shidx_ent { uint32_t shape, sym; int32_t slot; };
static struct korb_shidx_ent korb_shidx_cache[1u << KORB_SHIDX_BITS];
static inline int32_t korb_shape_index_cached(struct korb_vm *vm, uint32_t shape, uint32_t sym) {
    const uint32_t h = (shape * 2654435761u ^ sym * 40503u) & ((1u << KORB_SHIDX_BITS) - 1u);
    struct korb_shidx_ent *const e = &korb_shidx_cache[h];
    if (LIKELY(e->shape == shape && e->sym == sym)) return e->slot;
    const int32_t idx = korb_shape_index(vm, shape, sym);
    /* Cache both present and absent (idx==-1) results — shapes are immutable, so an
     * absent (shape,sym) stays absent forever.  The ivar-SET path on a fresh object
     * always probes an absent ivar before transitioning (hot for `@x = ...` in
     * initialize), so caching -1 avoids re-walking the shape each time. */
    e->shape = shape; e->sym = sym; e->slot = idx;
    return idx;
}

/* A class object's own instance variables live in a side hash (KorbClass has no
 * shape/ivars array).  `@x` with self = a Class/Module routes here. */
static VALUE
korb_class_ivar_get(VALUE cls, VALUE name_sym)
{
    const VALUE h = VAL2CLASS(cls)->class_ivars;
    if (h == KORB_NIL) return KORB_NIL;
    const int32_t idx = korb_hash_find(VAL2HASH(h), name_sym);
    return idx >= 0 ? korb_items_data(VAL2HASH(h)->items)[2 * idx + 1] : KORB_NIL;
}

static RESULT
korb_class_ivar_set(CTX *c, VALUE *slots, VALUE_REF clsref, VALUE name_sym, VALUE val)
{
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(clsref));   /* a frozen class/module takes no ivars */
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root across hash alloc/grow */
    VALUE_REF nref = SLOTS_PUSH(slots, name_sym);
    if (VAL2CLASS(VALUE_REF_GET(clsref))->class_ivars == KORB_NIL) {
        const VALUE h = UNWRAP(korb_hash_new(c, slots, 4));
        ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(clsref)),
                  (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(clsref))->class_ivars, h);
    }
    VALUE_REF href = SLOTS_PUSH(slots, VAL2CLASS(VALUE_REF_GET(clsref))->class_ivars);
    CHECK(korb_hash_set(c, slots, href, nref, VALUE_REF_GET(vref)));
    return RESULT_OK(VALUE_REF_GET(vref));
}

/* A custom Exception subclass instance keeps its ivars in a side hash (like a
 * Class), so `@id` on a NotFound exception works.  KorbException has no shape. */
static VALUE
korb_exc_ivar_get(VALUE exc, VALUE name_sym)
{
    const VALUE h = VAL2EXC(exc)->ivars;
    if (h == KORB_NIL) return KORB_NIL;
    const int32_t idx = korb_hash_find(VAL2HASH(h), name_sym);
    return idx >= 0 ? korb_items_data(VAL2HASH(h)->items)[2 * idx + 1] : KORB_NIL;
}

RESULT
korb_exc_ivar_set(CTX *c, VALUE *slots, VALUE_REF excref, VALUE name_sym, VALUE val)
{
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root across hash alloc/grow */
    VALUE_REF nref = SLOTS_PUSH(slots, name_sym);
    if (VAL2EXC(VALUE_REF_GET(excref))->ivars == KORB_NIL) {
        const VALUE h = UNWRAP(korb_hash_new(c, slots, 4));
        ARO_STORE(c, VAL2EXC(VALUE_REF_GET(excref)),
                  (VALUE *)(uintptr_t)&VAL2EXC(VALUE_REF_GET(excref))->ivars, h);
    }
    VALUE_REF href = SLOTS_PUSH(slots, VAL2EXC(VALUE_REF_GET(excref))->ivars);
    CHECK(korb_hash_set(c, slots, href, nref, VALUE_REF_GET(vref)));
    return RESULT_OK(VALUE_REF_GET(vref));
}

/* Raise a KeyError carrying #receiver (recv) and #key (key), as CRuby's
 * Hash#fetch / Array#fetch / sprintf-%{name} / ENV.fetch do.  `msg` is the
 * pre-built message.  recv/key are rooted across the ivar allocs. */
static RESULT
korb_raise_key(CTX *c, VALUE *slots, VALUE recv, VALUE key, const char *msg)
{
    slots[0] = recv; slots[1] = key;
    RESULT r = korb_raise(c, slots + 2, KORB_E_KEY, 0, "%s", msg);
    if (LIKELY(KORB_EXC_P(r.value))) {
        slots[2] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[2]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__key", 6)), slots[1]);
        r.value = VALUE_REF_GET(eref);
    }
    return r;
}

/* Generic-ivar side table for objects without a struct ivar slot (String/Array/
 * Hash/Proc/...).  Same lockstep-forwarded pattern as the sklass override table:
 * both columns are GC roots (see AROH_VISIT_ROOTS), so `obj == objivar_obj[i]`
 * stays a valid identity test across compaction. */
static VALUE korb_objivar_hash_of(const struct korb_vm *vm, VALUE obj) {
    if (!(((const AroObjectHeader *)(uintptr_t)obj)->flags & KORB_FL_HAS_IVARS)) return KORB_NIL;
    for (uint32_t i = 0; i < vm->objivar_cnt; i++)
        if (vm->objivar_obj[i] == obj) return vm->objivar_hash[i];
    return KORB_NIL;
}
static void korb_objivar_register(CTX *c, VALUE obj, VALUE h) {   /* libc realloc, not a GC point */
    struct korb_vm *const vm = c->vm;
    if (vm->objivar_cnt == vm->objivar_capa) {
        uint32_t nc = vm->objivar_capa ? vm->objivar_capa * 2 : 16;
        vm->objivar_obj  = realloc(vm->objivar_obj,  sizeof(VALUE) * nc);
        vm->objivar_hash = realloc(vm->objivar_hash, sizeof(VALUE) * nc);
        if (!vm->objivar_obj || !vm->objivar_hash) { fprintf(stderr, "koruby_precise: oom (objivar)\n"); abort(); }
        vm->objivar_capa = nc;
    }
    vm->objivar_obj[vm->objivar_cnt]  = obj;
    vm->objivar_hash[vm->objivar_cnt] = h;
    vm->objivar_cnt++;
    ((AroObjectHeader *)(uintptr_t)obj)->flags |= KORB_FL_HAS_IVARS;
}
static RESULT korb_objivar_set(CTX *c, VALUE *slots, VALUE_REF objref, VALUE name_sym, VALUE val) {
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val/name across hash alloc/grow */
    VALUE_REF nref = SLOTS_PUSH(slots, name_sym);
    VALUE h = korb_objivar_hash_of(c->vm, VALUE_REF_GET(objref));
    if (h == KORB_NIL) {
        h = UNWRAP(korb_hash_new(c, slots, 4));       /* GC; objref (VALUE_REF) tracks the move */
        korb_objivar_register(c, VALUE_REF_GET(objref), h);   /* libc; sets the flag */
    }
    VALUE_REF href = SLOTS_PUSH(slots, h);            /* h is now also a root via the table (forwarded in lockstep) */
    CHECK(korb_hash_set(c, slots, href, nref, VALUE_REF_GET(vref)));
    return RESULT_OK(VALUE_REF_GET(vref));
}
static VALUE korb_objivar_get(const struct korb_vm *vm, VALUE obj, VALUE name_sym) {
    const VALUE h = korb_objivar_hash_of(vm, obj);
    if (h == KORB_NIL) return KORB_NIL;
    const int32_t idx = korb_hash_find(VAL2HASH(h), name_sym);
    return idx >= 0 ? korb_items_data(VAL2HASH(h)->items)[2 * idx + 1] : KORB_NIL;
}
VALUE
korb_ivar_get(CTX *c, VALUE self, VALUE name_sym)
{
    if (UNLIKELY(!KORB_OBJECT_P(self))) {            /* class → side hash; other heap objects → generic-ivar table */
        if (KORB_CLASS_P(self)) return korb_class_ivar_get(self, name_sym);
        if (KORB_EXC_P(self)) return korb_exc_ivar_get(self, name_sym);
        if (AROH_IS_GC_OBJECT(self)) return korb_objivar_get(c->vm, self, name_sym);
        return KORB_NIL;                             /* immediate (Integer/Symbol/nil/...): no ivars */
    }
    const KorbObject *o = VAL2OBJ(self);
    int32_t idx = korb_shape_index_cached(c->vm, o->shape_id, SYM2ID(name_sym));
    if (idx < 0) return KORB_NIL;
    return korb_items_data(o->ivars)[idx];
}

void
korb_ivar_store_at(CTX *c, KorbObject *o, uint32_t slot, VALUE val)
{
    ARO_STORE(c, o->ivars, &korb_items_data(o->ivars)[slot], val);   /* values-only array */
}

RESULT
korb_ivar_set(CTX *c, VALUE *slots, VALUE_REF selfref, VALUE name_sym, VALUE val)
{
    if (UNLIKELY(!KORB_OBJECT_P(VALUE_REF_GET(selfref)))) {
        if (KORB_CLASS_P(VALUE_REF_GET(selfref)))     /* class object's own @ivars → side hash */
            return korb_class_ivar_set(c, slots, selfref, name_sym, val);
        if (KORB_EXC_P(VALUE_REF_GET(selfref)))       /* custom Exception subclass ivars → side hash */
            return korb_exc_ivar_set(c, slots, selfref, name_sym, val);
        if (AROH_IS_GC_OBJECT(VALUE_REF_GET(selfref)))   /* String/Array/Hash/Proc/... → generic-ivar table */
            return korb_objivar_set(c, slots, selfref, name_sym, val);
        return RESULT_OK(val);                        /* immediate (Integer/Symbol/nil/...): no ivar storage */
    }
    const uint32_t sym = SYM2ID(name_sym);
    KorbObject *o = VAL2OBJ(VALUE_REF_GET(selfref));
    if (UNLIKELY(o->head.flags & KORB_FL_FROZEN)) return korb_raise_frozen(c, slots, VALUE_REF_GET(selfref));
    int32_t idx = korb_shape_index_cached(c->vm, o->shape_id, sym);
    if (idx >= 0) {                                   /* existing ivar: in-place (no GC) */
        ARO_STORE(c, o->ivars, &korb_items_data(o->ivars)[idx], val);
        return RESULT_OK(val);
    }
    /* new ivar: transition shape + (maybe) grow the values array. */
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val across grow GC */
    const uint32_t nshape = korb_shape_transition(c->vm, o->shape_id, sym);   /* libc, no GC */
    const uint32_t ncount = c->vm->shapes[nshape].ivar_count;   /* = old count + 1 */
    o = VAL2OBJ(VALUE_REF_GET(selfref));
    if (!o->ivars || ncount > o->ivar_capa) {         /* grow / first alloc */
        uint32_t ncapa = o->ivar_capa ? o->ivar_capa * 2 : 4;
        while (ncapa < ncount) ncapa *= 2;
        KorbArrayItems *nit = korb_alloc(c, slots, sizeof(KorbArrayItems) + (size_t)ncapa * sizeof(VALUE),
                                         KORB_OBJ_VALUE_ARRAY);
        o = VAL2OBJ(VALUE_REF_GET(selfref));          /* re-read after GC */
        if (o->ivars) ARO_STORE_BULK(c, nit, korb_items_data(nit), korb_items_data(o->ivars), (size_t)(ncount - 1));
        ARO_STORE(c, o, (VALUE *)(uintptr_t)&o->ivars, (VALUE)(uintptr_t)nit);
        o->ivar_capa = ncapa;
    }
    o->shape_id = nshape;                             /* commit transition */
    ARO_STORE(c, o->ivars, &korb_items_data(o->ivars)[ncount - 1], VALUE_REF_GET(vref));
    return RESULT_OK(VALUE_REF_GET(vref));
}

/* instance_variable_defined? — membership, NOT value: an ivar set to nil is still
 * defined.  Objects consult their shape (no cost to the hot ivar_get path);
 * class/exc consult their side hash (which already distinguishes nil-set from
 * unset).  Returns false for immediates / other builtins. */
bool
korb_ivar_defined(CTX *c, VALUE self, VALUE name_sym)
{
    if (KORB_OBJECT_P(self))
        return korb_shape_index(c->vm, VAL2OBJ(self)->shape_id, SYM2ID(name_sym)) >= 0;
    if (KORB_CLASS_P(self)) {
        const VALUE h = VAL2CLASS(self)->class_ivars;
        return h != KORB_NIL && korb_hash_find(VAL2HASH(h), name_sym) >= 0;
    }
    if (KORB_EXC_P(self)) {
        const VALUE h = VAL2EXC(self)->ivars;
        return h != KORB_NIL && korb_hash_find(VAL2HASH(h), name_sym) >= 0;
    }
    if (AROH_IS_GC_OBJECT(self)) {                    /* container/heap object → generic-ivar table */
        const VALUE h = korb_objivar_hash_of(c->vm, self);
        return h != KORB_NIL && korb_hash_find(VAL2HASH(h), name_sym) >= 0;
    }
    return false;
}

/* remove_instance_variable core.  Objects: rebuild the shape without `name_sym`
 * (replay root→ remaining ivars in order) and compact the values array, so the
 * ivar truly leaves the shape — no sentinel, no per-read check.  Class/exc:
 * shift-delete from the side hash.  Returns the removed value; *found=false if
 * the ivar was not set.  No GC point (shape ops are libc; compaction is stores),
 * so `self` stays put. */
static VALUE
korb_ivar_remove(CTX *c, VALUE self, VALUE name_sym, bool *found)
{
    struct korb_vm *const vm = c->vm;
    const uint32_t sym = SYM2ID(name_sym);
    if (KORB_OBJECT_P(self)) {
        KorbObject *const o = VAL2OBJ(self);
        const int32_t idx = korb_shape_index(vm, o->shape_id, sym);
        if (idx < 0) { *found = false; return KORB_NIL; }
        *found = true;
        const VALUE old = korb_items_data(o->ivars)[idx];
        const uint32_t n = vm->shapes[o->shape_id].ivar_count;
        uint32_t *const syms = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));   /* libc, no GC */
        if (UNLIKELY(!syms)) { fprintf(stderr, "koruby_precise: oom (ivar remove)\n"); abort(); }
        for (uint32_t sid = o->shape_id; sid; ) {                  /* leaf→root: place each sym at its index */
            const struct korb_shape *const s = &vm->shapes[sid];
            if (s->ivar_count >= 1 && s->ivar_count <= n) syms[s->ivar_count - 1] = s->edge_sym;
            sid = s->parent;
        }
        uint32_t ns = 1;                                          /* rebuild from the root shape (id 1; see korb_obj_new), skipping idx */
        for (uint32_t i = 0; i < n; i++)
            if (i != (uint32_t)idx) ns = korb_shape_transition(vm, ns, syms[i]);
        free(syms);
        for (uint32_t i = (uint32_t)idx; i + 1 < n; i++)          /* compact values (store-only, no GC) */
            ARO_STORE(c, o->ivars, &korb_items_data(o->ivars)[i], korb_items_data(o->ivars)[i + 1]);
        o->shape_id = ns;
        return old;
    }
    const VALUE h = KORB_CLASS_P(self) ? VAL2CLASS(self)->class_ivars
                  : KORB_EXC_P(self)   ? VAL2EXC(self)->ivars
                  : AROH_IS_GC_OBJECT(self) ? korb_objivar_hash_of(vm, self)   /* container/heap object */
                  : KORB_NIL;
    if (h == KORB_NIL) { *found = false; return KORB_NIL; }
    const int32_t idx = korb_hash_find(VAL2HASH(h), name_sym);
    if (idx < 0) { *found = false; return KORB_NIL; }
    *found = true;
    KorbHash *const hh = VAL2HASH(h);
    KorbArrayItems *const it = hh->items;
    const VALUE old = korb_items_data(it)[2 * idx + 1];
    for (uint32_t i = (uint32_t)idx; i + 1 < hh->len; i++) {      /* shift to keep order */
        ARO_STORE(c, it, &korb_items_data(it)[2 * i],     korb_items_data(it)[2 * (i + 1)]);
        ARO_STORE(c, it, &korb_items_data(it)[2 * i + 1], korb_items_data(it)[2 * (i + 1) + 1]);
    }
    hh->len--;
    ARO_STORE(c, it, &korb_items_data(it)[2 * hh->len], KORB_NIL);
    ARO_STORE(c, it, &korb_items_data(it)[2 * hh->len + 1], KORB_NIL);
    KORB_HASH_DROP_INDEX(hh);
    return old;
}

/* korb_bt_append / korb_close_ret are declared in node.h (de-static'd so the
 * inlined korb_invoke_simple in the SDs can reach them on cold paths). */

/* ---------------------------------------------------------------------------
 * Classes + constants.  A class's instance-method table is a libc side-array
 * (no GC edges); constants live in vm->const_* (root-scanned).
 * ------------------------------------------------------------------------- */

RESULT
korb_class_new(CTX *c, VALUE *slots, uint32_t name_sym, VALUE superclass)
{
    VALUE_REF sref = SLOTS_PUSH(slots, superclass);   /* root super across alloc */
    KorbClass *k = korb_alloc(c, slots, sizeof(KorbClass), KORB_OBJ_CLASS);
    k->name_sym = name_sym;                            /* methods=NULL, cnts=0 (zero-init) */
    k->serial = ++c->vm->class_serial;                 /* GC-stable identity (name_sym is 0 for anonymous classes) */
    k->exc_etype = -1;                                 /* not an exception class by default */
    if (superclass != KORB_NIL) ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->superclass, VALUE_REF_GET(sref));
    /* A subclass of a Struct/Data class inherits its member layout: share the
     * (immutable) members list + is_data/keyword-init flags so the member-based
     * .new / inspect / to_a / … paths (which read the instance's own class) work. */
    for (VALUE a = VALUE_REF_GET(sref); KORB_CLASS_P(a); a = VAL2CLASS(a)->superclass) {
        if (VAL2CLASS(a)->members != KORB_NIL) {
            ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->members, VAL2CLASS(a)->members);
            k->is_data = VAL2CLASS(a)->is_data;
            k->struct_kwinit = VAL2CLASS(a)->struct_kwinit;
            break;
        }
    }
    aro_gc_finalize_register(c, k);                    /* free the libc methods[] + entries when k is collected */
    return RESULT_OK((VALUE)k);
}

/* Record `sub` in `super`'s direct-subclass list (for Class#subclasses).  Called
 * only from the user class-definition paths (class-body / Class.new), never from
 * korb_class_new, so per-object singleton classes don't pollute the list. */
static RESULT korb_register_subclass(CTX *c, VALUE *slots, VALUE super_cls, VALUE sub_cls) {
    if (!KORB_CLASS_P(super_cls) || !KORB_CLASS_P(sub_cls)) return RESULT_OK(KORB_NIL);
    slots[0] = super_cls; slots[1] = sub_cls;                       /* root across the Array alloc/grow */
    VALUE ary = VAL2CLASS(slots[0])->subclasses;
    if (ary == KORB_NIL) ary = UNWRAP(korb_ary_new(c, slots + 2, 4));
    slots[2] = ary;
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    ARO_STORE(c, VAL2CLASS(slots[0]), (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->subclasses, slots[2]);
    return RESULT_OK(KORB_NIL);
}

/* ---------------------------------------------------------------------------
 * Class variables (`@@x`).  Stored in a per-class KorbHash (sym→value) and
 * shared down the superclass chain: a read/write resolves to the nearest
 * ancestor that already defines the name, else (for a write) the cref itself.
 * The cref is the class/module the `@@x` is lexically inside: in a class body
 * self IS the class; in an instance method it is the method's def_class.
 * ------------------------------------------------------------------------- */

/* `entry_cell` is the frame's fs-2 cell: a tagged-odd method-entry pointer in an
 * instance method (def_class = entry->owner), as korb_super reads it. */
/* The lexical cref for a constant read: like korb_cvar_cref, but a method
 * defined in a `class << obj` body reports that (unnamed) singleton, not the
 * object — its body's constants live there. */
VALUE
korb_const_cref(VALUE self, VALUE entry_cell)
{
    if ((uintptr_t)entry_cell & 1u) {
        const struct korb_method *const m = (const struct korb_method *)((uintptr_t)entry_cell & ~(uintptr_t)1u);
        if (KORB_CLASS_P(m->owner) && VAL2CLASS(m->owner)->is_singleton) return m->owner;
    }
    return korb_cvar_cref(self, entry_cell);
}
VALUE
korb_cvar_cref(VALUE self, VALUE entry_cell)
{
    if (KORB_CLASS_P(self)) return self;              /* class body / class method: self */
    if ((uintptr_t)entry_cell & 1u) {                 /* instance method: def_class = entry->owner */
        const struct korb_method *const m = (const struct korb_method *)((uintptr_t)entry_cell & ~(uintptr_t)1u);
        return m->owner;
    }
    return KORB_NIL;                                   /* toplevel: no class scope */
}

/* the ancestor (cref-and-up) that defines class var `sym`, with *idx set to its
 * hash slot; KORB_NIL if undefined anywhere on the chain. */
static bool korb_cvar_in(VALUE k, VALUE sym, int32_t *idx_out)
{
    if (!KORB_CLASS_P(k)) return false;
    const VALUE cv = VAL2CLASS(k)->cvars;
    if (cv == KORB_NIL) return false;
    const int32_t idx = korb_hash_find(VAL2HASH(cv), sym);
    if (idx < 0) return false;
    *idx_out = idx;
    return true;
}

VALUE
korb_cvar_owner(VALUE cref, VALUE sym, int32_t *idx_out)
{
    for (VALUE k = cref; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass) {
        if (korb_cvar_in(k, sym, idx_out)) return k;
        /* a class shares its class variables with the modules it includes and
         * prepends, not just with its superclasses */
        const VALUE pre = VAL2CLASS(k)->prepended;
        if (pre != KORB_NIL) {
            const KorbArray *const pa = VAL2ARY(pre);
            for (uint32_t i = 0; i < pa->len; i++)
                if (korb_cvar_in(korb_items_data(pa->items)[i], sym, idx_out)) return korb_items_data(pa->items)[i];
        }
        const VALUE inc = VAL2CLASS(k)->included;
        if (inc != KORB_NIL) {
            const KorbArray *const ia = VAL2ARY(inc);
            for (uint32_t i = 0; i < ia->len; i++)
                if (korb_cvar_in(korb_items_data(ia->items)[i], sym, idx_out)) return korb_items_data(ia->items)[i];
        }
    }
    return KORB_NIL;
}

/* A `class << X` body shares X's class variables: the singleton is not a scope
 * of its own for @@vars (CRuby). */
/* The class scope @@vars resolve against; falls back to the cref instance_eval /
 * instance_exec kept, since self there names the receiver, not a class. */
static VALUE korb_dispatch_class(CTX *c, VALUE self);            /* fwd */
static VALUE korb_cvar_self_class(CTX *c, VALUE self);           /* fwd */
VALUE korb_cvar_self_class_pub(CTX *c, VALUE self) {
    return KORB_CLASS_P(self) ? self : korb_cvar_self_class(c, self);
}
static VALUE korb_cvar_scope_of(CTX *c, VALUE self, VALUE entry_cell);
static VALUE korb_cvar_scope(CTX *c, VALUE cref)
{
    if (!KORB_CLASS_P(cref) || !VAL2CLASS(cref)->is_singleton) return cref;
    const struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->sklass_cnt; i++)
        if (vm->sklass_cls[i] == cref && KORB_CLASS_P(vm->sklass_obj[i])) return vm->sklass_obj[i];
    return cref;
}

/* self's class with any singleton peeled off — the scope a @@var falls back to
 * when no lexical cref is reachable (a block does not carry its method's entry
 * cell).  KORB_NIL for the toplevel `main`, which must keep raising. */
static VALUE korb_cvar_self_class(CTX *c, VALUE self)
{
    if (self == KORB_NIL || (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL)) return KORB_NIL;
    VALUE k = korb_dispatch_class(c, self);
    while (KORB_CLASS_P(k) && VAL2CLASS(k)->is_singleton) k = VAL2CLASS(k)->superclass;
    return KORB_CLASS_P(k) ? k : KORB_NIL;
}
static VALUE korb_cvar_scope_of(CTX *c, VALUE self, VALUE entry_cell)
{
    if (entry_cell == KORB_UNDEF) return korb_cvar_scope(c, self);   /* explicit receiver (class_variable_get/set) */
    if ((uintptr_t)entry_cell & 1u) {                 /* a real method frame: its owner is the cref */
        const VALUE own = korb_cvar_scope(c, korb_cvar_cref(self, entry_cell));
        if (KORB_CLASS_P(own)) return own;
    }
    /* instance_eval/exec: the caller's scope, even when self is a Class — the
     * receiver never contributes one. */
    if (c->cvar_cref != KORB_NIL) return korb_cvar_scope(c, c->cvar_cref);
    const VALUE cref = korb_cvar_scope(c, korb_cvar_cref(self, entry_cell));
    if (KORB_CLASS_P(cref)) return cref;
    return korb_cvar_self_class(c, self);
}

/* soft: `@@x ||= v` / `&&=` read an undefined cvar as nil instead of raising. */
RESULT
korb_cvar_get(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, uint32_t soft)
{
    const VALUE cref = korb_cvar_scope_of(c, self, entry_cell);
    if (!KORB_CLASS_P(cref)) {
        if (soft) return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "class variable access from toplevel");
    }
    const VALUE sym = ID2SYM(sym_id);
    int32_t idx;
    const VALUE owner = korb_cvar_owner(cref, sym, &idx);
    if (owner == KORB_NIL) {
        if (soft) return RESULT_OK(KORB_NIL);
        slots[0] = cref;                                  /* root across raise + ivar_set */
        RESULT ne = korb_raise(c, slots + 1, KORB_E_NAME, 0, "uninitialized class variable %s in %s",
                          korb_sym_name(c->vm, sym_id), korb_type_name(cref));
        if (LIKELY(KORB_EXC_P(ne.value))) {               /* NameError#name → :@@x, #receiver → the resolving class */
            slots[1] = ne.value;
            VALUE_REF eref = VALUE_REF_AT(&slots[1]);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__name", 7)), sym);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
            ne.value = slots[1];
        }
        return ne;
    }
    return RESULT_OK(korb_items_data(VAL2HASH(VAL2CLASS(owner)->cvars)->items)[2 * idx + 1]);
}

RESULT
korb_cvar_set(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, VALUE val)
{
    const VALUE cref = korb_cvar_scope_of(c, self, entry_cell);
    if (!KORB_CLASS_P(cref))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "class variable assignment from toplevel");
    { RESULT fr = korb_check_def_frozen(c, slots, cref); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* @@cvar = / class_variable_set on a frozen class → FrozenError */
    const VALUE sym = ID2SYM(sym_id);
    int32_t idx;
    VALUE target = korb_cvar_owner(cref, sym, &idx);   /* update existing ancestor, else define in cref */
    if (target == KORB_NIL) target = cref;

    /* Root target + val across the hash alloc/grow (both may GC/move). */
    VALUE_REF tref = SLOTS_PUSH(slots, target);
    VALUE_REF vref = SLOTS_PUSH(slots, val);
    if (VAL2CLASS(VALUE_REF_GET(tref))->cvars == KORB_NIL) {
        const VALUE h = UNWRAP(korb_hash_new(c, slots, 4));
        ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(tref)),
                  (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(tref))->cvars, h);
    }
    VALUE_REF href = SLOTS_PUSH(slots, VAL2CLASS(VALUE_REF_GET(tref))->cvars);
    VALUE_REF kref = SLOTS_PUSH(slots, sym);
    CHECK(korb_hash_set(c, slots, href, kref, VALUE_REF_GET(vref)));
    return RESULT_OK(VALUE_REF_GET(vref));
}

/* the `@<member>` ivar symbol for a Struct member symbol (`:x` → `:@x`). */
static VALUE korb_member_ivar_sym(struct korb_vm *vm, VALUE member_sym) {
    const char *nm = korb_sym_name(vm, SYM2ID(member_sym));
    char buf[256];
    snprintf(buf, sizeof buf, "@%s", nm);
    return ID2SYM(korb_intern(vm, buf, strlen(buf)));
}

/* Struct.new(*members) — build an anonymous class with attr_accessor per member
 * and the member list recorded (for positional .new).  Symbol or String members;
 * a trailing keyword_init: hash is ignored (minimal). */
static inline VALUE korb_builtin_class_obj(const struct korb_vm *vm, enum korb_class e);

static struct korb_method *korb_class_method_slot(KorbClass *k, uint32_t mid);   /* fwd (defined below) */
static void korb_check_basic_op_redef(CTX *c, VALUE klass, uint32_t mid);        /* fwd (defined below) */
/* add a CFUNC method to a specific class object (struct classes get these). */
static void korb_class_def_cfn(CTX *c, VALUE klass, const char *name, korb_method_fn fn, int32_t arity) {
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(klass), korb_intern(c->vm, name, strlen(name)));
    m->kind = KORB_METHOD_CFUNC; m->uses_block = 0; m->params_cnt = arity;
    m->rfn = fn; m->rbfn = NULL; m->body = NULL; m->owner = klass;
    c->vm->method_serial++;
}
static void korb_class_def_cfn_blk(CTX *c, VALUE klass, const char *name, korb_method_blk_fn fn, int32_t arity) {
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(klass), korb_intern(c->vm, name, strlen(name)));
    m->kind = KORB_METHOD_CFUNC; m->uses_block = 1; m->params_cnt = arity;
    m->rbfn = fn; m->rfn = NULL; m->body = NULL; m->owner = klass;
    c->vm->method_serial++;
}
/* Struct instance methods — read the receiver's class `members` + the matching
 * @ivars.  `members` is rooted in slots[1]; korb_ivar_get does not allocate. */
#define STRUCT_MEMBERS(selfref) VAL2CLASS(VAL2OBJ(VALUE_REF_GET(selfref))->klass)->members
static RESULT korb_m_struct_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK(LONG2FIX(VAL2ARY(STRUCT_MEMBERS(self))->len));   /* Struct#size/length = member count */
}
static RESULT korb_m_struct_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);                            /* members (rooted, below the alloc scratch) */
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));          /* result */
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(VAL2ARY(slots[0])->items)[i]);
        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_ivar_get(c, VALUE_REF_GET(self), iv)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Struct#values_at(*indices) → the member values at those (signed) indices. */
static RESULT korb_m_struct_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = STRUCT_MEMBERS(self);                            /* members (rooted) */
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 3, n));           /* the struct's values, in member order */
    VALUE_REF vals = VALUE_REF_AT(&slots[1]);
    for (uint32_t k = 0; k < n; k++) {
        const VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(VAL2ARY(slots[0])->items)[k]);
        CHECK(korb_ary_push_val(c, slots + 3, vals, korb_ivar_get(c, VALUE_REF_GET(self), iv)));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 3, VALUE_SLICE_LEN(a)));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const VALUE av = VALUE_SLICE_GET(a, j);
        if (KORB_RANGE_P(av)) {                                /* a Range picks each index in it (nil-filled, like Array) */
            const KorbRange *r = VAL2RANGE(av);
            korb_sword_t b = 0, e2, last;
            if (r->rbegin != KORB_NIL) { if (UNLIKELY(!korb_to_index(r->rbegin, &b))) return korb_raise(c, slots + 3, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (b < 0) b += n; }
            if (r->rend == KORB_NIL) last = (korb_sword_t)n - 1;
            else { if (UNLIKELY(!korb_to_index(r->rend, &e2))) return korb_raise(c, slots + 3, KORB_E_TYPE, 0, "no implicit conversion into Integer"); if (e2 < 0) e2 += n; last = r->exclude_end ? e2 - 1 : e2; }
            if (UNLIKELY(b < 0)) {                             /* a begin still negative after wrap → RangeError (CRuby) */
                slots[4] = av;                                 /* root the range across #to_s + raise */
                RESULT ts = korb_send(c, slots + 5, korb_intern(c->vm, "to_s", 4), 0, 0);
                if (UNLIKELY(ts.state != KORB_NORMAL)) return ts;
                slots[4] = ts.value;
                return korb_raise(c, slots + 5, KORB_E_RANGE, 0, "%s out of range", KORB_STRING_P(slots[4]) ? (const char *)korb_strbuf_data(VAL2STR(slots[4])->buf) : "range");
            }
            for (korb_sword_t i = b; i <= last; i++)
                CHECK(korb_ary_push_val(c, slots + 3, dst, (i >= 0 && (uint32_t)i < n) ? korb_items_data(VAL2ARY(VALUE_REF_GET(vals))->items)[i] : KORB_NIL));
            continue;
        }
        korb_sword_t idx;
        if (UNLIKELY(!korb_to_index(av, &idx)))
            return korb_raise(c, slots + 3, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(av));
        const korb_sword_t orig = idx;
        if (idx < 0) idx += n;
        if (UNLIKELY(idx < 0 || (uint32_t)idx >= n))
            return korb_raise(c, slots + 3, KORB_E_INDEX, 0, orig < 0 ? "offset %ld too small for struct(size:%u)" : "offset %ld too large for struct(size:%u)", (long)orig, n);
        CHECK(korb_ary_push_val(c, slots + 3, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(vals))->items)[idx]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_struct_deconstruct_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (uses to_h) */
static RESULT korb_m_struct_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_hash_new(c, slots + 1, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        slots[2] = korb_items_data(VAL2ARY(slots[0])->items)[i];              /* member sym (key, rooted) */
        VALUE iv = korb_member_ivar_sym(c->vm, slots[2]);
        slots[3] = korb_ivar_get(c, VALUE_REF_GET(self), iv);      /* value (rooted) */
        CHECK(korb_hash_set(c, slots + 4, dst, VALUE_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Struct/Data#to_h — with a block, each [k,v] pair is transformed via the block's
 * returned 2-element [k,v]. */
static RESULT korb_m_struct_to_h_blk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_struct_to_h(c, slots, self, a);
    slots[0] = STRUCT_MEMBERS(self);                          /* members (rooted) */
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_hash_new(c, slots + 2, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        const VALUE k = korb_items_data(VAL2ARY(slots[0])->items)[i];
        slots[2] = k;                                         /* block arg 0: key */
        slots[3] = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, k));   /* arg 1: value */
        RESULT yr = korb_block_yield(c, slots + 4, block, def_env, &slots[2], 2, cself);
        if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
        if (UNLIKELY(!KORB_ARRAY_P(yr.value))) {             /* coerce the yielded pair via #to_ary */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            slots[4] = yr.value;                             /* root before respond_to?/to_ary dispatch */
            if (KORB_OBJECT_P(slots[4]) && korb_responds_to_coerce(c, slots + 5, slots[4], to_ary)) {
                RESULT ar = korb_send_impl(c, slots + 5, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                yr.value = ar.value;
            } else { yr.value = slots[4]; }
            if (UNLIKELY(!KORB_ARRAY_P(yr.value)))
                return korb_raise(c, slots + 4, KORB_E_TYPE, 0, "wrong element type %s (expected array)", korb_type_name(yr.value));
        }
        if (UNLIKELY(VAL2ARY(yr.value)->len != 2))           /* wrong length → ArgumentError (not TypeError) */
            return korb_raise(c, slots + 4, KORB_E_ARGUMENT, 0, "element has wrong array length (expected 2, was %u)", VAL2ARY(yr.value)->len);
        slots[4] = korb_items_data(VAL2ARY(yr.value)->items)[0];         /* new key */
        slots[5] = korb_items_data(VAL2ARY(yr.value)->items)[1];         /* new value */
        CHECK(korb_hash_set(c, slots + 6, dst, VALUE_REF_AT(&slots[4]), slots[5]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* nil → full hash; an Array of keys → just those members (stops at the first
 * non-member, so a partial pattern-match fails as in CRuby). */
static RESULT korb_m_struct_deconstruct_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE keys = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (keys == KORB_NIL)
        return korb_m_struct_to_h(c, slots, self, VALUE_SLICE_MAKE(NULL, 0));
    if (!KORB_ARRAY_P(keys))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Array or nil)", korb_type_name(keys));
    slots[0] = STRUCT_MEMBERS(self);                           /* members (symbols), rooted */
    slots[1] = keys;                                           /* requested keys, rooted */
    /* Data names its members only: an Integer key is a TypeError there, and a
     * non-Symbol key converts through #to_str (Struct still indexes by position). */
    const VALUE sc_ = korb_class_obj_of(c, VALUE_REF_GET(self));
    const bool is_data = KORB_CLASS_P(sc_) && VAL2CLASS(sc_)->is_data;
    const uint32_t nk = VAL2ARY(slots[1])->len;
    slots[2] = UNWRAP(korb_hash_new(c, slots + 3, nk));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    if (nk > VAL2ARY(slots[0])->len) return RESULT_OK(VALUE_REF_GET(dst));   /* more keys than members → {} */
    for (uint32_t k = 0; k < nk; k++) {
        const VALUE key = korb_items_data(VAL2ARY(slots[1])->items)[k];
        const KorbArray *const mem = VAL2ARY(slots[0]);        /* re-read post-GC */
        /* resolve key → member symbol: Symbol matches directly, String by name,
         * Integer by position. */
        VALUE msym = KORB_NIL, outkey = key;   /* the hash keeps the key as given, except a #to_str conversion */
        if (SYMBOL_P(key)) msym = key;
        else if (KORB_STRING_P(key)) msym = ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(key)->buf), VAL2STR(key)->len));
        else if (is_data) {                                    /* Data: #to_str or TypeError */
            VALUE kv = key;
            if (KORB_OBJECT_P(kv) && korb_responds_to_coerce_p(c, slots + 5, &kv, korb_intern(c->vm, "to_str", 6))) {
                slots[5] = kv;
                RESULT sr = korb_send_impl(c, slots + 6, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (KORB_STRING_P(sr.value)) {
                    msym = ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(sr.value)->buf), VAL2STR(sr.value)->len));
                    outkey = sr.value;
                }
                else { char db[192]; korb_recv_desc(c, slots + 6, sr.value, db, sizeof db);
                       return korb_raise(c, slots + 5, KORB_E_TYPE, 0, "can't convert %s to Symbol (%s#to_str gives %s)",
                                         korb_type_name(key), korb_type_name(key), korb_type_name(sr.value)); }
            } else {
                char db[64];
                if (FIXNUM_P(key)) snprintf(db, sizeof db, "%ld", (long)FIX2LONG(key));
                else               snprintf(db, sizeof db, "%s", korb_type_name(key));
                return korb_raise(c, slots + 5, KORB_E_TYPE, 0, "%s is not a symbol nor a string", db);
            }
        }
        else if (FIXNUM_P(key)) {
            korb_sword_t idx = FIX2LONG(key);
            if (idx < 0) idx += mem->len;                      /* negative index from the end */
            if (idx < 0 || (uint32_t)idx >= mem->len) break;
            msym = korb_items_data(mem->items)[idx];
        }
        else {                                                 /* other key → #to_int index, else TypeError */
            VALUE kv = key;
            RESULT cr = korb_coerce_to_int(c, slots + 5, &kv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            korb_sword_t idx;
            if (UNLIKELY(!korb_to_index(kv, &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(key));
            const KorbArray *const mem2 = VAL2ARY(slots[0]);   /* re-read after coerce GC */
            if (idx < 0) idx += mem2->len;
            if (idx < 0 || (uint32_t)idx >= mem2->len) break;
            msym = korb_items_data(mem2->items)[idx];
        }
        bool found = false;
        { const KorbArray *const memf = VAL2ARY(slots[0]);     /* re-read (coerce path may have GC'd) */
          for (uint32_t m = 0; m < memf->len; m++) if (korb_items_data(memf->items)[m] == msym) { found = true; break; } }
        if (!found) break;
        slots[3] = outkey;                                     /* hash uses the key as given (or its #to_str result) */
        slots[4] = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, msym));
        CHECK(korb_hash_set(c, slots + 5, dst, VALUE_REF_AT(&slots[3]), slots[4]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Struct/Data#hash — deterministic over class + member values (so #eql? values
 * hash equal).  Not bit-compatible with CRuby, only self-consistent. */
static RESULT korb_m_struct_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const VALUE klass = VAL2OBJ(VALUE_REF_GET(self))->klass;
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)VAL2CLASS(klass)->serial; h *= 1099511628211ULL;   /* per-class identity (distinguishes anonymous Struct/Data classes) */
    const KorbArray *const mem = VAL2ARY(VAL2CLASS(klass)->members);
    for (uint32_t i = 0; i < mem->len; i++) {
        const VALUE iv = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i]));
        h ^= korb_value_hash(iv); h *= 1099511628211ULL;
    }
    return RESULT_OK(LONG2FIX((korb_sword_t)(h & 0x3FFFFFFFFFFFFFFFULL)));
}
static RESULT korb_m_struct_members(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(slots[0])->items)[i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* class-level `Rec.members` — self IS the Struct class; copy its member array. */
static RESULT korb_m_struct_class_members(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VAL2CLASS(VALUE_REF_GET(self))->members;
    if (slots[0] == KORB_NIL) return korb_ary_new(c, slots, 0);
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(slots[0])->items)[i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* StructClass#keyword_init? — true / false (explicit) / nil (unspecified). */
static RESULT korb_m_struct_keyword_init_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const uint8_t ki = VAL2CLASS(VALUE_REF_GET(self))->struct_kwinit;
    return RESULT_OK(ki == 1 ? KORB_TRUE : (ki == 2 ? KORB_FALSE : KORB_NIL));
}
static RESULT korb_m_struct_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE mems = STRUCT_MEMBERS(self);
    const KorbArray *mem = VAL2ARY(mems);
    VALUE k = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(k) || KORB_FLOAT_P(k)) {                /* integer/Float(truncated) position */
        const korb_sword_t orig = FIXNUM_P(k) ? FIX2LONG(k) : (korb_sword_t)korb_float_val(k);
        korb_sword_t i = orig; if (i < 0) i += mem->len;
        if (UNLIKELY(i < 0 || (uint32_t)i >= mem->len)) return korb_raise(c, slots, KORB_E_INDEX, 0, orig < 0 ? "offset %ld too small for struct(size:%u)" : "offset %ld too large for struct(size:%u)", (long)orig, mem->len);
        return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i])));
    }
    if (UNLIKELY(!SYMBOL_P(k) && !KORB_STRING_P(k)))      /* not Integer/Float/Symbol/String → no implicit Integer conversion */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(k));
    uint32_t sym = SYMBOL_P(k) ? SYM2ID(k) : korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len);
    for (uint32_t i = 0; i < mem->len; i++)
        if (SYM2ID(korb_items_data(mem->items)[i]) == sym) return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i])));
    return korb_raise(c, slots, KORB_E_NAME, 0, "no member '%s' in struct", korb_sym_name(c->vm, sym));
}
/* Struct/Data#dig(key, *rest) — self[key], then recurse #dig on the result.
 * Lenient like CRuby's rb_struct_lookup: a non-member name digs to nil. */
static RESULT korb_m_struct_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    const VALUE key = VALUE_SLICE_GET(a, 0);
    RESULT first;
    if (SYMBOL_P(key) || KORB_STRING_P(key)) {               /* non-member name → nil (not NameError, unlike #[]) */
        const uint32_t sym = SYMBOL_P(key) ? SYM2ID(key) : korb_intern(c->vm, korb_strbuf_data(VAL2STR(key)->buf), VAL2STR(key)->len);
        const KorbArray *mem = VAL2ARY(STRUCT_MEMBERS(self));   /* read after intern (may GC) */
        first = RESULT_OK(KORB_NIL);
        for (uint32_t i = 0; i < mem->len; i++)
            if (SYM2ID(korb_items_data(mem->items)[i]) == sym) { first = RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i]))); break; }
    }
    else {                                                    /* numeric index → struct_aref (IndexError on out-of-range) */
        slots[0] = key;
        first = korb_m_struct_aref(c, slots + 1, self, VALUE_SLICE_MAKE(&slots[0], 1));
        if (UNLIKELY(first.state != KORB_NORMAL)) return first;
    }
    const uint32_t na = VALUE_SLICE_LEN(a);
    if (na == 1 || first.value == KORB_NIL) return first;
    slots[0] = first.value;                                  /* receiver for the recursive dig */
    const uint32_t mid_dig = korb_intern(c->vm, "dig", 3);
    if (UNLIKELY(!korb_responds_to(c, slots[0], mid_dig)))   /* CRuby: intermediate must respond to #dig */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(slots[0]));
    for (uint32_t i = 1; i < na; i++) slots[i] = VALUE_SLICE_GET(a, i);
    return korb_send_impl(c, slots + na, mid_dig, 0, na - 1, NULL, NULL, NULL);
}
static RESULT korb_m_struct_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));     /* []= on a frozen struct → FrozenError */
    const KorbArray *mem = VAL2ARY(STRUCT_MEMBERS(self));
    VALUE k = VALUE_SLICE_GET(a, 0), v = VALUE_SLICE_GET(a, 1);
    korb_sword_t idx = -1;
    if (FIXNUM_P(k) || KORB_FLOAT_P(k)) {                 /* integer position → IndexError when out of range */
        korb_sword_t i = FIXNUM_P(k) ? FIX2LONG(k) : (korb_sword_t)korb_float_val(k);
        const korb_sword_t orig = i; if (i < 0) i += mem->len;
        if (UNLIKELY(i < 0 || (uint32_t)i >= mem->len)) return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %ld too large for struct(size:%u)", (long)orig, mem->len);
        idx = i;
    } else if (SYMBOL_P(k) || KORB_STRING_P(k)) {         /* member name → NameError when unknown */
        const uint32_t sym = SYMBOL_P(k) ? SYM2ID(k) : korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len);
        for (uint32_t i = 0; i < mem->len; i++) if (SYM2ID(korb_items_data(mem->items)[i]) == sym) { idx = i; break; }
        if (UNLIKELY(idx < 0)) return korb_raise(c, slots, KORB_E_NAME, 0, "no member '%s' in struct", korb_sym_name(c->vm, sym));
    } else {                                              /* neither Integer/Float nor a name → no implicit Integer conversion */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(k));
    }
    slots[0] = v;
    CHECK(korb_ivar_set(c, slots + 1, self, korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[idx]), slots[0]));
    return RESULT_OK(slots[0]);
}
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);                /* fwd (enumerator.c) */
static RESULT korb_enum_gen_new(CTX *c, VALUE *slots, VALUE proc, VALUE size);            /* fwd (enumerator.c) */
static RESULT korb_lazy_gen_new(CTX *c, VALUE *slots, VALUE proc, bool src_inf);          /* fwd (enumerator.c) */
static RESULT korb_enum_gen_run(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t limit);    /* fwd (enumerator.c) */
static RESULT korb_m_yielder_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (enumerator.c) */
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);         /* fwd */
static RESULT korb_m_struct_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) {
        slots[0] = UNWRAP(korb_m_struct_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    slots[1] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[1])->len;
    for (uint32_t i = 0; i < n; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(VAL2ARY(slots[1])->items)[i]);
        VALUE val = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &val, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_struct_each_pair(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) {                                          /* no block → Enumerator over [member, value] pairs */
        slots[0] = STRUCT_MEMBERS(self);                         /* members (rooted) */
        const uint32_t n = VAL2ARY(slots[0])->len;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));        /* pairs (rooted) */
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < n; i++) {
            const VALUE msym = korb_items_data(VAL2ARY(slots[0])->items)[i];                       /* Symbol (immediate) */
            slots[2] = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, msym));   /* value (rooted) */
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));    /* [member, value] */
            VALUE_REF pair = VALUE_REF_AT(&slots[3]);
            CHECK(korb_ary_push_val(c, slots + 4, pair, msym));
            CHECK(korb_ary_push_val(c, slots + 4, pair, slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, dst, VALUE_REF_GET(pair)));
        }
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "each_pair"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(dst), slots[2]);
    }
    slots[1] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[1])->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE msym = korb_items_data(VAL2ARY(slots[1])->items)[i];
        VALUE argv[2] = { msym, korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, msym)) };
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_struct_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) {
        slots[0] = UNWRAP(korb_m_struct_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "map"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    slots[0] = STRUCT_MEMBERS(self);                         /* members (rooted, below alloc scratch) */
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));        /* result (rooted) */
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(VAL2ARY(slots[0])->items)[i]);
        VALUE val = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &val, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_struct_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o) || VAL2OBJ(o)->klass != VAL2OBJ(VALUE_REF_GET(self))->klass) return RESULT_OK(KORB_FALSE);
    const KorbArray *mem = VAL2ARY(STRUCT_MEMBERS(self));
    for (uint32_t i = 0; i < mem->len; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i]);
        const VALUE sv = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        const VALUE ov = korb_ivar_get(c, o, iv);
        if (sv == VALUE_REF_GET(self) && ov == o) continue;   /* direct self-reference at the same position → equal (breaks the cycle, CRuby-style) */
        if (!korb_value_eq(sv, ov)) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(KORB_TRUE);
}
/* Struct#eql? — like ==, but members are compared type-strictly (1 eql? 1.0 => false). */
static RESULT korb_m_struct_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o) || VAL2OBJ(o)->klass != VAL2OBJ(VALUE_REF_GET(self))->klass) return RESULT_OK(KORB_FALSE);
    const KorbArray *mem = VAL2ARY(STRUCT_MEMBERS(self));
    for (uint32_t i = 0; i < mem->len; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[i]);
        const VALUE sv = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        const VALUE ov = korb_ivar_get(c, o, iv);
        if (sv == VALUE_REF_GET(self) && ov == o) continue;   /* direct self-reference at the same position → equal */
        if (!korb_value_eql(sv, ov)) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_class_new_bracket(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd */
static RESULT korb_m_struct_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd */
static RESULT korb_m_struct_ivars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);     /* fwd (defined in symbol.c) */
RESULT korb_do_include(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods);   /* fwd (defined below) */
/* Struct#initialize(*values | member: v, …) — assign members positionally (a
 * shortfall leaves the rest nil; more than N → "struct size differs"), or by
 * keyword for a keyword_init struct.  Registered on every Struct class so a
 * subclass's custom #initialize can reach the member assignment via super(...). */
static RESULT korb_m_struct_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    const VALUE klass0 = VAL2OBJ(VALUE_REF_GET(self))->klass;
    slots[2] = VAL2CLASS(klass0)->members;                                /* member syms (rooted) */
    const uint32_t mlen = VAL2ARY(slots[2])->len;
    const uint32_t argc = VALUE_SLICE_LEN(a);
    const bool kwinit = VAL2CLASS(klass0)->struct_kwinit == 1 && argc >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, argc - 1));
    if (kwinit) {
        slots[0] = VALUE_SLICE_GET(a, argc - 1);                          /* kwargs hash (rooted) */
        const KorbHash *const kh = VAL2HASH(slots[0]);
        for (uint32_t hi = 0; hi < kh->len; hi++) {                       /* reject keywords that name no member */
            const VALUE key = korb_items_data(kh->items)[2 * hi];
            bool found = false;
            const KorbArray *const mm = VAL2ARY(slots[2]);
            for (uint32_t mi = 0; mi < mm->len; mi++) if (korb_items_data(mm->items)[mi] == key) { found = true; break; }
            if (UNLIKELY(!found))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unknown keywords: %s",
                                  SYMBOL_P(key) ? korb_sym_name(vm, SYM2ID(key)) : korb_type_name(key));
        }
    } else if (UNLIKELY(argc > mlen)) {
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "struct size differs");
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *const mem = VAL2ARY(VAL2CLASS(VAL2OBJ(VALUE_REF_GET(self))->klass)->members);   /* re-read (ivar_set GCs) */
        if (i >= mem->len) break;
        const VALUE iv = korb_member_ivar_sym(vm, korb_items_data(mem->items)[i]);
        if (kwinit) { int32_t hi = korb_hash_find(VAL2HASH(slots[0]), korb_items_data(mem->items)[i]); slots[1] = hi >= 0 ? korb_items_data(VAL2HASH(slots[0])->items)[2 * hi + 1] : KORB_NIL; }
        else slots[1] = (i < argc) ? VALUE_SLICE_GET(a, i) : KORB_NIL;
        CHECK(korb_ivar_set(c, slots + 3, self, iv, slots[1]));
    }
    return RESULT_OK(KORB_NIL);
}
/* Common Struct instance methods (read members + @ivars generically).  These go
 * on the base Struct class, once, so `Struct.new(:a) { include M }` lets M
 * override them (CRuby defines them on Struct too).  `pk` is a rooted slot:
 * korb_class_def_cfn interns the name → may GC → re-read the class each call. */
static void korb_def_struct_common(CTX *c, const VALUE *pk) {
    korb_class_def_cfn(c, *pk, "to_a", korb_m_struct_to_a, 0);
    /* no #to_ary: a Struct is not implicitly an Array in CRuby (it must not
     * splat in massign / block params / puts) */
    korb_class_def_cfn(c, *pk, "values", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, *pk, "size", korb_m_struct_size, 0);
    korb_class_def_cfn(c, *pk, "length", korb_m_struct_size, 0);
    korb_class_def_cfn(c, *pk, "deconstruct", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, *pk, "values_at", korb_m_struct_values_at, -1);
    korb_class_def_cfn(c, *pk, "dig", korb_m_struct_dig, -1);
    korb_class_def_cfn(c, *pk, "deconstruct_keys", korb_m_struct_deconstruct_keys, 1);
    korb_class_def_cfn_blk(c, *pk, "to_h", korb_m_struct_to_h_blk, 0);
    korb_class_def_cfn(c, *pk, "members", korb_m_struct_members, 0);
    korb_class_def_cfn(c, *pk, "[]", korb_m_struct_aref, 1);
    korb_class_def_cfn(c, *pk, "[]=", korb_m_struct_aset, 2);
    korb_class_def_cfn(c, *pk, "==", korb_m_struct_eq, 1);
    korb_class_def_cfn(c, *pk, "inspect", korb_m_struct_inspect, 0);
    korb_class_def_cfn(c, *pk, "instance_variables", korb_m_struct_ivars, 0);
    korb_class_def_cfn(c, *pk, "to_s", korb_m_struct_inspect, 0);
    korb_class_def_cfn(c, *pk, "eql?", korb_m_struct_eql, 1);
    korb_class_def_cfn(c, *pk, "hash", korb_m_struct_hash, 0);
    korb_class_def_cfn_blk(c, *pk, "each", korb_m_struct_each, 0);
    korb_class_def_cfn_blk(c, *pk, "each_pair", korb_m_struct_each_pair, 0);
    korb_class_def_cfn_blk(c, *pk, "map", korb_m_struct_map, 0);
    korb_class_def_cfn_blk(c, *pk, "collect", korb_m_struct_map, 0);
    korb_class_def_cfn(c, *pk, "initialize", korb_m_struct_initialize, -1);
}

/* `base` = the class .new was called on (Struct itself, or a Struct subclass
 * used as a factory like `class Apple < Struct`).  The new struct class is a
 * subclass of `base` and, when named, a constant under `base`. */
static RESULT korb_struct_define(CTX *c, VALUE *slots, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE base) {
    struct korb_vm *const vm = c->vm;
    uint32_t mstart = 0, name_id = 0; bool has_name = false;   /* String/nil first arg = the constant name slot */
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE first = VALUE_SLICE_GET(a, 0);
        if (!KORB_STRING_P(first) && first != KORB_NIL && !SYMBOL_P(first) && KORB_OBJECT_P(first)) {   /* coerce a #to_str first arg → constant name */
            const uint32_t to_str = korb_intern(vm, "to_str", 6);
            if (korb_responds_to_coerce_p(c, slots, &first, to_str)) {
                slots[0] = first;
                RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (KORB_STRING_P(sr.value)) first = sr.value;
            }
        }
        if (KORB_STRING_P(first)) {
            const KorbString *const ns = VAL2STR(first);
            bool valid = ns->len > 0 && korb_strbuf_data(ns->buf)[0] >= 'A' && korb_strbuf_data(ns->buf)[0] <= 'Z';
            for (uint32_t k = 1; valid && k < ns->len; k++) { const char ch = korb_strbuf_data(ns->buf)[k]; if (!(isalnum((unsigned char)ch) || ch == '_')) valid = false; }
            if (UNLIKELY(!valid)) return korb_raise(c, slots, KORB_E_NAME, 0, "identifier %.*s needs to be constant", (int)ns->len, korb_strbuf_data(ns->buf));
            name_id = korb_intern(vm, korb_strbuf_data(ns->buf), ns->len); has_name = true; mstart = 1;
        } else if (first == KORB_NIL) {
            mstart = 1;                                       /* explicit anonymous */
        }
    }
    { VALUE st = base;                                                 /* anon class, super = base (Struct or a Struct subclass) */
      if (!KORB_CLASS_P(st)) st = korb_const_get(vm, korb_intern(vm, "Struct", 6));
      if (!KORB_CLASS_P(st)) st = korb_builtin_class_obj(vm, KORB_C_OBJECT);
      slots[0] = UNWRAP(korb_class_new(c, slots, 0, st)); }
    VALUE_REF cls = VALUE_REF_AT(&slots[0]);
    slots[1] = korb_const_get(vm, korb_intern(vm, "Enumerable", 10));   /* Struct includes Enumerable */
    { RESULT ir = korb_do_include(c, slots + 2, VALUE_REF_GET(cls), VALUE_SLICE_MAKE(&slots[1], 1)); if (UNLIKELY(ir.state != KORB_NORMAL)) return ir; }
    uint8_t kwinit = 0;   /* 0 = unspecified (→ keyword_init? nil), 1 = true, 2 = explicit false */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF mem = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = mstart; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_HASH_P(sym)) {                               /* trailing keyword_init: true */
            const VALUE kw_sym = ID2SYM(korb_intern(vm, "keyword_init", 12));
            const KorbHash *const h = VAL2HASH(sym);
            /* only keyword_init: is allowed; a plain Hash literal is a bad
             * member (TypeError), keyword syntax is a bad keyword. */
            const bool kw_syntax = (((const AroObjectHeader *)(uintptr_t)sym)->flags & KORB_FL_KWARGS) != 0;
            for (uint32_t j = 0; j < h->len; j++)
                if (korb_items_data(h->items)[2 * j] != kw_sym) {
                    const VALUE bad = korb_items_data(h->items)[2 * j];
                    if (!kw_syntax)
                        return korb_raise_not_sym(c, slots, sym);
                    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unknown keyword: :%s",
                                      SYMBOL_P(bad) ? korb_sym_name(vm, SYM2ID(bad)) : korb_type_name(bad));
                }
            int32_t ki = korb_hash_find(h, kw_sym);
            if (ki >= 0) {                                   /* keyword_init: true → 1, false → 2 (explicit), nil → 0 (unspecified) */
                const VALUE kv = korb_items_data(VAL2HASH(sym)->items)[2*ki+1];
                kwinit = (kv == KORB_NIL) ? 0 : (KORB_TRUTHY(kv) ? 1 : 2);
            }
            continue;
        }
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, korb_strbuf_data(VAL2STR(sym)->buf), VAL2STR(sym)->len));
        if (UNLIKELY(!SYMBOL_P(sym)))                         /* member must be a Symbol/String */
            return korb_raise_not_sym(c, slots, VALUE_SLICE_GET(a, i));
        {   /* duplicate member → ArgumentError */
            const KorbArray *const mm = VAL2ARY(VALUE_REF_GET(mem));
            for (uint32_t k = 0; k < mm->len; k++)
                if (korb_items_data(mm->items)[k] == sym)
                    return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "duplicate member: %s", korb_sym_name(vm, SYM2ID(sym)));
        }
        CHECK(korb_ary_push_val(c, slots + 2, mem, sym));   /* accessors are defined below, after the common methods */
    }
    if (has_name) {                                                   /* .new("Name",..) → base::Name (nested under the factory class) */
        /* `base` is a bare parameter and the member loop above allocated, so it
         * may have moved: take the namespace from the new class's superclass. */
        VALUE owner = VAL2CLASS(VALUE_REF_GET(cls))->superclass;
        if (!KORB_CLASS_P(owner)) owner = korb_const_get(vm, korb_intern(vm, "Struct", 6));
        korb_const_define_owned(c, name_id, VALUE_REF_GET(cls), KORB_CLASS_P(owner) ? owner : KORB_NIL);
    }
    ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(cls)), (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(cls))->members, VALUE_REF_GET(mem));
    /* The common Struct instance methods live on the base Struct class (see
     * korb_def_struct_common), not here: a module `include`d into the struct
     * body must be able to override them, as in CRuby. */
    /* Member accessors are defined LAST so a member named like a built-in
     * (:hash/:each/:size/:members/…) keeps its accessor — CRuby defines the
     * accessors on the anonymous subclass, shadowing the inherited methods. */
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(mem))->len; i++) {
        const VALUE sym = korb_items_data(VAL2ARY(VALUE_REF_GET(mem))->items)[i];
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256];
        snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, nm, strlen(nm)), ivar, 0);   /* reader */
        snprintf(buf, sizeof buf, "%s=", nm); korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, buf, strlen(buf)), ivar, 1);  /* writer */
    }
    VAL2CLASS(VALUE_REF_GET(cls))->struct_kwinit = kwinit;
    /* class-level `Rec.members`: install on the class's singleton (name already
     * interned above → no GC in def_cfn). */
    slots[2] = VALUE_REF_GET(cls);                            /* root class across singleton alloc */
    slots[3] = UNWRAP(korb_obj_singleton(c, slots + 4, slots[2]));
    korb_class_def_cfn(c, slots[3], "members", korb_m_struct_class_members, 0);
    korb_class_def_cfn(c, slots[3], "keyword_init?", korb_m_struct_keyword_init_p, 0);
    korb_class_def_cfn(c, slots[3], "[]", korb_m_class_new_bracket, -1);   /* Rec[...] == Rec.new(...) */
    if (block != NULL) {                                      /* Struct.new(...) do ... end → class-body methods */
        slots[2] = VALUE_REF_GET(cls);                       /* root the class as the block's self/cref */
        const VALUE saved_definee = c->def_definee;   /* a class body: `def` lands on the class */
        c->def_definee = KORB_NIL;
        slots[3] = slots[2];                                 /* CRuby passes the new class to the block */
        RESULT br = korb_block_yield(c, slots + 4, block, def_env, &slots[3], 1, &slots[2]);
        c->def_definee = saved_definee;
        if (br.state == KORB_BREAK && !korb_break_owned(c, block, def_env)) return br;   /* someone else's break passes through */
        if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
    }
    return RESULT_OK(VALUE_REF_GET(cls));
}

/* ---- Data.define (Ruby 3.2+ immutable value class) ---------------------- */
/* true if every key of `h` is a Symbol that names a member of data class `k`. */
static bool korb_data_all_keys_members(const struct korb_vm *vm, const KorbClass *k, const KorbHash *h) {
    (void)vm;
    const KorbArray *mem = VAL2ARY(k->members);
    if (h->len == 0) return mem->len == 0;
    for (uint32_t i = 0; i < h->len; i++) {
        VALUE key = korb_items_data(h->items)[2 * i];
        bool found = false;
        for (uint32_t j = 0; j < mem->len; j++) if (korb_items_data(mem->items)[j] == key) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

/* Data#initialize(*values | **kwargs) — assign the immutable members.  Registered
 * on every Data class so `allocate` + an explicit #initialize call, and user
 * overrides that call `super`, both work.  (Data.new has its own fast path that
 * sets the members directly; this method backs the reflective / override routes.) */
static RESULT korb_m_data_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    slots[2] = VAL2CLASS(VAL2OBJ(VALUE_REF_GET(self))->klass)->members;   /* member syms (rooted) */
    const uint32_t mlen = VAL2ARY(slots[2])->len;
    const uint32_t argc = VALUE_SLICE_LEN(a);
    const bool kw = (argc == 1 && KORB_HASH_P(VALUE_SLICE_GET(a, 0)) &&
                     korb_data_all_keys_members(vm, VAL2CLASS(VAL2OBJ(VALUE_REF_GET(self))->klass), VAL2HASH(VALUE_SLICE_GET(a, 0))));
    slots[0] = kw ? VALUE_SLICE_GET(a, 0) : KORB_NIL;                     /* kwargs hash (rooted) */
    if (kw) {
        for (uint32_t k = 0; k < VAL2ARY(slots[2])->len; k++)            /* missing keyword */
            if (UNLIKELY(korb_hash_find(VAL2HASH(slots[0]), korb_items_data(VAL2ARY(slots[2])->items)[k]) < 0))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "missing keyword: :%s", korb_sym_name(vm, SYM2ID(korb_items_data(VAL2ARY(slots[2])->items)[k])));
    } else if (UNLIKELY(argc != mlen)) {
        const KorbArray *const mm = VAL2ARY(slots[2]);
        if (argc < mlen) {                                               /* shortfall → the unfilled members are missing keywords */
            char buf[512]; int off = snprintf(buf, sizeof buf, "missing keyword%s:", (mlen - argc) > 1 ? "s" : "");
            for (uint32_t i = argc; i < mm->len && off < (int)sizeof buf; i++)
                off += snprintf(buf + off, sizeof buf - off, "%s :%s", i > argc ? "," : "", korb_sym_name(vm, SYM2ID(korb_items_data(mm->items)[i])));
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "%s", buf);
        }
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..%u)", argc, mlen);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *const mem = VAL2ARY(slots[2]);                  /* re-read (ivar_set may GC) */
        if (i >= mem->len) break;
        const VALUE iv = korb_member_ivar_sym(vm, korb_items_data(mem->items)[i]);
        if (kw) { int32_t hi = korb_hash_find(VAL2HASH(slots[0]), korb_items_data(mem->items)[i]); slots[1] = hi >= 0 ? korb_items_data(VAL2HASH(slots[0])->items)[2*hi+1] : KORB_NIL; }
        else slots[1] = VALUE_SLICE_GET(a, i);
        CHECK(korb_ivar_set(c, slots + 3, self, iv, slots[1]));
    }
    return RESULT_OK(KORB_NIL);
}
/* Data#with(**changes) → a fresh instance with the named members replaced. */
static RESULT korb_m_data_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t alen = VALUE_SLICE_LEN(a);                 /* keyword changes arrive as a trailing Hash */
    const bool last_hash = alen >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, alen - 1));
    const uint32_t positional = alen - (last_hash ? 1u : 0u);
    if (UNLIKELY(positional > 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0)", positional);
    slots[0] = VAL2OBJ(VALUE_REF_GET(self))->klass;          /* the data class (rooted) */
    slots[1] = last_hash ? VALUE_SLICE_GET(a, alen - 1) : KORB_NIL;
    if (slots[1] != KORB_NIL) {                              /* normalize String keyword keys → Symbols */
        const KorbHash *const h0 = VAL2HASH(slots[1]);
        bool has_str = false;
        for (uint32_t j = 0; j < h0->len; j++) if (KORB_STRING_P(korb_items_data(h0->items)[2 * j])) { has_str = true; break; }
        if (has_str) {
            slots[3] = slots[1];
            slots[4] = UNWRAP(korb_hash_new(c, slots + 4, VAL2HASH(slots[3])->len));
            for (uint32_t j = 0; j < VAL2HASH(slots[3])->len; j++) {
                const VALUE k = korb_items_data(VAL2HASH(slots[3])->items)[2 * j];
                slots[5] = KORB_STRING_P(k) ? ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len)) : k;
                slots[6] = korb_items_data(VAL2HASH(slots[3])->items)[2 * j + 1];
                CHECK(korb_hash_set(c, slots + 7, VALUE_REF_AT(&slots[4]), VALUE_REF_AT(&slots[5]), slots[6]));
            }
            slots[1] = slots[4];
        }
    }
    slots[2] = UNWRAP(korb_obj_new(c, slots + 2, slots[0]));  /* new instance (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *mem = VAL2ARY(VAL2CLASS(slots[0])->members);   /* re-read (ivar_set may GC) */
        if (i >= mem->len) break;
        VALUE msym = korb_items_data(mem->items)[i];
        VALUE iv = korb_member_ivar_sym(c->vm, msym);
        VALUE val;
        if (slots[1] != KORB_NIL) {
            int32_t hi = korb_hash_find(VAL2HASH(slots[1]), msym);
            val = hi >= 0 ? korb_items_data(VAL2HASH(slots[1])->items)[2 * hi + 1] : korb_ivar_get(c, VALUE_REF_GET(self), iv);
        } else {
            val = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        }
        slots[3] = val;                                       /* root across ivar_set */
        CHECK(korb_ivar_set(c, slots + 4, VALUE_REF_AT(&slots[2]), iv, slots[3]));
    }
    return RESULT_OK(slots[2]);
}

/* Data#inspect → "#<data Name member=val, ...>" (anonymous → no Name). */
/* "#<KIND[ Name] m1=v1, m2=v2>" — shared by Data#inspect and Struct#inspect/to_s. */
static void korb_fprint_inspect_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth);   /* fwd (defined far below) */
static bool korb_fprint_class_qname(CTX *c, FILE *fp, VALUE cls);                         /* fwd */
static RESULT korb_struct_inspect_impl(CTX *c, VALUE *slots, VALUE_REF self, const char *kind) {
    const VALUE klass = VAL2OBJ(VALUE_REF_GET(self))->klass;
    const KorbClass *const k = VAL2CLASS(klass);
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    fputc('#', ms); fputc('<', ms); fputs(kind, ms);
    if (k->name_sym) { fputc(' ', ms); korb_fprint_class_qname(c, ms, klass); }   /* qualified name (not the #name method) */
    const KorbArray *const mem = VAL2ARY(k->members);
    for (uint32_t i = 0; i < mem->len; i++) {                 /* no GC in this loop (fprint writes to FILE) */
        const VALUE msym = korb_items_data(mem->items)[i];
        const VALUE val = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, msym));
        fputs(i == 0 ? " " : ", ", ms);
        fputs(korb_sym_name(c->vm, SYM2ID(msym)), ms);
        fputc('=', ms);
        if (val == VALUE_REF_GET(self)) {                        /* direct self-reference → "#<kind QualName:...>" (CRuby cycle form) */
            fputc('#', ms); fputc('<', ms); fputs(kind, ms);
            fputc(' ', ms);
            if (k->name_sym) korb_fprint_class_qname(c, ms, klass); else korb_fprint_inspect_d(c, NULL, ms, klass, 1);
            fputs(":...>", ms);
        } else {
            korb_fprint_inspect_d(c, NULL, ms, val, 1);          /* _d → nested Struct/Data fields render in full (NULL: keep this loop GC-free) */
        }
    }
    fputc('>', ms);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_data_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)a; return korb_struct_inspect_impl(c, slots, self, "data"); }
static RESULT korb_m_struct_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_struct_inspect_impl(c, slots, self, "struct"); }

/* Data.define(*members [, &block]) → an anonymous immutable value class. */
/* class-level `[]` constructor: Klass[a, b, ...] == Klass.new(a, b, ...).  Shared
 * by Struct and Data value classes; forwards positional + trailing-kwargs args. */
static RESULT korb_m_class_new_bracket(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t argc = VALUE_SLICE_LEN(a);
    slots[0] = VALUE_REF_GET(self);                       /* receiver */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    return korb_send(c, slots + 1 + argc, korb_intern(c->vm, "new", 3), 0, argc);
}
/* Common Data instance methods — on the base Data class, once (see
 * korb_def_struct_common for why).  `pk` is a rooted slot. */
static void korb_def_data_common(CTX *c, const VALUE *pk) {
    korb_class_def_cfn_blk(c, *pk, "to_h", korb_m_struct_to_h_blk, 0);
    korb_class_def_cfn(c, *pk, "deconstruct_keys", korb_m_struct_deconstruct_keys, 1);   /* requires exactly one arg (keys Array | nil) */
    korb_class_def_cfn(c, *pk, "members", korb_m_struct_members, 0);
    korb_class_def_cfn(c, *pk, "to_a", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, *pk, "deconstruct", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, *pk, "==", korb_m_struct_eq, 1);
    korb_class_def_cfn(c, *pk, "eql?", korb_m_struct_eql, 1);
    korb_class_def_cfn(c, *pk, "hash", korb_m_struct_hash, 0);
    korb_class_def_cfn(c, *pk, "with", korb_m_data_with, -1);
    korb_class_def_cfn(c, *pk, "inspect", korb_m_data_inspect, 0);
    korb_class_def_cfn(c, *pk, "instance_variables", korb_m_struct_ivars, 0);
    korb_class_def_cfn(c, *pk, "to_s", korb_m_data_inspect, 0);
    korb_class_def_cfn(c, *pk, "initialize", korb_m_data_initialize, -1);
}
static RESULT korb_data_define(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)self; (void)cself;
    struct korb_vm *const vm = c->vm;
    { VALUE dt = korb_const_get(vm, korb_intern(vm, "Data", 4));        /* anon class, super Data (is_a?(Data)) */
      if (!KORB_CLASS_P(dt)) dt = korb_builtin_class_obj(vm, KORB_C_OBJECT);
      slots[0] = UNWRAP(korb_class_new(c, slots, 0, dt)); }
    VALUE_REF cls = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF mem = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, korb_strbuf_data(VAL2STR(sym)->buf), VAL2STR(sym)->len));
        if (!SYMBOL_P(sym)) continue;
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256];
        snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, nm, strlen(nm)), ivar, 0);   /* reader only (immutable) */
        CHECK(korb_ary_push_val(c, slots + 2, mem, sym));
    }
    ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(cls)), (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(cls))->members, VALUE_REF_GET(mem));
    VAL2CLASS(VALUE_REF_GET(cls))->is_data = 1;
    /* the common Data instance methods live on the base Data class (see
     * korb_def_data_common) so an `include`d module can override them */
    slots[2] = VALUE_REF_GET(cls);                            /* root across singleton alloc */
    slots[3] = UNWRAP(korb_obj_singleton(c, slots + 4, slots[2]));
    korb_class_def_cfn(c, slots[3], "members", korb_m_struct_class_members, 0);
    korb_class_def_cfn(c, slots[3], "[]", korb_m_class_new_bracket, -1);   /* D[...] == D.new(...) */
    if (block != NULL) {                                      /* Data.define(...) do ... end → class-body methods */
        slots[2] = VALUE_REF_GET(cls);
        const VALUE saved_definee = c->def_definee;   /* a class body: `def` lands on the class */
        c->def_definee = KORB_NIL;
        RESULT br = korb_block_yield(c, slots + 3, block, def_env, NULL, 0, &slots[2]);
        c->def_definee = saved_definee;
        if (br.state == KORB_BREAK && !korb_break_owned(c, block, def_env)) return br;   /* someone else's break passes through */
        if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
    }
    return RESULT_OK(VALUE_REF_GET(cls));
}

/* ---- Regexp (matching via astrogre through koruby_regex.so → libastrogre.so) --
 * The bridge (regex_bridge.c) exposes koruby_re_exec (per-group captures),
 * koruby_re_ngroups, koruby_re_named, koruby_re_valid. */
#define KORB_RE_MAX_GROUPS 32
typedef struct { int matched; int n_groups; long starts[KORB_RE_MAX_GROUPS]; long ends[KORB_RE_MAX_GROUPS]; } korb_re_match_t;
typedef int (*korb_re_exec_fn_t)(const char *, size_t, unsigned, const char *, size_t, size_t, korb_re_match_t *);
typedef const char *(*korb_re_named_fn_t)(const char *, size_t, unsigned, int, int *);
typedef int (*korb_re_valid_fn_t)(const char *, size_t, unsigned);
/* Tell astrogre the C-stack floor of the fiber/thread we currently run on, so
 * its \g<> recursion guard fails gracefully instead of overflowing.  Cheap; no
 * regex engine load is forced (skips if the bridge was never dlopen'd). */
void korb_re_sync_floor(CTX *c) {
    if (c->vm == NULL) return;   /* called from korb_ctx_new before the VM exists */
    void *const fn = c->vm->re_floor_fn;
    if (fn && fn != (void *)(korb_sword_t)-1)
        ((void (*)(const void *))fn)(c->cstack_limit);
}
/* The engine's message for the last failed compile (NULL if unavailable). */
/* The engine's reason for the last parse failure, trimmed to the bare wording
 * CRuby's RegexpError carries: astrogre prefixes "regex parse error: " and
 * appends " (at offset N)" for its own CLI, neither of which Onigmo prints. */
static const char *korb_re_error(struct korb_vm *vm) {
    const char *const m = vm->re_err_fn ? ((const char *(*)(void))vm->re_err_fn)() : NULL;
    if (m == NULL) return NULL;
    static char buf[288];
    const char *p = m;
    const char *const pfx = "regex parse error: ";
    if (strncmp(p, pfx, strlen(pfx)) == 0) p += strlen(pfx);
    const char *const at = strstr(p, " (at offset ");
    const size_t n = at ? (size_t)(at - p) : strlen(p);
    const size_t cap = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, p, cap); buf[cap] = '\0';
    return buf;
}
#ifdef KORB_WASI
/* No dlopen: the bridge + astrogre are statically linked (wasi/Makefile's
 * libkoruby-regex-wasm.a; astrogre internals renamed to avoid symbol
 * clashes).  Same koruby_re_* API the dlsym path resolves. */
extern int koruby_re_exec(const char *, size_t, unsigned, const char *, size_t, size_t, void *);
extern const char *koruby_re_named(const char *, size_t, unsigned, int, int *);
extern int koruby_re_valid(const char *, size_t, unsigned);
extern void koruby_re_set_stack_floor(const void *);
extern const char *koruby_re_error(void);
static korb_re_exec_fn_t korb_re_load(struct korb_vm *vm) {
    if (vm->re_fn == NULL) {
        vm->re_fn       = (void *)koruby_re_exec;
        vm->re_named_fn = (void *)koruby_re_named;
        vm->re_valid_fn = (void *)koruby_re_valid;
        vm->re_floor_fn = (void *)koruby_re_set_stack_floor;
        vm->re_err_fn   = (void *)koruby_re_error;
    }
    return (korb_re_exec_fn_t)vm->re_fn;
}
#else
static korb_re_exec_fn_t korb_re_load(struct korb_vm *vm) {
    if (vm->re_fn == NULL) {
        void *h = dlopen(KORUBY_SRC_DIR "/koruby_regex.so", RTLD_NOW | RTLD_LOCAL);
        vm->re_fn       = h ? dlsym(h, "koruby_re_exec")  : NULL;
        vm->re_named_fn = h ? dlsym(h, "koruby_re_named") : NULL;
        vm->re_valid_fn = h ? dlsym(h, "koruby_re_valid") : NULL;
        vm->re_floor_fn = h ? dlsym(h, "koruby_re_set_stack_floor") : NULL;
        vm->re_err_fn   = h ? dlsym(h, "koruby_re_error") : NULL;
        if (vm->re_fn == NULL) vm->re_fn = (void *)(korb_sword_t)-1;   /* mark load failure */
    }
    return vm->re_fn == (void *)(korb_sword_t)-1 ? NULL : (korb_re_exec_fn_t)vm->re_fn;
}
#endif
/* fwd (defined in builtins/regexp.c, included after string.c) — lets String#/
 * Symbol#start_with? take a Regexp prefix. */
static RESULT korb_re_run(CTX *c, VALUE *slots, VALUE re, VALUE subj, size_t startb, korb_re_match_t *m);
static RESULT korb_re_build_md(CTX *c, VALUE *slots, VALUE subj, VALUE re, const korb_re_match_t *m);
static void korb_re_set_lastmatch(CTX *c, VALUE md_or_nil);
RESULT korb_regexp_new(CTX *c, VALUE *slots, VALUE source, uint32_t flags) {
    VALUE_REF sref = SLOTS_PUSH(slots, source);          /* root source across alloc */
    KorbRegexp *r = korb_alloc(c, slots, sizeof(KorbRegexp), KORB_OBJ_REGEXP);
    r->flags = flags;
    r->ci = (flags & 4u) ? 1u : 0u;
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->source, VALUE_REF_GET(sref));
    return RESULT_OK((VALUE)r);
}
/* obj.method(:sym) → a bound Method object (receiver + method id). */
RESULT korb_method_new(CTX *c, VALUE *slots, VALUE recv, uint32_t mid) {
    VALUE_REF rref = SLOTS_PUSH(slots, recv);            /* root recv across alloc */
    KorbMethod *m = korb_alloc(c, slots, sizeof(KorbMethod), KORB_OBJ_METHOD);
    m->mid = mid; m->unbound = 0;
    ARO_STORE(c, m, (VALUE *)(uintptr_t)&m->recv, VALUE_REF_GET(rref));
    return RESULT_OK((VALUE)m);
}
/* UnboundMethod: recv holds the OWNER class (no instance); bind re-attaches it. */
RESULT korb_unbound_new(CTX *c, VALUE *slots, VALUE owner, uint32_t mid) {
    RESULT r = korb_method_new(c, slots, owner, mid);
    if (LIKELY(r.state == KORB_NORMAL)) VAL2METH(r.value)->unbound = 1;
    return r;
}

/* Write VALUE `v` into a closed env's captured-locals array, with the write
 * barrier (the vals buffer is a heap VALUE_ARRAY). */
void korb_env_store(CTX *c, KorbEnv *e, uint32_t index, VALUE v) {
    KorbArrayItems *vi = (KorbArrayItems *)(uintptr_t)e->vals;
    ARO_STORE(c, vi, &korb_items_data(vi)[index], v);
}

/* The frame at `loc` owns an open env iff its EP cell loc[-2] is a clean even
 * KorbEnv with loc[-2]->loc == loc (set by korb_make_proc/binding).  Lets multiple
 * procs over the same activation share one env → shared mutation.  No global list. */
static KorbEnv *korb_open_env_find(VALUE *loc) {
    const VALUE pv = korb_ep_get(loc);
    if (pv != 0 && (pv & 1u) == 0) {
        KorbEnv *e = VAL2ENV(pv);
        if (!e->closed && e->loc == loc) return e;
    }
    return NULL;
}

/* Cold tail of the frame-return close hook: the returning frame's EP cell
 * (frame_base[-2]) holds its own open env (clean even KorbEnv, loc==frame_base —
 * see korb_frame_escaped).  Copy the live locals into a heap vals array so an
 * escaped closure over this activation survives the frame.  OUT-OF-LINE so the
 * (false) guard doesn't bloat the always-inlined invoke fast paths. */
RESULT __attribute__((noinline)) korb_close_ret(CTX *c, VALUE *scratch, VALUE *frame_base, RESULT r) {
    scratch[0] = r.value;                              /* root return value across vals alloc */
    scratch[1] = korb_ep_get(frame_base);                       /* root the env (EP cell) across alloc */
    const uint32_t n = VAL2ENV(scratch[1])->n;
    KorbArrayItems *vals = korb_alloc(c, scratch + 2, sizeof(KorbArrayItems) + (size_t)n * sizeof(VALUE), KORB_OBJ_VALUE_ARRAY);
    KorbEnv *e = VAL2ENV(scratch[1]);                  /* re-read after GC */
    for (uint32_t j = 0; j < n; j++) ARO_STORE(c, vals, &korb_items_data(vals)[j], e->loc[j]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->vals, (VALUE)(uintptr_t)vals);
    e->closed = 1;
    r.value = scratch[0];
    return r;
}

/* Build a Proc/lambda capturing a block body + its lexical env + self.  If the
 * body reads outer locals (cap_depth>0) the captured scope chain is materialized
 * into heap KorbEnv objects (open: loc->live slots, shared with the frame; the
 * frame's return closes them, copying slots->vals) so the closure survives the
 * frame (escape).  cap_depth==0 → no outer refs → env left as a tagged slots
 * sentinel (never dereferenced by the body). */
extern const struct NodeKind kind_node_entry;   /* node_alloc.c; distinguishes a real block entry from a node_unsupported placeholder */
RESULT korb_make_proc(CTX *c, VALUE *slots, struct Node *entry, VALUE *def_env, VALUE self_val, uint32_t is_lambda) {
    /* uncompilable block params (e.g. `|&b|`) → node_unsupported placeholder, not a
     * node_entry; surface the NotImplementedError rather than reading a bad union. */
    if (UNLIKELY(entry == KORB_BLK_CPROC)) return RESULT_OK(self_val);   /* forwarded Symbol/Method#to_proc: already a Proc (held in self_val) */
    if (UNLIKELY(entry->head.kind != &kind_node_entry)) return EVAL(c, entry, slots);
    /* def_env is a live slots base; some callers (node_send_blk → a builtin that
     * reifies its block, e.g. Hash.new {…}) hand it in odd-tagged.  Strip the tag
     * so the capture walk reads a real frame base (idempotent for untagged callers). */
    def_env = (VALUE *)(uintptr_t)((uintptr_t)def_env & ~(uintptr_t)1u);
    uint32_t depth = entry->u.node_entry.cap_depth;
    slots[0] = self_val;                                 /* root captured self across allocs */
    if (depth == 0) {                                    /* no captured outer locals */
        KorbProc *p = korb_alloc(c, slots + 1, sizeof(KorbProc), KORB_OBJ_PROC);
        p->iseq = entry; p->is_lambda = (uint8_t)is_lambda;
        ARO_GC_RAW_STORE(&p->env, (VALUE)((uintptr_t)def_env | 1u));   /* sentinel non-pointer (unused by body); WB-exempt */
        ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, slots[0]);
        return RESULT_OK((VALUE)p);
    }
    const uint16_t *ns = (const uint16_t *)entry->u.node_entry.cap_ns;
    if (depth > 64) depth = 64;                          /* sanity bound on nesting */
    VALUE *bases[64];
    /* Walk the PREV chain (slots base[-1], odd-tagged for a live frame).  An
     * ancestor that already escaped has an even KorbEnv* there — its chain is
     * already materialized, so stop and reuse it as the outer env (do NOT treat
     * the KorbEnv pointer as a slots base — that was the deep-capture bug). */
    bases[0] = def_env;                                  /* immediate enclosing scope locals */
    uint32_t nlive = 1;
    VALUE outer_env = 0;                                 /* existing KorbEnv chain to graft, or 0 */
    for (uint32_t k = 1; k < depth; k++) {
        const VALUE pv = korb_ep_get(bases[k-1]);
        if (pv & 1u) { bases[k] = (VALUE *)(uintptr_t)(pv & ~(uintptr_t)1u); nlive++; }
        else { outer_env = pv; break; }                 /* reached an already-materialized KorbEnv */
    }
    /* materialize the live levels outermost -> innermost, grafting onto
     * outer_env; slots[1] holds the current outer env (rooted across each alloc). */
    slots[1] = outer_env;
    for (int k = (int)nlive - 1; k >= 0; k--) {
        const VALUE pv = korb_ep_get(bases[k]);                   /* original outer link (preserve into e->prev) */
        KorbEnv *existing = korb_open_env_find(bases[k]);
        if (existing) { slots[1] = (VALUE)(uintptr_t)existing; continue; }   /* share this frame's env */
        KorbEnv *e = korb_alloc(c, slots + 2, sizeof(KorbEnv), KORB_OBJ_ENV);
        e->loc = bases[k];                               /* open: live slots */
        e->n = ns[k];
        e->closed = 0;
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->vals, 0);
        /* outer = materialized-so-far; for the outermost keep the original PREV
         * link so a deeper sibling closure can still walk past this frame. */
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->prev, slots[1] ? slots[1] : pv);
        slots[1] = (VALUE)(uintptr_t)e;
        korb_ep_set(bases[k], slots[1]);                         /* EP cell: this frame owns its env (clean even; GC roots via slots) */
    }
    KorbProc *p = korb_alloc(c, slots + 2, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = entry; p->is_lambda = (uint8_t)is_lambda;
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->env, slots[1]);   /* innermost env (even = KorbEnv) */
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, slots[0]);
    return RESULT_OK((VALUE)p);
}
/* The Proc for a block argument.  A FORWARDED block (`m(&blk)` where blk came in
 * as a block param, or a &method / &proc) arrives as a sentinel with the real
 * Proc in captured_self — materializing it again would treat the sentinel as a
 * frame base and crash. */
static RESULT korb_block_to_proc(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE *cself) {
    if (def_env == KORB_BLK_FWD || block == KORB_BLK_CPROC) return RESULT_OK(KORB_CSELF_VAL(cself));
    VALUE *const denv = (VALUE *)((uintptr_t)def_env & ~(uintptr_t)1u);   /* block-arg def_env is tagged (base|1) */
    return korb_make_proc(c, slots, block, denv, KORB_CSELF_VAL(cself), 0);
}

/* Build a Binding capturing the current frame: an open KorbEnv over `frame_base`
 * (shared with any closures over the same activation, promoted on frame exit),
 * `self`, and the immortal `names` table.  GC-safe (env/self rooted in slots). */
/* `rescue *obj` where obj is not an Array: CRuby converts with #to_a. */
RESULT
korb_rescue_splat_list(CTX *c, VALUE *slots, VALUE *listslot)
{
    const uint32_t to_a = korb_intern(c->vm, "to_a", 4);
    if (korb_responds_to(c, *listslot, to_a)) {
        slots[0] = *listslot;                          /* recv just below the cursor */
        const RESULT r = korb_send(c, slots + 1, to_a, 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) { *listslot = r.value; return RESULT_OK(r.value); }
    }
    /* splatting something that is not a list yields a one-element list, so
     * `rescue *SomeError` behaves like `rescue SomeError` */
    slots[0] = *listslot;
    const RESULT ar = korb_ary_new(c, slots + 1, 1);
    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
    slots[1] = ar.value;
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    *listslot = slots[1];
    return RESULT_OK(slots[1]);
}

/* node_eval.c-visible wrapper: #to_int coercion (string.c's is static). */
RESULT korb_coerce_to_int_pub(CTX *c, VALUE *slots, VALUE *v) { return korb_coerce_to_int(c, slots, v); }


/* Binding scope table accessors (packed layout: see parse.c
 * kp_binding_scope_tbl — [L, ns..., (sym,depth,slot)*name_cnt]). */
#define KORB_BIND_L(b)         ((b)->name_syms[0])
#define KORB_BIND_NS(b, d)     ((b)->name_syms[1 + (d)])
#define KORB_BIND_TRIPLE(b, i) ((b)->name_syms + 1 + KORB_BIND_L(b) + 3u * (i))

/* Build the flat, single-level variant of the packed table (toplevel binding /
 * embed startup).  Immortal malloc. */
const uint32_t *
korb_binding_tbl_flat(const uint32_t *syms, uint32_t cnt)
{
    uint32_t *tbl = malloc(sizeof(uint32_t) * (2 + 3 * (cnt ? cnt : 1)));
    if (!tbl) abort();
    tbl[0] = 1; tbl[1] = cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        tbl[2 + 3 * i] = syms[i]; tbl[2 + 3 * i + 1] = 0; tbl[2 + 3 * i + 2] = i;
    }
    return tbl;
}

RESULT korb_make_binding(CTX *c, VALUE *slots, VALUE *frame_base, const uint32_t *scope_tbl, uint32_t name_cnt, VALUE self_val) {
    slots[0] = self_val;                                  /* root self across allocs */
    /* Materialize an env for EVERY captured lexical level (same walk as
     * korb_make_proc's deep capture): enclosing locals must stay reachable —
     * and writable — through the chain even after their frames return. */
    const uint32_t L = scope_tbl[0];
    VALUE *bases[64];
    bases[0] = frame_base;
    uint32_t nlive = 1;
    VALUE outer_env = 0;
    for (uint32_t k = 1; k < L && k < 64; k++) {
        const VALUE pv = korb_ep_get(bases[k - 1]);
        if (pv & 1u) { bases[k] = (VALUE *)(uintptr_t)(pv & ~(uintptr_t)1u); nlive++; }
        else { outer_env = pv; break; }                  /* already-materialized chain */
    }
    slots[1] = outer_env;
    for (int k = (int)nlive - 1; k >= 0; k--) {
        const VALUE pv = korb_ep_get(bases[k]);
        KorbEnv *existing = korb_open_env_find(bases[k]);
        if (existing) { slots[1] = (VALUE)(uintptr_t)existing; continue; }
        KorbEnv *e = korb_alloc(c, slots + 2, sizeof(KorbEnv), KORB_OBJ_ENV);
        e->loc = bases[k];
        e->n = (uint16_t)scope_tbl[1 + (uint32_t)k];     /* level's full locals count */
        e->closed = 0;
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->vals, 0);
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->prev, slots[1] ? slots[1] : pv);
        slots[1] = (VALUE)(uintptr_t)e;
        korb_ep_set(bases[k], slots[1]);                 /* frame owns its env */
    }
    KorbBinding *b = korb_alloc(c, slots + 2, sizeof(KorbBinding), KORB_OBJ_BINDING);
    b->name_syms = scope_tbl; b->name_cnt = name_cnt;
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->env,  slots[1]);
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->self, slots[0]);
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->extra, KORB_NIL);
    return RESULT_OK((VALUE)b);
}
/* env of lexical level `depth` in the binding's chain (0 = binding site). */
static KorbEnv *korb_bind_level(const KorbBinding *b, uint32_t depth) {
    VALUE ev = b->env;
    for (uint32_t d = 0; d < depth; d++) {
        if (ev == 0 || (ev & 1u)) return NULL;           /* raw/absent beyond capture */
        ev = VAL2ENV(ev)->prev;
    }
    if (ev == 0 || (ev & 1u)) return NULL;
    return VAL2ENV(ev);
}
/* read name index `i` (walks to its level; open → live slots, closed → vals). */
static VALUE korb_bind_env_get(const KorbBinding *b, uint32_t i) {
    const uint32_t *t = KORB_BIND_TRIPLE(b, i);
    const KorbEnv *e = korb_bind_level(b, t[1]);
    if (e == NULL) return KORB_NIL;
    const VALUE *base = e->closed ? korb_items_data((KorbArrayItems *)(uintptr_t)e->vals) : e->loc;
    if (e->closed && t[2] >= e->n) return KORB_NIL;      /* level closed with fewer captures */
    return base[t[2]];
}
/* write name index `i` (open → live slots, closed → heap vals via WB). */
static void korb_bind_env_set(CTX *c, const KorbBinding *b, uint32_t i, VALUE v) {
    const uint32_t *t = KORB_BIND_TRIPLE(b, i);
    KorbEnv *e = korb_bind_level(b, t[1]);
    if (e == NULL) return;
    if (e->closed) { if (t[2] < e->n) korb_env_store(c, e, t[2], v); }
    else e->loc[t[2]] = v;
}
/* name index of `sym` in the binding's scope, or -1. */
static int korb_bind_find(const KorbBinding *b, uint32_t sym) {
    for (uint32_t i = 0; i < b->name_cnt; i++)
        if (KORB_BIND_TRIPLE(b, i)[0] == sym) return (int)i;
    return -1;
}
/* resolve a :sym / "str" arg to an interned id; SIZE_MAX on a type error (caller raises). */
static uint32_t korb_bind_argsym(CTX *c, VALUE v) {
    if (SYMBOL_P(v)) return SYM2ID(v);
    if (KORB_STRING_P(v)) return korb_intern(c->vm, korb_strbuf_data(VAL2STR(v)->buf), VAL2STR(v)->len);
    return UINT32_MAX;
}
static RESULT korb_m_bind_recv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2BIND(VALUE_REF_GET(self))->self);
}
static RESULT korb_srcloc_result(CTX *c, VALUE *slots, const struct Node *body);   /* fwd */
static RESULT korb_m_bind_source_location(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const struct Node *const node = VAL2BIND(VALUE_REF_GET(self))->src_node;   /* the `binding` call site */
    if (node == NULL) return RESULT_OK(KORB_NIL);
    return korb_srcloc_result(c, slots, node);
}
/* A binding's local names are plain identifiers: $global / @ivar / $~ and the
 * like are a NameError, not a fresh local (CRuby's check_local_id). */
static RESULT korb_bind_check_lvname(CTX *c, VALUE *slots, VALUE_REF self, uint32_t sym) {
    const char *const nm = korb_sym_name(c->vm, sym);
    const unsigned char c0 = (unsigned char)nm[0];
    if (LIKELY(c0 == '_' || (c0 >= 'a' && c0 <= 'z') || c0 >= 0x80)) return RESULT_OK(KORB_TRUE);
    char db[224]; korb_desc_inspect(c, VALUE_REF_GET(self), db, sizeof db);
    return korb_raise(c, slots, KORB_E_NAME, 0, "wrong local variable name '%s' for %s", nm, db);
}
static RESULT korb_m_bind_lvget(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t sym;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &sym); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    const KorbBinding *b = VAL2BIND(VALUE_REF_GET(self));   /* re-read after the coercion (may GC) */
    const int i = korb_bind_find(b, sym);
    if (i >= 0) return RESULT_OK(korb_bind_env_get(b, (uint32_t)i));
    if (b->extra != KORB_NIL) { const int32_t hi = korb_hash_find(VAL2HASH(b->extra), ID2SYM(sym)); if (hi >= 0) return RESULT_OK(korb_items_data(VAL2HASH(b->extra)->items)[2 * hi + 1]); }
    return korb_raise(c, slots, KORB_E_NAME, 0, "local variable '%s' is not defined for %s", korb_sym_name(c->vm, sym), "an instance of Binding");
}
static RESULT korb_m_bind_lvdefined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t sym;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &sym); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    const KorbBinding *b = VAL2BIND(VALUE_REF_GET(self));   /* re-read after the coercion (may GC) */
    if (korb_bind_find(b, sym) >= 0) return RESULT_OK(KORB_TRUE);
    if (b->extra != KORB_NIL && korb_hash_find(VAL2HASH(b->extra), ID2SYM(sym)) >= 0) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_bind_lvset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t sym;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &sym); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    CHECK(korb_bind_check_lvname(c, slots, self, sym));
    KorbBinding *b = VAL2BIND(VALUE_REF_GET(self));   /* re-read after the coercion (may GC) */
    const VALUE val = VALUE_SLICE_GET(a, 1);
    const int i = korb_bind_find(b, sym);
    if (i >= 0) {                                          /* existing frame local → write the env */
        KorbEnv *e = VAL2ENV(b->env);
        if (e->closed) korb_env_store(c, e, (uint32_t)i, val);
        else e->loc[i] = val;
        return RESULT_OK(val);
    }
    /* new local → the extra side-hash (frame can't grow) */
    slots[0] = VALUE_REF_GET(self);                        /* root self (holds extra) */
    if (b->extra == KORB_NIL) {
        slots[1] = UNWRAP(korb_hash_new(c, slots + 1, 4));
        ARO_STORE(c, VAL2BIND(slots[0]), (VALUE *)(uintptr_t)&VAL2BIND(slots[0])->extra, slots[1]);
    }
    slots[1] = VAL2BIND(slots[0])->extra;
    slots[2] = ID2SYM(sym); slots[3] = val;
    CHECK(korb_hash_set(c, slots + 4, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
    return RESULT_OK(val);
}
static RESULT korb_m_bind_lvars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const KorbBinding *b = VAL2BIND(VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_ary_new(c, slots, b->name_cnt + 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < VAL2BIND(VALUE_REF_GET(self))->name_cnt; i++)
        CHECK(korb_ary_push_val(c, slots + 1, dst, ID2SYM(KORB_BIND_TRIPLE(VAL2BIND(VALUE_REF_GET(self)), i)[0])));
    const VALUE ex = VAL2BIND(VALUE_REF_GET(self))->extra;
    if (ex != KORB_NIL) for (uint32_t i = 0; i < VAL2HASH(ex)->len; i++)
        CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2HASH(ex)->items)[2 * i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Regexp / MatchData / String-regex methods live in builtins/regexp.c
 * (#included after string.c so korb_utf8_* and korb_obj_singleton are visible). */

VALUE
korb_const_get(struct korb_vm *vm, uint32_t name_sym)
{
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) return vm->const_vals[i];
    return KORB_NIL;
}

/* Resolve a baked dotted constant path ("Net::HTTP") one component at a time.
 * A bare name (no "::") is the ordinary by-name lookup. */
VALUE
korb_const_get_path(struct korb_vm *vm, uint32_t name_sym)
{
    const char *const nm = korb_sym_name(vm, name_sym);
    const char *sep = strstr(nm, "::");
    if (sep == NULL) return korb_const_get(vm, name_sym);
    VALUE cur = korb_const_get(vm, korb_intern(vm, nm, (uint32_t)(sep - nm)));
    for (const char *p = sep + 2; KORB_CLASS_P(cur); ) {
        const char *const next = strstr(p, "::");
        const uint32_t len = next ? (uint32_t)(next - p) : (uint32_t)strlen(p);
        const uint32_t idx = korb_const_index_owned(vm, korb_intern(vm, p, len), cur);
        if (idx == UINT32_MAX) return KORB_NIL;
        cur = vm->const_vals[idx];
        if (next == NULL) break;
        p = next + 2;
    }
    return cur;
}

/* `$!` stack (per-CTX, realloc-backed; rescue bodies only).  Top == `$!`.
 * Visited as roots by AROH_VISIT_ROOTS so entries survive the body's GC. */
void korb_errinfo_push(CTX *c, VALUE v) {
    if (UNLIKELY(c->errinfo_n == c->errinfo_cap)) {
        const uint32_t nc = c->errinfo_cap ? c->errinfo_cap * 2 : 16;
        c->errinfo = realloc(c->errinfo, sizeof(VALUE) * nc);
        c->errinfo_cap = nc;
    }
    c->errinfo[c->errinfo_n++] = v;
    if (c->errinfo_n > c->errinfo_live) c->errinfo_live = c->errinfo_n;
}
void korb_errinfo_pop(CTX *c) { if (c->errinfo_n) c->errinfo_n--; }
VALUE korb_errinfo_top(const CTX *c) { return c->errinfo_n ? c->errinfo[c->errinfo_n - 1] : KORB_NIL; }

/* const-table index of name_sym (UINT32_MAX if absent).  The table is
 * append-only, so an index captured once stays valid; const_vals[idx] is
 * GC-forwarded, so reading through it always yields the live object.  Exported
 * (non-static) so AOT SDs for node_const can call it. */
uint32_t
korb_const_index(const struct korb_vm *vm, uint32_t name_sym)
{
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) return i;
    return UINT32_MAX;
}
/* Object IS the top-level namespace in Ruby: `Object::X` and a bare `X` name the
 * same constant, so both must key the table the same way (nil). */
static inline VALUE korb_const_owner_key(const struct korb_vm *vm, VALUE owner)
{
    return owner == korb_builtin_class_obj(vm, KORB_C_OBJECT) ? KORB_NIL : owner;
}
/* GC-stable key for a constant's owning module: its class serial (0 = top-level). */
static inline uint32_t korb_const_owner_serial(const struct korb_vm *vm, VALUE owner)
{
    owner = korb_const_owner_key(vm, owner);
    return KORB_CLASS_P(owner) ? VAL2CLASS(owner)->serial : 0;
}
/* index of the constant named `name_sym` owned by `owner` (nil = top-level), or
 * UINT32_MAX.  For owner-aware scoped reads (M::X) and class find-or-create. */
uint32_t
korb_const_index_owned(const struct korb_vm *vm, uint32_t name_sym, VALUE owner)
{
    owner = korb_const_owner_key(vm, owner);
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym && vm->const_owners[i] == owner) return i;
    return UINT32_MAX;
}
/* One MRO segment (a class + its prepended/included modules, in method-lookup
 * order) searched for a constant owned by it — mirrors korb_mro_seg_find. */
static uint32_t korb_const_mro_seg(const struct korb_vm *vm, VALUE klass, uint32_t name_sym, int depth)
{
    if (!KORB_CLASS_P(klass) || depth > 64) return UINT32_MAX;
    const KorbClass *const k = VAL2CLASS(klass);
    if (k->prepended != KORB_NIL) {                          /* prepended: most-recently-prepended first */
        const KorbArray *const pre = VAL2ARY(k->prepended);
        for (int32_t j = (int32_t)pre->len - 1; j >= 0; j--) {
            const uint32_t idx = korb_const_mro_seg(vm, korb_items_data(pre->items)[j], name_sym, depth + 1);
            if (idx != UINT32_MAX) return idx;
        }
    }
    uint32_t idx = korb_const_index_owned(vm, name_sym, klass);
    if (idx != UINT32_MAX) return idx;
    if (k->included != KORB_NIL) {                           /* included: most-recently-included first */
        const KorbArray *const inc = VAL2ARY(k->included);
        for (int32_t j = (int32_t)inc->len - 1; j >= 0; j--) {
            idx = korb_const_mro_seg(vm, korb_items_data(inc->items)[j], name_sym, depth + 1);
            if (idx != UINT32_MAX) return idx;
        }
    }
    return UINT32_MAX;
}
/* The lexically-enclosing module of a constant node: walk the parse-baked name
 * chain from the outermost link inward, each step scoped to the previous owner,
 * so `M::Inner::A` is never confused with another `A` that merely happens to
 * come first in the flat table.  The outermost link falls back to a by-name
 * lookup because a `module M::Inner` header bakes only `Inner`.  KORB_NIL when
 * there is no usable chain and `owner_name` names nothing. */
/* Resolve one chain element inside `owner` (nil = top level).  A `class A::B`
 * header bakes the whole dotted path as one element, so it is walked here. */
static VALUE
korb_cref_step(struct korb_vm *vm, VALUE owner, uint32_t elem)
{
    const char *const nm = korb_sym_name(vm, elem);
    const char *p = nm;
    VALUE cur = owner;
    for (;;) {
        const char *const sep = strstr(p, "::");
        const uint32_t len = sep ? (uint32_t)(sep - p) : (uint32_t)strlen(p);
        const uint32_t id = korb_intern(vm, p, len);
        uint32_t j = korb_const_index_owned(vm, id, KORB_CLASS_P(cur) ? cur : KORB_NIL);
        if (j == UINT32_MAX && cur == KORB_NIL) j = korb_const_index(vm, id);   /* top-level: allow the flat name */
        if (j == UINT32_MAX) return KORB_NIL;
        cur = vm->const_vals[j];
        if (!sep) return cur;
        if (!KORB_CLASS_P(cur)) return KORB_NIL;
        p = sep + 2;
    }
}
VALUE
korb_cref_resolve(struct korb_vm *vm, const uint32_t *chain, uint32_t chain_len, uint32_t owner_name)
{
    VALUE cref = KORB_NIL;
    if (chain != NULL && chain_len > 0) {
        cref = korb_cref_step(vm, KORB_NIL, chain[0]);
        for (uint32_t i = 1; KORB_CLASS_P(cref) && i < chain_len; i++)
            cref = korb_cref_step(vm, cref, chain[i]);
    }
    if (!KORB_CLASS_P(cref) && owner_name != 0) cref = korb_const_get(vm, owner_name);
    return cref;
}

/* Search a constant through `cref`'s ancestry (self + modules, then up the
 * superclass chain) in Ruby's constant-lookup order.  UINT32_MAX if absent. */
uint32_t
korb_const_in_ancestry(const struct korb_vm *vm, VALUE cref, uint32_t name_sym)
{
    for (VALUE k = cref; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass) {
        const uint32_t idx = korb_const_mro_seg(vm, k, name_sym, 0);
        if (idx != UINT32_MAX) return idx;
    }
    return UINT32_MAX;
}
/* Same walk for a scoped read (`Recv::NAME`), which skips Object unless Object
 * is the receiver — `Foo::Hash` is a NameError in Ruby >= 2.5. */
uint32_t
korb_const_in_ancestry_scoped(const struct korb_vm *vm, VALUE recv, uint32_t name_sym)
{
    const VALUE objc = korb_builtin_class_obj(vm, KORB_C_OBJECT);
    for (VALUE k = recv; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass) {
        if (k == objc && recv != objc) {
            /* Object's OWN constants are the top-level ones and are excluded,
             * but the modules included into Object still count (CRuby). */
            const VALUE inc = VAL2CLASS(k)->included;
            if (inc != KORB_NIL) {
                const KorbArray *const ia = VAL2ARY(inc);
                for (int32_t j = (int32_t)ia->len - 1; j >= 0; j--) {
                    const uint32_t idx = korb_const_mro_seg(vm, korb_items_data(ia->items)[j], name_sym, 0);
                    if (idx != UINT32_MAX) return idx;
                }
            }
            continue;
        }
        const uint32_t idx = korb_const_mro_seg(vm, k, name_sym, 0);
        if (idx != UINT32_MAX) return idx;
    }
    return UINT32_MAX;
}

void
korb_const_define_owned(CTX *c, uint32_t name_sym, VALUE val, VALUE owner)
{
    struct korb_vm *const vm = c->vm;
    owner = korb_const_owner_key(vm, owner);
    /* Ruby: assigning an anonymous class/module to a constant names it after
     * that constant (the first such assignment wins) and nests it under the
     * owning namespace so its qualified name is Owner::Name. */
    if (KORB_CLASS_P(val) && VAL2CLASS(val)->name_sym == 0) {
        VAL2CLASS(val)->name_sym = name_sym;
        /* Object is the top-level namespace: a constant assigned directly under it
         * is named by the bare constant ("X"), not "Object::X".  Only a genuine
         * nested namespace becomes the enclosing scope. */
        const VALUE objc = korb_builtin_class_obj(vm, KORB_C_OBJECT);
        bool cyclic = (owner == val);                 /* `X = X`-shaped nesting would loop the qname walk */
        for (VALUE o = KORB_CLASS_P(owner) ? VAL2CLASS(owner)->enclosing : KORB_NIL;
             !cyclic && KORB_CLASS_P(o); o = VAL2CLASS(o)->enclosing)
            if (o == val) cyclic = true;
        if (KORB_CLASS_P(owner) && owner != objc && !cyclic && VAL2CLASS(val)->enclosing == KORB_NIL)
            ARO_STORE(c, VAL2CLASS(val), (VALUE *)(uintptr_t)&VAL2CLASS(val)->enclosing, owner);
    }
    /* keyed by (name, owner): reassigning the same constant in the same namespace
     * updates in place, but M::C and a top-level C get distinct entries so both
     * coexist (a bare read still finds the first match by name — hot path). */
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym && vm->const_owners[i] == owner) {
            vm->const_vals[i] = val;
            return;
        }
    vm->const_serial++;                               /* a new (name, owner) can change what a lookup finds */
    if (vm->const_cnt == vm->const_capa) {
        uint32_t nc = vm->const_capa ? vm->const_capa * 2 : 16;
        vm->const_names  = realloc(vm->const_names,  sizeof(uint32_t) * nc);
        vm->const_vals   = realloc(vm->const_vals,   sizeof(VALUE) * nc);
        vm->const_owners = realloc(vm->const_owners, sizeof(VALUE) * nc);
        if (!vm->const_names || !vm->const_vals || !vm->const_owners) { fprintf(stderr, "koruby_precise: oom (consts)\n"); abort(); }
        vm->const_capa = nc;
    }
    vm->const_names[vm->const_cnt] = name_sym;
    vm->const_vals[vm->const_cnt] = val;      /* root cell (scanned); no WB needed */
    vm->const_owners[vm->const_cnt] = owner;
    vm->const_cnt++;
}
void
korb_const_define(CTX *c, uint32_t name_sym, VALUE val)
{
    korb_const_define_owned(c, name_sym, val, KORB_NIL);   /* top-level / builtin (no lexical owner) */
}

/* find mid in k->methods, or append a fresh entry.  Entries are individually
 * calloc'd immortal objects (never moved/freed) so a frame- or cache-held
 * korb_method* stays valid across method-table growth; the table itself is a
 * realloc'able libc array of entry pointers. */
static struct korb_method *
korb_class_method_slot(KorbClass *const k, uint32_t mid)
{
    struct korb_method *m = NULL;
    for (uint32_t i = 0; i < k->method_cnt; i++)
        if (k->methods[i]->mid == mid) { m = k->methods[i]; break; }
    if (!m) {
        if (k->method_cnt == k->method_capa) {
            uint32_t nc = k->method_capa ? k->method_capa * 2 : 8;
            k->methods = realloc(k->methods, sizeof(struct korb_method *) * nc);
            if (!k->methods) { fprintf(stderr, "koruby_precise: oom (methods)\n"); abort(); }
            k->method_capa = nc;
        }
        m = calloc(1, sizeof(struct korb_method));
        if (!m) { fprintf(stderr, "koruby_precise: oom (method entry)\n"); abort(); }
        m->mid = mid; m->orig_mid = mid;
        k->methods[k->method_cnt++] = m;
    }
    m->rfn = NULL; m->rbfn = NULL; m->bfn = NULL; m->is_simple = 0; m->dm_proc = KORB_NIL;
    return m;
}

/* Module#define_method(name, &block) / define_method(name, proc).  In a class
 * body self is the class; the block becomes the method body (run with self =
 * receiver).  The captured env is force-closed immediately so it survives past
 * the defining frame.  Returns the method name symbol. */
static VALUE korb_dispatch_class(CTX *c, VALUE self);            /* fwd */
static struct korb_method *korb_method_lookup(struct korb_vm *vm, uint32_t mid);   /* fwd */
static struct korb_method *korb_class_find_method(VALUE klass, uint32_t mid, VALUE *out_def);   /* fwd */
/* true if `anc` is `klass` or one of its ancestors (superclass chain + included
 * + prepended modules). */
static bool korb_class_has_ancestor(VALUE klass, VALUE anc) {
    while (KORB_CLASS_P(klass)) {
        if (klass == anc) return true;
        const VALUE pre = VAL2CLASS(klass)->prepended;
        if (pre != KORB_NIL) { const KorbArray *pa = VAL2ARY(pre); for (uint32_t j = 0; j < pa->len; j++) if (korb_items_data(pa->items)[j] == anc) return true; }
        const VALUE inc = VAL2CLASS(klass)->included;
        if (inc != KORB_NIL) { const KorbArray *ia = VAL2ARY(inc); for (uint32_t j = 0; j < ia->len; j++) if (korb_items_data(ia->items)[j] == anc) return true; }
        klass = VAL2CLASS(klass)->superclass;
    }
    return false;
}
/* Copy the method `oldm` (found on klass / its ancestors / the global table) into
 * a new slot `newm` on klass.  Shared by Module#alias_method and `alias`. */
RESULT korb_do_alias(CTX *c, VALUE *slots, VALUE klass, uint32_t newm, uint32_t oldm) {
    if (!KORB_CLASS_P(klass)) klass = korb_dispatch_class(c, klass);   /* top-level (self=main) → alias on its class (Object) */
    if (UNLIKELY(!KORB_CLASS_P(klass)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "alias on a non-class");
    { RESULT fr = korb_check_def_frozen(c, slots, klass); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* alias in a frozen class → FrozenError */
    const struct korb_method *src = korb_class_find_method(klass, oldm, NULL);
    if (src == NULL) src = korb_method_lookup(c->vm, oldm);
    if (src == NULL && KORB_CLASS_P(klass) && VAL2CLASS(klass)->is_module) {
        /* Kernel/Object instance methods live on Object in koruby; a module
         * (e.g. `module Kernel; alias_method ...`) aliasing one resolves it
         * there (mirrors define_method's Method-form fallback). */
        const VALUE objc = korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
        if (KORB_CLASS_P(objc)) src = korb_class_find_method(objc, oldm, NULL);
    }
    if (src == NULL && oldm == c->vm->mid_new) {
        /* `alias newobj new` (net/http does this on its metaclass): .new is a
         * dispatch special-case, not a table entry, so alias a trampoline that
         * re-sends :new — same body Class#[] uses. */
        struct korb_method *dst = korb_class_method_slot(VAL2CLASS(klass), newm);
        memset(dst, 0, sizeof *dst);
        dst->mid = newm; dst->orig_mid = oldm;
        dst->kind = KORB_METHOD_CFUNC; dst->params_cnt = -1;
        dst->rest_slot = -1;
        dst->owner = KORB_NIL; dst->dm_proc = KORB_NIL;
        dst->rfn = korb_m_class_new_bracket;
        c->vm->method_serial++;
        return RESULT_OK(KORB_NIL);
    }
    if (UNLIKELY(src == NULL))
        return korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for class '%s'",   /* CRuby: NameError, not NoMethodError */
                          korb_sym_name(c->vm, oldm), korb_type_name(klass));
    korb_check_basic_op_redef(c, klass, newm);          /* an alias can redefine a basic op too */
    struct korb_method *dst = korb_class_method_slot(VAL2CLASS(klass), newm);   /* libc alloc, no GC */
    const struct korb_method tmp = *src;   /* src may dangle if the slot array grows; snapshot first */
    *dst = tmp; dst->mid = newm; dst->owner = klass;
    c->vm->method_serial++;
    slots[0] = klass;                      /* park: the hook is Ruby code */
    CHECK(korb_fire_method_added(c, slots + 1, slots[0], newm));   /* an alias is a definition too */
    return RESULT_OK(ID2SYM(newm));
}
/* Resolve an alias_method name arg → mid: Symbol/String, or #to_str-coercible
 * (a #to_str that raises — e.g. NoMethodError — propagates); else TypeError. */
static RESULT korb_alias_argsym(CTX *c, VALUE *slots, VALUE v, uint32_t *out) {
    if (SYMBOL_P(v) || KORB_STRING_P(v)) { *out = korb_bind_argsym(c, v); return RESULT_OK(KORB_NIL); }
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, to_str)) {
        slots[0] = v;
        RESULT r = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_STRING_P(r.value)) { *out = korb_bind_argsym(c, r.value); return RESULT_OK(KORB_NIL); }
    }
    return korb_raise_not_sym(c, slots, v);
}
/* Module#alias_method(new, old) → new name symbol. */
static RESULT korb_m_class_alias_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    slots[0] = VALUE_REF_GET(self);                       /* root self across the coercion dispatches */
    uint32_t newm, oldm;
    { RESULT r = korb_alias_argsym(c, slots + 1, VALUE_SLICE_GET(a, 0), &newm); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    { RESULT r = korb_alias_argsym(c, slots + 1, VALUE_SLICE_GET(a, 1), &oldm); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    return korb_do_alias(c, slots + 1, slots[0], newm, oldm);
}
static VALUE korb_klass_override_get(const struct korb_vm *vm, VALUE obj);   /* fwd */
static RESULT korb_m_define_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE klass = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(klass)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "define_method called on a non-class");
    KORB_CHECK_FROZEN(c, slots, klass);                  /* def on a frozen module/class → FrozenError */
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    VALUE nv = VALUE_SLICE_GET(a, 0);
    uint32_t mid;
    if (SYMBOL_P(nv))            mid = SYM2ID(nv);
    else if (KORB_STRING_P(nv))  mid = korb_intern(c->vm, korb_strbuf_data(VAL2STR(nv)->buf), VAL2STR(nv)->len);
    else {                                               /* coerce via #to_str (a String-like name) */
        slots[0] = nv;
        RESULT sr = korb_send(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) {
            if (sr.state == KORB_RAISE && KORB_EXC_P(sr.value) && VAL2EXC(sr.value)->etype == KORB_E_NOMETHOD)
                return korb_raise_not_sym(c, slots, nv);
            return sr;
        }
        if (UNLIKELY(!KORB_STRING_P(sr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to String", korb_type_name(nv));
        mid = korb_intern(c->vm, korb_strbuf_data(VAL2STR(sr.value)->buf), VAL2STR(sr.value)->len);
        klass = VALUE_REF_GET(self);                     /* to_str dispatch may have moved the class (self is rooted) */
    }
    slots[0] = klass;                                    /* root class across allocs */
    /* An explicit 2nd argument wins over a block (CRuby): `define_method(:x, pr) { }`
     * defines pr, not the block. */
    const bool arg_body = VALUE_SLICE_LEN(a) >= 2 &&
                          (KORB_PROC_P(VALUE_SLICE_GET(a, 1)) || KORB_METHOD_P(VALUE_SLICE_GET(a, 1)));
    if (!arg_body && block != NULL && def_env == KORB_BLK_FWD) {   /* `define_method(:x, &proc)` — forwarded Proc: use as-is */
        slots[1] = KORB_CSELF_VAL(cself);
    } else if (!arg_body && block != NULL) {                          /* block form → a (lambda) proc */
        /* a block-arg's def_env arrives in tagged prev form (base|1); korb_make_proc
         * wants the raw frame base (it reads base[-2] for outer scopes).  The
         * captured open env is shared (korb_open_env_find) and promoted to heap when
         * the defining frame returns — so closures over a shared mutable local work. */
        VALUE *const denv = (VALUE *)((uintptr_t)def_env & ~(uintptr_t)1u);
        slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, denv, KORB_CSELF_VAL(cself), 1));
    } else if (VALUE_SLICE_LEN(a) >= 2 && KORB_PROC_P(VALUE_SLICE_GET(a, 1))) {
        slots[1] = VALUE_SLICE_GET(a, 1);                /* proc form: already self-contained */
    } else if (VALUE_SLICE_LEN(a) >= 2 && KORB_METHOD_P(VALUE_SLICE_GET(a, 1))) {
        /* Method / UnboundMethod form: copy the resolved definition under `mid`. */
        const KorbMethod *const mo = VAL2METH(VALUE_SLICE_GET(a, 1));
        const VALUE owner = mo->unbound ? mo->recv : korb_dispatch_class(c, mo->recv);
        const bool owner_mod = KORB_CLASS_P(owner) && VAL2CLASS(owner)->is_module;
        const struct korb_method *src = KORB_CLASS_P(owner) ? korb_class_find_method(owner, mo->mid, NULL) : NULL;
        if (src == NULL) src = korb_method_lookup(c->vm, mo->mid);
        if (src == NULL && owner_mod) {                  /* Kernel's methods live on Object in koruby */
            const VALUE objc = korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
            if (KORB_CLASS_P(objc)) src = korb_class_find_method(objc, mo->mid, NULL);
        }
        if (UNLIKELY(src == NULL))
            return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method '%s'", korb_sym_name(c->vm, mo->mid));
        /* a module-owned method (e.g. a Kernel UnboundMethod) binds to any class;
         * only a class owner requires the defining class to be a descendant. */
        if (UNLIKELY(KORB_CLASS_P(owner) && VAL2CLASS(owner)->is_singleton && owner != slots[0]))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't bind singleton method to a different class");
        if (UNLIKELY(KORB_CLASS_P(owner) && !owner_mod && !korb_class_has_ancestor(slots[0], owner))) {
            char onm[192]; korb_class_qname_into(c, owner, onm, sizeof onm);   /* name the CLASS, not "Class" */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "bind argument must be a subclass of %s", onm);
        }
        korb_check_basic_op_redef(c, slots[0], mid);
        struct korb_method *dst = korb_class_method_slot(VAL2CLASS(slots[0]), mid);
        *dst = *src;                                     /* copy the definition */
        dst->mid = mid; dst->owner = slots[0];           /* rename + re-own */
        /* the copy takes the DEFINING frame's visibility, not the source's */
        dst->visibility = (VAL2CLASS(slots[0])->cur_visibility == 3) ? 1 : VAL2CLASS(slots[0])->cur_visibility;
        c->vm->method_serial++;
        CHECK(korb_fire_method_added(c, slots + 2, slots[0], mid));
        return RESULT_OK(ID2SYM(mid));
    } else if (VALUE_SLICE_LEN(a) >= 2) {                /* a 2nd arg that isn't a Proc/Method/UnboundMethod */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Proc/Method/UnboundMethod)", korb_type_name(VALUE_SLICE_GET(a, 1)));
    } else {
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create Proc object without a block");
    }
    korb_check_basic_op_redef(c, slots[0], mid);
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(slots[0]), mid);
    m->kind = KORB_METHOD_DM;
    m->dm_proc = slots[1];
    m->owner = slots[0];
    m->visibility = (VAL2CLASS(slots[0])->cur_visibility == 3) ? 1 : VAL2CLASS(slots[0])->cur_visibility;   /* honor the current private/protected scope */
    m->params_cnt = -1;                                  /* lenient arity (block semantics) */
    m->uses_block = 0; m->rest_slot = -1; m->post_cnt = 0;
    m->param_info = (KORB_PROC_P(slots[1]) && VAL2PROC(slots[1])->iseq)   /* carry the block's params for Method#parameters */
                        ? VAL2PROC(slots[1])->iseq->u.node_entry.param_info : NULL;
    if (UNLIKELY(VAL2CLASS(slots[0])->cur_visibility == 3)) {   /* module_function mode: public copy on the singleton (as `def` does) */
        const VALUE sing = korb_klass_override_get(c->vm, slots[0]);
        if (KORB_CLASS_P(sing)) {
            const struct korb_method src = *m;
            struct korb_method *const sm = korb_class_method_slot(VAL2CLASS(sing), mid);
            *sm = src; sm->mid = mid; sm->owner = sing; sm->visibility = 0;
        }
    }
    c->vm->method_serial++;
    CHECK(korb_fire_method_added(c, slots + 2, slots[0], mid));
    return RESULT_OK(ID2SYM(mid));
}

/* A user redefinition of a node-fastpathed basic op on Integer/Float must deopt
 * the inline arithmetic/compare/eq/neg nodes (which bypass dispatch) to a real
 * send so the redefinition is honored (CRuby basic-op-redefined semantics).
 * Only Integer/Float matter — those are the types the fastpaths handle — so we
 * don't trip the flag for a redef on any other class (doing so broadly would
 * kill the fastpaths VM-wide; cf. koruby's note on fib regressing ~5×).  The
 * other operator-ish methods (** << >> & | ^ [] []=) have no node fastpath, so
 * they already dispatch and honor reopening without a flag. */
static void
korb_check_basic_op_redef(CTX *c, VALUE klass, uint32_t mid)
{
    struct korb_vm *const vm = c->vm;
    /* A user-defined `!` (Delegator, BasicObject proxies) makes node_not dispatch
     * instead of negating in place; one flag for the whole VM keeps the common
     * `!bool` free. */
    if (!vm->bang_redefined && mid == korb_intern(vm, "!", 1)) vm->bang_redefined = true;
    /* Array#[] / Array#[]= redefinition deopts the node_aref/node_aset fast path. */
    if (!vm->aref_redefined && klass == korb_builtin_class_obj(vm, KORB_C_ARRAY) &&
        (mid == vm->mid_aref || mid == vm->mid_aset))
        vm->aref_redefined = true;
    /* Array#<< redefinition deopts the node_shl Array fast path. */
    if (!vm->arr_shl_redefined && klass == korb_builtin_class_obj(vm, KORB_C_ARRAY) &&
        mid == vm->mid_shl)
        vm->arr_shl_redefined = true;
    /* Hash#[] redefinition deopts the node_aref Hash fast path. */
    if (!vm->hash_aref_redefined && klass == korb_builtin_class_obj(vm, KORB_C_HASH) &&
        mid == vm->mid_aref)
        vm->hash_aref_redefined = true;
    /* Method#[] redefinition deopts the node_aref Method fast path. */
    if (!vm->method_aref_redefined && klass == korb_builtin_class_obj(vm, KORB_C_METHOD) &&
        mid == vm->mid_aref)
        vm->method_aref_redefined = true;
    /* Integer#[] redefinition deopts the node_aref Integer bit-test fast path. */
    if (!vm->int_aref_redefined && klass == korb_builtin_class_obj(vm, KORB_C_INTEGER) &&
        mid == vm->mid_aref)
        vm->int_aref_redefined = true;
    if (vm->basic_op_redefined) return;   /* already deopted */
    if (klass != korb_builtin_class_obj(vm, KORB_C_INTEGER) &&
        klass != korb_builtin_class_obj(vm, KORB_C_FLOAT)) return;
    static const char *const ops[] = {
        "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "-@",
        "&", "|", "^", "<<", ">>",
    };
    const char *const nm = korb_sym_name(vm, mid);
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (strcmp(nm, ops[i]) == 0) { vm->basic_op_redefined = true; return; }
}

/* BasicObject#! — the default negation, reachable through send/respond_to?.
 * `!x` itself is node_not, which only dispatches for a user-defined `!`. */
static RESULT korb_m_obj_not(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    return RESULT_OK(KORB_TRUTHY(VALUE_REF_GET(self)) ? KORB_FALSE : KORB_TRUE);
}

/* True when `v`'s class (or singleton) defines `!` — node_not's deopt check.
 * Only reached while vm->bang_redefined is set. */
bool
korb_find_bang_override(CTX *c, VALUE v)
{
    const VALUE k = korb_dispatch_class(c, v);
    if (!KORB_CLASS_P(k)) return false;
    const struct korb_method *const m = korb_class_find_method(k, korb_intern(c->vm, "!", 1), NULL);
    return m != NULL && m->kind != KORB_METHOD_CFUNC;   /* the CFUNC one IS the default */
}

extern const struct NodeKind kind_node_ivar_get;   /* auto-attr detection */
static VALUE korb_klass_override_get(const struct korb_vm *vm, VALUE obj);   /* fwd (singleton lookup, no alloc) */

/* Module#dup / Class#dup: an anonymous copy that owns its own method table.
 * Sharing the source's entries would let the copy's undef_method/alias reach
 * back into the original (delegate.rb builds Delegator's superclass by dup'ing
 * Kernel and undef'ing half of it). */
RESULT
korb_class_dup(CTX *c, VALUE *slots, VALUE src)
{
    slots[0] = src;                                   /* root across every alloc below */
    const VALUE sup = VAL2CLASS(slots[0])->superclass;
    RESULT nr = korb_class_new(c, slots + 1, 0, sup); /* anonymous, same superclass */
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    slots[1] = nr.value;
    KorbClass *const d = VAL2CLASS(slots[1]);
    const KorbClass *const s = VAL2CLASS(slots[0]);
    d->is_module = s->is_module;
    d->is_data = s->is_data;
    d->struct_kwinit = s->struct_kwinit;
    d->exc_etype = s->exc_etype;
    d->cur_visibility = 0;                            /* a fresh body starts public */
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->included,  s->included);
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->prepended, s->prepended);
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->members,   s->members);
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->cvars,     s->cvars);
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->class_ivars, s->class_ivars);
    ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->enclosing, s->enclosing);
    const uint32_t n = VAL2CLASS(slots[0])->method_cnt;
    for (uint32_t i = 0; i < n; i++) {
        const struct korb_method src_m = *VAL2CLASS(slots[0])->methods[i];   /* snapshot: the slot below may realloc */
        struct korb_method *const dm = korb_class_method_slot(VAL2CLASS(slots[1]), src_m.mid);
        *dm = src_m;
        dm->owner = slots[1];
    }
    /* constants defined under the source module come along (Module#constants) */
    struct korb_vm *const vm = c->vm;
    const uint32_t cn = vm->const_cnt;                /* the loop appends; only copy the originals */
    for (uint32_t i = 0; i < cn; i++)
        if (vm->const_owners[i] == slots[0])
            korb_const_define_owned(c, vm->const_names[i], vm->const_vals[i], slots[1]);
    /* module/class methods live on the singleton class; copy the source's own
     * (inherited ones already reach the copy through the metaclass chain). */
    VALUE ssing = KORB_NIL;
    if (((const AroObjectHeader *)(uintptr_t)slots[0])->flags & KORB_FL_HAS_KLASS) {
        const VALUE ov = korb_klass_override_get(vm, slots[0]);
        if (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) ssing = ov;
    }
    if (ssing != KORB_NIL) {
        slots[2] = ssing;                             /* root: the alloc below may move it */
        slots[3] = UNWRAP(korb_obj_singleton(c, slots + 4, slots[1]));
        {   /* `extend`ed modules live on the singleton too */
            KorbClass *const ds = VAL2CLASS(slots[3]);
            const KorbClass *const ss = VAL2CLASS(slots[2]);
            ARO_STORE(c, ds, (VALUE *)(uintptr_t)&ds->included,  ss->included);
            ARO_STORE(c, ds, (VALUE *)(uintptr_t)&ds->prepended, ss->prepended);
        }
        const uint32_t sn = VAL2CLASS(slots[2])->method_cnt;
        for (uint32_t i = 0; i < sn; i++) {
            const struct korb_method src_m = *VAL2CLASS(slots[2])->methods[i];
            struct korb_method *const dm = korb_class_method_slot(VAL2CLASS(slots[3]), src_m.mid);
            *dm = src_m;
            dm->owner = slots[3];
        }
    }
    vm->method_serial++;
    return RESULT_OK(slots[1]);
}

/* CRuby keeps Object's own method table empty: the Object-level methods live on
 * Kernel (included) and a handful on BasicObject.  koruby registers them on
 * Object for convenience, so after boot we relocate them once.  Dispatch is
 * unaffected (Object's MRO segment searches Kernel next); what changes is what
 * reflection reports -- UnboundMethod#owner, Module#instance_methods(false),
 * `Kernel.dup` (delegate.rb builds Delegator's superclass from it), and `super`
 * from a method that overrides an Object-level one. */
void
korb_relocate_object_methods(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    /* rooted in slots: the singleton alloc further down can move all three */
    slots[0] = korb_builtin_class_obj(vm, KORB_C_OBJECT);
    slots[1] = korb_const_get(vm, korb_intern(vm, "Kernel", 6));
    slots[2] = korb_const_get(vm, korb_intern(vm, "BasicObject", 11));
    if (!KORB_CLASS_P(slots[0]) || !KORB_CLASS_P(slots[1]) || !KORB_CLASS_P(slots[2])) return;
    const VALUE objc = slots[0], kmod = slots[1], bobj = slots[2];
    /* BasicObject's own methods (CRuby's BasicObject.instance_methods(false)
     * plus its private ones); everything else on Object belongs to Kernel. */
    static const char *const basic[] = {
        "==", "equal?", "!", "__send__", "instance_eval", "instance_exec",
        "!=", "__id__", "initialize", "method_missing", "singleton_method_added",
        "singleton_method_removed", "singleton_method_undefined", NULL,
    };
    KorbClass *const ko = VAL2CLASS(objc);
    const uint32_t n = ko->method_cnt;
    for (uint32_t i = 0; i < n; i++) {
        struct korb_method *const m = ko->methods[i];
        const char *const nm = korb_sym_name(vm, m->mid);
        VALUE target = kmod;
        for (uint32_t b = 0; basic[b]; b++)
            if (strcmp(nm, basic[b]) == 0) { target = bobj; break; }
        KorbClass *const kt = VAL2CLASS(target);
        bool present = false;
        for (uint32_t j = 0; j < kt->method_cnt && !present; j++)
            if (kt->methods[j]->mid == m->mid) present = true;
        if (present) continue;                  /* target already defines it: keep that one */
        if (kt->method_cnt == kt->method_capa) {
            const uint32_t nc = kt->method_capa ? kt->method_capa * 2 : 8;
            kt->methods = realloc(kt->methods, sizeof(struct korb_method *) * nc);
            if (!kt->methods) { fprintf(stderr, "koruby_precise: oom (methods)\n"); abort(); }
            kt->method_capa = nc;
        }
        m->owner = target;                      /* `super` and #owner follow the move */
        kt->methods[kt->method_cnt++] = m;
    }
    ko->method_cnt = 0;
    /* Kernel-level global functions (putc/caller/binding/...) are registered in
     * the global function table, which reflection cannot see.  Mirror them onto
     * Kernel as private instance methods, as CRuby has them. */
    KorbClass *const kk = VAL2CLASS(kmod);
    for (uint32_t i = 0; i < vm->method_cnt; i++) {
        const struct korb_method *const g = vm->methods[i];
        if (g->kind != KORB_METHOD_BUILTIN) continue;
        bool present = false;
        for (uint32_t j = 0; j < kk->method_cnt && !present; j++)
            if (kk->methods[j]->mid == g->mid) present = true;
        if (present) continue;
        struct korb_method *const dm = korb_class_method_slot(kk, g->mid);
        *dm = *g;
        dm->owner = kmod;
        dm->visibility = 1;                 /* Kernel's global functions are private */
    }
    /* CRuby's Kernel.private_instance_methods(false): the "function" half of
     * Kernel (puts/raise/format/...) is private, the "object" half public. */
    static const char *const privnames[] = {
        "Array", "Complex", "Float", "Hash", "Integer", "Pathname",
        "Rational", "String", "__callee__", "__dir__", "__method__", "`",
        "abort", "at_exit", "autoload", "autoload?", "binding", "block_given?",
        "caller", "caller_locations", "catch", "eval", "exec", "exit",
        "exit!", "fail", "fork", "format", "gem", "gem_original_require",
        "gets", "global_variables", "initialize_clone", "initialize_copy",
        "initialize_dup", "instance_variables_to_inspect", "iterator?", "lambda",
        "load", "local_variables", "loop", "open", "p", "pp", "print", "printf",
        "proc", "putc", "puts", "raise", "rand", "readline", "readlines",
        "require", "require_relative", "respond_to_missing?", "select",
        "set_trace_func", "sleep", "spawn", "sprintf", "srand", "syscall",
        "system", "test", "throw", "trace_var", "trap", "untrace_var", "warn",
        NULL,
    };
    /* they are module functions: private on the instance side, public on Kernel
     * itself (`Kernel.Integer("42")`), so mirror each onto Kernel's singleton. */
    /* the few that are private WITHOUT being module functions */
    static const char *const notmodfn[] = {
        "respond_to_missing?", "initialize_copy", "initialize_clone", "initialize_dup",
        "instance_variables_to_inspect", "gem", "gem_original_require", "pp", NULL,
    };
    slots[3] = korb_obj_singleton(c, slots + 4, slots[1]).value;   /* may GC: slots[1] tracks */
    for (uint32_t p = 0; privnames[p]; p++) {
        bool modfn = true;
        for (uint32_t q = 0; notmodfn[q] && modfn; q++)
            if (strcmp(privnames[p], notmodfn[q]) == 0) modfn = false;
        const uint32_t pmid = korb_intern(vm, privnames[p], (uint32_t)strlen(privnames[p]));
        KorbClass *const kc = VAL2CLASS(slots[1]);
        for (uint32_t j = 0; j < kc->method_cnt; j++) {
            if (kc->methods[j]->mid != pmid) continue;
            kc->methods[j]->visibility = 1;
            if (modfn && KORB_CLASS_P(slots[3])) {
                const struct korb_method src_m = *kc->methods[j];
                struct korb_method *const sm = korb_class_method_slot(VAL2CLASS(slots[3]), pmid);
                *sm = src_m;
                sm->owner = slots[3];
                sm->visibility = 0;
            }
            break;
        }
    }
    vm->method_serial++;
}

void
korb_class_def_method(CTX *c, VALUE klass, uint32_t mid, NODE *body,
                      uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                      uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info)
{
    korb_check_basic_op_redef(c, klass, mid);
    KorbClass *const k = VAL2CLASS(klass);
    struct korb_method *m = korb_class_method_slot(k, mid);
    m->kind = KORB_METHOD_ISEQ;
    m->visibility = (k->cur_visibility == 3) ? 1 : k->cur_visibility;   /* mode 3 = module_function: private instance method */
    m->owner = klass;        /* super's def_class + __method__ source (frame fs-2) */
    m->uses_block = (uint8_t)uses_block;
    m->params_cnt = (int32_t)params_cnt;
    m->req_cnt = req_cnt;
    m->post_cnt = post_cnt;
    m->rest_slot = rest_slot;
    m->locals_cnt = locals_cnt;
    m->body = body;
    m->opt_defaults = opt_defaults;
    m->kw_info = kw_info;
    m->param_info = param_info;
    m->bfn = NULL;
    /* fixed positional arity, nothing exotic → streamlined invoke eligible. */
    m->is_simple = (kw_info == NULL && rest_slot < 0 && post_cnt == 0 &&
                    req_cnt == params_cnt && !uses_block);
    /* Auto-attr: a method whose body is exactly `@ivar` (no params, no block)
     * is an attr_reader — dispatch returns the ivar directly, skipping a frame.
     * A multi-statement body roots at node_seq, so only the bare single-read
     * case matches (observably identical to attr_reader). */
    if (params_cnt == 0 && !uses_block && body && body->head.kind == &kind_node_ivar_get) {
        m->kind = KORB_METHOD_ATTR_R;
        m->attr_ivar = body->u.node_ivar_get.name;
    }
    if (UNLIKELY(k->cur_visibility == 3)) {   /* module_function mode: public copy on the module's (pre-created) singleton */
        const VALUE sing = korb_klass_override_get(c->vm, klass);
        if (KORB_CLASS_P(sing)) {
            const struct korb_method src = *m;    /* snapshot: the singleton slot-array grow can't touch k's array, but be safe */
            struct korb_method *sm = korb_class_method_slot(VAL2CLASS(sing), mid);   /* libc alloc, no GC */
            *sm = src; sm->mid = mid; sm->owner = sing; sm->visibility = 0;
        }
    }
    c->vm->method_serial++;
}

void
korb_class_def_attr(CTX *c, VALUE klass, uint32_t mid, uint32_t ivar_sym, int is_writer)
{
    KorbClass *const k = VAL2CLASS(klass);
    struct korb_method *m = korb_class_method_slot(k, mid);
    m->kind = is_writer ? KORB_METHOD_ATTR_W : KORB_METHOD_ATTR_R;
    m->owner = klass;
    m->params_cnt = is_writer ? 1 : 0;
    m->attr_ivar = ivar_sym;
    m->body = NULL;
    m->bfn = NULL;
    m->visibility = k->cur_visibility;   /* attr_reader/writer inside `private`/`protected` inherits it */
    c->vm->method_serial++;
}

/* Install an undef tombstone for `mid` on `k` (owner `cls`). */
void
korb_class_undef_slot(KorbClass *k, VALUE cls, uint32_t mid)
{
    struct korb_method *const um = korb_class_method_slot(k, mid);   /* libc alloc, no GC */
    um->mid = mid; um->orig_mid = mid;
    um->kind = KORB_METHOD_UNDEF; um->owner = cls; um->visibility = 0;
    um->params_cnt = -1; um->req_cnt = 0; um->post_cnt = 0; um->rest_slot = -1;
    um->locals_cnt = 0; um->uses_block = 0; um->is_simple = 0;
    um->body = NULL; um->bfn = NULL; um->rfn = NULL; um->rbfn = NULL;
    um->dm_proc = KORB_NIL; um->opt_defaults = NULL; um->kw_info = NULL; um->param_info = NULL;
}

/* Fire a Module definition callback (method_removed / method_undefined). */
RESULT
korb_fire_def_hook(CTX *c, VALUE *slots, VALUE mod, uint32_t mid, const char *hook, uint32_t hook_len)
{
    if (!KORB_CLASS_P(mod)) return RESULT_OK(KORB_NIL);
    const uint32_t h = korb_intern(c->vm, hook, hook_len);
    if (LIKELY(!korb_responds_to(c, mod, h))) return RESULT_OK(KORB_NIL);
    slots[0] = mod; slots[1] = ID2SYM(mid);
    return korb_send(c, slots + 2, h, 0, 1);
}

/* Fire the definition hook after a non-`def` definition (attr_*, define_method,
 * alias_method).  A singleton class routes to its attached object's
 * singleton_method_added instead, like CRuby. */
RESULT
korb_fire_method_added(CTX *c, VALUE *slots, VALUE definee, uint32_t mid)
{
    if (!KORB_CLASS_P(definee)) return RESULT_OK(KORB_NIL);
    struct korb_vm *const vm = c->vm;
    VALUE recv = definee;
    uint32_t hook = korb_intern(vm, "method_added", 12);
    if (VAL2CLASS(definee)->is_singleton) {
        VALUE att = KORB_UNDEF;
        for (uint32_t i = 0; i < vm->sklass_cnt; i++)
            if (vm->sklass_cls[i] == definee) { att = vm->sklass_obj[i]; break; }
        if (att == KORB_UNDEF) return RESULT_OK(KORB_NIL);   /* singleton with no live owner */
        recv = att;
        hook = korb_intern(vm, "singleton_method_added", 22);
    }
    /* Always dispatched: the no-op defaults live on Module / BasicObject, so an
     * `undef_method :singleton_method_added` must surface as NoMethodError /
     * #method_missing exactly as CRuby does. */
    slots[0] = recv; slots[1] = ID2SYM(mid);
    return korb_send(c, slots + 2, hook, 0, 1);
}

/* lookup mid up the superclass chain; *out_def (if non-NULL) gets the class
 * that defines the found method (for `super`). */
/* Search ONE class/module's local MRO segment — its prepended modules (reverse,
 * recursively so a prepended module's own prepends/includes join the chain),
 * then its own methods, then its included modules (reverse, recursively).  Does
 * NOT follow `superclass` (the caller walks that).  `depth` bounds pathological
 * cycles. */
static struct korb_method *korb_mro_seg_find(VALUE klass, uint32_t mid, VALUE *out_def, int depth) {
    if (!KORB_CLASS_P(klass) || depth > 64) return NULL;
    KorbClass *const k = VAL2CLASS(klass);
    if (k->prepended != KORB_NIL) {                          /* prepended: most-recently-prepended first */
        const KorbArray *pre = VAL2ARY(k->prepended);
        for (int32_t j = (int32_t)pre->len - 1; j >= 0; j--) {
            struct korb_method *m = korb_mro_seg_find(korb_items_data(pre->items)[j], mid, out_def, depth + 1);
            if (m) return m;
        }
    }
    for (uint32_t i = 0; i < k->method_cnt; i++)
        if (k->methods[i]->mid == mid) { if (out_def) *out_def = klass; return k->methods[i]; }
    if (k->included != KORB_NIL) {                           /* included: most-recently-included first */
        const KorbArray *inc = VAL2ARY(k->included);
        for (int32_t j = (int32_t)inc->len - 1; j >= 0; j--) {
            struct korb_method *m = korb_mro_seg_find(korb_items_data(inc->items)[j], mid, out_def, depth + 1);
            if (m) return m;
        }
    }
    return NULL;
}
static struct korb_method *
korb_class_find_method(VALUE klass, uint32_t mid, VALUE *out_def)
{
    while (KORB_CLASS_P(klass)) {
        struct korb_method *m = korb_mro_seg_find(klass, mid, out_def, 0);
        if (m) return m->kind == KORB_METHOD_UNDEF ? NULL : m;   /* undef tombstone: stop, report missing */
        klass = VAL2CLASS(klass)->superclass;
    }
    return NULL;
}

#define KORB_MCACHE_N 4096u   /* direct-mapped, power of two */
/* cached korb_class_find_method for user-object dispatch (the hot path).  A hit
 * requires serial match → no def/GC since fill → cached klass ptr + method ptr
 * (libc methods array) are both still valid. */
static inline struct korb_method *
korb_mcache_find(struct korb_vm *vm, VALUE klass, uint32_t mid, VALUE *out_def)
{
    const uint32_t idx = (((uint32_t)((uintptr_t)klass >> 4)) ^ (mid * 2654435761u)) & (KORB_MCACHE_N - 1);
    struct korb_mcache_ent *const e = &vm->mcache[idx];
    if (LIKELY(e->serial == vm->method_serial && e->klass == klass && e->mid == mid)) {
        *out_def = e->def_class;
        return e->m;
    }
    VALUE def = KORB_NIL;
    struct korb_method *const m = korb_class_find_method(klass, mid, &def);
    e->serial = vm->method_serial; e->klass = klass; e->mid = mid; e->m = m; e->def_class = def;
    *out_def = def;
    return m;
}

/* Set up an ISEQ frame (args at slots[-argc..]) with `self` and `def_class`
 * (the class that defines this method — for `super`), and dispatch.  Shared by
 * instance dispatch, implicit self-calls, global calls, super, and new's init.
 * Frame reserved cells top-down: self(fs-1), def_class(fs-2), then the block
 * group {block_entry(fs-5), def_env(fs-4), captured_self(fs-3)} if it yields. */
static __attribute__((no_stack_protector)) RESULT
korb_invoke_method(CTX *c, VALUE *slots, struct korb_method *m, uint32_t argc,
                   uint32_t line, uint32_t mid, VALUE self, VALUE def_class,
                   NODE *block, VALUE *def_env, VALUE captured_self)
{
    struct korb_vm *const vm = c->vm;
    const struct korb_kw_info *const kw = (const struct korb_kw_info *)m->kw_info;
    VALUE *const base = slots - argc;
    /* a method with keyword params consumes a trailing Hash arg as kwargs. */
    uint32_t pos_argc = argc;
    VALUE kwhash = KORB_NIL;
    /* Only a Hash the call site wrote as keywords binds to keyword params; a plain
     * trailing Hash argument is positional (Ruby 3).  `**nil` (-3): never. */
    if (UNLIKELY(kw != NULL && kw->kwrest_slot == -3 && argc >= 1 && korb_kwargs_hash_p(base[argc - 1])))
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "no keywords accepted");
    if (kw && kw->kwrest_slot != -3 && argc >= 1 && korb_kwargs_hash_p(base[argc - 1]))
        { kwhash = base[argc - 1]; pos_argc = argc - 1; }
    const uint32_t min_pos = m->req_cnt + m->post_cnt;   /* posts are required too */
    const uint32_t max_pos = (uint32_t)m->params_cnt + m->post_cnt;   /* req+opt fixed slots + posts */
    if (UNLIKELY(pos_argc < min_pos || (m->rest_slot < 0 && pos_argc > max_pos))) {
        if (m->rest_slot >= 0)
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %u+)", pos_argc, min_pos);
        if (min_pos == max_pos)
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %u)", pos_argc, max_pos);
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %u..%u)", pos_argc, min_pos, max_pos);
    }
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    }
    /* *rest: collect surplus positionals BEFORE memset / writing the self+def_class
     * frame-top cells, which would clobber high arg slots when argc is large.
     * A trailing kwhash sits at base[pos_argc] (= base[argc-1]); rest scratch
     * starts at base+argc to skip it, so kwhash stays rooted across these allocs. */
    VALUE rest_arr = KORB_NIL; bool have_rest = false;
    VALUE postbuf[32]; const uint32_t npost = m->post_cnt;
    if (UNLIKELY(npost > 32)) return korb_raise(c, slots, KORB_E_NOTIMPL, line, "too many post parameters");
    const uint32_t avail = pos_argc - npost;           /* positionals available for front + rest */
    if (m->rest_slot >= 0) {
        /* posts (last npost positionals) split off the tail; rest takes the middle. */
        uint32_t surplus = (avail > (uint32_t)m->params_cnt) ? avail - (uint32_t)m->params_cnt : 0;
        VALUE *cur = base + argc;                       /* scratch above all staged args (incl. kwhash) */
        /* `self` (and the block's captured_self) are bare C-locals not yet stored
         * in the frame; the rest-array allocs below GC under STRESS and would
         * leave them stale.  Park them at the foot of the rest scratch (scanned,
         * since the allocs publish a higher cursor) and re-read afterwards. */
        const bool park_block = (block != NULL && m->uses_block);
        cur[0] = self;
        if (park_block) cur[1] = captured_self;
        VALUE *const rcur = cur + (park_block ? 2 : 1);
        rcur[0] = UNWRAP(korb_ary_new(c, rcur, surplus ? surplus : 4));
        VALUE_REF arr = VALUE_REF_AT(&rcur[0]);
        for (uint32_t i = 0; i < surplus; i++)
            CHECK(korb_ary_push_val(c, rcur + 1, arr, base[(uint32_t)m->params_cnt + i]));
        rest_arr = VALUE_REF_GET(arr); have_rest = true;   /* C-local; no alloc until stored below */
        self = cur[0];                                  /* re-read: rest allocs may have moved it */
        if (park_block) captured_self = cur[1];
        for (uint32_t i = 0; i < npost; i++) postbuf[i] = base[avail + i];   /* capture posts (no alloc until written) */
        if (kw && kwhash != KORB_NIL) kwhash = base[pos_argc];   /* re-read GC-updated kwhash (rest allocs moved it) */
        for (uint32_t i = (uint32_t)m->params_cnt; i < pos_argc; i++) base[i] = 0;   /* clear surplus + post-source slots */
    } else if (npost > 0) {
        /* no rest: posts are the last npost args (required-after-optional). No alloc
         * here, so capture directly; the front (req+opt) keeps base[0..avail). */
        for (uint32_t i = 0; i < npost; i++) postbuf[i] = base[avail + i];
    }
    if (locals_cnt > pos_argc) memset(base + pos_argc, 0, (locals_cnt - pos_argc) * sizeof(VALUE));
    if (have_rest) base[m->rest_slot] = rest_arr;        /* after memset (rest_slot may be >= pos_argc when no surplus) */
    /* posts follow the rest slot, or the optionals (locals[params_cnt..]) when no rest. */
    const int32_t post_base = (m->rest_slot >= 0) ? m->rest_slot + 1 : m->params_cnt;
    for (uint32_t i = 0; i < npost; i++) base[post_base + i] = postbuf[i];
    base[-1] = self;                                     /* self at base[-1] (bottom header); needed for Klass.new where base[-1]=class != obj */
    base[locals_cnt - 1] = (VALUE)((uintptr_t)m | 1u);   /* method entry at frame top (tagged -> GC skips); super reads owner, __method__ reads mid */
    korb_ep_set(base, 0);                                        /* EP cell (base[-2]): no open env yet */
    korb_frame_magic_set(base, KORB_FT_METHOD);                  /* base[-3] integrity marker (no-op unless KORB_FRAME_MAGIC) */
    (void)def_class;
    if (block != NULL && m->uses_block) {
        base[locals_cnt - 4] = (VALUE)((uintptr_t)block | 1u);
        base[locals_cnt - 3] = (VALUE)(uintptr_t)def_env;   /* raw PREV (odd slots / clean KorbEnv) */
        base[locals_cnt - 2] = captured_self;
    }
    if (kw && kw->kwrest_slot >= 0) base[kw->kwrest_slot] = kwhash;   /* root kwhash across the GC below (kwrest slot is never a positional/keyword slot) */
    /* fill missing optional params by evaluating their defaults in method scope
     * (cursor = body cursor; defaults may reference earlier params + self). */
    for (uint32_t pi = avail; pi < (uint32_t)m->params_cnt; pi++) {   /* optionals past the provided front get defaults */
        NODE *const dflt = m->opt_defaults[pi - m->req_cnt];
        RESULT dr = (*dflt->head.dispatcher)(c, dflt, base + locals_cnt);
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        base[pi] = dr.value;              /* below cursor → rooted for later defaults/body */
    }
    if (kw) {
        if (kw->kwrest_slot >= 0) kwhash = base[kw->kwrest_slot];   /* re-read (GC during opt-fill) */
        uint64_t present = 0;
        for (uint32_t j = 0; j < kw->count; j++) {                  /* pass 1: present keywords (no alloc) */
            int32_t idx = (kwhash != KORB_NIL) ? korb_hash_find(VAL2HASH(kwhash), ID2SYM(kw->entries[j].mid)) : -1;
            if (idx >= 0) { base[kw->entries[j].slot] = korb_items_data(VAL2HASH(kwhash)->items)[2*idx+1]; if (j < 64) present |= (1ull << j); }
        }
        for (uint32_t j = 0; j < kw->count; j++) {                  /* pass 2: defaults / required check */
            if (j < 64 && (present & (1ull << j))) continue;
            if (kw->entries[j].deflt) {
                RESULT dr = (*kw->entries[j].deflt->head.dispatcher)(c, kw->entries[j].deflt, base + locals_cnt);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                base[kw->entries[j].slot] = dr.value;
            } else {
                return korb_raise_missing_kw(c, slots, line, kw, present);
            }
        }
        if (kw->kwrest_slot == -1 && kwhash != KORB_NIL) {          /* no **rest: every key must be declared */
            const KorbHash *const kh = VAL2HASH(kwhash);
            for (uint32_t i = 0; i < kh->len; i++) {
                const VALUE key = korb_items_data(kh->items)[2 * i];
                bool declared = false;
                for (uint32_t j = 0; j < kw->count; j++) if (key == ID2SYM(kw->entries[j].mid)) { declared = true; break; }
                if (declared) continue;
                if (SYMBOL_P(key))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword: :%s", korb_sym_name(vm, SYM2ID(key)));
                if (KORB_STRING_P(key))                             /* a String key is reported quoted (CRuby) */
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword: \"%.*s\"",
                                      (int)VAL2STR(key)->len, korb_strbuf_data(VAL2STR(key)->buf));
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword: %s", korb_type_name(key));
            }
        }
        if (kw->kwrest_slot >= 0) {                                 /* collect undeclared keys into **rest */
            VALUE *cur = base + locals_cnt;
            cur[0] = UNWRAP(korb_hash_new(c, cur, 4));              /* new kwrest hash at cur[0] */
            VALUE_REF kr = VALUE_REF_AT(&cur[0]);
            VALUE kh = base[kw->kwrest_slot];
            if (kh != KORB_NIL) {
                uint32_t hn = VAL2HASH(kh)->len;
                for (uint32_t i = 0; i < hn; i++) {
                    VALUE key = korb_items_data(VAL2HASH(base[kw->kwrest_slot])->items)[2*i];
                    bool declared = false;
                    for (uint32_t j = 0; j < kw->count; j++) if (key == ID2SYM(kw->entries[j].mid)) { declared = true; break; }
                    if (declared) continue;
                    cur[1] = key;
                    VALUE val = korb_items_data(VAL2HASH(base[kw->kwrest_slot])->items)[2*i+1];
                    CHECK(korb_hash_set(c, cur + 2, kr, VALUE_REF_AT(&cur[1]), val));
                }
            }
            base[kw->kwrest_slot] = VALUE_REF_GET(kr);
        }
    }
    NODE *const body = m->body;
    /* a method body has its own default definee (its class), so an enclosing
     * instance_eval's must not leak into it */
    const VALUE saved_definee = c->def_definee, saved_cvar = c->cvar_cref;
    if (UNLIKELY(saved_definee != KORB_NIL)) c->def_definee = KORB_NIL;
    if (UNLIKELY(saved_cvar != KORB_NIL)) c->cvar_cref = KORB_NIL;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (UNLIKELY(saved_definee != KORB_NIL)) c->def_definee = saved_definee;
    if (UNLIKELY(saved_cvar != KORB_NIL)) c->cvar_cref = saved_cvar;
    if (r.state == KORB_RETURN) {
        /* Consume only a return targeted at this method (NULL = nearest-method,
         * the common case) — a block's `return` aimed at an outer method passes
         * through unchanged. */
        if (c->return_target == NULL || c->return_target == base) {
            r.state = KORB_NORMAL;
            c->return_target = NULL;
        }
    }
    else if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    korb_frame_magic_check(base, KORB_FT_METHOD, "korb_invoke");   /* frame integrity (no-op unless KORB_FRAME_MAGIC) */
    if (UNLIKELY(korb_frame_escaped(base))) r = korb_close_ret(c, base + locals_cnt, base, r);
    return r;
}

/* True if `m` can take the hash-free keyword fast path: an ISEQ with keyword
 * params but no **kwrest / *rest / post params (the box(x:,y:,z:) shape). */
static inline bool korb_kw_fast_eligible(const struct korb_method *m) {
    const struct korb_kw_info *kw = (const struct korb_kw_info *)m->kw_info;
    return m->kind == KORB_METHOD_ISEQ && kw != NULL && kw->kwrest_slot == -1 &&
           m->rest_slot < 0 && m->post_cnt == 0;   /* -2 = anonymous ** (accept-all) → use the hash path so extras are discarded, not rejected */
}

/* Hash-free keyword invoke (eligibility checked by korb_kw_fast_eligible).
 * Positionals at base[0..pos_argc); keyword VALUES at base[pos_argc..+kw_argc)
 * with names kw_syms[].  Binds keywords straight to their param slots — no
 * kwargs Hash is built.  GC discipline: the present keyword values are copied
 * into their (rooted) frame slots BEFORE any default expression runs, so the
 * stack-captured kwbuf is dead before the first GC. */
static __attribute__((no_stack_protector)) RESULT
korb_invoke_kw_simple(CTX *c, VALUE *slots, struct korb_method *m, uint32_t pos_argc,
                      const uint32_t *kw_syms, uint32_t kw_argc, uint32_t line, uint32_t mid,
                      VALUE self, VALUE def_class)
{
    struct korb_vm *const vm = c->vm;
    const struct korb_kw_info *const kw = (const struct korb_kw_info *)m->kw_info;
    VALUE *const base = slots - (pos_argc + kw_argc);
    (void)def_class;
    if (UNLIKELY(pos_argc < m->req_cnt || pos_argc > (uint32_t)m->params_cnt))
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %u..%d)", pos_argc, m->req_cnt, m->params_cnt);
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit || &cstack_probe < c->cstack_limit))
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    VALUE kwbuf[64];
    if (UNLIKELY(kw_argc > 64)) return korb_raise(c, slots, KORB_E_NOTIMPL, line, "too many keyword arguments");
    for (uint32_t p = 0; p < kw_argc; p++) kwbuf[p] = base[pos_argc + p];   /* capture before memset */
    if (locals_cnt > pos_argc) memset(base + pos_argc, 0, (locals_cnt - pos_argc) * sizeof(VALUE));
    base[locals_cnt - 1] = (VALUE)((uintptr_t)m | 1u);   /* method entry at frame top */
    korb_ep_set(base, 0);                                        /* EP cell (base[-2]): no open env yet */
    korb_frame_magic_set(base, KORB_FT_METHOD);                  /* base[-3] integrity marker (no-op unless KORB_FRAME_MAGIC) */
    (void)self;                                          /* self already at base[-1] (staged receiver, bottom header) */
    /* fast path: all keywords supplied in declared order (the common call shape,
     * e.g. box(x:,y:,z:) on def box(x:,y:,z:)) — direct positional bind, no scan,
     * no missing/unknown checks (every param present, every arg consumed). */
    if (LIKELY(kw_argc == kw->count)) {
        bool ordered = true;
        for (uint32_t j = 0; j < kw_argc; j++) if (kw_syms[j] != kw->entries[j].mid) { ordered = false; break; }
        if (LIKELY(ordered)) {
            for (uint32_t j = 0; j < kw_argc; j++) base[kw->entries[j].slot] = kwbuf[j];
            for (uint32_t pi = pos_argc; pi < (uint32_t)m->params_cnt; pi++) {   /* optional positional defaults */
                NODE *const dflt = m->opt_defaults[pi - m->req_cnt];
                RESULT dr = (*dflt->head.dispatcher)(c, dflt, base + locals_cnt);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                base[pi] = dr.value;
            }
            goto run_body;
        }
    }
    /* bind present keywords now (pure copies, no GC) → kwbuf dead afterwards */
    uint64_t present = 0;
    for (uint32_t j = 0; j < kw->count; j++) {
        for (uint32_t p = 0; p < kw_argc; p++)
            if (kw_syms[p] == kw->entries[j].mid) { base[kw->entries[j].slot] = kwbuf[p]; if (j < 64) present |= (1ull << j); break; }
    }
    /* unknown-keyword check (no **rest here) */
    for (uint32_t p = 0; p < kw_argc; p++) {
        bool declared = false;
        for (uint32_t j = 0; j < kw->count; j++) if (kw_syms[p] == kw->entries[j].mid) { declared = true; break; }
        if (UNLIKELY(!declared)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword: :%s", korb_sym_name(vm, kw_syms[p]));
    }
    /* optional positional defaults (after the provided positionals) */
    for (uint32_t pi = pos_argc; pi < (uint32_t)m->params_cnt; pi++) {
        NODE *const dflt = m->opt_defaults[pi - m->req_cnt];
        RESULT dr = (*dflt->head.dispatcher)(c, dflt, base + locals_cnt);
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        base[pi] = dr.value;
    }
    /* keyword defaults / required-missing check */
    for (uint32_t j = 0; j < kw->count; j++) {
        if (j < 64 && (present & (1ull << j))) continue;
        if (kw->entries[j].deflt) {
            RESULT dr = (*kw->entries[j].deflt->head.dispatcher)(c, kw->entries[j].deflt, base + locals_cnt);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            base[kw->entries[j].slot] = dr.value;
        } else {
            return korb_raise_missing_kw(c, slots, line, kw, present);
        }
    }
  run_body:;
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) { if (c->return_target == NULL || c->return_target == base) { r.state = KORB_NORMAL; c->return_target = NULL; } }
    else if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) { KorbException *e = VAL2EXC(r.value); korb_bt_append(vm, e->line, korb_sym_name(vm, mid)); e->line = line; }
    korb_frame_magic_check(base, KORB_FT_METHOD, "korb_invoke");   /* frame integrity (no-op unless KORB_FRAME_MAGIC) */
    if (UNLIKELY(korb_frame_escaped(base))) r = korb_close_ret(c, base + locals_cnt, base, r);
    return r;
}

/* Build a kwargs Hash from stacked (kw_syms[p], base[pos_argc+p]) pairs at
 * base[pos_argc], then dispatch m via korb_invoke_method as a trailing-hash
 * call — the fallback for callees the fast path can't take (**rest / no kw
 * params / rest+kw).  base is slots-(pos_argc+kw_argc). */
static RESULT
korb_invoke_kw_viahash(CTX *c, VALUE *slots, struct korb_method *m, uint32_t pos_argc,
                       const uint32_t *kw_syms, uint32_t kw_argc, uint32_t line, uint32_t mid,
                       VALUE self, VALUE def_class)
{
    VALUE *const base = slots - (pos_argc + kw_argc);
    VALUE *const cur = slots;                              /* scratch above the staged args */
    cur[0] = self;                                         /* park: building the hash below GCs,
    cur[1] = def_class;                                     * and both are bare C locals */
    cur[2] = UNWRAP(korb_hash_new(c, cur + 2, kw_argc));
    VALUE_REF h = VALUE_REF_AT(&cur[2]);
    for (uint32_t p = 0; p < kw_argc; p++) {
        cur[3] = ID2SYM(kw_syms[p]);
        cur[4] = base[pos_argc + p];                      /* re-read each iter (hash_set may GC) */
        CHECK(korb_hash_set(c, cur + 5, h, VALUE_REF_AT(&cur[3]), cur[4]));
    }
    base[pos_argc] = VALUE_REF_GET(h);                    /* trailing hash replaces the kw region head */
    ((AroObjectHeader *)(uintptr_t)base[pos_argc])->flags |= KORB_FL_KWARGS;   /* written as keywords */
    const VALUE rself = cur[0], rdef = cur[1];            /* read back before the frame overlaps cur */
    return korb_invoke_method(c, base + pos_argc + 1, m, pos_argc + 1, line, mid, rself, rdef, NULL, NULL, KORB_NIL);
}

/* korb_invoke_simple — the streamlined is_simple ISEQ invoke — now lives in
 * node.h as an always_inline so it folds into the code_store SDs too (node_call
 * inlines its own fast path). */

RESULT
korb_class_body(CTX *c, VALUE *slots, uint32_t name_sym, NODE *body_entry, VALUE superclass, int is_module, VALUE enclosing)
{
    if (superclass != KORB_NIL && !KORB_CLASS_P(superclass))
        { char rdb[224];
          return korb_raise(c, slots, KORB_E_TYPE, 0, "superclass must be an instance of Class (given %s)", korb_recv_desc(c, slots + 1, superclass, rdb, sizeof rdb)); }
    const bool super_given = (superclass != KORB_NIL);   /* `class C < X` vs plain `class C` */
    /* a class with no explicit superclass derives from Object, so its instances'
     * MRO reaches the universal Object methods (==, freeze, method, ...). */
    if (superclass == KORB_NIL && !is_module)
        superclass = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    VALUE_REF encl_ref = SLOTS_PUSH(slots, enclosing);   /* root the lexical namespace below; slots advances past it */
    /* find-or-create owner-aware: reopen the class/module of the SAME namespace
     * (M::C reopens M::C, not a top-level C). */
    const VALUE find_owner = KORB_CLASS_P(enclosing) ? enclosing : KORB_NIL;
    uint32_t fidx = korb_const_index_owned(c->vm, name_sym, find_owner);
    if (fidx == UINT32_MAX && korb_autoload_registered_p(c, find_owner, name_sym)) {
        /* reopening a name that is only registered as an autoload: require the
         * file first, then look again — otherwise the body would open a brand
         * new class and the file's definition would be lost. */
        slots[0] = superclass;                        /* park across the require's GC */
        slots[1] = find_owner;                        /* recv + arg sit just below the cursor */
        slots[2] = ID2SYM(name_sym);
        CHECK(korb_send(c, slots + 3, korb_intern(c->vm, "__autoload_open", 15), 0, 1));
        superclass = slots[0];
        enclosing = VALUE_REF_GET(encl_ref);          /* the require may have moved it */
        fidx = korb_const_index_owned(c->vm, name_sym, KORB_CLASS_P(enclosing) ? enclosing : KORB_NIL);
    }
    VALUE cls = (fidx != UINT32_MAX) ? c->vm->const_vals[fidx] : KORB_NIL;
    if (fidx != UINT32_MAX) {                    /* the name is taken: it must be the right KIND */
        const bool bad_kind = !KORB_CLASS_P(cls) ||
                              (is_module && !VAL2CLASS(cls)->is_module) ||
                              (!is_module && VAL2CLASS(cls)->is_module);
        if (bad_kind) {
            /* CRuby names the constant and points at its previous definition */
            char where[320] = "";
            for (uint32_t i = 0; i < c->vm->constloc_cnt; i++)
                if (c->vm->constlocs[i].name == name_sym &&
                    c->vm->constlocs[i].owner_serial == korb_const_owner_serial(c->vm, find_owner)) {
                    snprintf(where, sizeof where, "\n%s:%u: previous definition of %s was here",
                             korb_sym_name(c->vm, c->vm->constlocs[i].file_sym), c->vm->constlocs[i].line,
                             korb_sym_name(c->vm, name_sym));
                    break;
                }
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a %s%s",
                              korb_sym_name(c->vm, name_sym), is_module ? "module" : "class", where);
        }
        /* reopening with a DIFFERENT explicit superclass is an error */
        if (!is_module && super_given && VAL2CLASS(cls)->superclass != superclass &&
            !(VAL2CLASS(cls)->superclass == KORB_NIL &&
              superclass == korb_builtin_class_obj(c->vm, KORB_C_OBJECT)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "superclass mismatch for class %s", korb_sym_name(c->vm, name_sym));
    }
    if (!KORB_CLASS_P(cls)) {
        slots[0] = superclass;                   /* root super across korb_class_new's GC */
        slots[1] = UNWRAP(korb_class_new(c, slots + 2, name_sym, slots[0]));   /* cls (rooted) */
        if (is_module) VAL2CLASS(slots[1])->is_module = 1;
        const VALUE encl = VALUE_REF_GET(encl_ref);   /* re-read after korb_class_new's GC */
        const VALUE owner = KORB_CLASS_P(encl) ? encl : KORB_NIL;   /* normalize (top-level self=main → nil) so it matches find_owner */
        if (owner != KORB_NIL)                         /* lexical enclosing module/class → M::C names */
            ARO_STORE(c, VAL2CLASS(slots[1]), (VALUE *)(uintptr_t)&VAL2CLASS(slots[1])->enclosing, owner);
        korb_const_define_owned(c, name_sym, slots[1], owner);   /* owner = lexical module (nil top-level) → Module#constants + reopen */
        if (UNLIKELY(owner != KORB_NIL && korb_mod_hook_custom(c, owner, korb_intern(c->vm, "const_added", 11)))) {
            slots[2] = owner; slots[3] = ID2SYM(name_sym);   /* const_added runs before inherited (CRuby) */
            RESULT ar = korb_send_impl(c, slots + 4, korb_intern(c->vm, "const_added", 11), 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        }
        if (!is_module && slots[0] != KORB_NIL) {    /* fire superclass.inherited(cls) for a new subclass */
            CHECK(korb_register_subclass(c, slots + 2, slots[0], slots[1]));   /* record in super's subclass list */
            const uint32_t inh = korb_intern(c->vm, "inherited", 9);
            if (korb_responds_to(c, slots[0], inh)) {
                slots[2] = slots[0]; slots[3] = slots[1];   /* recv = super, arg0 = new class */
                RESULT hr = korb_send_impl(c, slots + 4, inh, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            }
        }
        cls = slots[1];                          /* the (possibly forwarded) new class */
    }
    slots[0] = cls;                              /* root for the body run + capture */
    VAL2CLASS(cls)->cur_visibility = 0;          /* each (re)opened body starts public */
    /* a class body's default definee is the class itself — an enclosing
     * instance_eval's definee must not reach into it */
    const VALUE saved_definee = c->def_definee, saved_cvar = c->cvar_cref;
    c->def_definee = KORB_NIL; c->cvar_cref = KORB_NIL;
    const RESULT br = korb_block_yield(c, slots + 1, body_entry, NULL, NULL, 0, &slots[0]);
    c->def_definee = saved_definee; c->cvar_cref = saved_cvar;
    return br;
}

/* `class << recv; body; end` — run the body with self = recv's singleton class,
 * so any statement (def / attr_accessor / private / alias_method / expressions)
 * applies to the singleton, not just method defs. */
RESULT
korb_sclass_body(CTX *c, VALUE *slots, NODE *body_entry, VALUE recv, VALUE enclosing)
{
    slots[0] = recv;                             /* root recv across the singleton alloc */
    slots[1] = enclosing;                        /* rooted: the alloc below can move it */
    const VALUE sing = UNWRAP(korb_obj_singleton(c, slots + 2, slots[0]));
    slots[0] = sing;                             /* self for the body = the singleton class */
    /* `class << obj` inside a module body is lexically nested in it, so record
     * that: constant reads in the body (and in classes nested in it) walk out
     * through `enclosing`, as Module.nesting does in CRuby. */
    if (KORB_CLASS_P(slots[1]) && VAL2CLASS(slots[0])->enclosing == KORB_NIL)
        ARO_STORE(c, VAL2CLASS(slots[0]), (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->enclosing, slots[1]);
    const VALUE saved_definee = c->def_definee;  /* the body defines on the singleton, via self */
    const VALUE saved_cvar = c->cvar_cref;
    c->def_definee = KORB_NIL; c->cvar_cref = KORB_NIL;
    const RESULT br = korb_block_yield(c, slots + 1, body_entry, NULL, NULL, 0, &slots[0]);
    c->def_definee = saved_definee; c->cvar_cref = saved_cvar;
    return br;
}

/* Write the fully-qualified class name ("M::Inner::E") to fp, walking the lexical
 * `enclosing` chain outermost-first.  Returns false (writing nothing) for an
 * anonymous class (name_sym 0); an anonymous link in the chain ends qualification.
 * No GC (only reads interned names + writes fp). */
/* A module has a PERMANENT name when it was bound to a constant and every step
 * of its lexical path was too — that is what discards a temporary name and what
 * Module#set_temporary_name refuses to overwrite. */
static bool
korb_class_permanent_p(VALUE cls)
{
    for (int depth = 0; KORB_CLASS_P(cls) && depth <= 32; depth++) {
        const KorbClass *const k = VAL2CLASS(cls);
        if (k->name_sym == 0) return false;
        if (k->enclosing == KORB_NIL) return true;                 /* top-level constant */
        cls = k->enclosing;
    }
    return false;
}
static bool
korb_fprint_class_qname_d(CTX *c, FILE *fp, VALUE cls, int depth)
{
    const KorbClass *const k = VAL2CLASS(cls);
    /* a temporary name replaces the whole qualified name, until the module
     * becomes reachable through a permanent path (CRuby discards it then) */
    if (k->temp_name_sym != 0 && !korb_class_permanent_p(cls)) {
        fputs(korb_sym_name(c->vm, k->temp_name_sym), fp);
        return true;
    }
    if (k->name_sym == 0) return false;
    if (depth > 32) { fputs("...", fp); return true; }   /* a cyclic `enclosing` must not hang the printer */
    if (k->enclosing != KORB_NIL && KORB_CLASS_P(k->enclosing)) {
        if (korb_fprint_class_qname_d(c, fp, k->enclosing, depth + 1))
            fputs("::", fp);
        else {   /* named class under an ANONYMOUS namespace → CRuby shows #<Module:0x…>::Name */
            const KorbClass *const e = VAL2CLASS(k->enclosing);
            fprintf(fp, "#<%s:0x%016zx>::", e->is_module ? "Module" : "Class", (size_t)(uintptr_t)k->enclosing);
        }
    }
    fputs(korb_sym_name(c->vm, k->name_sym), fp);
    return true;
}
static bool korb_fprint_class_qname(CTX *c, FILE *fp, VALUE cls) { return korb_fprint_class_qname_d(c, fp, cls, 0); }
/* Build the qualified name as a fresh String (nil for anonymous).  For Class#name. */
static RESULT
korb_class_qname_str(CTX *c, VALUE *slots, VALUE cls)
{
    if (VAL2CLASS(cls)->name_sym == 0 && VAL2CLASS(cls)->temp_name_sym == 0) return RESULT_OK(KORB_NIL);
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_class_qname(c, ms, cls);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
/* Qualified name into a caller buffer (for error messages).  `cls` must be a class. */
static void
korb_class_qname_into(CTX *c, VALUE cls, char *out, size_t outsz)
{
    char *b = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&b, &sz);
    if (ms) { korb_fprint_class_qname(c, ms, cls); fclose(ms); }
    snprintf(out, outsz, "%s", b ? b : "");
    free(b);
}

/* Like korb_class_qname_into, but an anonymous class/module renders in its
 * #to_s form ("#<Module:0x...>") instead of an empty string. */
void
korb_class_desc_into(CTX *c, VALUE cls, char *out, size_t outsz)
{
    if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->name_sym == 0)
        snprintf(out, outsz, "#<%s:0x%016lx>",
                 VAL2CLASS(cls)->is_module ? "Module" : "Class", (unsigned long)(uintptr_t)cls);
    else
        korb_class_qname_into(c, cls, out, outsz);
}

/* Append `mod` and its transitive included modules to cref's `included` list
 * (deduped against cref's current ancestors).  Order: a module's own includes go
 * in first (deeper), then the module itself, so the reverse-iterated list yields
 * [mod, mod's includes…] — i.e. C.ancestors gains mod before mod's ancestors. */
static RESULT
korb_include_collect(CTX *c, VALUE *slots, VALUE_REF cref, VALUE_REF dst, VALUE mod, bool prepend)
{
    if (UNLIKELY(!KORB_CLASS_P(mod))) return RESULT_OK(KORB_NIL);
    slots[0] = mod;                                          /* root mod across recursion/push */
    VALUE_REF mref = VALUE_REF_AT(&slots[0]);
    if (VAL2CLASS(VALUE_REF_GET(mref))->included != KORB_NIL) {
        slots[1] = VAL2CLASS(VALUE_REF_GET(mref))->included; /* root mod's include list */
        VALUE_REF list = VALUE_REF_AT(&slots[1]);
        for (uint32_t j = 0; j < VAL2ARY(VALUE_REF_GET(list))->len; j++)
            CHECK(korb_include_collect(c, slots + 2, cref, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(list))->items)[j], prepend));
    }
    /* include skips a module already anywhere in the ancestry; prepend only
     * skips one already in THIS class's own prepend list — `B < A` where A
     * includes M still gets M in front when B prepends it (CRuby). */
    bool have = false;
    if (prepend) {
        const KorbArray *const dl = VAL2ARY(VALUE_REF_GET(dst));
        for (uint32_t i = 0; i < dl->len && !have; i++) if (korb_items_data(dl->items)[i] == VALUE_REF_GET(mref)) have = true;
    } else have = korb_class_has_ancestor(VALUE_REF_GET(cref), VALUE_REF_GET(mref));
    if (!have) CHECK(korb_ary_push_val(c, slots + 1, dst, VALUE_REF_GET(mref)));
    return RESULT_OK(KORB_NIL);
}
/* korb_include_collect + CRuby's re-include rule: when `mod` is ALREADY an
 * ancestor, the sub-modules it has gained since must land directly below it in
 * the MRO, not at the top.  The list is reverse-iterated (last = nearest), so
 * that means rotating the freshly appended block down to mod's index. */
static RESULT
korb_include_apply(CTX *c, VALUE *slots, VALUE_REF cref, VALUE_REF dst, VALUE mod, bool prepend)
{
    const uint32_t before = VAL2ARY(VALUE_REF_GET(dst))->len;
    int32_t at = -1;
    for (uint32_t i = 0; i < before; i++)
        if (korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[i] == mod) { at = (int32_t)i; break; }
    CHECK(korb_include_collect(c, slots, cref, dst, mod, prepend));
    if (at < 0) return RESULT_OK(KORB_NIL);                  /* fresh include: collect order is already right */
    KorbArray *const arr = VAL2ARY(VALUE_REF_GET(dst));      /* no GC below */
    const uint32_t n = arr->len - before;
    if (n == 0) return RESULT_OK(KORB_NIL);
    VALUE *const items = korb_items_data(arr->items);
    for (uint32_t k = 0; k < n; k++) {                       /* rotate one element at a time (write-barriered) */
        const VALUE v = items[arr->len - 1];
        for (uint32_t i = arr->len - 1; i > (uint32_t)at; i--)
            ARO_STORE(c, arr->items, &items[i], items[i - 1]);
        ARO_STORE(c, arr->items, &items[at], v);
    }
    return RESULT_OK(KORB_NIL);
}

/* `include mod...` in a class/module body: append each module to klass->included
 * (later lookups check most-recently-included first). Returns the class. */
RESULT
korb_do_include(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods)
{
    c->vm->const_serial++;   /* the ancestry changed: constant lookups can resolve differently */
    if (UNLIKELY(VALUE_SLICE_LEN(mods) == 0))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    { RESULT fr = korb_check_def_frozen(c, slots, klass); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* include/extend on a frozen class/object → FrozenError */
    slots[0] = klass;                            /* root klass across allocs */
    VALUE_REF kref = VALUE_REF_AT(&slots[0]);
    if (VAL2CLASS(klass)->included == KORB_NIL) {
        VALUE arr = UNWRAP(korb_ary_new(c, slots + 1, 4));
        KorbClass *k = VAL2CLASS(VALUE_REF_GET(kref));   /* re-read after GC */
        ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->included, arr);
    }
    const uint32_t included_mid = korb_intern(c->vm, "included", 8);
    const uint32_t append_features_mid = korb_intern(c->vm, "append_features", 15);
    for (int32_t i = (int32_t)VALUE_SLICE_LEN(mods) - 1; i >= 0; i--) {   /* reverse: `include A, B` → A nearest */
        const VALUE mv = VALUE_SLICE_GET(mods, i);
        if (UNLIKELY(!KORB_CLASS_P(mv) || !VAL2CLASS(mv)->is_module))   /* a Class (not Module) → TypeError, like CRuby */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(mv));
        if (UNLIKELY(korb_class_has_ancestor(mv, VALUE_REF_GET(kref))))   /* mod already has us as an ancestor → cycle */
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cyclic include detected");
        slots[2] = mv;                                         /* mod (rooted across the dispatch + hook) */
        if (LIKELY(korb_responds_to(c, mv, append_features_mid))) {   /* overridable insertion (skipped pre-registration at init) */
            slots[3] = mv; slots[4] = VALUE_REF_GET(kref);     /* mod.append_features(base) */
            RESULT fr = korb_send_impl(c, slots + 5, append_features_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
        } else {                                               /* default insertion (init, before Module methods exist) */
            slots[1] = VAL2CLASS(VALUE_REF_GET(kref))->included;
            CHECK(korb_include_apply(c, slots + 3, kref, VALUE_REF_AT(&slots[1]), slots[2], false));
        }
        if (UNLIKELY(korb_responds_to(c, slots[2], included_mid))) {   /* fire Module#included(base) hook (direct module only) */
            slots[3] = slots[2];                               /* recv = mod */
            slots[4] = VALUE_REF_GET(kref);                    /* arg0 = base class */
            RESULT hr = korb_send_impl(c, slots + 5, included_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        }
    }
    return RESULT_OK(VALUE_REF_GET(kref));
}

/* `prepend mod...` in a class/module body: append each module to klass->prepended
 * (searched before the class's own methods; most-recently-prepended first).  A
 * method in a prepended module sees the class itself as its `super`. */
RESULT
korb_do_prepend(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods)
{
    c->vm->const_serial++;   /* the ancestry changed: constant lookups can resolve differently */
    if (UNLIKELY(VALUE_SLICE_LEN(mods) == 0))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    { RESULT fr = korb_check_def_frozen(c, slots, klass); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* prepend on a frozen class → FrozenError */
    slots[0] = klass;                            /* root klass across allocs */
    VALUE_REF kref = VALUE_REF_AT(&slots[0]);
    if (VAL2CLASS(klass)->prepended == KORB_NIL) {
        VALUE arr = UNWRAP(korb_ary_new(c, slots + 1, 4));
        KorbClass *k = VAL2CLASS(VALUE_REF_GET(kref));   /* re-read after GC */
        ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->prepended, arr);
    }
    const uint32_t prepended_mid = korb_intern(c->vm, "prepended", 9);
    const uint32_t prepend_features_mid = korb_intern(c->vm, "prepend_features", 16);
    for (int32_t i = (int32_t)VALUE_SLICE_LEN(mods) - 1; i >= 0; i--) {   /* reverse: `include A, B` → A nearest */
        const VALUE mv = VALUE_SLICE_GET(mods, i);
        if (UNLIKELY(!KORB_CLASS_P(mv) || !VAL2CLASS(mv)->is_module))   /* a Class (not Module) → TypeError, like CRuby */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(mv));
        if (UNLIKELY(korb_class_has_ancestor(mv, VALUE_REF_GET(kref))))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cyclic prepend detected");
        slots[2] = mv;                                         /* mod (rooted across the dispatch + hook) */
        if (LIKELY(korb_responds_to(c, mv, prepend_features_mid))) {   /* overridable insertion (skipped pre-registration at init) */
            slots[3] = mv; slots[4] = VALUE_REF_GET(kref);     /* mod.prepend_features(base) */
            RESULT fr = korb_send_impl(c, slots + 5, prepend_features_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
        } else {                                               /* default insertion (init, before Module methods exist) */
            slots[1] = VAL2CLASS(VALUE_REF_GET(kref))->prepended;
            CHECK(korb_include_collect(c, slots + 3, kref, VALUE_REF_AT(&slots[1]), slots[2], true));
        }
        if (UNLIKELY(korb_responds_to(c, slots[2], prepended_mid))) {   /* fire Module#prepended(base) hook */
            slots[3] = slots[2];                               /* recv = mod */
            slots[4] = VALUE_REF_GET(kref);                    /* arg0 = base class */
            RESULT hr = korb_send_impl(c, slots + 5, prepended_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        }
    }
    c->vm->method_serial++;                      /* MRO changed → flush method caches */
    return RESULT_OK(VALUE_REF_GET(kref));
}

/* Module#append_features(base) / #prepend_features(base): the overridable step
 * that actually inserts self (+ its transitive includes) into base's ancestor
 * chain.  korb_do_include / _do_prepend dispatch these (so a user override runs)
 * and then fire the #included / #prepended callback. */
static RESULT korb_mod_features(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool prepend) {
    const VALUE base = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(VALUE_REF_GET(self)) || !VAL2CLASS(VALUE_REF_GET(self))->is_module))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s_features must be called for modules", prepend ? "prepend" : "append");
    if (UNLIKELY(!KORB_CLASS_P(base)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(base));
    if (UNLIKELY(base == VALUE_REF_GET(self) || korb_class_has_ancestor(VALUE_REF_GET(self), base)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cyclic include detected");
    KORB_CHECK_FROZEN(c, slots, base);           /* frozen target → FrozenError before any change */
    slots[0] = base;                             /* [0] base (rooted) */
    slots[1] = VALUE_REF_GET(self);              /* [1] self = the module */
    VALUE *const listp = prepend ? (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->prepended
                                 : (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->included;
    if (*listp == KORB_NIL) {
        VALUE arr = UNWRAP(korb_ary_new(c, slots + 2, 4));
        ARO_STORE(c, VAL2CLASS(slots[0]), prepend ? (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->prepended
                                                  : (VALUE *)(uintptr_t)&VAL2CLASS(slots[0])->included, arr);
    }
    slots[2] = prepend ? VAL2CLASS(slots[0])->prepended : VAL2CLASS(slots[0])->included;   /* [2] dst */
    CHECK(korb_include_apply(c, slots + 3, VALUE_REF_AT(&slots[0]), VALUE_REF_AT(&slots[2]), slots[1], prepend));
    c->vm->method_serial++;                      /* MRO changed → flush caches */
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_mod_append_features(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_mod_features(c, slots, self, a, false); }
static RESULT korb_m_mod_prepend_features(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_mod_features(c, slots, self, a, true); }

static VALUE korb_dispatch_class(CTX *c, VALUE self);

/* Linearize `klass`'s MRO into buf: per class up the superclass chain, the
 * prepended modules (most-recent first), then the class, then the included
 * modules (most-recent first).  Caps at `max`.  Matches korb_class_find_method's
 * search order, so a forward scan finds the same method it would. */
/* One MRO segment into buf: the class's prepends (each a segment of its own —
 * a prepended module brings its own prepends), the class, then its includes.
 * Same order as korb_mro_seg_find; a repeat keeps its first (nearest) slot. */
static int
korb_linearize_seg(VALUE klass, VALUE *buf, int n, int max, int depth)
{
    if (!KORB_CLASS_P(klass) || depth > 64 || n >= max) return n;
    const KorbClass *const k = VAL2CLASS(klass);
    if (k->prepended != KORB_NIL) {
        const KorbArray *const pa = VAL2ARY(k->prepended);
        for (int32_t j = (int32_t)pa->len - 1; j >= 0; j--)
            n = korb_linearize_seg(korb_items_data(pa->items)[j], buf, n, max, depth + 1);
    }
    if (n < max) {
        /* A module can legitimately appear twice — prepended here and included
         * by an ancestor — so only skip an immediate repeat (a self-cycle). */
        if (n == 0 || buf[n - 1] != klass) buf[n++] = klass;
    }
    if (k->included != KORB_NIL) {
        const KorbArray *const ia = VAL2ARY(k->included);
        for (int32_t j = (int32_t)ia->len - 1; j >= 0; j--)
            n = korb_linearize_seg(korb_items_data(ia->items)[j], buf, n, max, depth + 1);
    }
    return n;
}
static int
korb_linearize_mro(VALUE klass, VALUE *buf, int max)
{
    int n = 0;
    while (KORB_CLASS_P(klass) && n < max) {
        n = korb_linearize_seg(klass, buf, n, max, 0);
        klass = VAL2CLASS(klass)->superclass;
    }
    return n;
}

/* Resolve what `super` would call from the frame whose entry cell is
 * `entry_cell`, without calling it: the MRO successor of the defining class.
 * NULL when there is none (what `defined?(super)` reports as nil).  Shared by
 * korb_super and node_defined_super so the two can never disagree. */
struct korb_method *
korb_super_find(CTX *c, uint32_t mid, VALUE entry_cell, VALUE self, VALUE *out_def_class)
{
    const struct korb_method *const cur =
        ((uintptr_t)entry_cell & 1u) ? (const struct korb_method *)((uintptr_t)entry_cell & ~(uintptr_t)1u) : NULL;
    const VALUE def_class = cur ? cur->owner : KORB_NIL;
    if (out_def_class) *out_def_class = def_class;
    VALUE found_def = KORB_NIL;
    struct korb_method *m = NULL;
    /* Walk self's linearized MRO and resume the search strictly after def_class.
     * This is what makes `super` from a PREPENDED module reach the class itself
     * (MRO [M, C, ...]); for plain inheritance / include it is equivalent to the
     * def_class.included-then-superclass walk. */
    if (KORB_CLASS_P(def_class)) {
        VALUE mro[256];
        const int n = korb_linearize_mro(korb_dispatch_class(c, self), mro, 256);
        int di = -1;
        for (int i = 0; i < n; i++) if (mro[i] == def_class) { di = i; break; }
        if (di >= 0) {
            for (int i = di + 1; i < n && m == NULL; i++) {
                KorbClass *mk = VAL2CLASS(mro[i]);
                for (uint32_t q = 0; q < mk->method_cnt; q++)
                    if (mk->methods[q]->mid == mid) { m = mk->methods[q]; found_def = mro[i]; break; }
            }
        } else {
            /* def_class not in self's class MRO (e.g. an extended/singleton or a
             * builtin super): fall back to def_class.included then superclass. */
            VALUE inc = VAL2CLASS(def_class)->included;
            if (inc != KORB_NIL) {
                const KorbArray *arr = VAL2ARY(inc);
                for (int32_t j = (int32_t)arr->len - 1; j >= 0 && m == NULL; j--) {
                    VALUE mod = korb_items_data(arr->items)[j];
                    if (!KORB_CLASS_P(mod)) continue;
                    KorbClass *mk = VAL2CLASS(mod);
                    for (uint32_t i = 0; i < mk->method_cnt; i++)
                        if (mk->methods[i]->mid == mid) { m = mk->methods[i]; found_def = mod; break; }
                }
            }
            if (m == NULL) m = korb_class_find_method(VAL2CLASS(def_class)->superclass, mid, &found_def);
        }
    }
    if (m != NULL && m->kind == KORB_METHOD_UNDEF) { m = NULL; found_def = KORB_NIL; }   /* undef tombstone */
    if (out_def_class) *out_def_class = found_def;   /* the class the method came from (super's def_class) */
    return m;
}

/* `super` — invoke `mid` starting from after def_class in self's MRO, keeping
 * self.  `entry_cell` is the frame's fs-2 cell: the running method's entry
 * (tagged korb_method*); its owner is the def_class whose MRO successor runs. */
RESULT
korb_super(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
           VALUE entry_cell, VALUE self, NODE *block, VALUE *def_env, VALUE captured_self)
{
    if (UNLIKELY(mid == c->vm->mid_dm_super)) {   /* `super` in a define_method body */
        if (UNLIKELY(c->dm_entry == NULL))
            return korb_raise(c, slots, KORB_E_NOMETHOD, line, "super called outside of method");
        mid = c->dm_entry->mid;
        entry_cell = (VALUE)((uintptr_t)c->dm_entry | 1u);
    } else if (UNLIKELY(!((uintptr_t)entry_cell & 1u))) {
        /* `super` inside a block: the block's frame carries no method entry, so
         * take the one the name currently resolves to on the receiver and search
         * above its owner.  (A block outliving a redefinition can pick the newer
         * definition — CRuby tracks the block's home method exactly.) */
        VALUE odef = KORB_NIL;
        const struct korb_method *const own = korb_class_find_method(korb_dispatch_class(c, self), mid, &odef);
        if (own != NULL) entry_cell = (VALUE)((uintptr_t)own | 1u);
    }
    VALUE found_def = KORB_NIL;
    struct korb_method *const m = korb_super_find(c, mid, entry_cell, self, &found_def);
    const VALUE def_class = ((uintptr_t)entry_cell & 1u)
        ? ((const struct korb_method *)((uintptr_t)entry_cell & ~(uintptr_t)1u))->owner : KORB_NIL;
    if (m == NULL && mid == c->vm->mid_new && KORB_CLASS_P(self)) {
        /* super from a user `def Klass.new`: the default allocator is a dispatch
         * special-case with no table entry, so route the super straight into a
         * plain `self.__korb_default_new(args)` — a hidden singleton the
         * dispatcher never intercepts is not needed; korb_send_impl's mid_new
         * cascade IS the default allocator, and it only runs when the class has
         * no user `new` in its dispatch chain.  Sidestep by sending :new to a
         * throwaway subclass?  No — simplest correct: call korb_send_impl with
         * the caller's own def-singleton temporarily hidden is intrusive.
         * Instead mark re-entry: stash the defining singleton so the smethod
         * override check skips it once. */
        slots[0] = self;
        for (uint32_t i = 0; i < argc; i++) slots[1 + i] = slots[-(korb_sword_t)argc + (korb_sword_t)i];
        c->vm->super_new_skip = def_class;             /* consumed by korb_send_impl's def self.new check */
        RESULT r = korb_send_impl(c, slots + 1 + argc, mid, line, argc, NULL, NULL, NULL);
        c->vm->super_new_skip = KORB_NIL;
        return r;
    }
    if (UNLIKELY(m == NULL))
        return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                          "super: no superclass method '%s'", korb_sym_name(c->vm, mid));
    if (m->kind == KORB_METHOD_ATTR_R)
        return RESULT_OK(korb_ivar_get(c, self, ID2SYM(m->attr_ivar)));
    if (m->kind == KORB_METHOD_CFUNC) {           /* super into a builtin (e.g. Exception#initialize) */
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
        slots[0] = self;                          /* receiver in scratch (rooted below the rfn cursor) */
        const VALUE_REF recv = VALUE_REF_AT(&slots[0]);
        const VALUE_SLICE args = VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc);
        if (m->uses_block) {
            slots[1] = captured_self;             /* park in scanned slot for rbfn cself ptr */
            return m->rbfn(c, slots + 2, recv, args, block, def_env, &slots[1]);
        }
        return m->rfn(c, slots + 1, recv, args);
    }
    if (m->kind == KORB_METHOD_DM) {              /* super into a define_method'd method → run its Proc body */
        const KorbProc *const p = VAL2PROC(m->dm_proc);
        slots[0] = self;                          /* captured_self = receiver (rooted scanned slot) */
        const struct korb_method *const dm_saved = c->dm_entry;
        c->dm_entry = m;                          /* chained super out of this body */
        RESULT r = korb_block_yield(c, slots + 1, p->iseq, (VALUE *)(uintptr_t)p->env,
                                    &slots[-(korb_sword_t)argc], argc, &slots[0]);
        c->dm_entry = dm_saved;
        if (r.state == KORB_RETURN) { r.state = KORB_NORMAL; c->return_target = NULL; }   /* return-from-method */
        return r;
    }
    /* restage [magic, EP, self, args] above the cursor so the callee frame has its
     * KORB_FRAME_HDR meta cells zeroed (base[-3]=magic, base[-2]=EP) with self at
     * base[-1]; super's args sit at slots[-argc..-1], self is separate. */
    for (uint32_t j = 0; j < argc; j++) slots[3 + j] = slots[-(korb_sword_t)argc + j];
    slots[0] = 0;                                 /* base[-3] (magic) */
    slots[1] = 0;                                 /* base[-2] (EP)    */
    slots[2] = self;                              /* base[-1]         */
    return korb_invoke_method(c, slots + 3 + argc, m, argc, line, mid, self, found_def, block, def_env, captured_self);
}

/* a descends from (or equals) b */
static bool
korb_class_le(VALUE a, VALUE b)
{
    while (KORB_CLASS_P(a)) {
        if (a == b) return true;
        /* Modules count: `rescue IO::WaitReadable` must match an exception class
         * that only *includes* that module, and Class#<= is ancestry, not just
         * the superclass chain. */
        const KorbClass *const k = VAL2CLASS(a);
        for (int which = 0; which < 2; which++) {
            const VALUE mods = which == 0 ? k->prepended : k->included;
            if (mods == KORB_NIL) continue;
            const KorbArray *const ma = VAL2ARY(mods);
            for (uint32_t j = 0; j < ma->len; j++)
                if (korb_items_data(ma->items)[j] == b) return true;
        }
        a = k->superclass;
    }
    return false;
}

/* does exception `exc` match rescue class `rescue_class`? */
bool
korb_exc_matches(CTX *c, VALUE exc, VALUE rescue_class)
{
    if (!KORB_CLASS_P(rescue_class) || !KORB_EXC_P(exc)) return false;
    const VALUE uc = VAL2EXC(exc)->exc_class;            /* user exception subclass */
    if (uc != KORB_NIL) return korb_class_le(uc, rescue_class);
    uint32_t et = VAL2EXC(exc)->etype;
    if (et >= 24) return false;
    VALUE exc_class = korb_const_get(c->vm, c->vm->exc_name[et]);
    return korb_class_le(exc_class, rescue_class);
}

/* Per-instance class override table (subclass instances / extended objects).
 * Linear scan, but only ever reached for objects with KORB_FL_HAS_KLASS set. */
static VALUE korb_klass_override_get(const struct korb_vm *vm, VALUE obj) {
    for (uint32_t i = 0; i < vm->sklass_cnt; i++)
        if (vm->sklass_obj[i] == obj) return vm->sklass_cls[i];
    return KORB_NIL;
}
/* Start-of-lookup class for a user instance: its singleton class if one exists
 * (so `send`/internal dispatch see singleton methods), else its plain class.
 * The HAS_KLASS bit gates the scan, so objects without a singleton pay nothing. */
static inline VALUE korb_obj_dispatch_klass(const struct korb_vm *vm, VALUE self) {
    if (UNLIKELY(((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
        const VALUE ov = korb_klass_override_get(vm, self);
        if (ov != KORB_NIL) return ov;
    }
    return VAL2OBJ(self)->klass;
}
static void korb_klass_override_set(CTX *c, VALUE obj, VALUE cls) {
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->sklass_cnt; i++)        /* replace if present */
        if (vm->sklass_obj[i] == obj) { vm->sklass_cls[i] = cls; return; }
    if (vm->sklass_cnt == vm->sklass_capa) {             /* libc realloc, not a GC point */
        uint32_t nc = vm->sklass_capa ? vm->sklass_capa * 2 : 16;
        vm->sklass_obj = realloc(vm->sklass_obj, sizeof(VALUE) * nc);
        vm->sklass_cls = realloc(vm->sklass_cls, sizeof(VALUE) * nc);
        if (!vm->sklass_obj || !vm->sklass_cls) { fprintf(stderr, "koruby_precise: oom (sklass)\n"); abort(); }
        vm->sklass_capa = nc;
    }
    vm->sklass_obj[vm->sklass_cnt] = obj;
    vm->sklass_cls[vm->sklass_cnt] = cls;
    vm->sklass_cnt++;
    ((AroObjectHeader *)(uintptr_t)obj)->flags |= KORB_FL_HAS_KLASS;
}

/* Walk cls's superclass chain; return the enum of the builtin base class it
 * derives from (KORB_C_STRING/ARRAY/... or KORB_C_OBJECT), or KORB_NCLASS if the
 * chain contains no registered builtin class. */
static enum korb_class korb_builtin_base_class(struct korb_vm *vm, VALUE cls) {
    while (KORB_CLASS_P(cls)) {
        for (int k = 0; k < KORB_NCLASS; k++)
            if (vm->class_name[k] != 0 && korb_const_get(vm, vm->class_name[k]) == cls)
                return (enum korb_class)k;
        cls = VAL2CLASS(cls)->superclass;
    }
    return KORB_NCLASS;
}

/* class object of `self` (for `.class` / is_a?).  User objects → their klass;
 * overridden builtins → their subclass; exceptions → the etype's class; else the
 * builtin class via class_name[]. */
VALUE
korb_class_obj_of(CTX *c, VALUE self)
{
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) return VAL2OBJ(self)->klass;
    if (AROH_IS_GC_OBJECT(self) && (((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
        VALUE ov = korb_klass_override_get(c->vm, self);
        while (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) ov = VAL2CLASS(ov)->superclass;  /* singleton is transparent */
        if (ov != KORB_NIL) return ov;
    }
    /* (checked before the is_module shortcut below, so `Class.new(Module).new`
     * reports its own subclass rather than plain Module) */
    if (KORB_EXC_P(self)) {
        if (VAL2EXC(self)->exc_class != KORB_NIL) return VAL2EXC(self)->exc_class;   /* user exception subclass */
        uint32_t et = VAL2EXC(self)->etype;
        if (et < 24) return korb_const_get(c->vm, c->vm->exc_name[et]);
    }
    if (KORB_CLASS_P(self) && VAL2CLASS(self)->is_module)    /* a Module object → Module (not Class) */
        return korb_const_get(c->vm, korb_intern(c->vm, "Module", 6));
    if (KORB_ENUM_P(self)) {                                 /* lazy-mode enumerators → Enumerator::Lazy */
        const uint8_t m = VAL2ENUM(self)->mode;
        if ((m == 1 || m == 4) && KORB_CLASS_P(c->vm->lazy_class)) return c->vm->lazy_class;
    }
    return korb_const_get(c->vm, c->vm->class_name[korb_class_of(self)]);
}

/* O(1) class object for a builtin tag enum (see vm->class_obj_idx). */
static inline VALUE
korb_builtin_class_obj(const struct korb_vm *vm, enum korb_class e)
{
    const uint32_t idx = vm->class_obj_idx[e];
    return (idx == UINT32_MAX) ? KORB_NIL : vm->const_vals[idx];
}

/* Starting class object for receiver dispatch of `self` (the head of its MRO).
 * Unlike korb_class_obj_of this keeps singleton classes (method lookup must see
 * singleton/extended methods).  Covers user objects, overridden/extended
 * builtins, exceptions (by etype), and plain builtins (by tag). */
static VALUE
korb_dispatch_class(CTX *c, VALUE self)
{
    struct korb_vm *const vm = c->vm;
    if (KORB_OBJECT_P(self)) {
        if (UNLIKELY(((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
            const VALUE ov = korb_klass_override_get(vm, self);   /* singleton class (super = real klass) */
            if (ov != KORB_NIL) return ov;
        }
        const VALUE k = VAL2OBJ(self)->klass;
        if (k != KORB_NIL) return k;                 /* user instance */
        /* `main` (klass==nil) falls through to Object */
    } else if (AROH_IS_GC_OBJECT(self) &&
               (((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
        const VALUE ov = korb_klass_override_get(vm, self);   /* raw: singleton kept */
        if (ov != KORB_NIL) return ov;
    }
    if (KORB_CLASS_P(self)) {
        /* a class with no own singleton: dispatch through the nearest ancestor's
         * singleton so inherited class methods (def self.x in a parent) are found. */
        for (VALUE k = VAL2CLASS(self)->superclass; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass)
            if (((const AroObjectHeader *)(uintptr_t)k)->flags & KORB_FL_HAS_KLASS) {
                const VALUE ov = korb_klass_override_get(vm, k);
                if (KORB_CLASS_P(ov)) return ov;
            }
        if (VAL2CLASS(self)->is_module) {             /* a module's class is Module, not Class */
            const VALUE mm = korb_const_get(vm, vm->name_module);
            if (KORB_CLASS_P(mm)) return mm;
        }
    }
    if (KORB_EXC_P(self)) {
        if (VAL2EXC(self)->exc_class != KORB_NIL) return VAL2EXC(self)->exc_class;   /* user exception subclass → its MRO */
        const uint32_t et = VAL2EXC(self)->etype;
        if (et < 24 && vm->exc_name[et]) {
            const VALUE k = korb_const_get(vm, vm->exc_name[et]);
            if (KORB_CLASS_P(k)) return k;
        }
    }
    if (KORB_ENUM_P(self)) {                          /* lazy-mode enumerators report Enumerator::Lazy */
        const uint8_t m = VAL2ENUM(self)->mode;
        if ((m == 1 || m == 4) && KORB_CLASS_P(vm->lazy_class)) return vm->lazy_class;
    }
    return korb_builtin_class_obj(vm, korb_class_of(self));
}

/* True if `self` responds to `mid` (own MRO incl. inherited builtins). */
bool
korb_responds_to(CTX *c, VALUE self, uint32_t mid)
{
    /* `new` is dispatched by a special case (not a registered method), so the
     * MRO walk misses it — every non-module class still responds to it. */
    if (mid == c->vm->mid_new && KORB_CLASS_P(self) && !VAL2CLASS(self)->is_module)
        return true;
    /* send / __send__ / public_send are also special-dispatched (not registered);
     * every object responds to them. */
    if (mid == c->vm->mid_send || mid == c->vm->mid___send__ || mid == c->vm->mid_public_send)
        return true;
    const VALUE start = korb_dispatch_class(c, self);
    return KORB_CLASS_P(start) && korb_class_find_method(start, mid, NULL) != NULL;
}
/* true when `mod` overrides a Module hook (const_added).  Module's own default
 * answers nil, so firing it would cost a dispatch per constant for nothing. */
bool
korb_mod_hook_custom(CTX *c, VALUE mod, uint32_t mid)
{
    const VALUE dcls = korb_dispatch_class(c, mod);
    if (!KORB_CLASS_P(dcls)) return false;
    VALUE owner = KORB_NIL;
    if (korb_class_find_method(dcls, mid, &owner) == NULL) return false;
    return owner != korb_const_get(c->vm, korb_intern(c->vm, "Module", 6));
}

/* true when `cls` defines its own #=== (not Module's) — a rescue clause must
 * dispatch it rather than use the built-in exception match. */
/* true when `v`'s #== (or #!=) is still the default identity one, so `x == x`
 * may shortcut — CRuby's opt_equality does exactly this test. */
bool
korb_default_eq_p(CTX *c, VALUE v, uint32_t mid)
{
    const VALUE dcls = korb_dispatch_class(c, v);
    if (!KORB_CLASS_P(dcls)) return true;
    const struct korb_method *const m = korb_class_find_method(dcls, mid, NULL);
    return m == NULL || m->kind == KORB_METHOD_CFUNC;   /* only the C defaults on Object/BasicObject are CFUNCs here */
}
bool
korb_rescue_custom_eqq(CTX *c, VALUE cls)
{
    const VALUE dcls = korb_dispatch_class(c, cls);
    if (!KORB_CLASS_P(dcls)) return false;
    VALUE owner = KORB_NIL;
    if (korb_class_find_method(dcls, korb_intern(c->vm, "===", 3), &owner) == NULL) return false;
    struct korb_vm *const vm = c->vm;
    return owner != korb_const_get(vm, korb_intern(vm, "Module", 6)) &&
           owner != korb_const_get(vm, korb_intern(vm, "Class", 5)) &&
           owner != korb_builtin_class_obj(vm, KORB_C_OBJECT);
}

/* like korb_responds_to but also honors a user-defined #respond_to_missing?
 * (the type-conversion protocols — #to_str/#to_ary/#to_int/#to_hash — check
 * respond_to? before dispatching, so a proxy/delegator/mock that answers via
 * respond_to_missing? must be seen).  Dispatches only on the slow path (no real
 * method) when respond_to_missing? is actually defined.  slots must have >= 3
 * free cells for that dispatch. */
/* Pointer variant: roots *selfp across the respond_to? dispatch and writes the
 * (possibly moved) receiver back, so a caller holding it in a plain local stays
 * valid.  slots needs >= 3 free cells. */
bool
korb_responds_to_coerce_p(CTX *c, VALUE *slots, VALUE *selfp, uint32_t mid)
{
    if (korb_responds_to(c, *selfp, mid)) return true;
    const VALUE dcls = korb_dispatch_class(c, *selfp);
    if (!KORB_CLASS_P(dcls)) return false;
    VALUE rt_def = KORB_NIL, rtm_def = KORB_NIL;
    (void)korb_class_find_method(dcls, korb_intern(c->vm, "respond_to?", 11), &rt_def);
    const bool custom_rt = rt_def != KORB_NIL && rt_def != korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
    /* Kernel's own #respond_to_missing? always answers false — only an override
     * is worth a dispatch. */
    const bool has_rtm = korb_class_find_method(dcls, korb_intern(c->vm, "respond_to_missing?", 19), &rtm_def) != NULL &&
                         rtm_def != korb_const_get(c->vm, korb_intern(c->vm, "Kernel", 6));
    if (!(custom_rt || has_rtm)) return false;
    slots[0] = *selfp; slots[1] = ID2SYM(mid); slots[2] = KORB_TRUE;
    /* implicit conversions see private methods too (CRuby's rb_check_funcall),
     * so #respond_to_missing? is asked with include_all = true */
    const RESULT r = korb_send_impl(c, slots + 3, korb_intern(c->vm, "respond_to?", 11), 0, 2, NULL, NULL, NULL);
    *selfp = slots[0];                                /* writeback: the dispatch may have moved the receiver */
    return r.state == KORB_NORMAL && KORB_TRUTHY(r.value);
}
bool
korb_responds_to_coerce(CTX *c, VALUE *slots, VALUE self, uint32_t mid)
{
    if (korb_responds_to(c, self, mid)) return true;
    const VALUE dcls = korb_dispatch_class(c, self);
    if (!KORB_CLASS_P(dcls)) return false;
    /* Ask the object's own #respond_to? only when it (or #respond_to_missing?) is
     * customized — otherwise the default answer is already `false` and we skip the
     * dispatch.  Dispatching #respond_to? (not #respond_to_missing? directly) means
     * a proxy/mock that overrides either one is honored correctly. */
    VALUE rt_def = KORB_NIL, rtm_def = KORB_NIL;
    (void)korb_class_find_method(dcls, korb_intern(c->vm, "respond_to?", 11), &rt_def);
    const bool custom_rt = rt_def != KORB_NIL && rt_def != korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
    /* Kernel's own #respond_to_missing? always answers false — only an override
     * is worth a dispatch. */
    const bool has_rtm = korb_class_find_method(dcls, korb_intern(c->vm, "respond_to_missing?", 19), &rtm_def) != NULL &&
                         rtm_def != korb_const_get(c->vm, korb_intern(c->vm, "Kernel", 6));
    if (custom_rt || has_rtm) {
        /* include_private = true: a conversion protocol (#to_int / #to_str / …)
         * is looked up with private methods visible, as rb_check_funcall does —
         * mocks assert on the two-argument form. */
        slots[0] = self; slots[1] = ID2SYM(mid); slots[2] = KORB_TRUE;
        const RESULT r = korb_send_impl(c, slots + 3, korb_intern(c->vm, "respond_to?", 11), 0, 2, NULL, NULL, NULL);
        return r.state == KORB_NORMAL && KORB_TRUTHY(r.value);
    }
    return false;
}
/* like korb_responds_to but public-only (used by defined?(recv.meth), which sees
 * only publicly-callable methods through an explicit receiver).  Non-static so
 * node_eval.c can call it. */
bool
korb_responds_to_public(CTX *c, VALUE self, uint32_t mid)
{
    if (mid == c->vm->mid_new && KORB_CLASS_P(self) && !VAL2CLASS(self)->is_module) return true;
    if (mid == c->vm->mid_send || mid == c->vm->mid___send__ || mid == c->vm->mid_public_send) return true;
    const VALUE start = korb_dispatch_class(c, self);
    if (!KORB_CLASS_P(start)) return false;
    const struct korb_method *const m = korb_class_find_method(start, mid, NULL);
    return m != NULL && m->visibility == 0;
}

/* defined?(recv.meth) — can the CALLER (slots[self_off]) call it?  Public always;
 * protected when the caller is an instance of the defining class; otherwise the
 * object still answers if it claims the name via #respond_to_missing?. */
bool
korb_defined_call_p(CTX *c, VALUE *slots, uint32_t mid, int32_t self_off)
{
    const VALUE recv = slots[0];
    if (korb_responds_to_public(c, recv, mid)) return true;
    const VALUE start = korb_dispatch_class(c, recv);
    if (KORB_CLASS_P(start)) {
        VALUE owner = KORB_NIL;
        const struct korb_method *const m = korb_class_find_method(start, mid, &owner);
        if (m != NULL) {
            if (m->visibility != 2) return false;             /* private → not defined? for an explicit receiver */
            const VALUE caller = slots[self_off];             /* protected: only from inside the family */
            return KORB_CLASS_P(owner) && korb_class_has_ancestor(korb_dispatch_class(c, caller), owner);
        }
    }
    const uint32_t rtm = korb_intern(c->vm, "respond_to_missing?", 19);
    if (!korb_responds_to(c, recv, rtm)) return false;
    slots[1] = recv;                                          /* recv + args just below the cursor */
    slots[2] = ID2SYM(mid);
    slots[3] = KORB_FALSE;
    const RESULT r = korb_send(c, slots + 4, rtm, 0, 2);
    return r.state == KORB_NORMAL && KORB_TRUTHY(r.value);
}

/* Comparable mixin methods (defined in builtins/set.c, included later). */
static RESULT korb_m_cmpbl_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_cmpbl_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);

/* Build the builtin class objects (Object/Integer/String/...) + register
 * constants + fill vm->class_name[].  Run before exception-class init. */
void
korb_init_builtin_classes(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    static const struct { const char *name; int cls; } defs[] = {
        { "Object", KORB_C_OBJECT }, { "Integer", KORB_C_INTEGER }, { "Float", KORB_C_FLOAT },
        { "String", KORB_C_STRING }, { "Symbol", KORB_C_SYMBOL }, { "Array", KORB_C_ARRAY },
        { "Hash", KORB_C_HASH }, { "Range", KORB_C_RANGE }, { "NilClass", KORB_C_NIL },
        { "TrueClass", KORB_C_TRUE }, { "FalseClass", KORB_C_FALSE }, { "Class", KORB_C_CLASS },
        { "Rational", KORB_C_RATIONAL }, { "Complex", KORB_C_COMPLEX },
        { "Enumerator", KORB_C_ENUMERATOR }, { "Set", KORB_C_SET }, { "Regexp", KORB_C_REGEXP },
        { "Method", KORB_C_METHOD }, { "Fiber", KORB_C_FIBER },
        { "ArithmeticSequence", KORB_C_ARITHSEQ }, { "Proc", KORB_C_PROC },
        { "MatchData", KORB_C_MATCHDATA }, { "Binding", KORB_C_BINDING },
        { "Random", KORB_C_RANDOM }, { "UnboundMethod", KORB_C_UNBOUND_METHOD },
        { "Thread", KORB_C_THREAD }, { "Mutex", KORB_C_MUTEX },
        { "ConditionVariable", KORB_C_CONDVAR },
    };
    for (int i = 0; i < KORB_NCLASS; i++) vm->class_obj_idx[i] = UINT32_MAX;
    /* Object's superclass is nil; every other builtin inherits Object.  Re-fetch
     * Object from the (GC-rooted) const table each iteration — a raw VALUE held
     * across korb_class_new's alloc would go stale under moving GC. */
    const uint32_t object_sym = korb_intern(vm, "Object", 6);
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        uint32_t name_sym = korb_intern(vm, defs[i].name, strlen(defs[i].name));
        VALUE objc = (defs[i].cls == KORB_C_OBJECT) ? KORB_NIL : korb_const_get(vm, object_sym);
        VALUE cls = korb_class_new(c, slots, name_sym, objc).value;
        korb_const_define(c, name_sym, cls);
        vm->class_name[defs[i].cls] = name_sym;
        vm->class_obj_idx[defs[i].cls] = korb_const_index(vm, name_sym);
    }
    vm->class_name[KORB_C_EXCEPTION] = korb_intern(vm, "Exception", 9);
    /* ArithmeticSequence's display name is the nested path (const stays keyed by
     * the rightmost "ArithmeticSequence", which is how flat const-paths resolve). */
    { VALUE as = korb_const_get(vm, vm->class_name[KORB_C_ARITHSEQ]);
      if (KORB_CLASS_P(as)) {
          VAL2CLASS(as)->name_sym = korb_intern(vm, "Enumerator::ArithmeticSequence", 30);
          /* also register it under Enumerator: a scoped read does not fall back
           * to the top-level table.  `enclosing` stays nil — the display name
           * above is already the full path. */
          const VALUE ec = korb_const_get(vm, vm->class_name[KORB_C_ENUMERATOR]);
          if (KORB_CLASS_P(ec)) {
              slots[0] = as;
              korb_const_define_owned(c, korb_intern(vm, "ArithmeticSequence", 18), slots[0], ec);
          }
      } }
    /* Enumerator::Lazy — a subclass of Enumerator; koruby's lazy enumerators are
     * KORB_OBJ_ENUMERATOR (mode 1/4) and dispatch reports this class for them.
     * Nested const under Enumerator so `Enumerator::Lazy` resolves. */
    { const VALUE ec = korb_const_get(vm, vm->class_name[KORB_C_ENUMERATOR]);
      if (KORB_CLASS_P(ec)) {
          const uint32_t lazy_sym = korb_intern(vm, "Lazy", 4);
          slots[0] = ec;                                            /* root super across class_new GC */
          slots[1] = korb_class_new(c, slots + 2, lazy_sym, slots[0]).value;
          ARO_STORE(c, VAL2CLASS(slots[1]), (VALUE *)(uintptr_t)&VAL2CLASS(slots[1])->enclosing, slots[0]);
          korb_const_define_owned(c, lazy_sym, slots[1], slots[0]);   /* name_sym stays "Lazy"; enclosing → qname "Enumerator::Lazy" */
          vm->lazy_class = slots[1];
      } }

    /* BasicObject = Object's superclass; Kernel = a module mixed into Object.
     * Wiring both makes ancestors / is_a? / superclass reflect the real MRO tail. */
    { uint32_t bo = korb_intern(vm, "BasicObject", 11);
      slots[0] = korb_class_new(c, slots, bo, KORB_NIL).value;          /* super nil */
      korb_const_define(c, bo, slots[0]);
      korb_const_define_owned(c, bo, slots[0], slots[0]);               /* CRuby also names it under itself */
      VALUE objc = korb_const_get(vm, object_sym);                      /* Object (rooted in const table) */
      ARO_STORE(c, VAL2CLASS(objc), (VALUE *)(uintptr_t)&VAL2CLASS(objc)->superclass, slots[0]); }
    { uint32_t kn = korb_intern(vm, "Kernel", 6);
      slots[0] = korb_class_new(c, slots, kn, KORB_NIL).value;
      VAL2CLASS(slots[0])->is_module = 1;
      korb_const_define(c, kn, slots[0]);
      slots[1] = korb_const_get(vm, object_sym);                        /* Object */
      slots[0] = korb_const_get(vm, kn);                               /* Kernel (re-read, rooted) */
      (void)korb_do_include(c, slots + 2, slots[1], VALUE_SLICE_MAKE(&slots[0], 1)); }

    /* Struct factory class — `Struct.new(*members)` builds anonymous subclasses. */
    { uint32_t s = korb_intern(vm, "Struct", 6);
      slots[0] = korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value;
      korb_const_define(c, s, slots[0]);
      korb_def_struct_common(c, &slots[0]); }   /* shared by every generated struct class (also the super target for a block #initialize) */
    /* Data factory class — `Data.define(*members)` builds anonymous immutable value subclasses. */
    { uint32_t s = korb_intern(vm, "Data", 4);
      slots[0] = korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value;
      korb_const_define(c, s, slots[0]);
      korb_def_data_common(c, &slots[0]);
      slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;    /* Data's singleton holds `define` */
      korb_class_def_cfn_blk(c, slots[1], "define", korb_data_define, -1); }
    { uint32_t s = korb_intern(vm, "Module", 6); vm->name_module = s; korb_const_define(c, s, korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value); }
    /* Class < Module < Object (CRuby).  Module was created with super=Object above;
     * re-parent Class's superclass from Object to Module. */
    { VALUE cls = korb_const_get(vm, vm->class_name[KORB_C_CLASS]), mod = korb_const_get(c->vm, c->vm->name_module);
      if (KORB_CLASS_P(cls) && KORB_CLASS_P(mod))
          ARO_STORE(c, VAL2CLASS(cls), (VALUE *)(uintptr_t)&VAL2CLASS(cls)->superclass, mod); }
    /* Complex.polar class method (on Complex's singleton). */
    { slots[0] = korb_const_get(vm, korb_intern(vm, "Complex", 7));
      slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;
      korb_class_def_cfn(c, slots[1], "polar", korb_m_cpx_polar, -1);
      korb_class_def_cfn(c, slots[1], "rect", korb_m_cpx_class_rect, -1);
      korb_class_def_cfn(c, slots[1], "rectangular", korb_m_cpx_class_rect, -1); }

    /* Comparable / Enumerable as builtin modules, mixed into the relevant types
     * so is_a?/kind_of? report membership (the comparison/iteration methods
     * already exist natively on those types). */
    uint32_t comp_sym = korb_intern(vm, "Comparable", 10);
    korb_const_define(c, comp_sym, KORB_NIL);                     /* reserve a const slot (rooted) */
    { VALUE comp = korb_class_new(c, slots, comp_sym, KORB_NIL).value; VAL2CLASS(comp)->is_module = 1; korb_const_define(c, comp_sym, comp);
      /* Comparable derives the comparison API from the includer's <=>. */
      KorbClass *const cm = VAL2CLASS(comp);
      #define KORB_DEF_COMPARABLE(nm, fnp, ar) do { \
          struct korb_method *_m = korb_class_method_slot(cm, korb_intern(vm, (nm), (uint32_t)strlen(nm))); \
          _m->kind = KORB_METHOD_CFUNC; _m->uses_block = 0; _m->params_cnt = (ar); \
          _m->rfn = (fnp); _m->rbfn = NULL; _m->body = NULL; _m->owner = comp; } while (0)
      KORB_DEF_COMPARABLE("<",  korb_m_cmpbl_lt, 1);
      KORB_DEF_COMPARABLE("<=", korb_m_cmpbl_le, 1);
      KORB_DEF_COMPARABLE(">",  korb_m_cmpbl_gt, 1);
      KORB_DEF_COMPARABLE(">=", korb_m_cmpbl_ge, 1);
      KORB_DEF_COMPARABLE("==", korb_m_cmpbl_eq, 1);
      KORB_DEF_COMPARABLE("between?", korb_m_cmpbl_between, 2);
      KORB_DEF_COMPARABLE("clamp", korb_m_cmpbl_clamp, -1);
      #undef KORB_DEF_COMPARABLE
    }
    uint32_t enum_sym = korb_intern(vm, "Enumerable", 10);
    korb_const_define(c, enum_sym, KORB_NIL);
    { VALUE enm = korb_class_new(c, slots, enum_sym, KORB_NIL).value; VAL2CLASS(enm)->is_module = 1; korb_const_define(c, enum_sym, enm); }
    /* Numeric: a real class (CRuby) — Object → Numeric → Integer/Float/Rational/
     * Complex — so `class MyNum < Numeric` reaches Object (Object methods resolve).
     * Numeric includes Comparable; the concrete numerics inherit it via Numeric. */
    uint32_t num_sym = korb_intern(vm, "Numeric", 7);
    korb_const_define(c, num_sym, KORB_NIL);
    { VALUE num = korb_class_new(c, slots, num_sym, korb_builtin_class_obj(vm, KORB_C_OBJECT)).value; korb_const_define(c, num_sym, num); }
    { slots[0] = korb_const_get(vm, comp_sym);                   /* Numeric includes Comparable (CRuby) */
      VALUE num = korb_const_get(vm, num_sym);
      (void)korb_do_include(c, slots + 1, num, VALUE_SLICE_MAKE(&slots[0], 1)); }
    static const int num_in[] = { KORB_C_INTEGER, KORB_C_FLOAT, KORB_C_RATIONAL, KORB_C_COMPLEX };
    for (size_t i = 0; i < sizeof(num_in)/sizeof(num_in[0]); i++) {
        const VALUE num = korb_const_get(vm, num_sym);
        VALUE k = korb_const_get(vm, vm->class_name[num_in[i]]);
        if (KORB_CLASS_P(k)) ARO_STORE(c, VAL2CLASS(k), (VALUE *)(uintptr_t)&VAL2CLASS(k)->superclass, num);   /* superclass = Numeric */
    }
    static const int comp_in[] = { KORB_C_STRING, KORB_C_SYMBOL };   /* numerics inherit Comparable via Numeric */
    for (size_t i = 0; i < sizeof(comp_in)/sizeof(comp_in[0]); i++) {
        slots[0] = korb_const_get(vm, comp_sym);
        VALUE k = korb_const_get(vm, vm->class_name[comp_in[i]]);
        (void)korb_do_include(c, slots + 1, k, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    static const int enum_in[] = { KORB_C_ARRAY, KORB_C_HASH, KORB_C_RANGE, KORB_C_SET, KORB_C_ENUMERATOR, KORB_C_ARITHSEQ };
    for (size_t i = 0; i < sizeof(enum_in)/sizeof(enum_in[0]); i++) {
        slots[0] = korb_const_get(vm, enum_sym);
        VALUE k = korb_const_get(vm, vm->class_name[enum_in[i]]);
        (void)korb_do_include(c, slots + 1, k, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    { slots[0] = korb_const_get(vm, enum_sym);                    /* Struct includes Enumerable (const-only class) */
      VALUE st = korb_const_get(vm, korb_intern(vm, "Struct", 6));
      if (KORB_CLASS_P(st)) (void)korb_do_include(c, slots + 1, st, VALUE_SLICE_MAKE(&slots[0], 1)); }
}

/* Build the builtin Exception class hierarchy + register constants.  `slots` is
 * scratch above any live frame (classes are rooted in the const table). */
void
korb_init_exception_classes(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    static const struct { const char *name; int etype; const char *super; } defs[] = {
        { "Exception",           -1,                "Object" },
        { "StandardError",       -1,                "Exception" },
        { "ScriptError",         -1,                "Exception" },
        { "NameError",           KORB_E_NAME,       "StandardError" },
        { "RuntimeError",        KORB_E_RUNTIME,    "StandardError" },
        { "FrozenError",         KORB_E_FROZEN,     "RuntimeError" },
        { "TypeError",           KORB_E_TYPE,       "StandardError" },
        { "ArgumentError",       KORB_E_ARGUMENT,   "StandardError" },
        { "UncaughtThrowError",  KORB_E_UNCAUGHT_THROW, "ArgumentError" },
        { "ZeroDivisionError",   KORB_E_ZERODIV,    "StandardError" },
        { "LocalJumpError",      KORB_E_LOCALJUMP,  "StandardError" },
        { "RangeError",          KORB_E_RANGE,      "StandardError" },
        { "IndexError",          KORB_E_INDEX,      "StandardError" },
        { "KeyError",            KORB_E_KEY,        "IndexError" },
        { "StopIteration",       KORB_E_STOP_ITERATION, "IndexError" },
        { "DomainError",         KORB_E_MATH_DOMAIN, "StandardError" },   /* Math::DomainError (flat const) */
        { "FloatDomainError",    KORB_E_FLOAT_DOMAIN, "RangeError" },
        { "NoMatchingPatternError",    KORB_E_NO_MATCHING_PATTERN, "StandardError" },
        { "NoMatchingPatternKeyError", KORB_E_NO_MATCHING_PATTERN_KEY, "NoMatchingPatternError" },
        { "NoMethodError",       KORB_E_NOMETHOD,   "NameError" },
        { "NotImplementedError", KORB_E_NOTIMPL,    "ScriptError" },
        { "SystemStackError",    KORB_E_SYSSTACK,   "Exception" },
        /* const-only (etype -1): exist with the right hierarchy for kind_of?/
         * ancestors/rescue-class checks; the runtime doesn't raise them itself. */
        { "LoadError",           KORB_E_LOADERR,    "ScriptError" },
        { "SyntaxError",         KORB_E_SYNTAX,     "ScriptError" },
        { "NoMemoryError",       -1,                "Exception" },
        { "SecurityError",       -1,                "Exception" },
        { "SystemExit",          -1,                "Exception" },
        { "SignalException",     -1,                "Exception" },
        { "Interrupt",           -1,                "SignalException" },
        { "EncodingError",       -1,                "StandardError" },
        { "IOError",             KORB_E_IOERROR,    "StandardError" },
        { "EOFError",            -1,                "IOError" },
        { "FiberError",          -1,                "StandardError" },
        { "ThreadError",         -1,                "StandardError" },
        { "ClosedQueueError",    -1,                "StopIteration" },
        { "RegexpError",         KORB_E_REGEXP,     "StandardError" },
        { "SystemCallError",     -1,                "StandardError" },
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        uint32_t name_sym = korb_intern(vm, defs[i].name, strlen(defs[i].name));
        VALUE super = defs[i].super
            ? korb_const_get(vm, korb_intern(vm, defs[i].super, strlen(defs[i].super)))
            : KORB_NIL;
        VALUE cls = korb_class_new(c, slots, name_sym, super).value;   /* never raises */
        VAL2CLASS(cls)->exc_etype = defs[i].etype;
        korb_const_define(c, name_sym, cls);
        if (defs[i].etype >= 0) vm->exc_name[defs[i].etype] = name_sym;
    }
    /* Exception is the tag's representative class object (receiver dispatch on a
     * bare exception value; subclass-specific lookups use exc_name[etype]). */
    vm->class_obj_idx[KORB_C_EXCEPTION] = korb_const_index(vm, korb_intern(vm, "Exception", 9));
}

/* ---------------------------------------------------------------------------
 * Range — {begin, end, exclude_end}.
 * ------------------------------------------------------------------------- */

RESULT
korb_range_new(CTX *c, VALUE *slots, VALUE_REF bref, VALUE end, uint32_t exclude_end)
{
    /* CRuby: two non-nil bounds must be comparable (begin <=> end != nil).
     * Skip the dispatch for trivially-comparable same-family bounds (the common
     * literal case: integer/float/string/symbol ranges). */
    {
        const VALUE bv = VALUE_REF_GET(bref);
        const bool trivial =
            (KORB_INTEGER_P(bv) || KORB_FLOAT_P(bv)) ? (KORB_INTEGER_P(end) || KORB_FLOAT_P(end)) :
            KORB_STRING_P(bv) ? KORB_STRING_P(end) :
            SYMBOL_P(bv)      ? SYMBOL_P(end) : false;
        if (bv != KORB_NIL && end != KORB_NIL && !trivial) {
            slots[0] = bv; slots[1] = end;
            RESULT cr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "<=>", 3), 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value == KORB_NIL) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "bad value for range");
            bref = VALUE_REF_AT(&slots[0]); end = slots[1];   /* re-read after dispatch (GC) */
            slots += 2;                                       /* consume the begin/end staging cells (bref points into them) */
        }
    }
    VALUE_REF eref = SLOTS_PUSH(slots, end);          /* root end across the alloc */
    KorbRange *r = korb_alloc(c, slots, sizeof(KorbRange), KORB_OBJ_RANGE);
    r->exclude_end = exclude_end;
    r->head.flags |= KORB_FL_FROZEN;                  /* Range instances are frozen (a dup'd copy is not — see korb_m_obj_dup) */
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rbegin, VALUE_REF_GET(bref));
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rend,   VALUE_REF_GET(eref));
    return RESULT_OK((VALUE)r);
}

/* ---------------------------------------------------------------------------
 * Type names for messages.
 * ------------------------------------------------------------------------- */

/* Class name for coercion errors: nil/true/false render as "nil"/"true"/"false",
 * a user instance as its actual class name (not the generic "Object"). */
const char *
korb_coerce_name(CTX *c, VALUE v)
{
    if (v == KORB_NIL)   return "nil";
    if (v == KORB_TRUE)  return "true";
    if (v == KORB_FALSE) return "false";
    if (KORB_CLASS_P(v)) return VAL2CLASS(v)->is_module ? "Module" : "Class";
    if (KORB_OBJECT_P(v)) {
        const VALUE k = VAL2OBJ(v)->klass;
        if (KORB_CLASS_P(k) && VAL2CLASS(k)->name_sym)
            return korb_sym_name(c->vm, VAL2CLASS(k)->name_sym);
    }
    return korb_type_name(v);                       /* builtins: String / Integer / ... */
}

const char *
korb_type_name(VALUE v)
{
    if (FIXNUM_P(v))  return "Integer";
    if (SYMBOL_P(v))  return "Symbol";
    if (v == KORB_NIL)   return "NilClass";
    if (v == KORB_TRUE)  return "TrueClass";
    if (v == KORB_FALSE) return "FalseClass";
    if (FLONUM_P(v))  return "Float";
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING:    return "String";
      case KORB_OBJ_EXCEPTION: return "Exception";
      case KORB_OBJ_ARRAY:     return "Array";
      case KORB_OBJ_HASH:      return "Hash";
      case KORB_OBJ_RANGE:     return "Range";
      case KORB_OBJ_FLOAT:     return "Float";
      case KORB_OBJ_RATIONAL:  return "Rational";
      case KORB_OBJ_COMPLEX:   return "Complex";
      case KORB_OBJ_ENUMERATOR: return "Enumerator";
      case KORB_OBJ_ARITHSEQ:  return "Enumerator::ArithmeticSequence";
      case KORB_OBJ_BIGNUM:    return "Integer";
      case KORB_OBJ_SET: return "Set";
      case KORB_OBJ_MATCHDATA: return "MatchData";
      case KORB_OBJ_BINDING:   return "Binding";
    }
    return "Object";
}

/* "for nil" / "for true" / "for an instance of String" (NoMethodError form) */
const char *
korb_a_type_name(VALUE v)
{
    if (v == KORB_NIL)   return "nil";
    if (v == KORB_TRUE)  return "true";
    if (v == KORB_FALSE) return "false";
    if (FIXNUM_P(v))     return "an instance of Integer";
    if (SYMBOL_P(v))     return "an instance of Symbol";
    if (FLONUM_P(v))     return "an instance of Float";
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: return "an instance of String";
      case KORB_OBJ_ARRAY:  return "an instance of Array";
      case KORB_OBJ_HASH:   return "an instance of Hash";
      case KORB_OBJ_RANGE:  return "an instance of Range";
      case KORB_OBJ_FLOAT:  return "an instance of Float";
      case KORB_OBJ_RATIONAL: return "an instance of Rational";
      case KORB_OBJ_COMPLEX:  return "an instance of Complex";
      case KORB_OBJ_ENUMERATOR: return "an instance of Enumerator";
      case KORB_OBJ_ARITHSEQ: return "an instance of Enumerator::ArithmeticSequence";
      case KORB_OBJ_BIGNUM: return "an instance of Integer";
      case KORB_OBJ_SET: return "an instance of Set";
      case KORB_OBJ_MATCHDATA: return "an instance of MatchData";
      case KORB_OBJ_BINDING:  return "an instance of Binding";
    }
    return "an instance of Object";
}

/* Receiver description for a NoMethodError message, CRuby-shaped:
 *   Class    → "class Foo"    / anonymous "class #<Class:0x…>"
 *   Module   → "module Bar"   / anonymous "module #<Module:0x…>"
 *   instance → "an instance of Foo" (the object's real class name)
 *   others   → korb_a_type_name (nil/true/Integer/String/…).
 * Formats into `buf` for the dynamic cases; returns a static string otherwise. */
/* Overridable #name of a class/module: dispatches the (user-overridable) `name`
 * method so a `def self.name` shows through in error messages (CRuby).  Returns
 * a static string into `buf` on success, or NULL to fall back.  `scratch` is a
 * VM-stack scratch window; `cls` must be a class/module. */
static const char *
korb_class_display_name(CTX *c, VALUE *scratch, VALUE cls, char *buf, size_t sz)
{
    scratch[0] = cls;                                    /* root across the dispatch */
    RESULT nr = korb_send(c, scratch + 1, korb_intern(c->vm, "name", 4), 0, 0);
    if (nr.state == KORB_NORMAL && KORB_STRING_P(nr.value) && VAL2STR(nr.value)->len > 0) {
        snprintf(buf, sz, "%.*s", (int)VAL2STR(nr.value)->len, korb_strbuf_data(VAL2STR(nr.value)->buf));
        return buf;
    }
    return NULL;                                         /* nil (anonymous) / non-String / raised → fall back */
}

/* v.inspect into a fixed buffer (no GC): for error messages that name a value. */
void
korb_desc_inspect(CTX *c, VALUE v, char *buf, size_t sz)
{
    char *ms_buf = NULL; size_t ms_len = 0;
    FILE *const ms = open_memstream(&ms_buf, &ms_len);
    if (ms) { korb_fprint_inspect(c, ms, v); fclose(ms); }
    const size_t n = ms_len < sz - 1 ? ms_len : sz - 1;
    if (ms_buf) memcpy(buf, ms_buf, n);
    buf[ms_buf ? n : 0] = '\0';
    free(ms_buf);
}

/* "1 is not a symbol nor a string" — CRuby names the VALUE, not its class. */
RESULT
korb_raise_not_sym(CTX *c, VALUE *slots, VALUE v)
{
    char db[224]; korb_desc_inspect(c, v, db, sizeof db);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", db);
}

static const char *
korb_recv_desc(CTX *c, VALUE *scratch, VALUE v, char *buf, size_t sz)
{
    if (KORB_CLASS_P(v)) {
        const KorbClass *const k = VAL2CLASS(v);
        const char *const kind = k->is_module ? "module" : "class";
        char nm[192];
        if (korb_class_display_name(c, scratch, v, nm, sizeof nm)) snprintf(buf, sz, "%s %s", kind, nm);
        else if (k->name_sym) { korb_class_qname_into(c, v, nm, sizeof nm); snprintf(buf, sz, "%s %s", kind, nm); }
        else snprintf(buf, sz, "%s #<%s:0x%016zx>", kind, k->is_module ? "Module" : "Class", (size_t)(uintptr_t)v);
        return buf;
    }
    if (KORB_OBJECT_P(v)) {
        const VALUE cls = VAL2OBJ(v)->klass;
        char nm[192];
        if (KORB_CLASS_P(cls) && korb_class_display_name(c, scratch, cls, nm, sizeof nm)) snprintf(buf, sz, "an instance of %s", nm);
        else if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->name_sym) { korb_class_qname_into(c, cls, nm, sizeof nm); snprintf(buf, sz, "an instance of %s", nm); }
        else if (KORB_CLASS_P(cls))
            snprintf(buf, sz, "an instance of #<Class:0x%016zx>", (size_t)(uintptr_t)cls);
        else
            snprintf(buf, sz, "an instance of Object");
        return buf;
    }
    return korb_a_type_name(v);
}

/* Coerce a method-name argument (Symbol / String / #to_str object) to an
 * interned mid; TypeError otherwise.  Uses slots[0..] as scratch (roots nothing
 * of the caller's — call with a scratch window). */
static RESULT korb_arg_to_mid(CTX *c, VALUE *slots, VALUE v, uint32_t *mid_out) {
    const uint32_t mid = korb_bind_argsym(c, v);
    if (LIKELY(mid != UINT32_MAX)) { *mid_out = mid; return RESULT_OK(KORB_NIL); }
    const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
    if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, to_str)) {
        slots[0] = v;
        RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (KORB_STRING_P(sr.value)) { *mid_out = korb_intern(c->vm, korb_strbuf_data(VAL2STR(sr.value)->buf), VAL2STR(sr.value)->len); return RESULT_OK(KORB_NIL); }
    }
    return korb_raise_not_sym(c, slots, v);
}
/* ---------------------------------------------------------------------------
 * Equality / comparison.
 * ------------------------------------------------------------------------- */

/* spaceship for sort/min/max: -1/0/1, or 2 if incomparable.  Integers compare
 * numerically, strings byte-lexicographically. */
static int
korb_cmp_values(VALUE a, VALUE b)
{
    if (UNLIKELY(KORB_BIGNUM_P(a) || KORB_BIGNUM_P(b))) {   /* a Bignum never fits the double path below */
        if (KORB_INTEGER_P(a) && KORB_INTEGER_P(b)) return korb_int_cmp(a, b);
        double d;
        if (KORB_BIGNUM_P(a) && (KORB_FLOAT_P(b) || KORB_RATIONAL_P(b)) && korb_num_to_d(b, &d))
            return (d != d) ? 2 : korb_big_flo_cmp(a, d);
        if (KORB_BIGNUM_P(b) && (KORB_FLOAT_P(a) || KORB_RATIONAL_P(a)) && korb_num_to_d(a, &d)) {
            if (d != d) return 2;
            const int r = korb_big_flo_cmp(b, d);
            return r == 2 ? 2 : -r;
        }
        return 2;
    }
    if ((FIXNUM_P(a) || KORB_FLOAT_P(a) || KORB_RATIONAL_P(a)) &&
        (FIXNUM_P(b) || KORB_FLOAT_P(b) || KORB_RATIONAL_P(b))) {   /* Rational ordered via double (GC-free; exact for realistic denominators) */
        double x = 0, y = 0; korb_num_to_d(a, &x); korb_num_to_d(b, &y);
        if (UNLIKELY(x != x || y != y)) return 2;   /* NaN → incomparable (<=> nil; sort raises) */
        return (x > y) - (x < y);
    }
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        uint32_t m = x->len < y->len ? x->len : y->len;
        int c = memcmp(korb_strbuf_data(x->buf), korb_strbuf_data(y->buf), m);
        if (c) return c < 0 ? -1 : 1;
        return (x->len > y->len) - (x->len < y->len);
    }
    if (a == b && (a == KORB_NIL || a == KORB_TRUE || a == KORB_FALSE)) return 0;   /* NilClass#<=>(nil) etc. */
    return 2;   /* incomparable */
}

/* CTX-aware compare adding Symbol ordering (by name) — for sort/min/max. */
static int
korb_cmp_full(CTX *c, VALUE a, VALUE b)
{
    if (SYMBOL_P(a) && SYMBOL_P(b)) {
        int r = strcmp(korb_sym_name(c->vm, SYM2ID(a)), korb_sym_name(c->vm, SYM2ID(b)));
        return (r > 0) - (r < 0);
    }
    if (KORB_ARRAY_P(a) && KORB_ARRAY_P(b)) {         /* Array#<=>: element-wise, shorter < longer */
        const KorbArray *x = VAL2ARY(a), *y = VAL2ARY(b);
        uint32_t m = x->len < y->len ? x->len : y->len;
        for (uint32_t i = 0; i < m; i++) {
            const VALUE xi = korb_items_data(x->items)[i], yi = korb_items_data(y->items)[i];
            if (xi == yi && !KORB_FLOAT_P(xi)) continue;   /* identical element → equal here (also breaks self-referential Array#<=>); NaN re-checks below */
            int r = korb_cmp_full(c, xi, yi);
            if (r != 0) return r;
        }
        return (x->len > y->len) - (x->len < y->len);
    }
    return korb_cmp_values(a, b);
}

bool
korb_value_eq(VALUE a, VALUE b)
{
    if (UNLIKELY(KORB_FLOAT_P(a) && isnan(korb_float_val(a)))) return false;   /* NaN == anything is false, even itself (eql? differs) */
    if (a == b) return true;    /* fixnum / symbol / singletons / identity */
    if (FIXNUM_P(a) && FIXNUM_P(b)) return false;   /* two Fixnums: equal iff identical (handled above) — skip korb_int_cmp (hot in Array#== of int arrays) */
    if (SYMBOL_P(a)) return false;   /* a Symbol is eql? only to the identical Symbol — skip the type cascade (hot in symbol-keyed Hash / kwargs scans) */
    if (KORB_INTEGER_P(a) && KORB_INTEGER_P(b)) return korb_int_cmp(a, b) == 0;   /* Bignum/Fixnum */
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        if (x->len != y->len || memcmp(korb_strbuf_data(x->buf), korb_strbuf_data(y->buf), x->len) != 0) return false;
        /* same bytes: equal unless the encodings differ AND the content is not
         * plain ASCII (CRuby's rb_str_comparable) */
        const uint32_t ea = KORB_STR_ENC(a), eb = KORB_STR_ENC(b);
        if (LIKELY(ea == eb)) return true;
        const char *const d = korb_strbuf_data(x->buf);
        for (uint32_t i = 0; i < x->len; i++) if ((unsigned char)d[i] >= 0x80) return false;
        return true;
    }
    if (KORB_ARRAY_P(a) && KORB_ARRAY_P(b)) {         /* Array#==: same length, element-wise == */
        KorbArray *const x = VAL2ARY(a); const KorbArray *const y = VAL2ARY(b);
        if (x->len != y->len) return false;
        if (x->head.flags & KORB_FL_JOIN_VISITING) return true;   /* recursive comparison → CRuby assumes equal */
        x->head.flags |= KORB_FL_JOIN_VISITING;
        bool eq = true;
        for (uint32_t i = 0; i < x->len; i++) {
            const VALUE xi = korb_items_data(x->items)[i], yi = korb_items_data(y->items)[i];   /* inline the common element cases (skip the recursive call) */
            if (xi == yi) continue;                                       /* identical: Fixnum / Symbol / same object */
            if (FIXNUM_P(xi) && FIXNUM_P(yi)) { eq = false; break; }      /* two distinct Fixnums */
            if (!korb_value_eq(xi, yi)) { eq = false; break; }            /* heap / mixed: recurse */
        }
        x->head.flags &= ~KORB_FL_JOIN_VISITING;
        return eq;
    }
    if (KORB_SET_P(a) && KORB_SET_P(b)) {             /* Set#==: same members (order-independent) */
        const KorbArray *x = VAL2ARY(VAL2SET(a)->elems), *y = VAL2ARY(VAL2SET(b)->elems);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++) {
            bool found = false;
            for (uint32_t j = 0; j < y->len; j++) if (korb_value_eq(korb_items_data(x->items)[i], korb_items_data(y->items)[j])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    if (KORB_HASH_P(a) && KORB_HASH_P(b)) {           /* Hash#==: same pairs (order-independent) */
        KorbHash *const x = VAL2HASH(a); const KorbHash *const y = VAL2HASH(b);
        if (x->len != y->len) return false;
        if (x->head.flags & KORB_FL_JOIN_VISITING) return true;   /* recursive comparison → CRuby assumes equal */
        x->head.flags |= KORB_FL_JOIN_VISITING;
        bool eq = true;
        for (uint32_t i = 0; i < x->len; i++) {
            int32_t j = korb_hash_find(y, korb_items_data(x->items)[2*i]);
            if (j < 0 || !korb_value_eq(korb_items_data(x->items)[2*i+1], korb_items_data(y->items)[2*j+1])) { eq = false; break; }
        }
        x->head.flags &= ~KORB_FL_JOIN_VISITING;
        return eq;
    }
    if (KORB_RANGE_P(a) && KORB_RANGE_P(b)) {         /* Range#==: == begin/end + same exclude_end */
        const KorbRange *const x = VAL2RANGE(a), *const y = VAL2RANGE(b);
        return x->exclude_end == y->exclude_end && korb_value_eq(x->rbegin, y->rbegin) && korb_value_eq(x->rend, y->rend);
    }
    if (KORB_COMPLEX_P(a) || KORB_COMPLEX_P(b)) {     /* complex == complex / == real(im 0) */
        if (KORB_COMPLEX_P(a) && KORB_COMPLEX_P(b))
            return korb_value_eq(VAL2CPX(a)->re, VAL2CPX(b)->re) && korb_value_eq(VAL2CPX(a)->im, VAL2CPX(b)->im);
        VALUE cx = KORB_COMPLEX_P(a) ? a : b, ot = KORB_COMPLEX_P(a) ? b : a;
        double im;
        return korb_num_to_d(VAL2CPX(cx)->im, &im) && im == 0.0 && korb_value_eq(VAL2CPX(cx)->re, ot);
    }
    if (KORB_RATIONAL_P(a) || KORB_RATIONAL_P(b)) {   /* (1/2) == 0.5 / == 3 / == (1/2) */
        int cmp = korb_rat_cmp(a, b);
        if (cmp != 2) return cmp == 0;
    }
    /* Bignum (or a Fixnum too large for an exact double) vs Float: compare
     * exactly — casting the integer to double would lose precision. */
    if (KORB_FLOAT_P(a) ^ KORB_FLOAT_P(b)) {          /* exactly one is Float */
        const VALUE iv = KORB_FLOAT_P(a) ? b : a;
        if (KORB_BIGNUM_P(iv) ||
            (FIXNUM_P(iv) && (FIX2LONG(iv) > (1LL << 53) || FIX2LONG(iv) < -(1LL << 53))))
            return korb_big_flo_cmp(iv, korb_float_val(KORB_FLOAT_P(a) ? a : b)) == 0;
    }
    double da, db;              /* numeric ==: 1 == 1.0, 1.0 == 1.0 */
    if ((KORB_FLOAT_P(a) || KORB_FLOAT_P(b)) && korb_num_to_d(a, &da) && korb_num_to_d(b, &db))
        return da == db;
    return false;
}

/* eql? semantics (Array#uniq/&/|/-, Set, hash membership): like ==, but numerics
 * are type-strict — 1 is NOT eql? 1.0 / (1/1).  Non-numeric → identical to ==. */
static bool korb_value_eql_d(VALUE a, VALUE b, int depth) {
    int ta = FIXNUM_P(a) ? 1 : KORB_FLOAT_P(a) ? 2 : KORB_RATIONAL_P(a) ? 3 : 0;
    int tb = FIXNUM_P(b) ? 1 : KORB_FLOAT_P(b) ? 2 : KORB_RATIONAL_P(b) ? 3 : 0;
    if ((ta || tb) && ta != tb) return false;        /* mixed numeric types → not eql? */
    if (!ta && KORB_ARRAY_P(a) && KORB_ARRAY_P(b)) {  /* Array#eql?: element-wise eql? (type-strict, unlike ==) */
        if (depth > 8) return true;                  /* recursion cap: self-referential arrays are eql? at the cycle (identity-skip handles same-object; this bounds distinct recursive arrays) */
        const KorbArray *const x = VAL2ARY(a), *const y = VAL2ARY(b);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++) {
            const VALUE xi = korb_items_data(x->items)[i], yi = korb_items_data(y->items)[i];
            if (xi != yi && !korb_value_eql_d(xi, yi, depth + 1)) return false;   /* identity skip breaks self-referential eql?/uniq */
        }
        return true;
    }
    return korb_value_eq(a, b);
}
static bool korb_value_eql(VALUE a, VALUE b) { return korb_value_eql_d(a, b, 0); }

/* case equality `pat === val`: Range membership, Class is-a, else ==. No alloc. */
static bool
korb_case_eq(CTX *c, VALUE pat, VALUE val)
{
    if (KORB_RANGE_P(pat)) {
        const KorbRange *r = VAL2RANGE(pat);
        int lc = korb_cmp_values(r->rbegin, val);
        int uc = korb_cmp_values(val, r->rend);
        if (lc == 2 || uc == 2) return false;
        bool lower = (lc <= 0);
        bool upper = r->exclude_end ? (uc < 0) : (uc <= 0);
        return lower && upper;
    }
    if (KORB_CLASS_P(pat)) {                              /* Module#=== ⟺ val.is_a?(pat): walk ancestors incl. modules */
        if (pat == korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT])) return true;
        VALUE cls = (AROH_IS_GC_OBJECT(val) && (((const AroObjectHeader *)(uintptr_t)val)->flags & KORB_FL_HAS_KLASS))
                      ? korb_klass_override_get(c->vm, val)   /* singleton/extended modules count */
                      : korb_class_obj_of(c, val);
        while (KORB_CLASS_P(cls)) {
            if (cls == pat) return true;
            const VALUE pre = VAL2CLASS(cls)->prepended;
            if (pre != KORB_NIL) { const KorbArray *pa = VAL2ARY(pre); for (uint32_t j = 0; j < pa->len; j++) if (korb_items_data(pa->items)[j] == pat) return true; }
            const VALUE inc = VAL2CLASS(cls)->included;
            if (inc != KORB_NIL) { const KorbArray *ia = VAL2ARY(inc); for (uint32_t j = 0; j < ia->len; j++) if (korb_items_data(ia->items)[j] == pat) return true; }
            cls = VAL2CLASS(cls)->superclass;
        }
        return false;
    }
    if (KORB_REGEXP_P(pat)) {                             /* Regexp#=== : match against a String or Symbol (Symbol coerced to its name, CRuby) */
        const char *sdata; uint32_t slen;
        if (KORB_STRING_P(val)) { sdata = korb_strbuf_data(VAL2STR(val)->buf); slen = VAL2STR(val)->len; }
        else if (SYMBOL_P(val)) { sdata = korb_sym_name(c->vm, SYM2ID(val)); slen = (uint32_t)strlen(sdata); }
        else return false;
        const korb_re_exec_fn_t fn = korb_re_load(c->vm);
        if (UNLIKELY(fn == NULL)) return false;
        korb_re_sync_floor(c);
        const KorbString *const p = VAL2STR(VAL2RE(pat)->source);
        return fn(korb_strbuf_data(p->buf), p->len, VAL2RE(pat)->flags, sdata, slen, 0, NULL) == 1;
    }
    return korb_value_eq(pat, val);
}

/* `pat === val` for the quantifier builtins, dispatching #=== when pat is a
 * user object (korb_case_eq is alloc-free and so can't call back into Ruby). */
static RESULT
korb_pat_eq(CTX *c, VALUE *slots, VALUE pat, VALUE val, bool *const out)
{
    if (LIKELY(!KORB_OBJECT_P(pat))) { *out = korb_case_eq(c, pat, val); return RESULT_OK(KORB_NIL); }
    slots[0] = pat;
    slots[1] = val;
    const RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "===", 3), 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    *out = KORB_TRUTHY(r.value);
    return RESULT_OK(KORB_NIL);
}

/* true iff `v` is a plain Array we can match directly.  CRuby calls #deconstruct
 * even on arrays, but the default Array#deconstruct returns self, so only a
 * singleton/extended override is observable → dispatch only when one may exist. */
static inline bool
korb_pat_plain_array(VALUE v)
{
    return KORB_ARRAY_P(v) && !(((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_HAS_KLASS);
}

/* Recursive pattern matcher (node_match_pred/req).  `base` is the match node's
 * frame view (binding writes land in its locals); `cur` is scratch above the
 * rooted subject.  Returns RESULT{KORB_TRUE|KORB_FALSE} (or a raise from a value
 * pattern's EVAL).  Subject re-read from subjref after any GC point. */
RESULT
korb_pat_match(CTX *c, VALUE *base, VALUE *cur, VALUE_REF subjref, const struct korb_pat *p)
{
    switch (p->kind) {
      case 0:                                            /* binding: always matches */
        base[p->bind_off] = VALUE_REF_GET(subjref);
        return RESULT_OK(KORB_TRUE);
      case 1: {                                          /* value: pat === subject */
        RESULT pv = EVAL(c, p->value_node, cur);         /* may alloc; subject stays rooted */
        if (UNLIKELY(pv.state != KORB_NORMAL)) return pv;
        /* A Proc/lambda or a user object may define a custom #=== (e.g.
         * `in -> x { … }`); a Range may span user-comparable endpoints (e.g.
         * `^(t1..t2)` of Times) whose <=> the no-alloc korb_case_eq can't
         * dispatch → route these through the real #=== method.  Class/Regexp/
         * literal patterns keep the fast path. */
        if (KORB_PROC_P(pv.value) || KORB_OBJECT_P(pv.value) || KORB_RANGE_P(pv.value)) {
            cur[0] = pv.value; cur[1] = VALUE_REF_GET(subjref);
            RESULT er = korb_send(c, cur + 2, korb_intern(c->vm, "===", 3), 0, 1);
            if (UNLIKELY(er.state != KORB_NORMAL)) return er;
            return RESULT_OK(KORB_TRUTHY(er.value) ? KORB_TRUE : KORB_FALSE);
        }
        return RESULT_OK(korb_case_eq(c, pv.value, VALUE_REF_GET(subjref)) ? KORB_TRUE : KORB_FALSE);
      }
      case 7: {                                          /* pin `^var`: EVAL in the match frame (base) so the
                                                          * pinned local resolves (its EP is base[-2], not cur[-2]),
                                                          * then === subject.  value_node is a simple read → no
                                                          * scratch write, so the subject at base[0] survives. */
        RESULT pv = EVAL(c, p->value_node, base);
        if (UNLIKELY(pv.state != KORB_NORMAL)) return pv;
        return RESULT_OK(korb_case_eq(c, pv.value, VALUE_REF_GET(subjref)) ? KORB_TRUE : KORB_FALSE);
      }
      case 2: {                                          /* array pattern [e0..en) — Array, else #deconstruct'd to one */
        if (p->value_node) { RESULT cv = EVAL(c, p->value_node, cur); if (UNLIKELY(cv.state != KORB_NORMAL)) return cv;   /* `Const[…]` → Const === subject */
                             if (!korb_case_eq(c, cv.value, VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE); }
        if (korb_pat_plain_array(VALUE_REF_GET(subjref))) {
            cur[0] = VALUE_REF_GET(subjref);
        } else {
            const uint32_t mid_dc = korb_intern(c->vm, "deconstruct", 11);   /* Struct/Data/custom hook */
            if (!korb_responds_to(c, VALUE_REF_GET(subjref), mid_dc)) return RESULT_OK(KORB_FALSE);
            cur[0] = VALUE_REF_GET(subjref);                                  /* receiver */
            RESULT dr = korb_send(c, cur + 1, mid_dc, 0, 0);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            if (!KORB_ARRAY_P(dr.value)) return korb_raise(c, cur, KORB_E_TYPE, 0, "deconstruct must return Array (%s given)", korb_type_name(dr.value));
            cur[0] = dr.value;
        }
        if (VAL2ARY(cur[0])->len != p->n) return RESULT_OK(KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {
            cur[1] = korb_items_data(VAL2ARY(cur[0])->items)[i];    /* element at cur[1]; cur[0] keeps the array rooted */
            RESULT r = korb_pat_match(c, base, cur + 2, VALUE_REF_AT(&cur[1]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(KORB_TRUE);
      }
      case 3: {                                          /* hash pattern {k: e ...} */
        if (p->value_node) { RESULT cv = EVAL(c, p->value_node, cur); if (UNLIKELY(cv.state != KORB_NORMAL)) return cv;   /* `Const(k: …)` → Const === subject */
                             if (!korb_case_eq(c, cv.value, VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE); }
        /* materialize the hash at cur[0]: a real Hash directly, else via the
         * object's #deconstruct_keys (Struct/Data/custom pattern-match hook). */
        if (KORB_HASH_P(VALUE_REF_GET(subjref))) {
            cur[0] = VALUE_REF_GET(subjref);
        }
        else {
            const uint32_t mid_dk = korb_intern(c->vm, "deconstruct_keys", 16);
            if (!korb_responds_to(c, VALUE_REF_GET(subjref), mid_dk)) return RESULT_OK(KORB_FALSE);
            cur[0] = VALUE_REF_GET(subjref);              /* recv (scanned slot) */
            /* CRuby passes the pattern's keys as an Array so the object may build
             * a minimal hash; a *named* **rest needs every key → pass nil. */
            if (p->npost == 1 && p->bind_off != INT32_MIN) {
                cur[1] = KORB_NIL;
            } else {
                cur[1] = UNWRAP(korb_ary_new(c, cur + 1, p->n));
                VALUE_REF kh = VALUE_REF_AT(&cur[1]);
                for (uint32_t i = 0; i < p->n; i++) { cur[2] = p->keys[i]; CHECK(korb_ary_push_val(c, cur + 3, kh, cur[2])); }
            }
            RESULT dr = korb_send(c, cur + 2, mid_dk, 0, 1);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            if (!KORB_HASH_P(dr.value))                   /* CRuby: TypeError, not a plain non-match */
                return korb_raise(c, cur, KORB_E_TYPE, 0, "deconstruct_keys must return Hash (%s given)", korb_type_name(dr.value));
            cur[0] = dr.value;
        }
        if (p->n == 0 && p->npost == 0)                   /* `in {}` matches only an empty hash */
            return RESULT_OK(VAL2HASH(cur[0])->len == 0 ? KORB_TRUE : KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {
            const int32_t idx = korb_hash_find(VAL2HASH(cur[0]), p->keys[i]);
            if (idx < 0) return RESULT_OK(KORB_FALSE);
            cur[1] = korb_items_data(VAL2HASH(cur[0])->items)[2 * idx + 1];
            RESULT r = korb_pat_match(c, base, cur + 2, VALUE_REF_AT(&cur[1]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        if (p->npost == 2) {                              /* **nil: forbid any unmatched entries */
            if (VAL2HASH(cur[0])->len != p->n) return RESULT_OK(KORB_FALSE);
        } else if (p->npost == 1 && p->bind_off != INT32_MIN) {   /* **rest: bind a Hash of the unmatched entries */
            cur[1] = UNWRAP(korb_hash_new(c, cur + 1, 4));
            VALUE_REF rh = VALUE_REF_AT(&cur[1]);
            for (uint32_t i = 0; i < VAL2HASH(cur[0])->len; i++) {
                const VALUE k = korb_items_data(VAL2HASH(cur[0])->items)[2 * i];
                bool matched = false;
                for (uint32_t j = 0; j < p->n; j++) if (k == p->keys[j]) { matched = true; break; }
                if (matched) continue;
                cur[2] = k;                               /* root key + value across hash_set GC */
                cur[3] = korb_items_data(VAL2HASH(cur[0])->items)[2 * i + 1];
                CHECK(korb_hash_set(c, cur + 4, rh, VALUE_REF_AT(&cur[2]), cur[3]));
            }
            base[p->bind_off] = VALUE_REF_GET(rh);
        }
        return RESULT_OK(KORB_TRUE);
      }
      case 8: {                                          /* find pattern [*left, mid..., *right] */
        if (p->value_node) { RESULT cv = EVAL(c, p->value_node, cur); if (UNLIKELY(cv.state != KORB_NORMAL)) return cv;   /* `Const[*, …, *]` */
                             if (!korb_case_eq(c, cv.value, VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE); }
        if (korb_pat_plain_array(VALUE_REF_GET(subjref))) {
            cur[0] = VALUE_REF_GET(subjref);
        } else {
            const uint32_t mid_dc = korb_intern(c->vm, "deconstruct", 11);
            if (!korb_responds_to(c, VALUE_REF_GET(subjref), mid_dc)) return RESULT_OK(KORB_FALSE);
            cur[0] = VALUE_REF_GET(subjref);
            RESULT dr = korb_send(c, cur + 1, mid_dc, 0, 0);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            if (!KORB_ARRAY_P(dr.value)) return korb_raise(c, cur, KORB_E_TYPE, 0, "deconstruct must return Array (%s given)", korb_type_name(dr.value));
            cur[0] = dr.value;
        }
        const uint32_t len = VAL2ARY(cur[0])->len;
        const int32_t right_off = (int32_t)p->npost;
        for (uint32_t s = 0; p->n <= len && s <= len - p->n; s++) {   /* try the mid run at each position */
            bool all = true;
            for (uint32_t i = 0; i < p->n; i++) {
                cur[1] = korb_items_data(VAL2ARY(cur[0])->items)[s + i];
                RESULT r = korb_pat_match(c, base, cur + 2, VALUE_REF_AT(&cur[1]), p->elems[i]);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                if (r.value != KORB_TRUE) { all = false; break; }
            }
            if (!all) continue;
            if (p->bind_off != INT32_MIN) {                /* bind *left = [0..s) */
                cur[1] = UNWRAP(korb_ary_new(c, cur + 1, s));
                VALUE_REF lh = VALUE_REF_AT(&cur[1]);
                for (uint32_t i = 0; i < s; i++) { cur[2] = korb_items_data(VAL2ARY(cur[0])->items)[i]; CHECK(korb_ary_push_val(c, cur + 3, lh, cur[2])); }
                base[p->bind_off] = VALUE_REF_GET(lh);
            }
            if (right_off != INT32_MIN) {                  /* bind *right = [s+n..len) */
                const uint32_t rstart = s + p->n;
                cur[1] = UNWRAP(korb_ary_new(c, cur + 1, len - rstart));
                VALUE_REF rh = VALUE_REF_AT(&cur[1]);
                for (uint32_t i = rstart; i < len; i++) { cur[2] = korb_items_data(VAL2ARY(cur[0])->items)[i]; CHECK(korb_ary_push_val(c, cur + 3, rh, cur[2])); }
                base[right_off] = VALUE_REF_GET(rh);
            }
            return RESULT_OK(KORB_TRUE);
        }
        return RESULT_OK(KORB_FALSE);
      }
      case 4: {                                          /* capture: inner pattern, then bind */
        RESULT r = korb_pat_match(c, base, cur, subjref, p->elems[0]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        base[p->bind_off] = VALUE_REF_GET(subjref);
        return RESULT_OK(KORB_TRUE);
      }
      case 5: {                                          /* alternation: elems[0] | elems[1] */
        for (uint32_t i = 0; i < p->n; i++) {
            RESULT r = korb_pat_match(c, base, cur, subjref, p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value == KORB_TRUE) return RESULT_OK(KORB_TRUE);
        }
        return RESULT_OK(KORB_FALSE);
      }
      case 6: {                                          /* array w/ rest: [pre..., *rest, post...] — Array or #deconstruct'd */
        if (p->value_node) { RESULT cv = EVAL(c, p->value_node, cur); if (UNLIKELY(cv.state != KORB_NORMAL)) return cv;   /* `Const[…, *rest, …]` */
                             if (!korb_case_eq(c, cv.value, VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE); }
        if (korb_pat_plain_array(VALUE_REF_GET(subjref))) {
            cur[0] = VALUE_REF_GET(subjref);
        } else {
            const uint32_t mid_dc = korb_intern(c->vm, "deconstruct", 11);
            if (!korb_responds_to(c, VALUE_REF_GET(subjref), mid_dc)) return RESULT_OK(KORB_FALSE);
            cur[0] = VALUE_REF_GET(subjref);
            RESULT dr = korb_send(c, cur + 1, mid_dc, 0, 0);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            if (!KORB_ARRAY_P(dr.value)) return korb_raise(c, cur, KORB_E_TYPE, 0, "deconstruct must return Array (%s given)", korb_type_name(dr.value));
            cur[0] = dr.value;
        }
        VALUE_REF aref = VALUE_REF_AT(&cur[0]);          /* the subject array, parked + rooted at cur[0] */
        VALUE *const cur2 = cur + 1;                     /* working slots above the parked array */
        const uint32_t len = VAL2ARY(VALUE_REF_GET(aref))->len;
        if (len < p->n + p->npost) return RESULT_OK(KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {            /* pre */
            cur2[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(aref))->items)[i];
            RESULT r = korb_pat_match(c, base, cur2 + 1, VALUE_REF_AT(&cur2[0]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        if (p->bind_off != INT32_MIN) {                  /* bind *rest to a fresh sub-array (array stays rooted) */
            const uint32_t rest_len = len - p->n - p->npost;
            cur2[0] = UNWRAP(korb_ary_new(c, cur2, rest_len));
            VALUE_REF rest = VALUE_REF_AT(&cur2[0]);
            for (uint32_t i = 0; i < rest_len; i++) {
                cur2[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(aref))->items)[p->n + i];   /* re-read (push may GC) */
                CHECK(korb_ary_push_val(c, cur2 + 2, rest, cur2[1]));
            }
            base[p->bind_off] = VALUE_REF_GET(rest);
        }
        for (uint32_t i = 0; i < p->npost; i++) {        /* post (from the tail) */
            cur2[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(aref))->items)[len - p->npost + i];
            RESULT r = korb_pat_match(c, base, cur2 + 1, VALUE_REF_AT(&cur2[0]), p->elems[p->n + i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(KORB_TRUE);
      }
    }
    return RESULT_OK(KORB_FALSE);
}

static const char *const korb_cmp_op_name[] = { "<", "<=", ">", ">=" };

/* CRuby rb_cmperr flavor: immediates render via inspect, others by class. */
static void
korb_cmperr_operand(VALUE v, char *buf, size_t cap)
{
    if (FIXNUM_P(v))          snprintf(buf, cap, "%lld", (long long)FIX2LONG(v));
    else if (v == KORB_NIL)   snprintf(buf, cap, "nil");
    else if (v == KORB_TRUE)  snprintf(buf, cap, "true");
    else if (v == KORB_FALSE) snprintf(buf, cap, "false");
    else                      snprintf(buf, cap, "%s", korb_type_name(v));
}

static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup);
static RESULT korb_hash_cmp_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op);
static RESULT korb_m_set_subset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_superset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_psubset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_psuperset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             NODE *block, VALUE *def_env, VALUE *captured_self);

RESULT
korb_cmp_slow(CTX *c, VALUE *slots, VALUE l, VALUE r, int op, uint32_t line)
{
    if (FIXNUM_P(l) && FIXNUM_P(r)) {                    /* both Integer (reached via send/cmethod, not the node fast path) */
        korb_sword_t x = FIX2LONG(l), y = FIX2LONG(r);
        bool t = op == 0 ? x < y : op == 1 ? x <= y : op == 2 ? x > y : x >= y;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_INTEGER_P(l) && KORB_INTEGER_P(r)) {        /* at least one Bignum */
        int cmp = korb_int_cmp(l, r);
        bool t = op == 0 ? cmp < 0 : op == 1 ? cmp <= 0 : op == 2 ? cmp > 0 : cmp >= 0;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_HASH_P(l)) {                                /* subset/superset; coerce a non-Hash r via #to_hash (in korb_hash_cmp_op) */
        slots[0] = l; slots[1] = r;
        VALUE_REF lref = VALUE_REF_AT(&slots[0]);
        VALUE_SLICE rsl = VALUE_SLICE_MAKE(&slots[1], 1);
        return korb_hash_cmp_op(c, slots + 2, lref, rsl, op);
    }
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(r)) {     /* exact rational/int compare */
        int cmp = korb_rat_cmp(l, r);
        if (cmp != 2) {
            bool t = (op == 0) ? cmp < 0 : (op == 1) ? cmp <= 0 : (op == 2) ? cmp > 0 : cmp >= 0;
            return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
        }
    }
    /* Bignum vs Float: compare exactly — casting the Bignum to double would lose
     * precision (e.g. 2**64+39 <= (2**64+39).to_f is false, not true). */
    if ((KORB_BIGNUM_P(l) && KORB_FLOAT_P(r)) || (KORB_FLOAT_P(l) && KORB_BIGNUM_P(r))) {
        int cmp = KORB_BIGNUM_P(l) ? korb_big_flo_cmp(l, korb_float_val(r))
                                   : korb_big_flo_cmp(r, korb_float_val(l));
        if (cmp == 2) return RESULT_OK(KORB_FALSE);           /* NaN: <,<=,>,>= all false */
        if (KORB_FLOAT_P(l)) cmp = -cmp;                       /* compared r(Bignum) vs l(Float) */
        const bool t = op == 0 ? cmp < 0 : op == 1 ? cmp <= 0 : op == 2 ? cmp > 0 : cmp >= 0;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    double ld, rd;
    if ((KORB_FLOAT_P(l) || KORB_FLOAT_P(r)) && korb_num_to_d(l, &ld) && korb_num_to_d(r, &rd)) {
        bool t;
        switch (op) {
          case 0:  t = ld <  rd; break;
          case 1:  t = ld <= rd; break;
          case 2:  t = ld >  rd; break;
          default: t = ld >= rd; break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_STRING_P(l) && KORB_STRING_P(r)) {
        const KorbString *x = VAL2STR(l), *y = VAL2STR(r);
        uint32_t min = x->len < y->len ? x->len : y->len;
        int cmp = memcmp(korb_strbuf_data(x->buf), korb_strbuf_data(y->buf), min);
        if (cmp == 0) cmp = (x->len > y->len) - (x->len < y->len);
        bool t;
        switch (op) {
          case 0:  t = cmp <  0; break;
          case 1:  t = cmp <= 0; break;
          case 2:  t = cmp >  0; break;
          default: t = cmp >= 0; break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (SYMBOL_P(l) && SYMBOL_P(r)) {
        int cmp = strcmp(korb_sym_name(c->vm, SYM2ID(l)), korb_sym_name(c->vm, SYM2ID(r)));
        bool t = (op == 0) ? cmp < 0 : (op == 1) ? cmp <= 0 : (op == 2) ? cmp > 0 : cmp >= 0;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_SET_P(l)) {                               /* Set </<=/>/>= → proper/sub/super-set */
        slots[0] = l; slots[1] = r;
        VALUE_REF sref = VALUE_REF_AT(&slots[0]);
        VALUE_SLICE sl = VALUE_SLICE_MAKE(&slots[1], 1);
        return op == 0 ? korb_m_set_psubset(c, slots + 2, sref, sl)
             : op == 1 ? korb_m_set_subset(c, slots + 2, sref, sl)
             : op == 2 ? korb_m_set_psuperset(c, slots + 2, sref, sl)
             :           korb_m_set_superset(c, slots + 2, sref, sl);
    }
    /* user class / instance with the operator defined (e.g. Class#<, a class
     * that defines its own `<`).  Guarded to exclude builtin types whose `<`
     * routes back here (Fixnum/String/Symbol) — that would recurse forever. */
    if (KORB_CLASS_P(l) || KORB_OBJECT_P(l)) {
        uint32_t mid = korb_intern(c->vm, korb_cmp_op_name[op], (uint32_t)strlen(korb_cmp_op_name[op]));
        /* A user object with no such method still dispatches: korb_send_impl
         * runs the method_missing protocol, which `obj >= x` must reach like
         * any other missing method.  (Classes keep the responds_to gate — a
         * Class falling through means Module#< below, not method_missing.) */
        if (KORB_OBJECT_P(l) || korb_responds_to(c, l, mid)) {
            slots[0] = l; slots[1] = r;
            return korb_send_impl(c, slots + 2, mid, 0, 1, NULL, NULL, NULL);
        }
    }
    if ((KORB_INTEGER_P(l) || KORB_FLOAT_P(l) || KORB_RATIONAL_P(l)) && KORB_OBJECT_P(r)) {
        bool h; RESULT cr = korb_try_coerce(c, slots, l, r, korb_cmp_op_name[op], line, &h);   /* a, b = r.coerce(l); a OP b */
        if (h) return cr;
    }
    if (KORB_INTEGER_P(l) || KORB_FLOAT_P(l) || KORB_RATIONAL_P(l) || KORB_STRING_P(l) || SYMBOL_P(l)) {
        char rdesc[64];                                  /* numeric/String/Symbol vs incomparable → ArgumentError */
        korb_cmperr_operand(r, rdesc, sizeof(rdesc));
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "comparison of %s with %s failed", korb_type_name(l), rdesc);
    }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '%s' for %s",
                      korb_cmp_op_name[op], korb_a_type_name(l));
}

/* ---------------------------------------------------------------------------
 * Binop slow paths (String variants + type errors).
 * ------------------------------------------------------------------------- */
static RESULT korb_m_set_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_diff(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_subset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_superset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_psubset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_set_psuperset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);

/* If `l` is a user class/instance defining the binary operator `op`, dispatch
 * l.op(rhs) and report handled=true.  Guarded to KORB_CLASS_P/KORB_OBJECT_P so
 * builtin types whose operator routes back through the slow path can't recurse. */
RESULT korb_user_binop(CTX *c, VALUE *slots, VALUE l, VALUE rhs, const char *op, bool *handled) {
    /* The last rung of the arithmetic ladders: every builtin case for this
     * operator has already been tried and declined, so a method the receiver's
     * class actually defines is the right answer whatever the receiver's type
     * is.  (Gating this on OBJECT/CLASS used to hide Ruby-defined operators on
     * builtin-typed receivers — Enumerator#+ was unreachable via `a + b`.)
     * A plain user object with NO such method still goes through the dispatch:
     * korb_send_impl runs the method_missing protocol, which `obj >= x` must
     * reach like any other missing method (mspec's operator matchers are
     * method_missing-driven BasicObject shells). */
    const uint32_t mid = korb_intern(c->vm, op, (uint32_t)strlen(op));
    if (korb_responds_to(c, l, mid) || KORB_OBJECT_P(l)) {
        slots[0] = l; slots[1] = rhs; *handled = true;
        return korb_send_impl(c, slots + 2, mid, 0, 1, NULL, NULL, NULL);
    }
    *handled = false;
    return RESULT_OK(KORB_NIL);
}

/* Numeric coerce protocol: `l OP rhs` where rhs is non-numeric → if rhs responds
 * to #coerce, do `a, b = rhs.coerce(l); a OP b`.  *handled stays false if rhs has
 * no #coerce (caller raises its own TypeError). */
RESULT korb_try_coerce(CTX *c, VALUE *slots, VALUE l, VALUE rhs, const char *op, uint32_t line, bool *handled) {
    *handled = false;
    const uint32_t coerce_id = korb_intern(c->vm, "coerce", 6);
    if (!korb_responds_to(c, rhs, coerce_id)) return RESULT_OK(KORB_NIL);
    *handled = true;
    slots[0] = rhs; slots[1] = l;                            /* recv=rhs, arg=l for #coerce */
    RESULT cr = korb_send_impl(c, slots + 2, coerce_id, line, 1, NULL, NULL, NULL);
    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
    if (cr.value == KORB_NIL) { *handled = false; return RESULT_OK(KORB_NIL); }   /* nil → not coercible: let the caller raise its own TypeError/ArgumentError */
    if (UNLIKELY(!KORB_ARRAY_P(cr.value) || VAL2ARY(cr.value)->len != 2))
        return korb_raise(c, slots, KORB_E_TYPE, line, "coerce must return [x, y]");
    slots[0] = cr.value;
    slots[1] = korb_items_data(VAL2ARY(slots[0])->items)[0];           /* a (recv) */
    slots[2] = korb_items_data(VAL2ARY(slots[0])->items)[1];           /* b (arg) */
    return korb_send_impl(c, slots + 3, korb_intern(c->vm, op, (uint32_t)strlen(op)), line, 1, NULL, NULL, NULL);
}
RESULT
korb_plus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_OBJECT_P(l)) {                          /* a user/builtin object (Time, ...) → dispatch its own + */
        bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "+", &h);
        if (h) return ur;
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '+' for %s", korb_a_type_name(l));
    }
    if (KORB_STRING_P(l) && KORB_STRING_P(rhs)) {
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_str_plus_ref(c, slots, lhs, r);
    }
    if (KORB_STRING_P(l)) {                           /* String + non-String → #to_str, else TypeError */
        const char *const rcls = korb_coerce_name(c, rhs);   /* name it before any dispatch moves it */
        VALUE o = rhs;
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(o) && korb_responds_to_coerce_p(c, slots, &o, to_str)) {
            slots[0] = o;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) {
                VALUE_REF r = SLOTS_PUSH(slots, sr.value);
                return korb_str_plus_ref(c, slots, lhs, r);
            }
        }
        return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into String", rcls);
    }
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(rhs)) return korb_cpx_arith(c, slots, l, rhs, 0);
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs)) return korb_num_arith(c, slots, l, rhs, 0, line);
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(rhs)) return korb_rat_arith(c, slots, l, rhs, 0);
    if (KORB_SET_P(l)) { slots[0] = rhs; return korb_m_set_union(c, slots + 1, lhs, VALUE_SLICE_MAKE(&slots[0], 1)); }   /* Set + → union */
    if (KORB_ARRAY_P(l)) {
        if (!KORB_ARRAY_P(rhs)) {                    /* Array + non-Array → coerce via #to_ary (lhs is a VALUE_REF) */
            RESULT cr = korb_coerce_to_ary(c, slots, &rhs);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE)
                return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Array", korb_type_name(rhs));
        }
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_ary_plus_ref(c, slots, lhs, r);
    }
    if (KORB_INTEGER_P(l)) {                          /* Fixnum or Bignum: coerce protocol, else TypeError */
        bool h; RESULT cr = korb_try_coerce(c, slots, l, rhs, "+", line, &h);
        if (h) return cr;
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Integer", korb_type_name(rhs));
    }
    if (KORB_STRING_P(l)) {                          /* String + non-String → coerce via #to_str */
        const uint32_t to_str_mid = korb_intern(c->vm, "to_str", 6);
        if (!korb_responds_to_coerce_p(c, slots, &rhs, to_str_mid))
            return korb_raise(c, slots, KORB_E_TYPE, line,
                              "no implicit conversion of %s into String", korb_type_name(rhs));
        const char *const rcls = korb_type_name(rhs);   /* capture before dispatch (STRESS-safe) */
        slots[0] = rhs;                              /* receiver, rooted across dispatch */
        const RESULT tr = korb_send_impl(c, slots + 1, to_str_mid, line, 0, NULL, NULL, NULL);
        if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
        if (UNLIKELY(!KORB_STRING_P(tr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to String (%s#to_str gives %s)",
                              rcls, rcls, korb_type_name(tr.value));
        VALUE_REF r = SLOTS_PUSH(slots, tr.value);   /* the coerced String (lhs stays rooted) */
        return korb_str_plus_ref(c, slots, lhs, r);
    }
    { bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "+", &h); if (h) return ur; }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '+' for %s", korb_a_type_name(l));
}

/* `-` cold ladder, kept out-of-line so node_minus's SD stays small (mirrors
 * korb_plus_slow).  Reached only for non-(fixnum/float) operands. */
RESULT
korb_minus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_OBJECT_P(l)) {                          /* a user/builtin object (Time, ...) → dispatch its own - */
        bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "-", &h);
        if (h) return ur;
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '-' for %s", korb_a_type_name(l));
    }
    if (KORB_INTEGER_P(l) && KORB_INTEGER_P(rhs)) return korb_int_arith(c, slots, l, rhs, 1, line);   /* fixnum overflow / bignum */
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(rhs)) return korb_cpx_arith(c, slots, l, rhs, 1);
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs)) return korb_num_arith(c, slots, l, rhs, 1, line);   /* mixed Float (e.g. Float-Rational) */
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(rhs)) return korb_rat_arith(c, slots, l, rhs, 1);
    if (KORB_ARRAY_P(l) || KORB_SET_P(l)) return korb_sub_slow(c, slots, lhs, rhs, line);
    if (KORB_INTEGER_P(l)) {                          /* Fixnum or Bignum: coerce protocol, else TypeError */
        bool h; RESULT cr = korb_try_coerce(c, slots, l, rhs, "-", line, &h);
        if (h) return cr;
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Integer", korb_type_name(rhs));
    }
    { bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "-", &h); if (h) return ur; }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '-' for %s", korb_a_type_name(l));
}

static RESULT korb_m_ary_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);

RESULT
korb_mul_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(rhs)) return korb_cpx_arith(c, slots, l, rhs, 2);
    if (!KORB_ARRAY_P(l) && !KORB_STRING_P(l) && (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs))) return korb_num_arith(c, slots, l, rhs, 2, line);
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(rhs)) return korb_rat_arith(c, slots, l, rhs, 2);
    if (KORB_STRING_P(l)) {
        korb_sword_t cnt;
        if (UNLIKELY(!korb_to_index(rhs, &cnt))) {       /* coerce the count via #to_int (lhs is a VALUE_REF → GC-safe) */
            RESULT cr = korb_coerce_to_int(c, slots, &rhs);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(rhs, &cnt)) {
                if (KORB_BIGNUM_P(rhs))                  /* a Bignum count can never fit a long */
                    return korb_raise(c, slots, KORB_E_RANGE, line, "bignum too big to convert into 'long'");
                return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Integer", korb_type_name(rhs));
            }
        }
        return korb_str_repeat_ref(c, slots, lhs, cnt, line);
    }
    if (KORB_ARRAY_P(l) && KORB_OBJECT_P(rhs)) {        /* Array * obj: CRuby tries #to_str (join) first, then #to_int (repeat count) */
        const uint32_t to_int = korb_intern(c->vm, "to_int", 6);
        if (!korb_responds_to_coerce_p(c, slots, &rhs, korb_intern(c->vm, "to_str", 6)) && korb_responds_to_coerce_p(c, slots, &rhs, to_int)) {
            slots[1] = rhs;                              /* receiver for the dispatch (base[-1]) */
            RESULT ir = korb_send_impl(c, slots + 2, to_int, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (UNLIKELY(!KORB_INTEGER_P(ir.value)))
                return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to Integer (%s#to_int gives %s)",
                                  korb_type_name(slots[1]), korb_type_name(slots[1]), korb_type_name(ir.value));
            if (UNLIKELY(!FIXNUM_P(ir.value))) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "argument too big");
            rhs = ir.value;                              /* now a Fixnum → fall into the repeat path */
            l = VALUE_REF_GET(lhs);                      /* re-read: the dispatch may have moved the array */
        }
    }
    if (KORB_ARRAY_P(l) && (FIXNUM_P(rhs) || KORB_FLOAT_P(rhs))) {   /* Array * n → repeated array (Float coerced via to_int) */
        korb_sword_t cnt = FIXNUM_P(rhs) ? FIX2LONG(rhs) : (korb_sword_t)korb_float_val(rhs);
        if (cnt < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
        uint32_t len = VAL2ARY(l)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)cnt * len)));
        for (korb_sword_t r = 0; r < cnt; r++)
            for (uint32_t i = 0; i < len; i++)
                CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(lhs))->items)[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_ARRAY_P(l)) {                           /* Array * String-or-#to_str → join (join coerces the sep) */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_STRING_P(rhs) || (KORB_OBJECT_P(rhs) && korb_responds_to_coerce_p(c, slots, &rhs, to_str))) {
            slots[0] = rhs;
            return korb_m_ary_join(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
        }
        return korb_raise(c, slots, KORB_E_TYPE, line,   /* non-int / non-str */
                          "no implicit conversion of %s into Integer", korb_type_name(rhs));
    }
    if (KORB_INTEGER_P(l)) {                          /* Fixnum or Bignum: coerce protocol, else TypeError */
        bool h; RESULT cr = korb_try_coerce(c, slots, l, rhs, "*", line, &h);
        if (h) return cr;
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Integer", korb_type_name(rhs));
    }
    if (KORB_STRING_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into Integer", korb_type_name(rhs));
    { bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "*", &h); if (h) return ur; }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '*' for %s", korb_a_type_name(l));
}

/* ---------------------------------------------------------------------------
 * Symbols (interned names; immediates — libc table, never GC-scanned).
 * ------------------------------------------------------------------------- */

static inline uint32_t korb_str_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;                         /* FNV-1a */
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1u;
}
/* insert id into the open-addressing index (caller ensures capacity + no dup). */
static void korb_sym_hash_put(struct korb_vm *vm, uint32_t id)
{
    const uint32_t mask = vm->sym_hash_cap - 1;
    const uint32_t h = korb_str_hash(vm->sym_names[id], vm->sym_lens[id]);
    uint32_t slot = h & mask;
    while (vm->sym_hash[slot]) slot = (slot + 1) & mask;
    vm->sym_hash[slot] = id + 1;
}
uint32_t
korb_intern(struct korb_vm *vm, const char *name, size_t len)
{
    if (UNLIKELY(vm->sym_hash_cap == 0)) {            /* lazy init */
        vm->sym_hash_cap = 1024;
        vm->sym_hash = calloc(vm->sym_hash_cap, sizeof(uint32_t));
        if (!vm->sym_hash) { fprintf(stderr, "koruby_precise: oom (sym hash)\n"); abort(); }
        for (uint32_t i = 0; i < vm->sym_cnt; i++) korb_sym_hash_put(vm, i);   /* index any pre-existing */
    }
    const uint32_t h = korb_str_hash(name, len);
    uint32_t mask = vm->sym_hash_cap - 1;
    for (uint32_t slot = h & mask; ; slot = (slot + 1) & mask) {   /* find-or-miss */
        const uint32_t e = vm->sym_hash[slot];
        if (e == 0) break;                            /* empty slot → not interned */
        const uint32_t id = e - 1;
        if (vm->sym_lens[id] == len && memcmp(vm->sym_names[id], name, len) == 0)
            return id;
    }
    if (vm->sym_cnt == vm->sym_capa) {
        vm->sym_capa = vm->sym_capa ? vm->sym_capa * 2 : 64;
        vm->sym_names = realloc(vm->sym_names, sizeof(char *) * vm->sym_capa);
        vm->sym_lens  = realloc(vm->sym_lens,  sizeof(uint32_t) * vm->sym_capa);
        if (!vm->sym_names || !vm->sym_lens) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    }
    char *copy = malloc(len + 1);
    if (!copy) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    memcpy(copy, name, len);
    copy[len] = '\0';
    const uint32_t id = vm->sym_cnt++;
    vm->sym_names[id] = copy;
    vm->sym_lens[id] = (uint32_t)len;
    if ((vm->sym_cnt * 4u) >= (vm->sym_hash_cap * 3u)) {   /* grow index past 0.75 load, then re-index all */
        vm->sym_hash_cap *= 2;
        vm->sym_hash = realloc(vm->sym_hash, sizeof(uint32_t) * vm->sym_hash_cap);
        if (!vm->sym_hash) { fprintf(stderr, "koruby_precise: oom (sym hash)\n"); abort(); }
        memset(vm->sym_hash, 0, sizeof(uint32_t) * vm->sym_hash_cap);
        for (uint32_t i = 0; i < vm->sym_cnt; i++) korb_sym_hash_put(vm, i);
    } else {
        korb_sym_hash_put(vm, id);
    }
    return id;
}

const char *
korb_sym_name(const struct korb_vm *vm, uint32_t id)
{
    return id < vm->sym_cnt ? vm->sym_names[id] : "?";
}

/* ---------------------------------------------------------------------------
 * Method table.
 * ------------------------------------------------------------------------- */

static struct korb_method *
korb_method_slot(CTX *c, uint32_t mid)
{
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->method_cnt; i++) {
        if (vm->methods[i]->mid == mid) return vm->methods[i];
    }
    if (vm->method_cnt == vm->method_capa) {
        vm->method_capa = vm->method_capa ? vm->method_capa * 2 : 32;
        vm->methods = realloc(vm->methods, sizeof(struct korb_method *) * vm->method_capa);
        if (!vm->methods) { fprintf(stderr, "koruby_precise: out of memory (methods)\n"); abort(); }
    }
    struct korb_method *m = calloc(1, sizeof(struct korb_method));   /* immortal entry */
    if (!m) { fprintf(stderr, "koruby_precise: out of memory (method entry)\n"); abort(); }
    m->mid = mid; m->orig_mid = mid;
    vm->methods[vm->method_cnt++] = m;
    return m;
}

void
korb_method_define(CTX *c, uint32_t mid, NODE *body,
                   uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                   uint32_t uses_block, struct Node **opt_defaults, void *kw_info, void *param_info)
{
    struct korb_method *m = korb_method_slot(c, mid);
    m->kind = KORB_METHOD_ISEQ;
    m->uses_block = (uint8_t)uses_block;
    m->params_cnt = (int32_t)params_cnt;
    m->req_cnt = req_cnt;
    m->post_cnt = post_cnt;
    m->rest_slot = rest_slot;
    m->locals_cnt = locals_cnt;
    m->body = body;
    m->opt_defaults = opt_defaults;
    m->kw_info = kw_info;
    m->param_info = param_info;
    m->bfn = NULL;
    m->is_simple = (kw_info == NULL && rest_slot < 0 && post_cnt == 0 &&
                    req_cnt == params_cnt && !uses_block);
    c->vm->method_serial++;   /* invalidate call caches */
}

void
korb_builtin_define(CTX *c, const char *name, korb_builtin_fn fn, int32_t params_cnt)
{
    uint32_t mid = korb_intern(c->vm, name, strlen(name));
    struct korb_method *m = korb_method_slot(c, mid);
    m->kind = KORB_METHOD_BUILTIN;
    m->params_cnt = params_cnt;   /* -1 = variadic */
    m->locals_cnt = 0;
    m->body = NULL;
    m->bfn = fn;
    c->vm->method_serial++;
}

static struct korb_method *
korb_method_lookup(struct korb_vm *vm, uint32_t mid)
{
    for (uint32_t i = 0; i < vm->method_cnt; i++) {
        if (vm->methods[i]->mid == mid) return vm->methods[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Raise + unwind backtrace.
 *
 * The exception object travels in RESULT.value (registers).  The UNWRAP /
 * CHECK propagation path has no GC points, so this is safe; only the
 * unwind bookkeeping below runs between raise and the toplevel report, and
 * it allocates libc memory only.
 * ------------------------------------------------------------------------- */

void
korb_bt_append(struct korb_vm *vm, uint32_t line, const char *name)
{
    if (vm->bt_cnt == vm->bt_capa) {
        vm->bt_capa = vm->bt_capa ? vm->bt_capa * 2 : 16;
        vm->bt = realloc(vm->bt, sizeof(struct korb_bt_entry) * vm->bt_capa);
        if (!vm->bt) { fprintf(stderr, "koruby_precise: out of memory (backtrace)\n"); abort(); }
    }
    /* CRuby caps the displayed backtrace; cap accumulation so a runaway
     * recursion unwind doesn't grow without bound. */
    if (vm->bt_cnt >= 4096) return;
    vm->bt[vm->bt_cnt].line = line;
    vm->bt[vm->bt_cnt].name = name;
    vm->bt_cnt++;
}

/* Snapshot the current unwind backtrace (vm->bt) into the exception parked at
 * slots[0], unless it already has one — CRuby captures the backtrace once (at
 * the raise site) and preserves it across re-raise.  The frames mirror
 * korb_report_uncaught: bt[0..n-1] (innermost first) then the trailing
 * '<main>' frame at e->line.  slots[1..2] stage the array + each string. */
RESULT
korb_capture_backtrace(CTX *c, VALUE *slots)
{
    if (!KORB_EXC_P(slots[0])) return RESULT_OK(KORB_NIL);
    if (VAL2EXC(slots[0])->backtrace != KORB_NIL) return RESULT_OK(KORB_NIL);
    struct korb_vm *const vm = c->vm;
    const char *const file = vm->script_name ? vm->script_name : "?";
    const uint32_t n = vm->bt_cnt;
    /* CRuby never shows the Kernel#raise / #fail entry point in the backtrace;
     * it appears as the innermost recorded frame (line 0) — elide it. */
    uint32_t start = 0;
    if (n > 0 && vm->bt[0].line == 0 && vm->bt[0].name &&
        (strcmp(vm->bt[0].name, "raise") == 0 || strcmp(vm->bt[0].name, "fail") == 0))
        start = 1;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n - start + 1));
    char buf[600];
    for (uint32_t i = start; i < n; i++) {
        const int len = snprintf(buf, sizeof buf, "%s:%d:in '%s'",
                                 file, (int32_t)vm->bt[i].line, vm->bt[i].name);
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, buf, (uint32_t)len));
        UNWRAP(korb_ary_push_val(c, slots, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    const int mlen = snprintf(buf, sizeof buf, "%s:%d:in '<main>'",
                              file, (int32_t)VAL2EXC(slots[0])->line);
    slots[2] = UNWRAP(korb_str_new(c, slots + 2, buf, (uint32_t)mlen));
    UNWRAP(korb_ary_push_val(c, slots, VALUE_REF_AT(&slots[1]), slots[2]));
    KorbException *const e = VAL2EXC(slots[0]);
    ARO_STORE(c, e, &e->backtrace, slots[1]);
    return RESULT_OK(KORB_NIL);
}

RESULT
korb_raise(CTX *c, VALUE *slots, unsigned int etype, uint32_t line,
           const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    c->vm->bt_cnt = 0;     /* fresh unwind */

    VALUE_REF msg = SLOTS_PUSH(slots, KORB_NIL);
    VALUE_REF_SET(msg, UNWRAP(korb_str_new(c, slots, buf, (uint32_t)strlen(buf))));
    KorbException *e = korb_alloc(c, slots, sizeof(KorbException), KORB_OBJ_EXCEPTION);
    e->etype = etype;
    e->line = line;
    ARO_STORE(c, e, &e->msg, VALUE_REF_GET(msg));
    const VALUE cause_v = korb_errinfo_top(c);                /* $! at raise time → #cause */
    if (cause_v != KORB_NIL && cause_v != (VALUE)e) ARO_STORE(c, e, &e->cause, cause_v);
    return RESULT_RAISE_((VALUE)e);
}

static const char *
korb_etype_name(unsigned int etype)
{
    switch (etype) {
      case KORB_E_TYPE:     return "TypeError";
      case KORB_E_ARGUMENT: return "ArgumentError";
      case KORB_E_ZERODIV:  return "ZeroDivisionError";
      case KORB_E_NOMETHOD: return "NoMethodError";
      case KORB_E_SYSSTACK: return "SystemStackError";
      case KORB_E_NOTIMPL:  return "NotImplementedError";
      case KORB_E_NAME:     return "NameError";
      case KORB_E_LOCALJUMP: return "LocalJumpError";
      case KORB_E_RANGE:    return "RangeError";
      case KORB_E_INDEX:    return "IndexError";
      case KORB_E_REGEXP:   return "RegexpError";
      case KORB_E_FROZEN:   return "FrozenError";
      case KORB_E_KEY:      return "KeyError";
      case KORB_E_STOP_ITERATION: return "StopIteration";
      case KORB_E_UNCAUGHT_THROW: return "UncaughtThrowError";
      case KORB_E_MATH_DOMAIN:    return "Math::DomainError";
      case KORB_E_FLOAT_DOMAIN:   return "FloatDomainError";
      case KORB_E_NO_MATCHING_PATTERN:     return "NoMatchingPatternError";
      case KORB_E_NO_MATCHING_PATTERN_KEY: return "NoMatchingPatternKeyError";
      case KORB_E_SYNTAX:   return "SyntaxError";
      case KORB_E_LOADERR:  return "LoadError";
      case KORB_E_IOERROR:  return "IOError";
      default:              return "RuntimeError";
    }
}

/* FrozenError with CRuby's message shape: "can't modify frozen <Type>: <inspect>".
 * Called from the KORB_CHECK_FROZEN macro (error path → the inspect cost is fine). */
/* Is the feature an `autoload` registration names already in $LOADED_FEATURES?
 * The registration keeps the path as written ("foo", "foo.rb", "lib/foo"), while
 * $LOADED_FEATURES holds absolute paths, so the ".rb"-completed form is matched
 * against a whole trailing path component (mirrors prelude Module#autoload?). */
static bool
korb_autoload_feature_loaded_p(CTX *c, VALUE pathv)
{
    const VALUE lf = korb_const_get(c->vm, korb_intern(c->vm, "$LOADED_FEATURES", 16));
    if (!KORB_ARRAY_P(lf)) return false;
    const KorbString *const p = VAL2STR(pathv);
    const char *const path = korb_strbuf_data(p->buf);
    const size_t plen = p->len;
    const bool has_rb = plen >= 3 && memcmp(path + plen - 3, ".rb", 3) == 0;
    const size_t slen = has_rb ? plen : plen + 3;          /* length of the ".rb" form */
    const KorbArray *const a = VAL2ARY(lf);
    for (uint32_t i = 0; i < a->len; i++) {
        const VALUE e = korb_items_data(a->items)[i];
        if (!KORB_STRING_P(e)) continue;
        const KorbString *const s = VAL2STR(e);
        const char *const f = korb_strbuf_data(s->buf);
        if (s->len == plen && memcmp(f, path, plen) == 0) return true;   /* verbatim */
        if (s->len < slen) continue;
        const char *const tail = f + s->len - slen;
        if (s->len > slen && tail[-1] != '/') continue;    /* only a whole component matches */
        if (memcmp(tail, path, plen) != 0) continue;
        if (has_rb || memcmp(tail + plen, ".rb", 3) == 0) return true;
    }
    return false;
}

/* private_constant / public_constant: mark (owner, name) as unreachable through
 * an explicit `Owner::NAME`.  Lexical (bare) reads inside the module still see
 * it, exactly as CRuby. */
void
korb_const_set_private(CTX *c, VALUE owner, uint32_t sym, bool private_p)
{
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->privconst_cnt; i++)
        if (vm->privconsts[i].name == sym && vm->privconsts[i].owner == owner) {
            if (!private_p) vm->privconsts[i] = vm->privconsts[--vm->privconst_cnt];
            return;
        }
    if (!private_p) return;
    if (vm->privconst_cnt == vm->privconst_capa) {
        vm->privconst_capa = vm->privconst_capa ? vm->privconst_capa * 2 : 8;
        vm->privconsts = realloc(vm->privconsts, sizeof(*vm->privconsts) * vm->privconst_capa);
        if (!vm->privconsts) abort();
    }
    vm->privconsts[vm->privconst_cnt].name = sym;
    vm->privconsts[vm->privconst_cnt].owner = owner;
    vm->privconst_cnt++;
}
/* deprecate_constant: mark (owner, name); reading it warns once per read. */
void
korb_const_set_deprecated(CTX *c, VALUE owner, uint32_t sym)
{
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->deprconst_cnt; i++)
        if (vm->deprconsts[i].name == sym && vm->deprconsts[i].owner == owner) return;
    if (vm->deprconst_cnt == vm->deprconst_capa) {
        vm->deprconst_capa = vm->deprconst_capa ? vm->deprconst_capa * 2 : 8;
        vm->deprconsts = realloc(vm->deprconsts, sizeof(*vm->deprconsts) * vm->deprconst_capa);
        if (!vm->deprconsts) abort();
    }
    vm->deprconsts[vm->deprconst_cnt].name = sym;
    vm->deprconsts[vm->deprconst_cnt].owner = owner;
    vm->deprconst_cnt++;
}
bool
korb_const_deprecated_p(const struct korb_vm *vm, VALUE owner, uint32_t sym)
{
    for (uint32_t i = 0; i < vm->deprconst_cnt; i++)
        if (vm->deprconsts[i].name == sym && vm->deprconsts[i].owner == owner) return true;
    return false;
}
/* "warning: constant Owner::NAME is deprecated", gated on Warning[:deprecated]. */
void
korb_const_deprecated_warn(CTX *c, VALUE *slots, VALUE owner, uint32_t sym)
{
    struct korb_vm *const vm = c->vm;
    if (LIKELY(vm->deprconst_cnt == 0) || !korb_const_deprecated_p(vm, owner, sym)) return;
    {   /* Warning::CATEGORIES__[:deprecated] is the gate (Kernel#warn's own check) */
        const VALUE wm = korb_const_get(vm, korb_intern(vm, "Warning", 7));
        if (!KORB_CLASS_P(wm)) return;
        const uint32_t ci = korb_const_index_owned(vm, korb_intern(vm, "CATEGORIES__", 12), wm);
        if (ci == UINT32_MAX || !KORB_HASH_P(vm->const_vals[ci])) return;
        const int32_t hi = korb_hash_find(VAL2HASH(vm->const_vals[ci]), ID2SYM(korb_intern(vm, "deprecated", 10)));
        if (hi < 0 || !KORB_TRUTHY(korb_items_data(VAL2HASH(vm->const_vals[ci])->items)[2 * hi + 1])) return;
    }
    char qn[256]; korb_class_desc_into(c, owner, qn, sizeof qn);
    korb_warn(c, slots, "constant %s::%s is deprecated", qn, korb_sym_name(vm, sym));
}
bool
korb_const_private_p(const struct korb_vm *vm, VALUE owner, uint32_t sym)
{
    for (uint32_t i = 0; i < vm->privconst_cnt; i++)
        if (vm->privconsts[i].name == sym && vm->privconsts[i].owner == owner) return true;
    return false;
}

/* Is `sym` registered as a (not yet loaded) autoload on `mod`?  The registry is
 * the module's own @__autoloads Hash (prelude Module#autoload).  CRuby reports
 * such a constant as defined before the file is loaded, and removing it must not
 * trigger the load. */
bool
korb_autoload_registered_p(CTX *c, VALUE mod, uint32_t sym)
{
    if (!KORB_CLASS_P(mod)) return false;
    const VALUE t = korb_ivar_get(c, mod, ID2SYM(korb_intern(c->vm, "@__autoloads", 12)));
    if (!KORB_HASH_P(t)) return false;
    const int32_t idx = korb_hash_find(VAL2HASH(t), ID2SYM(sym));
    if (idx < 0) return false;
    /* a registration whose feature was already required is inert: the file will
     * not run again, so the constant is simply not there (CRuby reports it as
     * undefined and #autoload? as nil). */
    const VALUE pathv = korb_items_data(VAL2HASH(t)->items)[2 * idx + 1];
    return !(KORB_STRING_P(pathv) && korb_autoload_feature_loaded_p(c, pathv));
}

RESULT
korb_raise_frozen(CTX *c, VALUE *slots, VALUE v)
{
    /* name the object's real class and let it inspect itself: a user instance is
     * not "Object", and a Time renders as a Time.  nil/true/false are named by
     * their class here (NilClass), not by the value as in a TypeError. */
    slots[0] = v;
    if (v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE) {
        const char *const cn = (v == KORB_NIL) ? "NilClass" : (v == KORB_TRUE) ? "TrueClass" : "FalseClass";
        const char *const iv = (v == KORB_NIL) ? "nil" : (v == KORB_TRUE) ? "true" : "false";
        return korb_raise(c, slots + 1, KORB_E_FROZEN, 0, "can't modify frozen %s: %s", cn, iv);
    }
    if (KORB_OBJECT_P(slots[0])) {                     /* an object renders through its own #inspect */
        const RESULT ir = korb_send(c, slots + 1, korb_intern(c->vm, "inspect", 7), 0, 0);
        if (ir.state == KORB_NORMAL && KORB_STRING_P(ir.value)) {
            const KorbString *const is = VAL2STR(ir.value);
            return korb_raise(c, slots + 1, KORB_E_FROZEN, 0, "can't modify frozen %s: %.*s",
                              korb_coerce_name(c, slots[0]), (int)is->len, korb_strbuf_data(is->buf));
        }
    }
    char *ibuf = NULL; size_t ilen = 0;
    FILE *ims = open_memstream(&ibuf, &ilen);
    if (ims) { korb_fprint_inspect_s(c, slots + 1, ims, slots[0]); fclose(ims); }
    RESULT r = korb_raise(c, slots + 1, KORB_E_FROZEN, 0, "can't modify frozen %s: %s",
                          korb_coerce_name(c, slots[0]), ibuf ? ibuf : "");
    free(ibuf);
    return r;
}

/* Guard a method definition against a frozen definee.  Adding a method to a
 * singleton class modifies its attached object, so a `class << frozen_obj` (or
 * frozen class) def must raise FrozenError against that object.  Returns a RAISE
 * result when frozen, else NORMAL. */
RESULT
korb_check_def_frozen(CTX *c, VALUE *slots, VALUE definee)
{
    VALUE target = definee;
    if (KORB_CLASS_P(definee) && VAL2CLASS(definee)->is_singleton) {   /* singleton → its attached object governs */
        struct korb_vm *const vm = c->vm;
        for (uint32_t i = 0; i < vm->sklass_cnt; i++)
            if (vm->sklass_cls[i] == definee) { target = vm->sklass_obj[i]; break; }
    }
    if (UNLIKELY(AROH_IS_GC_OBJECT(target) &&
                 (((const AroObjectHeader *)(uintptr_t)target)->flags & KORB_FL_FROZEN)))
        return korb_raise_frozen(c, slots, target);
    return RESULT_OK(KORB_NIL);
}

/* etype for `Klass.new` on an exception class (mirrors the `raise Klass` path):
 * nearest builtin-exception ancestor's exc_etype, else KORB_E_RUNTIME for an
 * abstract base / user subclass of Exception, else -1 (not an exception). */
static int
korb_class_exc_etype(struct korb_vm *vm, VALUE cls)
{
    for (VALUE cc = cls; KORB_CLASS_P(cc); cc = VAL2CLASS(cc)->superclass)
        if (VAL2CLASS(cc)->exc_etype >= 0) return VAL2CLASS(cc)->exc_etype;
    const VALUE exc_base = korb_builtin_class_obj(vm, KORB_C_EXCEPTION);
    if (KORB_CLASS_P(exc_base) && korb_class_le(cls, exc_base)) return KORB_E_RUNTIME;
    return -1;
}

void
korb_report_uncaught(CTX *c, VALUE exc)
{
    struct korb_vm *const vm = c->vm;
    const char *file = vm->script_name ? vm->script_name : "?";

    if (!KORB_EXC_P(exc)) {
        fprintf(stderr, "%s: uncaught non-exception raise\n", file);
        return;
    }
    KorbException *e = VAL2EXC(exc);
    /* A const-only class (LoadError, ThreadError, a user subclass, …) carries a
     * generic etype and records its real class in exc_class — report that name,
     * not the etype's ("cannot load such file" is a LoadError, not RuntimeError). */
    char clsbuf[192];
    const char *cls = korb_etype_name(e->etype);
    if (KORB_CLASS_P(e->exc_class) && VAL2CLASS(e->exc_class)->name_sym) {
        korb_class_qname_into(c, e->exc_class, clsbuf, sizeof clsbuf);
        cls = clsbuf;
    }
    const char *msg = (e->msg != KORB_NIL) ? korb_strbuf_data(VAL2STR(e->msg)->buf) : cls;

    if (vm->bt_cnt > 0) {
        fprintf(stderr, "%s:%d:in '%s': %s (%s)\n", file, (int32_t)vm->bt[0].line, vm->bt[0].name, msg, cls);
        /* elide the middle of very deep unwinds (SystemStackError) */
        uint32_t head = vm->bt_cnt, tail = 0;
        if (vm->bt_cnt > 20) { head = 12; tail = 4; }
        for (uint32_t i = 1; i < head; i++) {
            fprintf(stderr, "\tfrom %s:%d:in '%s'\n", file, (int32_t)vm->bt[i].line, vm->bt[i].name);
        }
        if (tail) {
            fprintf(stderr, "\t ... %u levels...\n", vm->bt_cnt - head - tail);
            for (uint32_t i = vm->bt_cnt - tail; i < vm->bt_cnt; i++) {
                fprintf(stderr, "\tfrom %s:%d:in '%s'\n", file, (int32_t)vm->bt[i].line, vm->bt[i].name);
            }
        }
        fprintf(stderr, "\tfrom %s:%d:in '<main>'\n", file, (int32_t)e->line);
    }
    else {
        fprintf(stderr, "%s:%d:in '<main>': %s (%s)\n", file, (int32_t)e->line, msg, cls);
    }
}

/* ---------------------------------------------------------------------------
 * Calls.
 * ------------------------------------------------------------------------- */

/* Shared call path.  Frame reserved cells (top-down): self at base[fs-1]
 * (always, written from `self`); when the callee yields (m->uses_block) the
 * block group {block_entry, def_env, captured_self} sits just below at
 * base[fs-4..fs-2], odd-tagged so the GC root scan skips the two pointers.
 * `block` is NULL for an ordinary call (block ignored by non-yielding callees). */
static VALUE  korb_set_elems_of(VALUE v);
static RESULT korb_set_new(CTX *c, VALUE *slots, VALUE elems);
static RESULT korb_set_from_array(CTX *c, VALUE *slots, VALUE_REF src);
static RESULT korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_fiber_new(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE *captured_self);
RESULT korb_fiber_check_storage(CTX *c, VALUE *slots, VALUE h);   /* fwd (builtins/fiber.c) */
static RESULT korb_thread_s_new(CTX *c, VALUE *slots, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);   /* thread.c */
static RESULT korb_mutex_s_new(CTX *c, VALUE *slots);      /* thread.c */
static RESULT korb_thread_alloc_handle(CTX *c, VALUE *slots);   /* thread.c: 未初期化 thread */
static RESULT korb_raise_thread_error(CTX *c, VALUE *slots, const char *msg);   /* thread.c */
static RESULT korb_thread_init_body(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_condvar_s_new(CTX *c, VALUE *slots);    /* thread.c */
static RESULT korb_cproc_yield(CTX *c, VALUE *restrict slots, VALUE procv,
                               const VALUE *restrict argv, uint32_t argc);   /* fwd: Method/Symbol#to_proc body */

/* Invoke a resolved method `m` on the staged receiver (send layout: recv at
 * slots[-argc-1], args at slots[-argc..]).  Handles every method kind, so all
 * receiver dispatch funnels through one place. */
static RESULT korb_block_yield_full(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                                    const VALUE *argv, uint32_t argc, VALUE *captured_self,
                                    NODE *bp_blk, VALUE *bp_denv, VALUE *bp_self, uint32_t is_lam);   /* fwd */
static __attribute__((no_stack_protector)) RESULT
korb_dispatch_method(CTX *c, VALUE *slots, struct korb_method *m, uint32_t mid,
                     uint32_t line, uint32_t argc, VALUE def_class,
                     NODE *block, VALUE *def_env, VALUE *captured_self)
{
    VALUE *const recv_slot = &slots[-(korb_sword_t)argc - 1];
    const VALUE self = *recv_slot;
    switch (m->kind) {
      case KORB_METHOD_BUILTIN: {   /* global C function reached via send / method_missing / Kernel.x */
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
        RESULT r = m->bfn(c, slots, VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc));
        if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) { KorbException *e = VAL2EXC(r.value); korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid)); e->line = line; }
        return r;
      }
      case KORB_METHOD_ATTR_R:
        return RESULT_OK(korb_ivar_get(c, self, ID2SYM(m->attr_ivar)));
      case KORB_METHOD_ATTR_W: {
        if (UNLIKELY(argc != 1))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected 1)", argc);
        KORB_CHECK_FROZEN(c, slots, *recv_slot);
        const VALUE v = slots[-(korb_sword_t)argc];
        CHECK(korb_ivar_set(c, slots, VALUE_REF_AT(recv_slot), ID2SYM(m->attr_ivar), v));
        return RESULT_OK(slots[-(korb_sword_t)argc]);
      }
      case KORB_METHOD_CFUNC: {
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
        const VALUE_REF recv = VALUE_REF_AT(recv_slot);
        const VALUE_SLICE args = VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc);
        RESULT r = m->uses_block ? m->rbfn(c, slots, recv, args, block, def_env, captured_self)
                                 : m->rfn(c, slots, recv, args);
        if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {
            KorbException *e = VAL2EXC(r.value);
            korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid));
            e->line = line;
        }
        return r;
      }
      case KORB_METHOD_DM: {   /* define_method: run the (env-pre-closed) Proc body with self = receiver */
        const KorbProc *const p = VAL2PROC(m->dm_proc);
        /* a define_method'd method enforces arity like a method regardless of
         * whether the source was a block, lambda or plain proc (CRuby converts
         * proc arity to method arity).  Positional-only; skip when keywords are
         * declared (a trailing kw Hash would skew the count). */
        if (p->iseq != NULL && p->iseq != KORB_BLK_CPROC &&
            p->iseq->head.kind == &kind_node_entry && p->iseq->u.node_entry.kw_info == NULL) {
            const NODE *const e = p->iseq;
            const uint32_t pc = e->u.node_entry.params_cnt;
            const bool has_rest = e->u.node_entry.rest_slot >= 0;
            const bool variable = (e->u.node_entry.opt_defaults != NULL) || has_rest;
            const uint32_t req = variable ? e->u.node_entry.req_cnt : pc;
            if (UNLIKELY(argc < req || (!has_rest && argc > pc))) {
                char exp[32];
                if (has_rest)       snprintf(exp, sizeof exp, "%u+", req);
                else if (req == pc) snprintf(exp, sizeof exp, "%u", req);
                else                snprintf(exp, sizeof exp, "%u..%u", req, pc);
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected %s)", argc, exp);
            }
        }
        /* A Method#to_proc / Symbol#to_proc body (KORB_BLK_CPROC) has no node_entry:
         * it dispatches a send instead, and needs the PROC (not the receiver) as
         * its "captured self".  korb_block_yield's CPROC branch reads that from
         * captured_self, so route this case directly — passing recv_slot there
         * would dereference the receiver as a Proc.
         * (`define_method(:m, &SomeClass.method(:c))`, as ruby/spec's
         * TimeSpecs::MethodHolder does.) */
        if (UNLIKELY(p->iseq == NULL || p->iseq == KORB_BLK_CPROC)) {   /* iseq==NULL: Symbol/Method#to_proc */
            slots[0] = m->dm_proc;                       /* root the proc; args live below at slots[-argc] */
            return korb_cproc_yield(c, slots + 1, slots[0], &slots[-(korb_sword_t)argc], argc);
        }
        /* Forward the method's own block into the body's `|&b|` — a
         * define_method'd method takes a block like any other. */
        const struct korb_method *const dm_saved = c->dm_entry;
        c->dm_entry = m;                             /* a `super` in the body resolves through this */
        RESULT r = korb_block_yield_full(c, slots, p->iseq, (VALUE *)(uintptr_t)p->env,
                                         &slots[-(korb_sword_t)argc], argc, recv_slot,
                                         block, def_env, captured_self, 0);   /* captured_self = receiver slot */
        c->dm_entry = dm_saved;
        /* A define_method body behaves like a lambda: `return`, `break` and
         * `next` all just leave the method with that value (a bare `break` in a
         * plain block would unwind past it and end the program). */
        if (r.state == KORB_RETURN || r.state == KORB_BREAK) { r.state = KORB_NORMAL; c->return_target = NULL; }
        else if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {   /* a non-exception RAISE payload (e.g. thread kill) carries no line */
            KorbException *e = VAL2EXC(r.value);
            korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid));
            e->line = line;
        }
        return r;
      }
      default: /* KORB_METHOD_ISEQ */
        if (LIKELY(m->is_simple))
            return korb_invoke_simple(c, slots, m, argc, line, mid, self, def_class);
        return korb_invoke_method(c, slots, m, argc, line, mid, self, def_class,
                                  block, def_env, KORB_CSELF_VAL(captured_self));
    }
}
static RESULT korb_m_fiber_yield(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_fiber_s_current(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_fiber_s_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_fiber_s_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_fiber_s_blocking_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT
korb_call_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
               struct korb_callcache *cc, uint32_t argc,
               VALUE self, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    struct korb_vm *const vm = c->vm;

    /* implicit-self send / __send__ / public_send → re-dispatch with self as the
     * receiver (korb_send_impl shifts arg0 = the target method name). */
    if (UNLIKELY(mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send)) {
        if (UNLIKELY(argc == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "no method name given");
        for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(korb_sword_t)argc + j];
        slots[0] = self;                            /* recv below the args */
        return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
    }

    /* implicit self-call on a user instance → dispatch through its class chain
     * (a miss falls through to the global function table). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_mcache_find(vm, korb_obj_dispatch_klass(vm, self), mid, &def_class);
        if (um) {
            if (um->kind == KORB_METHOD_ATTR_R)
                return RESULT_OK(korb_ivar_get(c, self, ID2SYM(um->attr_ivar)));
            if (um->kind == KORB_METHOD_ATTR_W) {
                if (UNLIKELY(argc != 1))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                      "wrong number of arguments (given %u, expected 1)", argc);
                slots[0] = self;                       /* root self for the set */
                VALUE v = slots[-(korb_sword_t)argc];
                CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(um->attr_ivar), v));
                return RESULT_OK(slots[-(korb_sword_t)argc]);
            }
            if (um->kind == KORB_METHOD_ISEQ) {
                if (LIKELY(um->is_simple))
                    return korb_invoke_simple(c, slots, um, argc, line, mid, self, def_class);
                return korb_invoke_method(c, slots, um, argc, line, mid, self, def_class, block, def_env, KORB_CSELF_VAL(captured_self));
            }
            /* CFUNC (inherited builtin, e.g. implicit `freeze`) → re-dispatch as send */
            for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(korb_sword_t)argc + j];
            slots[0] = self;
            return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
        }
    }

    /* implicit self-call where self is a builtin / overridden builtin / class,
     * e.g. `upcase` inside a String-subclass method = `self.upcase`.  If self
     * responds to mid via its class-object MRO, re-dispatch as a send (recv at
     * slots[0], args above).  Otherwise fall through to the global function
     * table (p/puts/top-level defs).  `main` (klass=nil) is excluded so
     * top-level keeps using globals. */
    if ((AROH_IS_GC_OBJECT(self) &&
         (!(KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL) || block != NULL))
        || !AROH_IS_GC_OBJECT(self)) {
        /* `main` (klass=nil) normally keeps globals, but a block-bearing call
         * (e.g. top-level `loop do … end`, `tap { }`) must reach the Object
         * method — top-level user defs are globals and won't respond here.  An
         * immediate self (Integer/Symbol/Float/nil/true/false) always re-dispatches
         * a bare call on its own class (there is no "main" case for immediates);
         * korb_responds_to below still lets true globals fall through. */
        if (korb_responds_to(c, self, mid)) {
            for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(korb_sword_t)argc + j];
            slots[0] = self;                            /* recv below the args */
            return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
        }
    }

    /* `include Mod...` inside a class/module body (self is the class), or at the
     * top level (self is main) where it includes into Object. */
    if (argc >= 1 && strcmp(korb_sym_name(vm, mid), "include") == 0 &&
        (KORB_CLASS_P(self) || (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL))) {
        const VALUE target = KORB_CLASS_P(self) ? self : korb_builtin_class_obj(vm, KORB_C_OBJECT);
        return korb_do_include(c, slots, target, VALUE_SLICE_MAKE(slots - argc, argc));
    }
    /* `prepend Mod...` inside a class/module body (self is the class) */
    if (KORB_CLASS_P(self) && argc >= 1 && strcmp(korb_sym_name(vm, mid), "prepend") == 0) {
        return korb_do_prepend(c, slots, self, VALUE_SLICE_MAKE(slots - argc, argc));
    }

    struct korb_method *m = cc->m;
    if (UNLIKELY(cc->serial != vm->method_serial)) {
        m = korb_method_lookup(vm, mid);
        if (UNLIKELY(m == NULL)) {
            /* `main` (klass=nil): no global def by this name — fall back to
             * Object/Kernel methods (to_s, inspect, class, freeze, ...) so the
             * top-level self behaves like a real Object instance. */
            if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL &&
                korb_responds_to(c, self, mid)) {
                for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(korb_sword_t)argc + j];
                slots[0] = self;
                return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
            }
            slots[0] = self;                               /* root receiver across raise + ivar_set */
            char rdbuf2[256];
            const char *const rd2 = (KORB_OBJECT_P(slots[0]) && VAL2OBJ(slots[0])->klass == KORB_NIL)
                                        ? "main" : korb_recv_desc(c, slots + 2, slots[0], rdbuf2, sizeof rdbuf2);
            RESULT nmr = korb_raise(c, slots + 1, KORB_E_NOMETHOD, line,
                              "undefined method '%s' for %s", korb_sym_name(vm, mid), rd2);
            if (LIKELY(KORB_EXC_P(nmr.value))) {
                slots[1] = nmr.value;
                VALUE_REF eref = VALUE_REF_AT(&slots[1]);
                korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__name", 7)), ID2SYM(mid));
                korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__has_recv", 11)), KORB_TRUE);
                korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__receiver", 11)), slots[0]);
                RESULT ar = korb_ary_new(c, slots + 2, argc);   /* @__args = the args passed to the missing method */
                if (LIKELY(ar.state == KORB_NORMAL)) {
                    slots[2] = ar.value;
                    VALUE_REF argsref = VALUE_REF_AT(&slots[2]);
                    for (uint32_t j = 0; j < argc; j++)
                        korb_ary_push_val(c, slots + 3, argsref, slots[-(korb_sword_t)argc + j]);
                    korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(vm, "@__args", 7)), VALUE_REF_GET(argsref));
                }
                nmr.value = VALUE_REF_GET(eref);
            }
            return nmr;
        }
        cc->m = m;
        cc->serial = vm->method_serial;
    }

    VALUE *const base = slots - argc;     /* staged args = parameter window */

    if (m->kind == KORB_METHOD_BUILTIN) {
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc)) {
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)",
                              argc, m->params_cnt);
        }
        RESULT r = m->bfn(c, slots, VALUE_SLICE_MAKE(base, argc));
        if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {
            KorbException *e = VAL2EXC(r.value);
            korb_bt_append(vm, e->line, korb_sym_name(vm, m->mid));
            e->line = line;
        }
        return r;
    }

    /* ISEQ global function: no defining class (super in a global fn has none). */
    if (LIKELY(m->is_simple))
        return korb_invoke_simple(c, slots, m, argc, line, mid, self, KORB_NIL);
    return korb_invoke_method(c, slots, m, argc, line, mid, self, KORB_NIL,
                              block, def_env, KORB_CSELF_VAL(captured_self));
}

RESULT
korb_call(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
          struct korb_callcache *cc, uint32_t argc, VALUE self)
{
    return korb_call_impl(c, slots, mid, line, cc, argc, self, NULL, NULL, NULL);
}

/* Invoke a resolved user-instance method with EXPLICIT self (implicit-call
 * layout: args at slots[-argc..], self separate — NOT staged below).  Handles
 * the less-common ATTR + non-simple ISEQ kinds; the hot ISEQ-simple case is
 * handled inline by korb_call_cached.  Returns false for CFUNC (caller falls to
 * korb_call_impl). */
static bool
korb_invoke_self(CTX *c, VALUE *slots, struct korb_method *m, uint32_t argc,
                 uint32_t line, uint32_t mid, VALUE self, VALUE def_class, RESULT *out)
{
    switch (m->kind) {
      case KORB_METHOD_ATTR_R:
        *out = RESULT_OK(korb_ivar_get(c, self, ID2SYM(m->attr_ivar)));
        return true;
      case KORB_METHOD_ATTR_W:
        if (UNLIKELY(argc != 1)) {
            *out = korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected 1)", argc);
            return true;
        }
        if (UNLIKELY(AROH_IS_GC_OBJECT(self) && (((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_FROZEN))) {
            *out = korb_raise(c, slots, KORB_E_FROZEN, line, "can't modify frozen %s", korb_type_name(self));
            return true;
        }
        slots[0] = self;
        { VALUE v = slots[-(korb_sword_t)argc];
          RESULT chk = korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(m->attr_ivar), v);
          if (UNLIKELY(chk.state != KORB_NORMAL)) { *out = chk; return true; } }
        *out = RESULT_OK(slots[-(korb_sword_t)argc]);
        return true;
      case KORB_METHOD_ISEQ:   /* non-simple (rest/opt/post/kw/block) */
        *out = korb_invoke_method(c, slots, m, argc, line, mid, self, def_class, NULL, NULL, KORB_NIL);
        return true;
      default:                 /* CFUNC (inherited builtin via implicit self) */
        return false;
    }
}

/* Per-call-site cached implicit-self call (no block).  Monomorphic user-instance
 * sites shortcut to korb_invoke_simple directly (inlines), skipping korb_call_impl's
 * prologue + send/__send__ probe + mcache hash.  Everything else (main/global via
 * cc, builtin self, CFUNC, method_missing) falls to korb_call_impl; those sites
 * don't fill `ic`. */
__attribute__((no_stack_protector)) RESULT
korb_call_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                 struct korb_callcache *cc, struct korb_inlcache *ic,
                 uint32_t argc, VALUE self)
{
    struct korb_vm *const vm = c->vm;
    if (LIKELY(KORB_OBJECT_P(self))) {
        const VALUE klass = VAL2OBJ(self)->klass;
        if (LIKELY(klass != KORB_NIL)) {              /* user-instance self-call (inline cache) */
            struct korb_method *m;
            VALUE def_class;
            if (LIKELY(ic->serial == vm->method_serial && ic->klass == klass)) {
                m = ic->m; def_class = ic->def_class;
            } else {
                def_class = KORB_NIL;
                m = korb_mcache_find(vm, klass, mid, &def_class);
                if (UNLIKELY(m == NULL)) return korb_call_impl(c, slots, mid, line, cc, argc, self, NULL, NULL, NULL);
                ic->serial = vm->method_serial; ic->klass = klass; ic->m = m; ic->def_class = def_class;
            }
            if (LIKELY(m->kind == KORB_METHOD_ISEQ && m->is_simple))   /* hot path: inlines */
                return korb_invoke_simple(c, slots, m, argc, line, mid, self, def_class);
            RESULT r;
            if (korb_invoke_self(c, slots, m, argc, line, mid, self, def_class, &r))
                return r;   /* ATTR / non-simple ISEQ */
            /* CFUNC → fall through to korb_call_impl */
        } else if (LIKELY(cc->serial == vm->method_serial && cc->m != NULL &&
                          cc->m->kind == KORB_METHOD_ISEQ && cc->m->is_simple)) {
            /* top-level (main, klass-less) call of a cached simple ISEQ global
             * function (fib / ackermann / inc) — skip korb_call_impl's maze.
             * No send-variant guard needed: send/__send__/public_send sites are
             * intercepted in korb_call_impl (line ~2901) before cc->m is ever
             * filled, so a non-NULL simple-ISEQ cc->m is never a send variant. */
            return korb_invoke_simple(c, slots, cc->m, argc, line, mid, self, KORB_NIL);
        }
    }
    return korb_call_impl(c, slots, mid, line, cc, argc, self, NULL, NULL, NULL);
}

/* Implicit-self keyword call `f(pos..., k: v...)`.  Positionals at
 * base[0..pos_argc); keyword VALUES at base[pos_argc..+kw_argc) with names
 * kw_syms[].  Resolves the method (cc/ic cached) and, when it's a kw-param ISEQ
 * with no **rest/rest/post, binds keywords straight to slots (no Hash).  Anything
 * else builds a kwargs Hash and routes through the normal call path. */
__attribute__((no_stack_protector)) RESULT
korb_call_kw(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, struct korb_callcache *cc,
             struct korb_inlcache *ic, uint32_t pos_argc, const uint32_t *kw_syms,
             uint32_t kw_argc, VALUE self)
{
    struct korb_vm *const vm = c->vm;
    struct korb_method *m = NULL; VALUE def_class = KORB_NIL;
    if (LIKELY(KORB_OBJECT_P(self))) {
        const VALUE klass = VAL2OBJ(self)->klass;
        if (LIKELY(klass != KORB_NIL)) {                 /* user-instance self-call */
            if (LIKELY(ic->kind == KORB_IC_INSTANCE && ic->serial == vm->method_serial && ic->klass == klass)) {
                m = ic->m; def_class = ic->def_class;
            } else {
                m = korb_mcache_find(vm, klass, mid, &def_class);
                if (m) { ic->serial = vm->method_serial; ic->klass = klass; ic->m = m; ic->def_class = def_class; ic->kind = KORB_IC_INSTANCE; }
            }
        } else {                                         /* main / top-level global function */
            if (LIKELY(cc->serial == vm->method_serial && cc->m != NULL)) m = cc->m;
            else { m = korb_method_lookup(vm, mid); if (m) { cc->serial = vm->method_serial; cc->m = m; } }
        }
    }
    if (LIKELY(m != NULL && m->kind == KORB_METHOD_ISEQ)) {
        const struct korb_kw_info *const mkw = (const struct korb_kw_info *)m->kw_info;
        if (UNLIKELY(kw_argc > 0 && mkw != NULL && mkw->kwrest_slot == -3))   /* `**nil`: keyword syntax is refused outright */
            return korb_raise(c, slots - (pos_argc + kw_argc), KORB_E_ARGUMENT, line, "no keywords accepted");
        if (LIKELY(korb_kw_fast_eligible(m) && kw_argc <= 64))
            return korb_invoke_kw_simple(c, slots, m, pos_argc, kw_syms, kw_argc, line, mid, self, def_class);
        return korb_invoke_kw_viahash(c, slots, m, pos_argc, kw_syms, kw_argc, line, mid, self, def_class);
    }
    /* unresolved / CFUNC / ATTR → materialize a kwargs Hash and use the normal path. */
    VALUE *const base = slots - (pos_argc + kw_argc);
    VALUE *const cur = slots;
    cur[0] = UNWRAP(korb_hash_new(c, cur, kw_argc));
    VALUE_REF h = VALUE_REF_AT(&cur[0]);
    for (uint32_t p = 0; p < kw_argc; p++) {
        cur[1] = ID2SYM(kw_syms[p]); cur[2] = base[pos_argc + p];
        CHECK(korb_hash_set(c, cur + 3, h, VALUE_REF_AT(&cur[1]), cur[2]));
    }
    base[pos_argc] = VALUE_REF_GET(h);
    ((AroObjectHeader *)(uintptr_t)base[pos_argc])->flags |= KORB_FL_KWARGS;   /* written as keywords */
    /* `self` (the by-value param) may have moved during the Hash build above — it
     * is staged at base[-1] (a scanned slot), so re-read the forwarded receiver. */
    return korb_call(c, base + pos_argc + 1, mid, line, cc, pos_argc + 1, base[-1]);
}

RESULT
korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              struct korb_callcache *cc, uint32_t argc,
              VALUE self, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    RESULT r = korb_call_impl(c, slots, mid, line, cc, argc, self, block, def_env, captured_self);
    /* `break [v]` in the block = this call's value — but only if the break came
     * from the block *this* call site handed over (a break from a block merely
     * forwarded through here belongs to an outer call). */
    if (r.state == KORB_BREAK && korb_break_owned(c, block, def_env)) r.state = KORB_NORMAL;
    return r;
}

/* ---- node_entry accessors + yield ----------------------------------------- */

static RESULT korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             NODE *block, VALUE *def_env, VALUE *captured_self);

/* Coerce a `&obj` block argument to a Proc (CRuby calls obj.to_proc).  No-op for a
 * Proc or nil; otherwise sends #to_proc and writes the result back to *pslot — a
 * scanned caller slot, so the coerced Proc stays rooted for the forward.  Raises
 * TypeError when obj has no #to_proc (or it returns a non-Proc), matching CRuby. */
RESULT korb_blockarg_to_proc(CTX *c, VALUE *slots, VALUE *restrict pslot, uint32_t line) {
    VALUE pv = *pslot;
    if (LIKELY(KORB_PROC_P(pv)) || pv == KORB_NIL) return RESULT_OK(pv);
    const uint32_t to_proc = korb_intern(c->vm, "to_proc", 7);
    if (UNLIKELY(!korb_responds_to_coerce_p(c, slots, &pv, to_proc)))
        return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Proc", korb_type_name(pv));
    slots[0] = pv;
    const RESULT r = korb_send_impl(c, slots + 1, to_proc, line, 0, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (UNLIKELY(!KORB_PROC_P(r.value)))
        return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Proc", korb_type_name(pv));
    *pslot = r.value;
    return RESULT_OK(r.value);
}

/* Yield to a forwarded C-proc (Symbol#to_proc / Method#to_proc): there is no block
 * frame to build — dispatch the captured send with the yielded args.  `procv` is read
 * fresh from the FWD-rooted slot by the caller, so a GC move before this is safe.
 * Args are copied high-to-low so an argv that aliases the cursor below isn't clobbered. */
static RESULT korb_cproc_yield(CTX *c, VALUE *restrict slots, VALUE procv,
                               const VALUE *restrict argv, uint32_t argc) {
    const KorbProc *const p = VAL2PROC(procv);
    const uint32_t mid = p->sym_mid;
    if (p->self != KORB_NIL) {                           /* Method#to_proc (bound receiver): recv.mid(args...) */
        const VALUE recv = p->self;
        for (int32_t i = (int32_t)argc - 1; i >= 0; i--) slots[1 + i] = argv[i];
        slots[0] = recv;
        return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, NULL, NULL, NULL);
    }
    if (UNLIKELY(argc < 1))                              /* Symbol#to_proc: args[0].mid(args[1..]) */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver is available");
    for (int32_t i = (int32_t)argc - 1; i >= 0; i--) slots[i] = argv[i];
    return korb_send_impl(c, slots + argc, mid, 0, argc - 1, NULL, NULL, NULL);
}

/* CPROC (a forwarded Symbol/Method#to_proc) has no node_entry — builtin iterators
 * that read the block's arity must treat it as a 1-arg block (the yielded element
 * is handed to the coerced proc whole).  Guards every iterator at one point. */
uint32_t korb_entry_params_cnt(NODE *entry) { return entry == KORB_BLK_CPROC ? 1u : entry->u.node_entry.params_cnt; }
uint32_t korb_entry_locals_cnt(NODE *entry) { return entry->u.node_entry.locals_cnt; }
static uint32_t korb_entry_destructure_n(NODE *entry) { return entry->u.node_entry.destructure_n; }
static int32_t  korb_entry_rest_slot(NODE *entry) { return entry->u.node_entry.rest_slot; }   /* -1 = no rest param */
/* Bind one destructuring-spec entry (see parse.c): 0x00 = scalar leaf,
 * 0xFF <k> = a group of k nested entries.  Returns the next entry. */
static const uint8_t *korb_bind_destr_entry(CTX *c, VALUE *bf, uint32_t nlocals, uint32_t *loc, const uint8_t *sp, VALUE pv)
{
    if (*sp == 0xFE) {                                  /* |(a, *b, c)| — split group */
        const uint32_t nl = sp[1], rest_named = sp[2], nr = sp[3];
        const uint8_t *cur = sp + 4;
        const KorbArray *const ar = KORB_ARRAY_P(pv) ? VAL2ARY(pv) : NULL;
        const uint32_t n = ar ? ar->len : (pv == KORB_NIL ? 0 : 1);
        const VALUE *const items = ar ? korb_items_data(ar->items) : &pv;
        const uint32_t surplus = (n > nl + nr) ? n - nl - nr : 0;
        /* Stage the source values in the (scanned) block frame before the rest
         * array is allocated: `items` points into the source Array, which the
         * alloc's GC would move. */
        VALUE *const stage = bf + 1 + nlocals;
        for (uint32_t j = 0; j < n; j++) stage[j] = items[j];
        for (uint32_t j = 0; j < nl; j++)
            cur = korb_bind_destr_entry(c, bf, nlocals, loc, cur, (j < n) ? stage[j] : KORB_NIL);
        if (rest_named) {                               /* the middle slice as an Array */
            const uint32_t rloc = *cur++;               /* the rest leaf's local index */
            VALUE *const acur = stage + n;
            RESULT ra = korb_ary_new(c, acur, surplus ? surplus : 4);
            if (UNLIKELY(ra.state != KORB_NORMAL)) { bf[1 + rloc] = KORB_NIL; }
            else {
                acur[0] = ra.value;
                for (uint32_t j = 0; j < surplus; j++)
                    (void)korb_ary_push_val(c, acur + 1, VALUE_REF_AT(&acur[0]), stage[nl + j]);
                bf[1 + rloc] = acur[0];
            }
            (*loc)++;
        }
        for (uint32_t j = 0; j < nr; j++) {
            const uint32_t idx = nl + surplus + j;
            cur = korb_bind_destr_entry(c, bf, nlocals, loc, cur, (idx < n) ? stage[idx] : KORB_NIL);
        }
        return cur;
    }
    if (*sp != 0xFF) { (*loc)++; bf[1 + sp[1]] = pv; return sp + 2; }   /* 0x00 <local> */
    const uint32_t k = sp[1];
    const uint8_t *cur = sp + 2;
    const KorbArray *const ar = KORB_ARRAY_P(pv) ? VAL2ARY(pv) : NULL;
    for (uint32_t j = 0; j < k; j++) {
        const VALUE sub = ar ? (j < ar->len ? korb_items_data(ar->items)[j] : KORB_NIL)
                             : (j == 0 ? pv : KORB_NIL);   /* non-Array → first leaf, rest nil */
        cur = korb_bind_destr_entry(c, bf, nlocals, loc, cur, sub);
    }
    return cur;
}
static uint32_t korb_entry_post_cnt(NODE *entry)  { return entry->u.node_entry.post_cnt; }    /* trailing required params */
static struct Node **korb_entry_opt_defaults(NODE *entry) { return (struct Node **)entry->u.node_entry.opt_defaults; }
static uint32_t korb_entry_req_cnt(NODE *entry) { return entry->u.node_entry.req_cnt; }
static const struct korb_kw_info *korb_entry_kw_info(NODE *entry) { return (const struct korb_kw_info *)entry->u.node_entry.kw_info; }
static int32_t korb_entry_blk_param_slot(NODE *entry) { return entry->u.node_entry.blk_param_slot; }   /* -1 = no `&blk` param */
static const uint8_t *korb_entry_destructure_spec(NODE *entry) { return (const uint8_t *)entry->u.node_entry.destructure_spec; }
NODE    *korb_entry_body(NODE *entry)       { return entry->u.node_entry.body; }

/* bp_blk (NULL = none) is a block FORWARDED to this block via `|&b|` — proc.call
 * passes the block given at its call site so the body's &b local binds to it as a
 * Proc.  Only the (non-hot) full path takes it; the simple wrapper passes NULL,
 * and a |&b| block yielded without a forwarded block just gets &b = nil from the
 * normal locals zeroing, so the hot path is untouched. */
static __attribute__((no_stack_protector)) RESULT
korb_block_yield_full(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                      const VALUE *argv, uint32_t argc, VALUE *captured_self,
                      NODE *bp_blk, VALUE *bp_denv, VALUE *bp_self, uint32_t is_lam);

/* Block invocation fast path: the overwhelmingly common block has only scalar
 * required params (no kw / destructure / rest / opt).  Binding it inline here —
 * a small function with a small frame — avoids korb_block_yield_full's ~150-byte
 * worst-case frame (sized for the rest-array / opt-default / kw-scan locals) on
 * every `each`/`map`/`times` yield.  Anything non-simple (or a CPROC / non-entry
 * block) delegates verbatim to the full path; the frame layout, dispatch, and
 * escape handling are identical to it. */
/* Zero the block's local slots beyond its params (Ruby: block-locals start nil).
 * The count is 0-few for almost every block, so inline the small cases — letting
 * the compiler emit a memset PLT call to zero ~1 slot per yield is pure waste
 * (it showed as ~5% of a block-heavy profile). */
static inline __attribute__((always_inline)) void
korb_block_nil_locals(VALUE *restrict dst, uint32_t n) {
    switch (n) {
        case 0: return;
        case 4: dst[3] = 0; /* fallthrough */
        case 3: dst[2] = 0; /* fallthrough */
        case 2: dst[1] = 0; /* fallthrough */
        case 1: dst[0] = 0; return;
        default: memset(dst, 0, (size_t)n * sizeof(VALUE)); return;
    }
}
__attribute__((no_stack_protector)) RESULT
korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                 const VALUE *argv, uint32_t argc, VALUE *captured_self)
{
    if (UNLIKELY(block == NULL || block == KORB_BLK_CPROC || block->head.kind != &kind_node_entry ||
                 korb_entry_kw_info(block) || korb_entry_destructure_spec(block) ||
                 korb_entry_destructure_n(block) || korb_entry_rest_slot(block) != -1 ||
                 korb_entry_opt_defaults(block) ||
                 /* a lone non-Array arg for a multi-param block may need #to_ary */
                 (argc == 1 && !KORB_ARRAY_P(argv[0]) && KORB_OBJECT_P(argv[0]) &&
                  block->u.node_entry.params_cnt > 1)))   /* NULL (no block) → _full raises; keeps the cold epilogue out of the hot path */
        return korb_block_yield_full(c, slots, block, def_env, argv, argc, captured_self, NULL, NULL, NULL, 0);   /* is_lam via fwd-detection inside */

    const bool fwd = (def_env == KORB_BLK_FWD);
    /* A lambda forwarded as a block (`m(&lam); yield`) enforces arity and never
     * auto-splats — unlike a plain block/proc, which is lenient.  (Only the FWD
     * path can carry a lambda; its proc is reachable via captured_self.) */
    const bool is_lambda = fwd && VAL2PROC(*captured_self)->is_lambda;
    const VALUE prev = fwd ? VAL2PROC(*captured_self)->env : (VALUE)(uintptr_t)def_env;
    const uint32_t blocals = korb_entry_locals_cnt(block);   /* incl. self cell */
    VALUE *const bf = slots + 2;                             /* block frame base (bottom header) */
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit))
        return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    const uint32_t np = block->u.node_entry.params_cnt;   /* fast path: block is a node_entry (CPROC/non-entry went to _full above) */
    if (UNLIKELY(is_lambda && argc != np))                /* lambda: exact arity (fast path = all-required, no rest/opt) */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %u)", argc, np);
    bf[-2] = 0;          /* B[-3] magic (zeroed for GC scan)   */
    bf[-1] = prev;       /* B[-2] EP / PREV link               */
    bf[0]  = 0;          /* B[-1] block lexical self (set below) */
    korb_frame_magic_set(bf + 1, KORB_FT_BLOCK);
    if (LIKELY(is_lambda || !(np > 1 && argc == 1 && KORB_ARRAY_P(argv[0])))) {   /* scalar bind (lambda never auto-splats) */
        for (uint32_t i = 0; i < np; i++) bf[1 + i] = (i < argc) ? argv[i] : KORB_NIL;
        if (blocals > np) korb_block_nil_locals(&bf[1 + np], blocals - np);
    } else {                                                          /* auto-splat one Array */
        const KorbArray *ar = VAL2ARY(argv[0]);
        for (uint32_t i = 0; i < np; i++) bf[1 + i] = i < ar->len ? korb_items_data(ar->items)[i] : KORB_NIL;
        if (blocals > np) korb_block_nil_locals(&bf[1 + np], blocals - np);
    }
    bf[0] = fwd ? VAL2PROC(*captured_self)->self : *captured_self;   /* lexical self → B[-1] */

    RESULT r;
    do { r = (*block->head.dispatcher)(c, block, bf + 1 + blocals); }   /* `redo`: same bindings, run again */
    while (UNLIKELY(r.state == KORB_REDO));
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;
    else r = korb_break_claim(c, r, block, is_lambda);   /* a break raised in this body belongs to whoever was handed this block */
    korb_frame_magic_check(bf + 1, KORB_FT_BLOCK, "korb_block_yield");
    if (UNLIKELY(korb_frame_escaped(bf + 1)))
        r = korb_close_ret(c, bf + 1 + blocals, bf + 1, r);
    return r;
}

/* Core block invocation: lay out the block frame at cursor `slots` and
 * dispatch the entry.  Args come from `argv` (argv[i] copied into block
 * params; extra dropped, missing → nil — CRuby semantics).  argv may alias the
 * cursor region (node_yield passes &slots[-argc]); copies happen before any
 * GC, so raw VALUEs in argv are safe.  A stack-overflow check returns RAISE. */
static __attribute__((no_stack_protector)) RESULT
korb_block_yield_full(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                 const VALUE *argv, uint32_t argc, VALUE *captured_self,
                 NODE *bp_blk, VALUE *bp_denv, VALUE *bp_self, uint32_t is_lam)
{
    if (UNLIKELY(block == NULL))                         /* builtin / yield with no block passed (folded here from the fast path) */
        return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    /* A block whose params we couldn't compile (e.g. `|&b|`) is a node_unsupported
     * placeholder, not a node_entry — running it raises NotImplementedError instead
     * of dereferencing node_entry fields off the wrong union member (→ SEGV). */
    if (UNLIKELY(block == KORB_BLK_CPROC))               /* forwarded Symbol/Method#to_proc: no frame, dispatch a send */
        return korb_cproc_yield(c, slots, *captured_self, argv, argc);
    if (UNLIKELY(block->head.kind != &kind_node_entry)) return EVAL(c, block, slots);
    /* &block forward: re-read prev (proc->env) from the rooted Proc slot each
     * call so a GC-moved escaped env is never stale. */
    const bool fwd = (def_env == KORB_BLK_FWD);
    const VALUE prev = fwd ? VAL2PROC(*captured_self)->env : (VALUE)(uintptr_t)def_env;
    const uint32_t blocals = korb_entry_locals_cnt(block);   /* incl. self cell */
    /* block frame (bottom header): locals base B = bf+1, with B[-2]=EP (PREV:
     * tagged-odd slots handle, or even KorbEnv* for an escaped Proc) and
     * B[-3]=magic.  bf is shifted +2 above the caller's cursor so the two meta
     * cells land in fresh scratch (slots[0]=magic, slots[1]=EP) — no caller
     * reservation needed.  Block locals at bf[1..1+blocals), self cell at bf[blocals]. */
    VALUE *const bf = slots + 2;
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    }
    bf[-2] = 0;          /* B[-3] (magic; zeroed for GC scan, overwritten by magic_set in debug) */
    bf[-1] = prev;       /* B[-2] (EP / PREV link)     */
    bf[0]  = 0;          /* B[-1] (block lexical self, set just before dispatch) */
    korb_frame_magic_set(bf + 1, KORB_FT_BLOCK);   /* B[-3] integrity marker (no-op unless KORB_FRAME_MAGIC) */
    /* keyword params: a trailing Hash is consumed as kwargs (like methods), so
     * the positional binding below sees only the positional args. */
    const struct korb_kw_info *const kw = korb_entry_kw_info(block);
    const uint32_t orig_argc = argc;
    /* `**nil` forbids keyword syntax outright; a plain positional Hash is fine. */
    if (UNLIKELY(kw != NULL && kw->kwrest_slot == -3 && argc >= 1 && korb_kwargs_hash_p(argv[argc - 1])))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no keywords accepted");
    const bool has_kw_hash = (kw && kw->kwrest_slot != -3 && argc >= 1 && korb_kwargs_hash_p(argv[argc - 1]));
    if (has_kw_hash) argc--;   /* positional binding below sees only positionals */
    /* A lambda forwarded as a block enforces its positional arity (unlike a plain
     * block/proc) and never auto-splats a single Array.  The proc is reachable via
     * captured_self on the FWD path (the only path that can carry a lambda). */
    const bool is_lambda = is_lam || (fwd && VAL2PROC(*captured_self)->is_lambda);   /* explicit (proc.call) or forwarded &lam */
    if (UNLIKELY(is_lambda)) {
        const uint32_t pc = korb_entry_params_cnt(block);
        const bool has_rest = korb_entry_rest_slot(block) != -1;   /* named (>=0) or discard (-2) */
        const uint32_t rs = korb_entry_rest_slot(block) >= 0 ? (uint32_t)korb_entry_rest_slot(block) : pc;
        const uint32_t npost = has_rest ? ((pc > rs + 1) ? (pc - rs - 1) : 0)
                                        : korb_entry_post_cnt(block);   /* |a, b=1, c| — posts are required too */
        const uint32_t lo = korb_entry_req_cnt(block) + npost;   /* required front + required post */
        if (UNLIKELY(argc < lo || (!has_rest && argc > pc))) {
            if (has_rest)   return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %u+)", argc, lo);
            if (lo == pc)   return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %u)", argc, pc);
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %u..%u)", argc, lo, pc);
        }
    }
    /* A block that takes more than one positional param auto-splats a single
     * yielded value: CRuby converts a non-Array through #to_ary first (nil keeps
     * the object as-is; a non-Array return is a TypeError).  Done once here, so
     * every binding branch below sees a real Array. */
    VALUE splat_conv = KORB_UNDEF;
    if (!is_lambda && argc == 1 && !KORB_ARRAY_P(argv[0])) {
        const uint32_t np0 = korb_entry_params_cnt(block);
        const bool wants_many = (np0 > 1) || korb_entry_destructure_n(block) > 0 ||
                                (korb_entry_destructure_spec(block) != NULL) ||
                                (np0 == 1 && korb_entry_rest_slot(block) == -2);
        if (wants_many && KORB_OBJECT_P(argv[0])) {
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            const VALUE recv = argv[0];
            if (korb_responds_to(c, recv, to_ary)) {
                slots[0] = recv;
                RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                if (ar.value != KORB_NIL) {
                    if (UNLIKELY(!KORB_ARRAY_P(ar.value)))
                        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#to_ary gives %s)",
                                          korb_coerce_name(c, recv), korb_coerce_name(c, recv), korb_type_name(ar.value));
                    splat_conv = ar.value;
                }
            }
        }
    }
    VALUE conv_buf[1];
    if (splat_conv != KORB_UNDEF) { conv_buf[0] = splat_conv; argv = conv_buf; }
    const uint8_t *spec = korb_entry_destructure_spec(block);
    uint32_t dn = korb_entry_destructure_n(block);
    if (spec != NULL && korb_entry_rest_slot(block) < 0) {   /* mixed scalar + destructuring params, e.g. |a, (k, v)| or |(a, (b, c))| */
        const uint32_t np = korb_entry_params_cnt(block);   /* logical param count */
        const VALUE *src = argv; uint32_t srcn = argc;
        if (!is_lambda && np > 1 && argc == 1 && KORB_ARRAY_P(argv[0])) {  /* auto-splat one yielded Array across params */
            const KorbArray *ar = VAL2ARY(argv[0]); src = korb_items_data(ar->items); srcn = ar->len;
        }
        for (uint32_t i = 0; i < blocals; i++) bf[1 + i] = KORB_NIL;   /* leaves write by local index */
        uint32_t loc = 0;
        const uint8_t *sp = spec;
        for (uint32_t p = 0; p < np; p++)
            sp = korb_bind_destr_entry(c, bf, blocals, &loc, sp, (p < srcn) ? src[p] : KORB_NIL);
    } else if (dn > 0) {                                /* |(a, b, ...)| — splat the array arg */
        VALUE arr = (argc >= 1) ? argv[0] : KORB_NIL;
        if (KORB_ARRAY_P(arr)) {
            const KorbArray *ar = VAL2ARY(arr);
            for (uint32_t i = 0; i < dn; i++) bf[1 + i] = i < ar->len ? korb_items_data(ar->items)[i] : KORB_NIL;
        } else {
            bf[1] = arr;                               /* non-array → first target, rest nil */
            for (uint32_t i = 1; i < dn; i++) bf[1 + i] = KORB_NIL;
        }
        for (uint32_t i = dn; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (korb_entry_rest_slot(block) >= 0) {     /* |front..., *rest[, post...]| (front may include optionals) */
        const uint32_t np = korb_entry_params_cnt(block);
        const uint32_t rs = (uint32_t)korb_entry_rest_slot(block);
        const uint32_t reqc = korb_entry_req_cnt(block);
        struct Node **const opts = korb_entry_opt_defaults(block);          /* front optionals' defaults (NULL if none) */
        const uint32_t npost = korb_entry_post_cnt(block) ? korb_entry_post_cnt(block)
                             : ((np > rs + 1) ? (np - rs - 1) : 0);
        /* logical params before *rest: the spec carries it in its header byte
         * (locals != logical params once a param destructures) */
        const uint32_t nfront = spec ? spec[0] : ((np > npost + 1) ? np - npost - 1 : 0);
        const bool splat = (!is_lambda && np > 1 && argc == 1 && KORB_ARRAY_P(argv[0]));   /* auto-splat one Array (lambda: never) */
        const uint32_t srcn = splat ? VAL2ARY(argv[0])->len : argc;
        /* Copy the source args into block-frame scratch FIRST (rooted): argv may
         * point at non-scanned C-locals (builtin yielders pass &elem) that the
         * rest-array alloc's GC would leave stale.  No alloc during this copy. */
        VALUE *const stage = bf + 1 + blocals;
        for (uint32_t i = 0; i < srcn; i++)
            stage[i] = splat ? korb_items_data(VAL2ARY(argv[0])->items)[i] : argv[i];
        if (spec) for (uint32_t i = 0; i < blocals; i++) bf[1 + i] = KORB_NIL;   /* leaves write by local index */
        const uint32_t surplus = (srcn > nfront + npost) ? (srcn - nfront - npost) : 0;
        VALUE *const rcur = stage + srcn;                                    /* alloc above the staged source */
        rcur[0] = UNWRAP(korb_ary_new(c, rcur, surplus ? surplus : 4));
        VALUE_REF rarr = VALUE_REF_AT(&rcur[0]);
        for (uint32_t i = 0; i < surplus; i++)
            CHECK(korb_ary_push_val(c, rcur + 1, rarr, stage[nfront + i]));  /* stage rooted below rcur */
        /* front (req + opt): provided arg, else an optional's default (evaluated
         * after the rest array so rcur+1 scratch is free; rest array stays rooted
         * at rcur[0]), else nil for a missing required (block-lenient). */
        const uint8_t *fsp = spec ? spec + 1 : NULL;                         /* skip the header byte */
        uint32_t sploc = 0;
        for (uint32_t i = 0; i < nfront; i++) {
            if (fsp) {                                                       /* |(a,b), *rest| etc. */
                fsp = korb_bind_destr_entry(c, bf, blocals, &sploc, fsp, (i < srcn) ? stage[i] : KORB_NIL);
                continue;
            }
            if (i < reqc) bf[1 + i] = (i < srcn) ? stage[i] : KORB_NIL;      /* required: first args */
            else if ((int32_t)i < (int32_t)srcn - (int32_t)npost)           /* optional: only args left after reserving posts */
                bf[1 + i] = stage[i];
            else if (opts != NULL) {                                        /* optional → default */
                RESULT dr = EVAL(c, opts[i - reqc], rcur + 1);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                bf[1 + i] = dr.value;
            } else bf[1 + i] = KORB_NIL;
        }
        if (fsp) {                          /* the rest param's own entry is 0x00 <local> */
            bf[1 + fsp[1]] = VALUE_REF_GET(rarr);
            fsp += 2; sploc++;
        } else {
            bf[1 + rs] = VALUE_REF_GET(rarr);
        }
        for (uint32_t j = 0; j < npost; j++) {                               /* trailing post params → the last npost args */
            const int32_t pidx = (int32_t)srcn - (int32_t)npost + (int32_t)j;
            const VALUE pv = (pidx >= 0 && pidx < (int32_t)srcn) ? stage[pidx] : KORB_NIL;
            if (fsp) fsp = korb_bind_destr_entry(c, bf, blocals, &sploc, fsp, pv);
            else     bf[1 + rs + 1 + j] = pv;
        }
        if (!spec) for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (korb_entry_opt_defaults(block) != NULL || korb_entry_post_cnt(block) > 0) {   /* |req..., opt=default..., post...| (no rest) */
        const uint32_t np = korb_entry_params_cnt(block);
        const uint32_t reqc = korb_entry_req_cnt(block);
        const uint32_t npost = korb_entry_post_cnt(block);
        const uint32_t nfront = (np > npost) ? np - npost : 0;   /* req + opt (posts bind from the end) */
        struct Node **const opts = korb_entry_opt_defaults(block);
        const bool splat = (!is_lambda && np > 1 && argc == 1 && KORB_ARRAY_P(argv[0]));
        const uint32_t srcn = splat ? VAL2ARY(argv[0])->len : argc;
        const VALUE *const src = splat ? korb_items_data(VAL2ARY(argv[0])->items) : argv;
        /* CRuby order: required front, then as many optionals as the surplus
         * allows, then the posts — args are consumed strictly left to right and
         * any extra beyond that is dropped. */
        const uint32_t nopt = (nfront > reqc) ? nfront - reqc : 0;
        uint32_t nopt_fill = 0;
        if (srcn > reqc + npost) {
            nopt_fill = srcn - reqc - npost;
            if (nopt_fill > nopt) nopt_fill = nopt;
        }
        uint32_t take = 0;
        for (uint32_t i = 0; i < nfront; i++) {
            if (i < reqc || i - reqc < nopt_fill) bf[1 + i] = (take < srcn) ? src[take++] : KORB_NIL;
            else if (i >= reqc && opts) {                   /* optional → eval default in block scope */
                RESULT dr = EVAL(c, opts[i - reqc], bf + 1 + blocals);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                bf[1 + i] = dr.value;
            } else bf[1 + i] = KORB_NIL;                    /* missing required → nil (block-lenient) */
        }
        for (uint32_t j = 0; j < npost; j++)                /* posts follow the consumed front */
            bf[1 + nfront + j] = (take < srcn) ? src[take++] : KORB_NIL;
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (korb_entry_rest_slot(block) == -2) {     /* |s,| / |s,*| — discard extras, splat like np+1 params */
        const uint32_t np = korb_entry_params_cnt(block);
        if (!is_lambda && np + 1 > 1 && argc == 1 && KORB_ARRAY_P(argv[0])) {
            const KorbArray *ar = VAL2ARY(argv[0]);
            for (uint32_t i = 0; i < np; i++) bf[1 + i] = i < ar->len ? korb_items_data(ar->items)[i] : KORB_NIL;
        } else {
            for (uint32_t i = 0; i < np; i++)  bf[1 + i] = (i < argc) ? argv[i] : KORB_NIL;
        }
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else {
        const uint32_t np = korb_entry_params_cnt(block);   /* np <= blocals - 1 */
        if (!is_lambda && np > 1 && argc == 1 && KORB_ARRAY_P(argv[0])) {  /* auto-splat: |a,b| yielded one Array */
            const KorbArray *ar = VAL2ARY(argv[0]);
            for (uint32_t i = 0; i < np; i++) bf[1 + i] = i < ar->len ? korb_items_data(ar->items)[i] : KORB_NIL;
        } else {
            for (uint32_t i = 0; i < np; i++)  bf[1 + i] = (i < argc) ? argv[i] : KORB_NIL;
        }
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    }
    if (kw) {                                           /* bind keyword params from the kwargs hash */
        /* re-read the kwargs hash from argv (scanned for yield/proc.call) — a
         * positional rest/opt alloc above may have moved it. */
        const VALUE kwhash = has_kw_hash ? argv[orig_argc - 1] : KORB_NIL;
        VALUE *const kcur = bf + 1 + blocals;           /* default-eval scratch (block scope) */
        uint64_t present = 0;
        for (uint32_t j = 0; j < kw->count; j++) {      /* pass 1: provided keywords (no alloc) */
            int32_t idx = (kwhash != KORB_NIL) ? korb_hash_find(VAL2HASH(kwhash), ID2SYM(kw->entries[j].mid)) : -1;
            if (idx >= 0) { bf[1 + kw->entries[j].slot] = korb_items_data(VAL2HASH(kwhash)->items)[2 * idx + 1]; if (j < 64) present |= (1ull << j); }
        }
        for (uint32_t j = 0; j < kw->count; j++) {      /* pass 2: defaults / required check */
            if (j < 64 && (present & (1ull << j))) continue;
            if (kw->entries[j].deflt) {
                RESULT dr = (*kw->entries[j].deflt->head.dispatcher)(c, kw->entries[j].deflt, kcur);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                bf[1 + kw->entries[j].slot] = dr.value;
            } else {
                return korb_raise_missing_kw(c, slots, 0, kw, present);
            }
        }
        if (kw->kwrest_slot >= 0) {                      /* collect undeclared keys into **rest (like korb_invoke_method) */
            VALUE *cur = bf + 1 + blocals;              /* scratch above the block frame */
            bf[1 + kw->kwrest_slot] = kwhash;          /* park (scanned) for re-read across the alloc below */
            cur[0] = UNWRAP(korb_hash_new(c, cur, 4));
            VALUE_REF kr = VALUE_REF_AT(&cur[0]);
            const VALUE kh = bf[1 + kw->kwrest_slot];
            if (kh != KORB_NIL) {
                const uint32_t hn = VAL2HASH(kh)->len;
                for (uint32_t i = 0; i < hn; i++) {
                    const VALUE key = korb_items_data(VAL2HASH(bf[1 + kw->kwrest_slot])->items)[2 * i];
                    bool declared = false;
                    for (uint32_t j = 0; j < kw->count; j++) if (key == ID2SYM(kw->entries[j].mid)) { declared = true; break; }
                    if (declared) continue;
                    cur[1] = key;
                    const VALUE val = korb_items_data(VAL2HASH(bf[1 + kw->kwrest_slot])->items)[2 * i + 1];
                    CHECK(korb_hash_set(c, cur + 2, kr, VALUE_REF_AT(&cur[1]), val));
                }
            }
            bf[1 + kw->kwrest_slot] = VALUE_REF_GET(kr);
        }
    }
    /* `|&b|`: materialize a forwarded block into its local as a Proc (rare; only
     * proc.call passes bp_blk).  Done after positional binding, before dispatch —
     * korb_make_proc may GC but bf locals are rooted and the new Proc lands in a
     * rooted slot.  No forwarded block → &b stays nil (from the locals zeroing). */
    if (UNLIKELY(bp_blk != NULL)) {
        const int32_t bps = korb_entry_blk_param_slot(block);
        if (bps >= 0) {
            /* a FORWARDED Proc keeps its identity (`->(&b){b}.call(&pr)` is pr),
             * so only a literal block is wrapped into a fresh Proc */
            if (bp_denv == KORB_BLK_FWD && bp_self != NULL && KORB_PROC_P(*bp_self))
                bf[1 + bps] = *bp_self;
            else
                bf[1 + bps] = UNWRAP(korb_make_proc(c, bf + 1 + blocals, bp_blk, bp_denv, *bp_self, 0));
        }
    }
    bf[0] = fwd ? VAL2PROC(*captured_self)->self : *captured_self;   /* block's lexical self → B[-1] (bottom header; re-read fresh) */

    RESULT r;
    do { r = (*block->head.dispatcher)(c, block, bf + 1 + blocals); }   /* `redo`: same bindings, run again */
    while (UNLIKELY(r.state == KORB_REDO));
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;   /* `next [v]` = block value */
    else r = korb_break_claim(c, r, block, is_lambda);   /* a break here belongs to whoever was handed this block */
    korb_frame_magic_check(bf + 1, KORB_FT_BLOCK, "korb_block_yield");   /* frame integrity (no-op unless KORB_FRAME_MAGIC) */
    if (UNLIKELY(korb_frame_escaped(bf + 1)))          /* block locals base = bf+1; close its own env if escaped */
        r = korb_close_ret(c, bf + 1 + blocals, bf + 1, r);
    return r;
}

RESULT
korb_yield(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
           VALUE block_cell, VALUE def_env_cell, VALUE *captured_self)
{
    /* Frame cells are odd-tagged when a block is present; nil (0) = none. */
    if (UNLIKELY(((uintptr_t)block_cell & 1u) == 0)) {
        return korb_raise(c, slots, KORB_E_LOCALJUMP, line, "no block given (yield)");
    }
    NODE  *entry   = (NODE  *)(uintptr_t)((uintptr_t)block_cell & ~(uintptr_t)1u);
    /* def_env_cell holds the raw PREV (odd slots / clean KorbEnv) — pass through. */
    return korb_block_yield(c, slots, entry, (VALUE *)(uintptr_t)def_env_cell, slots - argc, argc, captured_self);
}

/* `yield` from INSIDE a block: the block frame carries no method block trio, so
 * walk `depth` env links (like node_eget) to the enclosing method frame and read
 * its trio (block_entry/def_env/captured_self at node[trio_base..trio_base+2]),
 * then yield.  Only the live slots-handle chain is reachable — an escaped block
 * whose method has already returned (KorbEnv chain; trio not captured) raises
 * LocalJumpError, matching Ruby.  args staged at slots[-argc..] as for korb_yield. */
RESULT
korb_yield_outer(CTX *c, VALUE *slots, uint32_t argc, uint32_t line,
                 VALUE prev_handle, uint32_t depth, int32_t trio_base)
{
    if (UNLIKELY((prev_handle & 1u) == 0))   /* escaped KorbEnv: the method frame is gone */
        return korb_raise(c, slots, KORB_E_LOCALJUMP, line, "no block given (yield)");
    VALUE *node = (VALUE *)(uintptr_t)(prev_handle & ~(uintptr_t)1u);
    for (uint32_t k = 1; k < depth; k++) {   /* walk like node_eget */
        const VALUE h = korb_ep_get(node);
        if (UNLIKELY((h & 1u) == 0))
            return korb_raise(c, slots, KORB_E_LOCALJUMP, line, "no block given (yield)");
        node = (VALUE *)(uintptr_t)(h & ~(uintptr_t)1u);
    }
    return korb_yield(c, slots, argc, line, node[trio_base], node[trio_base + 1], &node[trio_base + 2]);
}

/* Walk `depth` env links (like node_eget / korb_yield_outer) to the enclosing
 * method frame's base — the target a block's `return` must unwind to.  Returns
 * NULL for an escaped (KorbEnv) chain (home frame gone), in which case the
 * return falls back to nearest-method semantics. */
VALUE *
korb_outer_frame_base(VALUE prev_handle, uint32_t depth)
{
    if ((prev_handle & 1u) == 0) return NULL;
    VALUE *node = (VALUE *)(uintptr_t)(prev_handle & ~(uintptr_t)1u);
    for (uint32_t k = 1; k < depth; k++) {
        const VALUE h = korb_ep_get(node);
        if ((h & 1u) == 0) return NULL;
        node = (VALUE *)(uintptr_t)(h & ~(uintptr_t)1u);
    }
    return node;
}

/* ---------------------------------------------------------------------------
 * Receiver method dispatch (x.foo) — built-in methods on core types.
 * ------------------------------------------------------------------------- */

enum korb_class
korb_class_of(VALUE v)
{
    if (FIXNUM_P(v))     return KORB_C_INTEGER;
    if (v == KORB_NIL)   return KORB_C_NIL;
    if (v == KORB_TRUE)  return KORB_C_TRUE;
    if (v == KORB_FALSE) return KORB_C_FALSE;
    if (SYMBOL_P(v))     return KORB_C_SYMBOL;
    if (FLONUM_P(v))     return KORB_C_FLOAT;
    if (AROH_IS_GC_OBJECT(v)) {
        switch (KORB_OBJ_TYPE(v)) {
          case KORB_OBJ_STRING: return KORB_C_STRING;
          case KORB_OBJ_ARRAY:  return KORB_C_ARRAY;
          case KORB_OBJ_HASH:   return KORB_C_HASH;
          case KORB_OBJ_RANGE:  return KORB_C_RANGE;
          case KORB_OBJ_CLASS:  return KORB_C_CLASS;
          case KORB_OBJ_EXCEPTION: return KORB_C_EXCEPTION;
          case KORB_OBJ_FLOAT:  return KORB_C_FLOAT;
          case KORB_OBJ_RATIONAL: return KORB_C_RATIONAL;
          case KORB_OBJ_COMPLEX:  return KORB_C_COMPLEX;
          case KORB_OBJ_ENUMERATOR: return KORB_C_ENUMERATOR;
          case KORB_OBJ_ARITHSEQ: return KORB_C_ARITHSEQ;
          case KORB_OBJ_BIGNUM: return KORB_C_INTEGER;
          case KORB_OBJ_SET: return KORB_C_SET;
          case KORB_OBJ_REGEXP: return KORB_C_REGEXP;
          case KORB_OBJ_METHOD:
            return VAL2METH(v)->unbound ? KORB_C_UNBOUND_METHOD : KORB_C_METHOD;
          case KORB_OBJ_FIBER:  return KORB_C_FIBER;
          case KORB_OBJ_THREAD: return KORB_C_THREAD;
          case KORB_OBJ_MUTEX:  return KORB_C_MUTEX;
          case KORB_OBJ_CONDVAR: return KORB_C_CONDVAR;
          case KORB_OBJ_PROC:   return KORB_C_PROC;
          case KORB_OBJ_MATCHDATA: return KORB_C_MATCHDATA;
          case KORB_OBJ_BINDING:   return KORB_C_BINDING;
        }
    }
    return KORB_C_OBJECT;
}

const char *
korb_class_name(enum korb_class cls)
{
    switch (cls) {
      case KORB_C_INTEGER: return "Integer";
      case KORB_C_STRING:  return "String";
      case KORB_C_SYMBOL:  return "Symbol";
      case KORB_C_ARRAY:   return "Array";
      case KORB_C_HASH:    return "Hash";
      case KORB_C_RANGE:   return "Range";
      case KORB_C_CLASS:   return "Class";
      case KORB_C_FLOAT:   return "Float";
      case KORB_C_RATIONAL: return "Rational";
      case KORB_C_COMPLEX:  return "Complex";
      case KORB_C_ENUMERATOR: return "Enumerator";
      case KORB_C_SET: return "Set";
      case KORB_C_REGEXP: return "Regexp";
      case KORB_C_METHOD: return "Method";
      case KORB_C_UNBOUND_METHOD: return "UnboundMethod";
      case KORB_C_FIBER:  return "Fiber";
      case KORB_C_THREAD: return "Thread";
      case KORB_C_MUTEX:  return "Mutex";
      case KORB_C_CONDVAR: return "ConditionVariable";
      case KORB_C_PROC:   return "Proc";
      case KORB_C_ARITHSEQ: return "Enumerator::ArithmeticSequence";
      case KORB_C_MATCHDATA: return "MatchData";
      case KORB_C_BINDING:   return "Binding";
      case KORB_C_NIL:     return "NilClass";
      case KORB_C_TRUE:    return "TrueClass";
      case KORB_C_FALSE:   return "FalseClass";
      default:             return "Object";
    }
}

/* Register a builtin (C) method as an ordinary KORB_METHOD_CFUNC entry on the
 * tag's class object, so it lives in the same MRO that user reopens / subclasses
 * extend — one dispatch path, drop-in reopen semantics.  Run at init, before any
 * user code or dispatch-cache fill. */
/* Get a fresh CFUNC slot for `name` on tag `cls`'s class object, or NULL if a
 * method of that name is already registered.  First registration wins — the old
 * flat cmethod table was scanned front-to-back, so a duplicate def was shadowed
 * by the earlier one; keep that exact semantics (a few intentional shadow pairs
 * rely on it). */
static struct korb_method *
korb_cmethod_slot(struct korb_vm *vm, enum korb_class cls, const char *name)
{
    const uint32_t mid = korb_intern(vm, name, strlen(name));
    const VALUE clsobj = korb_builtin_class_obj(vm, cls);
    KorbClass *const k = VAL2CLASS(clsobj);
    for (uint32_t i = 0; i < k->method_cnt; i++)
        if (k->methods[i]->mid == mid) return NULL;   /* earlier registration wins */
    struct korb_method *const m = korb_class_method_slot(k, mid);
    m->owner = clsobj;
    return m;
}

void
korb_def_cmethod(CTX *c, enum korb_class cls, const char *name,
                 korb_method_fn fn, int32_t arity)
{
    struct korb_method *const m = korb_cmethod_slot(c->vm, cls, name);
    if (!m) return;
    m->kind = KORB_METHOD_CFUNC;
    m->uses_block = 0;                  /* takes_block */
    m->params_cnt = arity;
    m->rfn = fn;
    m->rbfn = NULL;
    m->body = NULL;
}

void
korb_def_cmethod_blk(CTX *c, enum korb_class cls, const char *name,
                     korb_method_blk_fn fn, int32_t arity)
{
    struct korb_method *const m = korb_cmethod_slot(c->vm, cls, name);
    if (!m) return;
    m->kind = KORB_METHOD_CFUNC;
    m->uses_block = 1;                  /* takes_block */
    m->params_cnt = arity;
    m->rfn = NULL;
    m->rbfn = fn;
    m->body = NULL;
}

/* Shared receiver dispatch.  `block`/`def_env` are NULL for a plain send;
 * non-NULL for a `{ ... }` form.  A block handed to a non-yielding method is
 * ignored (CRuby); a yielding method called without a block gets NULL. */
/* True when a Range subclass still uses the stock constructor: no #initialize
 * of its own anywhere below the builtin default (which lives on Object).  Such
 * a class gets the real Range payload from `new`; one that redefines
 * #initialize opts into the generic-object path instead. */
static bool korb_range_default_init_p(CTX *c, VALUE cls) {
    VALUE found_def = KORB_NIL;
    struct korb_method *const m = korb_class_find_method(cls, c->vm->mid_initialize, &found_def);
    /* the inherited default lives on BasicObject (korb_relocate_object_methods);
     * Object is still checked for subsystems registered after that pass. */
    return m == NULL || found_def == korb_builtin_class_obj(c->vm, KORB_C_OBJECT) ||
           found_def == korb_builtin_class_obj(c->vm, KORB_C_RANGE) ||   /* Range's own (allocate + send(:initialize)) */
           found_def == korb_const_get(c->vm, korb_intern(c->vm, "BasicObject", 11));
}

static RESULT
korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
               NODE *block, VALUE *def_env, VALUE *captured_self)
{
    struct korb_vm *const vm = c->vm;
    /* Internal C dispatch stages [recv, args] flush against the caller's cursor
     * (no @framehdr gap).  A user-method callee built off these args needs its
     * KORB_FRAME_HDR meta cells (EP at base[-2], magic at base[-3]) zeroed below
     * the receiver, so relocate the [recv,args] block up to a fresh region with
     * the gap.  recv lands at slots[2] (= new base[-1]); slots[0]/slots[1] become
     * the magic/EP cells.  Cheap (argc small) and only on this slow dispatch path
     * — the @framehdr fast path inlines korb_invoke_simple and never gets here. */
    memmove(slots + 2, slots - (korb_sword_t)argc - 1, ((size_t)argc + 1) * sizeof(VALUE));
    slots[0] = 0;                                       /* callee base[-3] (magic)   */
    slots[1] = 0;                                       /* callee base[-2] (EP)      */
    slots += (korb_sword_t)argc + 3;                        /* new cursor: recv at slots[-argc-1], gap below */
    VALUE *const recv_slot = &slots[-(korb_sword_t)argc - 1];
    VALUE self = *recv_slot;

    /* send / __send__ / public_send: redispatch by the symbol/string name in arg0.
     * Shift recv into arg0's slot so [recv | arg1..] forms an argc-1 call.
     * A class may define its own #send (UDPSocket#send is a real method, not
     * the reflective one), and that must win — the universal forms are not
     * registered in any method table, so any hit here is a real definition. */
    if (UNLIKELY(mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send) &&
        korb_class_find_method(korb_class_obj_of(c, self), mid, NULL) == NULL) {
        if (UNLIKELY(argc == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "no method name given");
        {
            VALUE name = slots[-(korb_sword_t)argc];           /* arg0 */
            uint32_t rmid;
            if (SYMBOL_P(name)) rmid = SYM2ID(name);
            else if (KORB_STRING_P(name)) rmid = korb_intern(vm, korb_strbuf_data(VAL2STR(name)->buf), VAL2STR(name)->len);
            else {                                          /* coerce a #to_str name (scratch above the args; args/self are GC-rooted below) */
                const uint32_t to_str = korb_intern(vm, "to_str", 6);
                if (UNLIKELY(!(KORB_OBJECT_P(name) && korb_responds_to_coerce_p(c, slots, &name, to_str))))
                    return korb_raise_not_sym(c, slots, slots[-(korb_sword_t)argc]);
                slots[0] = name;
                RESULT sr = korb_send_impl(c, slots + 1, to_str, line, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (UNLIKELY(!KORB_STRING_P(sr.value)))
                    return korb_raise_not_sym(c, slots, slots[-(korb_sword_t)argc]);
                slots[0] = sr.value;                        /* root the coerced String while interning */
                rmid = korb_intern(vm, korb_strbuf_data(VAL2STR(slots[0])->buf), VAL2STR(slots[0])->len);
                self = *recv_slot;                          /* re-read: the #to_str dispatch may have GC-moved self */
            }
            /* public_send cannot reach top-level defs: CRuby exposes them as
             * private Object methods (send/__send__ bypass privacy, public_send
             * does not). */
            if (mid == vm->mid_public_send &&
                KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL &&
                !korb_responds_to(c, self, rmid) && korb_method_lookup(vm, rmid) != NULL)
                return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                                  "private method '%s' called for main", korb_sym_name(vm, rmid));
            if (mid == vm->mid_public_send) {              /* public_send refuses private/protected methods */
                const VALUE dcls = korb_dispatch_class(c, self);
                VALUE mdef = KORB_NIL;
                const struct korb_method *const me = KORB_CLASS_P(dcls) ? korb_class_find_method(dcls, rmid, &mdef) : NULL;
                if (me != NULL && me->visibility != 0)
                    return korb_raise(c, slots, KORB_E_NOMETHOD, line, "%s method '%s' called for %s",
                                      me->visibility == 1 ? "private" : "protected", korb_sym_name(vm, rmid), korb_a_type_name(self));
            }
            slots[-(korb_sword_t)argc] = self;                 /* recv → arg0 slot; args shift down by one */
            return korb_send_impl(c, slots, rmid, line, argc - 1, block, def_env, captured_self);
        }
    }

    /* user instance → dispatch through its class chain (miss falls to Object). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_mcache_find(vm, korb_obj_dispatch_klass(vm, self), mid, &def_class);
        if (um) return korb_dispatch_method(c, slots, um, mid, line, argc, def_class, block, def_env, captured_self);
    }
    /* class receiver → Klass.new (allocate + initialize). */
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_FIBER) &&
             (mid == vm->mid_yield || mid == korb_intern(vm, "current", 7) ||
              mid == korb_intern(vm, "[]", 2) || mid == korb_intern(vm, "[]=", 3) ||
              mid == korb_intern(vm, "blocking?", 9))) {
        const VALUE_SLICE fa = VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc);
        if (mid == vm->mid_yield)
            return korb_m_fiber_yield(c, slots, VALUE_REF_AT(recv_slot), fa);
        if (mid == korb_intern(vm, "[]", 2))
            return korb_m_fiber_s_aref(c, slots, VALUE_REF_AT(recv_slot), fa);
        if (mid == korb_intern(vm, "[]=", 3))
            return korb_m_fiber_s_aset(c, slots, VALUE_REF_AT(recv_slot), fa);
        if (mid == korb_intern(vm, "blocking?", 9))
            return korb_m_fiber_s_blocking_p(c, slots, VALUE_REF_AT(recv_slot), fa);
        return korb_m_fiber_s_current(c, slots, VALUE_REF_AT(recv_slot), fa);
    }
    else if (KORB_CLASS_P(self) && VAL2CLASS(self)->is_module &&
             VAL2CLASS(self)->name_sym == korb_intern(vm, "Kernel", 6)) {     /* Kernel.Integer / .Float / .puts ... → the global function */
        struct korb_method *gm = korb_method_lookup(vm, mid);
        if (gm) {
            VALUE *const base = slots - argc;
            if (gm->kind == KORB_METHOD_BUILTIN) {                            /* global C builtin (Integer/Float/p/...) */
                if (UNLIKELY(gm->params_cnt >= 0 && (uint32_t)gm->params_cnt != argc))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected %d)", argc, gm->params_cnt);
                RESULT r = gm->bfn(c, slots, VALUE_SLICE_MAKE(base, argc));
                if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) { KorbException *e = VAL2EXC(r.value); korb_bt_append(vm, e->line, korb_sym_name(vm, mid)); e->line = line; }
                return r;
            }
            if (gm->kind == KORB_METHOD_ISEQ)                                 /* global ISEQ (top-level def) */
                return gm->is_simple ? korb_invoke_simple(c, slots, gm, argc, line, mid, self, KORB_NIL)
                                     : korb_invoke_method(c, slots, gm, argc, line, mid, self, KORB_NIL, block, def_env, KORB_CSELF_VAL(captured_self));
        }
        VALUE dc = KORB_NIL;                                                  /* else a Kernel-own or Module-level method */
        struct korb_method *m2 = korb_mcache_find(vm, self, mid, &dc);
        if (!m2) {                                                            /* Module-level (ancestors/name/method_added/...): the metaclass, like korb_send_cached */
            const VALUE meta = korb_dispatch_class(c, self);
            dc = KORB_NIL;
            if (KORB_CLASS_P(meta)) m2 = korb_class_find_method(meta, mid, &dc);
        }
        if (m2) return korb_dispatch_method(c, slots, m2, mid, line, argc, dc, block, def_env, captured_self);
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '%s' for Kernel", korb_sym_name(vm, mid));
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref &&
             self == korb_builtin_class_obj(vm, KORB_C_SET)) {       /* Set[a, b, ...] */
        VALUE *const base = &slots[-(korb_sword_t)argc];
        slots[0] = UNWRAP(korb_ary_new(c, slots, argc));
        VALUE_REF arr = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 1, arr, base[i]));
        return korb_set_from_array(c, slots + 1, arr);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref &&
             (self == korb_builtin_class_obj(vm, KORB_C_ARRAY) ||
              self == korb_builtin_class_obj(vm, KORB_C_HASH))) {
        VALUE *const base = &slots[-(korb_sword_t)argc];
        if (self == korb_builtin_class_obj(vm, KORB_C_ARRAY)) {   /* Array[a, b, ...] → [a, b, ...] */
            slots[0] = UNWRAP(korb_ary_new(c, slots, argc));
            VALUE_REF dst = VALUE_REF_AT(&slots[0]);
            for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, base[i]));
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        /* Hash[k,v,k,v,...] | Hash[[[k,v],...]] | Hash[{...}] */
        slots[0] = UNWRAP(korb_hash_new(c, slots, argc));
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        if (argc == 1 && !KORB_HASH_P(base[0]) && !KORB_ARRAY_P(base[0]) && KORB_OBJECT_P(base[0]) &&
            korb_responds_to_coerce(c, slots + 2, base[0], korb_intern(vm, "to_hash", 7))) {  /* Hash[obj] → obj.to_hash (tried before to_ary) */
            slots[1] = base[0];
            RESULT hr = korb_send_impl(c, slots + 2, korb_intern(vm, "to_hash", 7), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            if (KORB_HASH_P(hr.value)) base[0] = hr.value;
        }
        if (argc == 1 && !KORB_HASH_P(base[0]) && !KORB_ARRAY_P(base[0]) && KORB_OBJECT_P(base[0]) &&
            korb_responds_to_coerce(c, slots + 2, base[0], korb_intern(vm, "to_ary", 6))) {  /* Hash[obj] → obj.to_ary */
            slots[1] = base[0];
            RESULT ar = korb_send_impl(c, slots + 2, korb_intern(vm, "to_ary", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
            if (KORB_ARRAY_P(ar.value)) base[0] = ar.value;
        }
        if (argc == 1 && KORB_HASH_P(base[0])) {                          /* copy an existing Hash */
            const uint32_t n = VAL2HASH(base[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const KorbHash *src = VAL2HASH(base[0]);                   /* re-read: base[0] rooted, may move */
                slots[1] = korb_items_data(src->items)[2*i];                         /* key + val into rooted slots */
                slots[2] = korb_items_data(src->items)[2*i+1];
                CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[1]), slots[2]));
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (argc == 1 && KORB_ARRAY_P(base[0])) {                         /* array of [k,v] pairs */
            const uint32_t n = VAL2ARY(base[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const VALUE pr = korb_items_data(VAL2ARY(base[0])->items)[i];        /* re-read */
                if (UNLIKELY(!KORB_ARRAY_P(pr)))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong element type %s at %u (expected array)", korb_type_name(pr), i);
                if (UNLIKELY(VAL2ARY(pr)->len < 1 || VAL2ARY(pr)->len > 2))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "invalid number of elements (%u for 1..2)", VAL2ARY(pr)->len);
                slots[1] = korb_items_data(VAL2ARY(pr)->items)[0];                   /* key (rooted) */
                slots[2] = VAL2ARY(pr)->len >= 2 ? korb_items_data(VAL2ARY(pr)->items)[1] : KORB_NIL;
                CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[1]), slots[2]));
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (UNLIKELY(argc & 1u)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "odd number of arguments for Hash");
        for (uint32_t i = 0; i < argc; i += 2)
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&base[i]), base[i+1]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref && korb_builtin_base_class(vm, self) == KORB_C_ARRAY) {
        /* SubArray[a, b, ...] — Array's [] class-constructor on a subclass → a
         * subclass instance holding the elements (the exact-Array case is handled above). */
        slots[0] = self;                                   /* root the subclass across allocs */
        VALUE *const abase = &slots[-(korb_sword_t)argc];
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, argc));
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 2, dst, abase[i]));
        korb_klass_override_set(c, slots[1], slots[0]);
        return RESULT_OK(slots[1]);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref && korb_builtin_base_class(vm, self) == KORB_C_SET) {
        /* SubSet[a, b, ...] → a Set-subclass instance with the (deduped) elements. */
        slots[0] = self;                                   /* root the subclass */
        VALUE *const abase = &slots[-(korb_sword_t)argc];
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, argc));
        VALUE_REF arr = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 2, arr, abase[i]));
        RESULT sr = korb_set_from_array(c, slots + 2, arr);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[1] = sr.value;
        korb_klass_override_set(c, slots[1], slots[0]);
        return RESULT_OK(slots[1]);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref && korb_builtin_base_class(vm, self) == KORB_C_HASH) {
        /* SubHash[k,v,...] | SubHash[[[k,v],...]] | SubHash[{...}] → subclass instance. */
        slots[0] = self;                                   /* root the subclass */
        VALUE *const abase = &slots[-(korb_sword_t)argc];
        slots[1] = UNWRAP(korb_hash_new(c, slots + 1, argc));
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        if (argc == 1 && KORB_HASH_P(abase[0])) {          /* copy an existing Hash */
            const uint32_t n = VAL2HASH(abase[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const KorbHash *src = VAL2HASH(abase[0]);
                slots[2] = korb_items_data(src->items)[2*i]; slots[3] = korb_items_data(src->items)[2*i+1];
                CHECK(korb_hash_set(c, slots + 4, dst, VALUE_REF_AT(&slots[2]), slots[3]));
            }
        } else if (argc == 1 && KORB_ARRAY_P(abase[0])) {  /* array of [k,v] pairs */
            const uint32_t n = VAL2ARY(abase[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const VALUE pr = korb_items_data(VAL2ARY(abase[0])->items)[i];
                if (UNLIKELY(!KORB_ARRAY_P(pr))) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong element type %s at %u (expected array)", korb_type_name(pr), i);
                slots[2] = korb_items_data(VAL2ARY(pr)->items)[0];
                slots[3] = VAL2ARY(pr)->len >= 2 ? korb_items_data(VAL2ARY(pr)->items)[1] : KORB_NIL;
                CHECK(korb_hash_set(c, slots + 4, dst, VALUE_REF_AT(&slots[2]), slots[3]));
            }
        } else {
            if (UNLIKELY(argc & 1u)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "odd number of arguments for Hash");
            for (uint32_t i = 0; i < argc; i += 2) CHECK(korb_hash_set(c, slots + 2, dst, VALUE_REF_AT(&abase[i]), abase[i+1]));
        }
        korb_klass_override_set(c, slots[1], slots[0]);
        return RESULT_OK(slots[1]);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_ARRAY) &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* Array.try_convert(obj) */
        VALUE arg = argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL;
        if (KORB_ARRAY_P(arg)) return RESULT_OK(arg);
        const uint32_t to_ary = korb_intern(vm, "to_ary", 6);
        if (KORB_OBJECT_P(arg) && korb_responds_to_coerce_p(c, slots, &arg, to_ary)) {
            slots[0] = arg;
            RESULT r = korb_send_impl(c, slots + 1, to_ary, line, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_ARRAY_P(r.value)) return r;
            if (r.value == KORB_NIL) return RESULT_OK(KORB_NIL);
            return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to Array (%s#to_ary gives %s)", korb_type_name(slots[0]), korb_type_name(slots[0]), korb_type_name(r.value));
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_INTEGER) &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* Integer.try_convert(obj) → obj/to_int/nil */
        VALUE arg = argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL;
        if (KORB_INTEGER_P(arg)) return RESULT_OK(arg);
        const uint32_t to_int = korb_intern(vm, "to_int", 6);
        if (korb_responds_to_coerce_p(c, slots, &arg, to_int)) {
            slots[0] = arg;
            RESULT r = korb_send_impl(c, slots + 1, to_int, line, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_INTEGER_P(r.value)) return r;
            if (r.value == KORB_NIL) return RESULT_OK(KORB_NIL);
            return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to Integer (%s#to_int gives %s)", korb_type_name(slots[0]), korb_type_name(slots[0]), korb_type_name(r.value));
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_STRING) &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* String.try_convert(obj) → obj/to_str/nil */
        VALUE arg = argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL;
        if (KORB_STRING_P(arg)) return RESULT_OK(arg);
        const uint32_t to_str = korb_intern(vm, "to_str", 6);
        if (korb_responds_to_coerce_p(c, slots, &arg, to_str)) {
            slots[0] = arg;
            RESULT r = korb_send_impl(c, slots + 1, to_str, line, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_STRING_P(r.value)) return r;
            if (r.value == KORB_NIL) return RESULT_OK(KORB_NIL);
            return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to String (%s#to_str gives %s)", korb_type_name(slots[0]), korb_type_name(slots[0]), korb_type_name(r.value));
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_HASH) &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* Hash.try_convert(obj) → obj/to_hash/nil */
        VALUE arg = argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL;
        if (KORB_HASH_P(arg)) return RESULT_OK(arg);
        const uint32_t to_hash = korb_intern(vm, "to_hash", 7);
        if (korb_responds_to_coerce_p(c, slots, &arg, to_hash)) {
            slots[0] = arg;
            RESULT r = korb_send_impl(c, slots + 1, to_hash, line, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_HASH_P(r.value)) return r;
            if (r.value == KORB_NIL) return RESULT_OK(KORB_NIL);
            return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to Hash (%s#to_hash gives %s)", korb_type_name(slots[0]), korb_type_name(slots[0]), korb_type_name(r.value));
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_HASH) &&
             mid == korb_intern(vm, "ruby2_keywords_hash?", 20)) {          /* koruby doesn't flag ruby2_keywords */
        return RESULT_OK(KORB_FALSE);
    }
    else if (KORB_CLASS_P(self) && self == korb_builtin_class_obj(vm, KORB_C_HASH) &&
             mid == korb_intern(vm, "ruby2_keywords_hash", 19)) {           /* return the hash unchanged (no flag tracked) */
        return RESULT_OK(argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_new &&
             korb_builtin_base_class(vm, self) == KORB_C_RANGE &&
             korb_range_default_init_p(c, self)) {
        /* Range.new(begin, end, exclude_end=false) — also for Range subclasses
         * with no #initialize of their own (a real Range payload is built and
         * the subclass identity is recorded in the override table, the same
         * scheme String/Array subclasses use).  A subclass that defines its own
         * #initialize falls through to the generic-object path instead. */
        if (UNLIKELY(argc < 2 || argc > 3))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected 2..3)", argc);
        VALUE *const base = &slots[-(korb_sword_t)argc];
        const uint32_t excl = (argc >= 3 && KORB_TRUTHY(base[2])) ? 1u : 0u;
        if (self == korb_builtin_class_obj(vm, KORB_C_RANGE))
            return korb_range_new(c, slots, VALUE_REF_AT(&base[0]), base[1], excl);
        slots[0] = self;                                    /* root the subclass across the alloc */
        const VALUE rng = UNWRAP(korb_range_new(c, slots + 1, VALUE_REF_AT(&base[0]), base[1], excl));
        slots[1] = rng;
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags &= ~(uint32_t)KORB_FL_FROZEN;   /* only Range itself makes frozen instances */
        ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_HAS_KLASS;
        korb_klass_override_set(c, slots[1], slots[0]);     /* both rooted; set does not GC */
        return RESULT_OK(slots[1]);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_new) {
        /* A user-defined `def self.new` (e.g. the Thread stub) overrides the built-in
         * allocator: dispatch it with the args + block.  The non-block path resolves
         * this in node_send_cached; this shared path (block / send) must too. */
        {
            const VALUE sing = korb_dispatch_class(c, self);
            VALUE sdef = KORB_NIL;
            struct korb_method *snew =
                KORB_CLASS_P(sing) ? korb_class_find_method(sing, vm->mid_new, &sdef) : NULL;
            /* super from inside that very `def self.new` (marker set by the
             * super path): skip the override once so the default allocator runs. */
            if (snew && vm->super_new_skip != KORB_NIL && sdef == vm->super_new_skip) snew = NULL;
            /* A user `def self.new` (ISEQ), a builtin singleton `new` (CFUNC —
             * Regexp/Time/File/Dir) or a `define_method(:new, ...)` singleton (DM)
             * all override the default allocator; the direct path resolves this in
             * node_send_cached, this shared (send/block) path must too. */
            if (snew && (snew->kind == KORB_METHOD_ISEQ || snew->kind == KORB_METHOD_CFUNC ||
                         snew->kind == KORB_METHOD_DM))
                return korb_dispatch_method(c, slots, snew, mid, line, argc, sdef, block, def_env, captured_self);
        }
        uint32_t cname = VAL2CLASS(self)->name_sym;
        if (self == korb_builtin_class_obj(vm, KORB_C_NIL) || self == korb_builtin_class_obj(vm, KORB_C_TRUE) ||
            self == korb_builtin_class_obj(vm, KORB_C_FALSE) || self == korb_builtin_class_obj(vm, KORB_C_INTEGER) ||
            self == korb_builtin_class_obj(vm, KORB_C_FLOAT) || self == korb_builtin_class_obj(vm, KORB_C_SYMBOL) ||
            self == korb_builtin_class_obj(vm, KORB_C_RATIONAL) || self == korb_builtin_class_obj(vm, KORB_C_COMPLEX))   /* #new is undefined (built via Rational()/Complex()) */
            return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method 'new' for class %s", korb_sym_name(vm, cname));
        if (self == korb_builtin_class_obj(vm, KORB_C_FIBER)) {
            const RESULT fr = korb_fiber_new(c, slots, block, def_env, captured_self);
            if (LIKELY(fr.state == KORB_NORMAL) && argc >= 1) {   /* Fiber.new(storage: h) { … } */
                const VALUE opts = slots[-(korb_sword_t)argc];
                if (KORB_HASH_P(opts)) {
                    const int32_t bi = korb_hash_find(VAL2HASH(opts), ID2SYM(korb_intern(vm, "blocking", 8)));
                    if (bi >= 0 && KORB_TRUTHY(korb_items_data(VAL2HASH(opts)->items)[2 * bi + 1]))
                        VAL2FIBER(fr.value)->rep->blocking = 1;
                    const int32_t si = korb_hash_find(VAL2HASH(opts), ID2SYM(korb_intern(vm, "storage", 7)));
                    if (si >= 0) {
                        const VALUE sv = korb_items_data(VAL2HASH(opts)->items)[2 * si + 1];
                        slots[0] = fr.value;                 /* park across the checks' allocs */
                        CHECK(korb_fiber_check_storage(c, slots + 1, sv));
                        if (sv != KORB_NIL) VAL2FIBER(slots[0])->rep->storage = sv;   /* nil → keep the inherited copy */
                        return RESULT_OK(slots[0]);
                    }
                }
            }
            return fr;
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_THREAD))
            return korb_thread_s_new(c, slots, VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc), block, def_env, captured_self);
        if (self == korb_builtin_class_obj(vm, KORB_C_MUTEX))   return korb_mutex_s_new(c, slots);
        if (self == korb_builtin_class_obj(vm, KORB_C_CONDVAR)) return korb_condvar_s_new(c, slots);
        if (self == korb_builtin_class_obj(vm, KORB_C_CLASS) || self == korb_const_get(vm, vm->name_module) ||
            (KORB_CLASS_P(self) && korb_class_le(self, korb_const_get(vm, vm->name_module)) &&
             !korb_class_le(self, korb_builtin_class_obj(vm, KORB_C_CLASS)))) {   /* Class.new([super]) / Module.new / a Module subclass */
            const bool is_mod = self != korb_builtin_class_obj(vm, KORB_C_CLASS) &&
                                !korb_class_le(self, korb_builtin_class_obj(vm, KORB_C_CLASS));
            const bool recv_is_subclass = (self != korb_builtin_class_obj(vm, KORB_C_CLASS) &&
                                           self != korb_const_get(vm, vm->name_module));

            slots[0] = (!is_mod && argc >= 1) ? slots[-(korb_sword_t)argc] : korb_builtin_class_obj(vm, KORB_C_OBJECT);   /* super (rooted) */
            if (UNLIKELY(!is_mod && (!KORB_CLASS_P(slots[0]) || VAL2CLASS(slots[0])->is_module || VAL2CLASS(slots[0])->is_singleton)))
                { char rdb[224];   /* a Module or a metaclass is not a valid superclass */
                  return korb_raise(c, slots, KORB_E_TYPE, line, "superclass must be an instance of Class (given %s)", korb_recv_desc(c, slots + 1, slots[0], rdb, sizeof rdb)); }
            if (UNLIKELY(!is_mod && (slots[0] == korb_builtin_class_obj(vm, KORB_C_CLASS) ||
                                     slots[0] == korb_const_get(vm, vm->name_module))))   /* Class/Module are not subclassable */
                return korb_raise(c, slots, KORB_E_TYPE, line, "can't make subclass of %s",
                                  slots[0] == korb_builtin_class_obj(vm, KORB_C_CLASS) ? "Class" : "Module");
            slots[1] = UNWRAP(korb_class_new(c, slots + 1, 0, is_mod ? KORB_NIL : slots[0]));   /* anonymous (name_sym 0) */
            if (is_mod) VAL2CLASS(slots[1])->is_module = 1;
            if (recv_is_subclass)                            /* a Module/Class SUBCLASS reports itself as #class */
                korb_klass_override_set(c, slots[1], *recv_slot);   /* rooted receiver slot: self may have moved */
            if (!is_mod) {                                  /* fire superclass.inherited(new_class) before the body block */
                CHECK(korb_register_subclass(c, slots + 4, slots[0], slots[1]));   /* record in super's subclass list */
                const uint32_t inh = korb_intern(vm, "inherited", 9);
                if (korb_responds_to(c, slots[0], inh)) {
                    slots[2] = slots[0]; slots[3] = slots[1];
                    RESULT hr = korb_send_impl(c, slots + 4, inh, line, 1, NULL, NULL, NULL);
                    if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
                }
            }
            if (block != NULL) {                            /* body block: def's land on the new class/module */
                const VALUE saved_definee = c->def_definee;   /* a class body, not an instance_eval */
                c->def_definee = KORB_NIL;
                RESULT br = korb_block_yield(c, slots + 2, block, def_env, NULL, 0, &slots[1]);
                c->def_definee = saved_definee;
                if (br.state == KORB_BREAK && !korb_break_owned(c, block, def_env)) return br;
                if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
            }
            return RESULT_OK(slots[1]);
        }
        if (VAL2CLASS(self)->members == KORB_NIL) {
            /* Struct.new(*members) OR a Struct-derived factory class with no
             * members of its own (`class Apple < Struct; end; Apple.new("C", :x)`
             * → Apple::C < Apple).  A real struct class has members set, so it
             * falls through to instance creation. */
            bool struct_factory = self == korb_const_get(vm, vm->name_struct);
            for (VALUE sc = self; !struct_factory && KORB_CLASS_P(sc); sc = VAL2CLASS(sc)->superclass)
                if (sc == korb_const_get(vm, vm->name_struct)) struct_factory = true;
            if (struct_factory)
                return korb_struct_define(c, slots, VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc), block, def_env, self);   /* → new struct class */
        }
        /* Struct/Data members are inherited (korb_class_new shares the member list
         * onto subclasses), so the receiver class itself carries them. */
        if (VAL2CLASS(self)->members != KORB_NIL) {        /* StructSubclass.new(*vals) / .new(member: v) → init */
            const bool is_data = VAL2CLASS(*recv_slot)->is_data;
            if (is_data) {                                 /* user-overridden Data#initialize (ISEQ) → allocate + dispatch it */
                VALUE didef = KORB_NIL;
                struct korb_method *const duinit = korb_class_find_method(*recv_slot, vm->mid_initialize, &didef);
                if (duinit && duinit->kind == KORB_METHOD_ISEQ) {   /* the default is a CFUNC, so ISEQ ⇒ a real override */
                    const uint32_t nmem = VAL2ARY(VAL2CLASS(*recv_slot)->members)->len;
                    const bool kw_form = argc == 1 && KORB_HASH_P(slots[-(korb_sword_t)argc]) &&
                                         korb_data_all_keys_members(vm, VAL2CLASS(*recv_slot), VAL2HASH(slots[-(korb_sword_t)argc]));
                    /* Data.new always calls #initialize with KEYWORDS: map exact-count
                     * positional args to a member-keyed Hash so an override's **kw sees them.
                     * Build the kwargs Hash BEFORE allocating the instance (the instance
                     * lives in a slot right up to the invoke, no alloc after it). */
                    if (!kw_form && argc == nmem && nmem > 0) {
                        slots[0] = UNWRAP(korb_hash_new(c, slots, nmem));   /* kwargs hash (rooted) */
                        VALUE_REF kh = VALUE_REF_AT(&slots[0]);
                        for (uint32_t i = 0; i < nmem; i++) {
                            slots[1] = korb_items_data(VAL2ARY(VAL2CLASS(*recv_slot)->members)->items)[i];   /* member sym (re-read; rooted) */
                            slots[2] = slots[-(korb_sword_t)argc + (korb_sword_t)i];                       /* positional value (re-read from args region) */
                            CHECK(korb_hash_set(c, slots + 3, kh, VALUE_REF_AT(&slots[1]), slots[2]));
                        }
                        ((AroObjectHeader *)(uintptr_t)slots[0])->flags |= KORB_FL_KWARGS;   /* written as keywords, so **kw collects it */
                        slots[1] = UNWRAP(korb_obj_new(c, slots + 1, *recv_slot));   /* instance (allocated last, rooted) */
                        VALUE *const ibase = slots + 2;
                        ibase[0] = slots[0];               /* the single kwargs Hash → override's **kw */
                        RESULT ir = korb_invoke_method(c, ibase + 1, duinit, 1, line, vm->mid_initialize, slots[1], didef, block, def_env, KORB_CSELF_VAL(captured_self));
                        if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                        return RESULT_OK(slots[1]);
                    }
                    slots[0] = UNWRAP(korb_obj_new(c, slots, *recv_slot));   /* the instance (rooted) */
                    VALUE *const ibase = slots + 1;
                    for (uint32_t i = 0; i < argc; i++) ibase[i] = slots[-(korb_sword_t)argc + (korb_sword_t)i];   /* forward args (post-alloc, GC-safe) */
                    RESULT ir = korb_invoke_method(c, ibase + argc, duinit, argc, line, vm->mid_initialize, slots[0], didef, block, def_env, KORB_CSELF_VAL(captured_self));
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(slots[0]);            /* re-read the (possibly moved) instance */
                }
            } else {                                       /* Struct: dispatch a user-overridden #initialize (ISEQ); default is the CFUNC */
                VALUE sudef = KORB_NIL;
                struct korb_method *const suinit = korb_class_find_method(*recv_slot, vm->mid_initialize, &sudef);
                if (suinit && suinit->kind == KORB_METHOD_ISEQ) {   /* a real override (the built-in korb_m_struct_initialize is a CFUNC) */
                    slots[0] = UNWRAP(korb_obj_new(c, slots, *recv_slot));   /* the instance (rooted) */
                    VALUE *const ibase = slots + 1;
                    for (uint32_t i = 0; i < argc; i++) ibase[i] = slots[-(korb_sword_t)argc + (korb_sword_t)i];   /* forward args (post-alloc, GC-safe) */
                    RESULT ir = korb_invoke_method(c, ibase + argc, suinit, argc, line, vm->mid_initialize, slots[0], sudef, block, def_env, KORB_CSELF_VAL(captured_self));
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(slots[0]);            /* members set by the override's super(...) → korb_m_struct_initialize */
                }
            }
            if (is_data && argc == 1 && korb_kwargs_hash_p(slots[-(korb_sword_t)argc])) {   /* normalize String keyword keys → Symbols (dup String/Symbol → last wins) */
                const KorbHash *const h0 = VAL2HASH(slots[-(korb_sword_t)argc]);
                bool has_str = false;
                for (uint32_t j = 0; j < h0->len; j++)
                    if (!SYMBOL_P(korb_items_data(h0->items)[2 * j])) { has_str = true; break; }
                if (has_str) {
                    slots[0] = slots[-(korb_sword_t)argc];
                    slots[1] = UNWRAP(korb_hash_new(c, slots + 1, VAL2HASH(slots[0])->len));
                    for (uint32_t j = 0; j < VAL2HASH(slots[0])->len; j++) {
                        VALUE k = korb_items_data(VAL2HASH(slots[0])->items)[2 * j];
                        if (!SYMBOL_P(k) && !KORB_STRING_P(k)) {      /* a key names a member via #to_str (CRuby) */
                            slots[4] = k;
                            if (UNLIKELY(!korb_responds_to(c, k, korb_intern(vm, "to_str", 6))))
                                return korb_raise_not_sym(c, slots, k);
                            const RESULT kr = korb_send(c, slots + 5, korb_intern(vm, "to_str", 6), 0, 0);
                            if (UNLIKELY(kr.state != KORB_NORMAL)) return kr;
                            if (UNLIKELY(!KORB_STRING_P(kr.value)))
                                return korb_raise(c, slots, KORB_E_TYPE, line, "can't convert %s to String", korb_type_name(slots[4]));
                            k = kr.value;
                        }
                        slots[2] = KORB_STRING_P(k) ? ID2SYM(korb_intern(vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len)) : k;
                        slots[3] = korb_items_data(VAL2HASH(slots[0])->items)[2 * j + 1];
                        CHECK(korb_hash_set(c, slots + 5, VALUE_REF_AT(&slots[1]), VALUE_REF_AT(&slots[2]), slots[3]));
                    }
                    ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_KWARGS;   /* still keywords */
                    slots[-(korb_sword_t)argc] = slots[1];
                }
            }
            /* Data kwargs: a single Hash of all-symbol keys is taken as keyword form,
             * so validate it against the members (unknown / missing keyword). */
            if (is_data && argc == 1 && korb_kwargs_hash_p(slots[-(korb_sword_t)argc])) {
                const KorbHash *const h = VAL2HASH(slots[-(korb_sword_t)argc]);
                bool all_sym = h->len > 0;
                for (uint32_t j = 0; j < h->len; j++) if (!SYMBOL_P(korb_items_data(h->items)[2 * j])) { all_sym = false; break; }
                if (all_sym) {
                    const KorbArray *const mm = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                    for (uint32_t j = 0; j < h->len; j++) {       /* unknown keyword: key names no member */
                        bool found = false;
                        for (uint32_t k2 = 0; k2 < mm->len; k2++) if (korb_items_data(mm->items)[k2] == korb_items_data(h->items)[2 * j]) { found = true; break; }
                        if (UNLIKELY(!found)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword: :%s", korb_sym_name(vm, SYM2ID(korb_items_data(h->items)[2 * j])));
                    }
                    for (uint32_t k2 = 0; k2 < mm->len; k2++)     /* missing keyword: member absent */
                        if (UNLIKELY(korb_hash_find(h, korb_items_data(mm->items)[k2]) < 0))
                            return korb_raise(c, slots, KORB_E_ARGUMENT, line, "missing keyword: :%s", korb_sym_name(vm, SYM2ID(korb_items_data(mm->items)[k2])));
                }
            }
            /* Data.new accepts positional OR keyword; the keyword form is a single
             * Hash the call site tagged as keywords (KORB_FL_KWARGS). */
            if (is_data && !(argc == 1 && korb_kwargs_hash_p(slots[-(korb_sword_t)argc])) &&
                argc != VAL2ARY(VAL2CLASS(*recv_slot)->members)->len) {
                const KorbArray *const mm = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                if (argc < mm->len) {                          /* positional shortfall → the unfilled members are missing keywords */
                    char buf[512]; int off = snprintf(buf, sizeof buf, "missing keyword%s:", (mm->len - argc) > 1 ? "s" : "");
                    for (uint32_t i = argc; i < mm->len && off < (int)sizeof buf; i++)
                        off += snprintf(buf + off, sizeof buf - off, "%s :%s", i > argc ? "," : "", korb_sym_name(vm, SYM2ID(korb_items_data(mm->items)[i])));
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "%s", buf);
                }
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected 0..%u)", argc, mm->len);
            }
            if (!is_data) {                                /* too many positional values → ArgumentError */
                const KorbArray *const mm = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                const bool kw = VAL2CLASS(*recv_slot)->struct_kwinit == 1 && argc >= 1 && KORB_HASH_P(slots[-(korb_sword_t)argc]);   /* keyword_init accepts a plain Hash too */
                if (UNLIKELY(!kw && argc > mm->len))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "struct size differs");
            }
            VALUE obj = UNWRAP(korb_obj_new(c, slots, *recv_slot));
            slots[0] = obj;
            const bool kwinit = is_data
                ? (argc == 1 && korb_kwargs_hash_p(slots[-(korb_sword_t)argc]))
                : (VAL2CLASS(*recv_slot)->struct_kwinit == 1 && argc >= 1 && KORB_HASH_P(slots[-(korb_sword_t)argc]));
            /* keyword_init: true takes keywords ONLY — positional values are an
             * arity error (CRuby), not a silent member-by-position fill. */
            if (UNLIKELY(!is_data && VAL2CLASS(*recv_slot)->struct_kwinit == 1 && !kwinit && argc > 0))
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected 0)", argc);
            if (kwinit) {                                  /* reject keyword arguments that aren't members */
                const KorbHash *const kh = VAL2HASH(slots[-(korb_sword_t)argc]);
                const KorbArray *const mem0 = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                char ukbuf[256]; int uklen = 0, ukn = 0;
                ukbuf[0] = '\0';
                for (uint32_t hi = 0; hi < kh->len; hi++) {
                    const VALUE key = korb_items_data(kh->items)[2 * hi];
                    bool found = false;
                    for (uint32_t mi = 0; mi < mem0->len; mi++) if (korb_items_data(mem0->items)[mi] == key) { found = true; break; }
                    if (found || uklen >= (int)sizeof(ukbuf) - 64) { if (!found) ukn++; continue; }
                    if (SYMBOL_P(key))                      /* Struct names them bare, Data with the colon */
                        uklen += snprintf(ukbuf + uklen, sizeof(ukbuf) - (size_t)uklen, "%s%s%s", ukn++ ? ", " : "",
                                          is_data ? ":" : "", korb_sym_name(vm, SYM2ID(key)));
                    else if (KORB_STRING_P(key))            /* a String key is reported quoted (CRuby) */
                        uklen += snprintf(ukbuf + uklen, sizeof(ukbuf) - (size_t)uklen, "%s\"%.*s\"", ukn++ ? ", " : "",
                                          (int)VAL2STR(key)->len, korb_strbuf_data(VAL2STR(key)->buf));
                    else
                        uklen += snprintf(ukbuf + uklen, sizeof(ukbuf) - (size_t)uklen, "%s%s", ukn++ ? ", " : "", korb_type_name(key));
                }
                if (ukn > 0) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "unknown keyword%s: %s",
                                               (!is_data || ukn > 1) ? "s" : "", ukbuf);
            }
            for (uint32_t i = 0; ; i++) {
                const KorbArray *mem = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                if (i >= mem->len) break;
                VALUE iv = korb_member_ivar_sym(vm, korb_items_data(mem->items)[i]);
                if (kwinit) {                              /* keyword_init: pull member by name from the kwargs hash */
                    int32_t hi = korb_hash_find(VAL2HASH(slots[-(korb_sword_t)argc]), korb_items_data(mem->items)[i]);
                    slots[1] = hi >= 0 ? korb_items_data(VAL2HASH(slots[-(korb_sword_t)argc])->items)[2*hi+1] : KORB_NIL;
                } else {
                    slots[1] = (i < argc) ? slots[-(korb_sword_t)argc + (korb_sword_t)i] : KORB_NIL;
                }
                CHECK(korb_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[0]), iv, slots[1]));
            }
            if (is_data && AROH_IS_GC_OBJECT(slots[0]))   /* Data instances are frozen (CRuby) */
                ((AroObjectHeader *)(uintptr_t)slots[0])->flags |= KORB_FL_FROZEN;
            return RESULT_OK(slots[0]);
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_ARRAY)) {       /* Array.new(n[,v]) / Array.new(n){|i|} / Array.new(ary) */
            if (UNLIKELY(argc >= 3)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected 0..2)", argc);
            if (argc == 1) {                               /* Array.new(ary) / #to_ary-able → a copy */
                VALUE av = slots[-(korb_sword_t)argc];
                if (!KORB_ARRAY_P(av) && KORB_OBJECT_P(av)) {
                    const uint32_t to_ary = korb_intern(vm, "to_ary", 6);
                    if (korb_responds_to_coerce_p(c, slots, &av, to_ary)) {
                        slots[0] = av;
                        RESULT r = korb_send_impl(c, slots + 1, to_ary, line, 0, NULL, NULL, NULL);
                        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                        if (KORB_ARRAY_P(r.value)) av = r.value;
                    }
                }
                if (KORB_ARRAY_P(av)) {
                    slots[0] = av;
                    const uint32_t m = VAL2ARY(slots[0])->len;
                    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, m));
                    VALUE_REF cdst = VALUE_REF_AT(&slots[1]);
                    for (uint32_t i = 0; i < m; i++) CHECK(korb_ary_push_val(c, slots + 2, cdst, korb_items_data(VAL2ARY(slots[0])->items)[i]));
                    return RESULT_OK(VALUE_REF_GET(cdst));
                }
            }
            korb_sword_t n = 0;
            if (argc >= 1) {
                VALUE nv = slots[-(korb_sword_t)argc];
                if (UNLIKELY(!korb_to_index(nv, &n))) {
                    if (KORB_BIGNUM_P(nv)) {                 /* a real Integer, just too large for an array size */
                        if (korb_mp_sgn(VAL2BIG(nv)->z) < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative array size");
                        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "array size too big");
                    }
                    RESULT cr = korb_coerce_to_int(c, slots, &nv);   /* else coerce the size via #to_int */
                    if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                    if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion into Integer");
                }
                if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative array size");
            }
            slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)n));
            VALUE_REF dst = VALUE_REF_AT(&slots[0]);
            for (korb_sword_t i = 0; i < n; i++) {
                if (block != NULL) {
                    VALUE iv = LONG2FIX(i);
                    RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 1, dst, argc >= 2 ? slots[-(korb_sword_t)argc + 1] : KORB_NIL));
                }
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_PROC)) {         /* Proc.new { } → a real Proc (not a generic Object) */
            if (UNLIKELY(block == NULL))
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "tried to create Proc object without a block");
            /* `Proc.new(&p)` / `Proc.new(&method)` — the block is a forwarded Proc
             * (def_env is the FWD sentinel, captured_self holds it): return it as-is. */
            if (def_env == KORB_BLK_FWD)
                return RESULT_OK(KORB_CSELF_VAL(captured_self));
            /* block-arg def_env arrives tagged (base|1); korb_make_proc wants the raw base. */
            return korb_block_to_proc(c, slots, block, def_env, captured_self);
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_ENUMERATOR) && block != NULL) {   /* Enumerator.new([size]) { |y| ... } — deferred generator */
            slots[0] = UNWRAP(korb_block_to_proc(c, slots, block, def_env, captured_self));
            const VALUE gsize = (argc >= 1) ? slots[-(korb_sword_t)argc] : KORB_NIL;   /* optional leading size arg — re-read after proc alloc (may be a heap callable) */
            return korb_enum_gen_new(c, slots + 1, slots[0], gsize);   /* store the block + known/callable size; terminals drive it (bounded) */
        }
        if (KORB_CLASS_P(vm->lazy_class) && *recv_slot == vm->lazy_class && block != NULL) {   /* Enumerator::Lazy.new(obj[, size]) { |y, *vals| ... } */
            const VALUE lsize = (argc >= 2) ? slots[-(korb_sword_t)argc + 1] : KORB_NIL;   /* size = 2nd arg */
            slots[0] = UNWRAP(korb_block_to_proc(c, slots, block, def_env, captured_self));
            RESULT lr = korb_lazy_gen_new(c, slots + 1, slots[0], false);   /* a lazy generator; the size tests never drive it */
            if (LIKELY(lr.state == KORB_NORMAL) && KORB_ENUM_P(lr.value)) VAL2ENUM(lr.value)->size = FIXNUM_P(lsize) ? lsize : KORB_NIL;
            return lr;
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_HASH)) {         /* Hash.new([default]) / Hash.new { |h,k| } */
            if (argc > 1) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %d, expected 0..1)", (int)argc);
            if (block != NULL && argc >= 1) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given 1, expected 0)");
            slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
            if (block != NULL) {                            /* default_proc: called on [] miss with (hash, key) */
                if (def_env == KORB_BLK_FWD) slots[1] = KORB_CSELF_VAL(captured_self);   /* Hash.new(&pr) → keep pr's identity, don't re-wrap */
                else slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
                ARO_STORE(c, VAL2HASH(slots[0]), (VALUE *)(uintptr_t)&VAL2HASH(slots[0])->default_proc, slots[1]);
            } else if (argc >= 1) {
                ARO_STORE(c, VAL2HASH(slots[0]), (VALUE *)(uintptr_t)&VAL2HASH(slots[0])->default_val, slots[-(korb_sword_t)argc]);
            }
            return RESULT_OK(slots[0]);
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_SET)) {          /* Set.new([enum]) { |o| … } */
            const VALUE arg = argc >= 1 ? slots[-(korb_sword_t)argc] : KORB_NIL;
            if (argc < 1 || arg == KORB_NIL) {              /* Set.new / Set.new(nil) → empty */
                slots[0] = UNWRAP(korb_ary_new(c, slots, 0)); return korb_set_new(c, slots + 1, slots[0]);
            }
            VALUE src = korb_set_elems_of(arg);
            if (src == KORB_NIL) {
                /* CRuby drives the argument with #each_entry when it has one and
                 * #each otherwise; #to_a is only a shortcut for what we can see. */
                const uint32_t m_ee = korb_intern(vm, "each_entry", 10);
                const uint32_t m_each = korb_intern(vm, "each", 4);
                const uint32_t m_toa = korb_intern(vm, "to_a", 4);
                const bool has_ee = korb_responds_to(c, arg, m_ee);
                if (UNLIKELY(!has_ee && !korb_responds_to(c, arg, m_toa) && !korb_responds_to(c, arg, m_each)))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line, "value must be enumerable");
                slots[0] = arg;
                const uint32_t drive = has_ee ? m_ee : (korb_responds_to(c, arg, m_toa) ? m_toa : m_each);
                if (drive == m_toa) {
                    RESULT ar = korb_send_impl(c, slots + 1, m_toa, line, 0, NULL, NULL, NULL);
                    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                    if (UNLIKELY(!KORB_ARRAY_P(ar.value))) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "value must be enumerable");
                    src = ar.value;
                } else {   /* collect what the iterator yields via Enumerator */
                    slots[1] = ID2SYM(drive);
                    RESULT er = korb_send_impl(c, slots + 2, korb_intern(vm, "to_enum", 7), line, 1, NULL, NULL, NULL);
                    if (UNLIKELY(er.state != KORB_NORMAL)) return er;
                    slots[0] = er.value;
                    RESULT ar = korb_send_impl(c, slots + 1, m_toa, line, 0, NULL, NULL, NULL);
                    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                    if (UNLIKELY(!KORB_ARRAY_P(ar.value))) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "value must be enumerable");
                    src = ar.value;
                }
            }
            slots[0] = src;                                 /* rooted src array */
            if (block != NULL) {                            /* Set.new(enum){ |o| … } → map each element */
                VALUE_REF sref = VALUE_REF_AT(&slots[0]);
                slots[1] = UNWRAP(korb_ary_new(c, slots + 2, VAL2ARY(src)->len));
                VALUE_REF dst = VALUE_REF_AT(&slots[1]);
                for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(sref))->len; i++) {
                    slots[2] = korb_items_data(VAL2ARY(VALUE_REF_GET(sref))->items)[i];
                    RESULT yr = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, captured_self);
                    if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
                    slots[2] = yr.value;
                    RESULT pr = korb_ary_push_val(c, slots + 3, dst, slots[2]);
                    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
                }
                slots[0] = VALUE_REF_GET(dst);
            }
            return korb_set_from_array(c, slots + 1, VALUE_REF_AT(&slots[0]));
        }
        if (self == korb_builtin_class_obj(vm, KORB_C_STRING)) {       /* String.new([str]) */
            if (argc >= 1 && KORB_STRING_P(slots[-(korb_sword_t)argc])) {
                slots[0] = slots[-(korb_sword_t)argc];          /* root source across the alloc */
                uint32_t len = VAL2STR(slots[0])->len;
                KorbString *r = korb_str_alloc(c, slots + 1, len);
                memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(slots[0])->buf), len);   /* re-read src (moved) */
                KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(slots[0]));   /* String.new(str) keeps str's encoding */
                return RESULT_OK((VALUE)r);
            }
            if (argc == 0) {                            /* String.new → ASCII-8BIT (CRuby: no source literal) */
                RESULT nr = korb_str_new(c, slots, "", 0);
                if (LIKELY(nr.state == KORB_NORMAL)) KORB_STR_ENC_SET(nr.value, KORB_ENC_BINARY);
                return nr;
            }
            /* non-String source (#to_str-able) or kwargs-only: alloc empty, run
             * #initialize (which #to_str-coerces + replaces).  Instance rooted at
             * slots[1], args below base — same layout as the subclass path. */
            slots[0] = *recv_slot;                              /* root recv (String class) */
            slots[1] = UNWRAP(korb_str_new(c, slots + 1, "", 0));   /* instance (rooted) */
            RESULT ir = korb_m_str_initialize(c, slots + 2, VALUE_REF_AT(&slots[1]),
                                              VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc));
            if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
            return RESULT_OK(slots[1]);
        }
        /* subclass of a constructible builtin (String/Array/Hash/Set): build that
         * payload, tag it with the subclass via the override table, run the
         * subclass's initialize if it defines one (else builtin-construct args). */
        {
            enum korb_class base = korb_builtin_base_class(vm, self);
            /* subclass of Proc / Fiber: build the real payload via its constructor
             * (which consumes the block), then tag it with the subclass class. */
            if (base == KORB_C_THREAD) {                       /* Thread subclass: 実 rep + user #initialize */
                slots[0] = *recv_slot;                         /* subclass (root) */
                slots[1] = UNWRAP(korb_thread_alloc_handle(c, slots + 2));   /* 未初期化 thread */
                korb_klass_override_set(c, slots[1], slots[0]);
                VALUE tidef = KORB_NIL;
                struct korb_method *const tinit = korb_class_find_method(slots[0], vm->mid_initialize, &tidef);
                if (tinit != NULL && tinit->kind == KORB_METHOD_ISEQ) {
                    /* user #initialize(args…) — super は Thread class obj 上の cfn (init_body) に届く。
                     * invoke_method が [self|args] を組み直し recv slot に instance を書く
                     * (String subclass path と同型) */
                    VALUE *const ibase = slots - argc;
                    RESULT ir = korb_invoke_method(c, slots, tinit, argc, line, vm->mid_initialize,
                                                   slots[1], tidef, block, def_env, KORB_CSELF_VAL(captured_self));
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    if (UNLIKELY(VAL2THREAD(ibase[-1])->rep->blk == KORB_NIL))   /* initialize が super を呼ばなかった */
                        return korb_raise_thread_error(c, slots, "uninitialized thread - check 'initialize'");
                    return RESULT_OK(ibase[-1]);
                }
                RESULT ir = korb_thread_init_body(c, slots + 2, VALUE_REF_AT(&slots[1]),
                                                  VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc), block, def_env, captured_self);
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_OK(slots[1]);
            }
            if (base == KORB_C_MUTEX)   { slots[0] = *recv_slot; slots[1] = UNWRAP(korb_mutex_s_new(c, slots + 2));   korb_klass_override_set(c, slots[1], slots[0]); return RESULT_OK(slots[1]); }
            if (base == KORB_C_CONDVAR) { slots[0] = *recv_slot; slots[1] = UNWRAP(korb_condvar_s_new(c, slots + 2)); korb_klass_override_set(c, slots[1], slots[0]); return RESULT_OK(slots[1]); }
            if (base == KORB_C_PROC || base == KORB_C_FIBER) {
                if (UNLIKELY(block == NULL))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                      base == KORB_C_FIBER ? "tried to create a Fiber without a block"
                                                           : "tried to create Proc object without a block");
                slots[0] = *recv_slot;                         /* root the subclass (recv) */
                VALUE inst = (base == KORB_C_FIBER)
                    ? UNWRAP(korb_fiber_new(c, slots + 1, block, def_env, captured_self))
                    : UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
                slots[1] = inst;                               /* root instance across the override set */
                korb_klass_override_set(c, slots[1], slots[0]);   /* override class = the subclass */
                if (base == KORB_C_FIBER && argc >= 1) {       /* Fiber.new(storage: h) { … } */
                    const VALUE opts = slots[-(korb_sword_t)argc];
                    if (KORB_HASH_P(opts)) {
                        const int32_t si = korb_hash_find(VAL2HASH(opts), ID2SYM(korb_intern(vm, "storage", 7)));
                        if (si >= 0) {
                            const VALUE sv = korb_items_data(VAL2HASH(opts)->items)[2 * si + 1];
                            if (UNLIKELY(sv != KORB_NIL && !KORB_HASH_P(sv)))
                                return korb_raise(c, slots + 2, KORB_E_TYPE, line, "storage must be a hash");
                            VAL2FIBER(slots[1])->rep->storage = sv;
                        }
                    }
                }
                {   /* a user `initialize` on the subclass still runs (CRuby): the
                     * object already exists, so this is a plain dispatch. */
                    VALUE idef = KORB_NIL;
                    const struct korb_method *const uinit =
                        korb_class_find_method(slots[0], vm->mid_initialize, &idef);
                    if (uinit && uinit->kind == KORB_METHOD_ISEQ) {
                        slots[2] = slots[1];                   /* recv = the new instance */
                        for (uint32_t k = 0; k < argc; k++) slots[3 + k] = slots[-(korb_sword_t)argc + (korb_sword_t)k];
                        RESULT ir = korb_send_impl(c, slots + 3 + argc, vm->mid_initialize, line, argc,
                                                   block, def_env, captured_self);
                        if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
                    }
                }
                return RESULT_OK(slots[1]);
            }
            if (base == KORB_C_STRING || base == KORB_C_ARRAY || base == KORB_C_HASH || base == KORB_C_SET) {
                uint32_t imid = vm->mid_initialize;
                VALUE idef = KORB_NIL;
                struct korb_method *uinit = korb_class_find_method(*recv_slot, imid, &idef);
                /* a builtin CFUNC initialize (e.g. Array#initialize for send) is
                 * not a user override — only ISEQ bodies customize construction. */
                if (uinit && uinit->kind != KORB_METHOD_ISEQ) uinit = NULL;
                slots[0] = *recv_slot;                         /* root the subclass (recv) */
                VALUE inst;
                if (base == KORB_C_ARRAY)      inst = UNWRAP(korb_ary_new(c, slots + 1, 0));
                else if (base == KORB_C_HASH)  inst = UNWRAP(korb_hash_new(c, slots + 1, 4));
                else if (base == KORB_C_SET) { slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0)); inst = UNWRAP(korb_set_new(c, slots + 2, slots[1])); }
                else {                                          /* String: copy a string arg unless initialize overrides */
                    if (!uinit && argc >= 1 && KORB_STRING_P(slots[-(korb_sword_t)argc])) {
                        slots[1] = slots[-(korb_sword_t)argc];
                        uint32_t len = VAL2STR(slots[1])->len;
                        KorbString *r = korb_str_alloc(c, slots + 2, len);
                        memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(slots[1])->buf), len);
                        KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(slots[1]));   /* copy keeps the source's encoding */
                        inst = (VALUE)r;
                    } else {
                        inst = UNWRAP(korb_str_new(c, slots + 1, "", 0));
                        /* no source string → ASCII-8BIT, as String.new (a user
                         * #initialize that replaces content overrides this). */
                        KORB_STR_ENC_SET(inst, KORB_ENC_BINARY);
                    }
                }
                slots[1] = inst;                               /* root instance */
                korb_klass_override_set(c, slots[1], slots[0]);   /* override class = the subclass */
                if (!uinit && base == KORB_C_ARRAY && (argc >= 1 || block != NULL)) {
                    /* no ISEQ override → run the builtin Array#initialize so the args
                     * (size+default / array-copy / block) populate the subclass instance. */
                    RESULT ir = korb_m_ary_initialize(c, slots + 2, VALUE_REF_AT(&slots[1]),
                                                      VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc), block, def_env, captured_self);
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(slots[1]);
                }
                if (!uinit && base == KORB_C_HASH && (argc >= 1 || block != NULL)) {   /* Hash subclass: default value / default_proc */
                    if (block != NULL) {
                        slots[2] = UNWRAP(korb_make_proc(c, slots + 2, block, def_env, KORB_CSELF_VAL(captured_self), 0));
                        ARO_STORE(c, VAL2HASH(slots[1]), (VALUE *)(uintptr_t)&VAL2HASH(slots[1])->default_proc, slots[2]);
                    } else {
                        ARO_STORE(c, VAL2HASH(slots[1]), (VALUE *)(uintptr_t)&VAL2HASH(slots[1])->default_val, slots[-(korb_sword_t)argc]);
                    }
                    return RESULT_OK(slots[1]);
                }
                if (!uinit && base == KORB_C_SET && argc >= 1) {   /* Set subclass: populate from the enumerable */
                    const VALUE src = korb_set_elems_of(slots[-(korb_sword_t)argc]);
                    if (UNLIKELY(src == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "value must be enumerable");
                    slots[2] = src;
                    RESULT sr = korb_set_from_array(c, slots + 3, VALUE_REF_AT(&slots[2]));
                    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                    slots[1] = sr.value;
                    korb_klass_override_set(c, slots[1], slots[0]);   /* re-apply the subclass override to the populated set */
                    return RESULT_OK(slots[1]);
                }
                if (uinit) {
                    VALUE *ibase = slots - argc;
                    RESULT ir = korb_invoke_method(c, slots, uinit, argc, line, imid, slots[1], idef, block, def_env, KORB_CSELF_VAL(captured_self));
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(ibase[-1]);   /* the (possibly moved) instance */
                }
                return RESULT_OK(slots[1]);
            }
        }
        /* exception class → a KorbException (not a generic object): etype from
         * its MRO, optional string message, user-subclass tagged for #class /
         * rescue.  A user-defined ISEQ #initialize runs (its `super(msg)` reaches
         * korb_m_exc_initialize); otherwise arg0 string is the message. */
        const int et = korb_class_exc_etype(vm, *recv_slot);
        if (et >= 0) {
            slots[0] = *recv_slot;                                  /* root the class across allocs */
            KorbException *const e = korb_alloc(c, slots + 1, sizeof(KorbException), KORB_OBJ_EXCEPTION);
            e->etype = (unsigned)et;
            e->line = line;
            slots[1] = (VALUE)e;                                    /* root the exception */
            if (VAL2CLASS(slots[0])->exc_etype < 0)                 /* user subclass → tag instance */
                ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->exc_class, slots[0]);
            VALUE eidef = KORB_NIL;
            struct korb_method *const euinit = korb_class_find_method(slots[0], vm->mid_initialize, &eidef);
            if (euinit && euinit->kind == KORB_METHOD_ISEQ) {
                VALUE *const ibase = slots + 2;
                for (uint32_t i = 0; i < argc; i++) ibase[i] = slots[-(korb_sword_t)argc + (korb_sword_t)i];
                RESULT ir = korb_invoke_method(c, ibase + argc, euinit, argc, line, vm->mid_initialize, slots[1], eidef, block, def_env, KORB_CSELF_VAL(captured_self));
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_OK(slots[1]);                          /* exception identity (mutated in place) */
            }
            if (argc >= 1)                                 /* default: arg0 is the message (any object; #to_s'd lazily) */
                ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->msg, slots[-(korb_sword_t)argc]);
            return RESULT_OK(slots[1]);
        }

        uint32_t init_mid = vm->mid_initialize;
        VALUE obj = UNWRAP(korb_obj_new(c, slots, *recv_slot));   /* klass=class (rooted) */
        /* find initialize AFTER the alloc-GC, re-reading the class from the
         * rooted recv slot (the pre-alloc class pointer would be stale). */
        VALUE init_def = KORB_NIL;
        struct korb_method *init = korb_class_find_method(*recv_slot, init_mid, &init_def);
        if (init) {
            VALUE *base = slots - argc;
            base[-1] = obj;                    /* stage obj as self at the frame's bottom header */
            /* korb_dispatch_method handles a CFUNC initialize (e.g. the default
             * Object#initialize, or a builtin) as well as ISEQ; korb_invoke_method
             * is ISEQ-only and would misread a CFUNC's locals_cnt (SIGBUS) — this
             * is the `Klass.new(...) { block }` path, so a block may be present. */
            RESULT ir = korb_dispatch_method(c, slots, init, init_mid, line, argc, init_def, block, def_env, captured_self);
            if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
            if (UNLIKELY(ir.state == KORB_BREAK) && korb_break_owned(c, block, def_env)) return RESULT_OK(ir.value);   /* `break v` in the block passed to new (its home is new) → new returns v */
            if (UNLIKELY(ir.state == KORB_BREAK)) return ir;
            return RESULT_OK(base[-1]);        /* the (possibly moved) obj */
        }
        if (UNLIKELY(argc != 0))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected 0)", argc);
        return RESULT_OK(obj);
    }

    /* Unified receiver dispatch for builtins, overridden/extended builtins,
     * exceptions and `main`: resolve `mid` through the receiver's class-object
     * MRO (which holds the native builtin methods as CFUNC entries, plus any
     * reopened/redefined user methods and included modules) via the method
     * cache, then invoke.  One path → drop-in reopen semantics. */
    const VALUE start_cls = korb_dispatch_class(c, self);
    VALUE def_class = KORB_NIL;
    struct korb_method *const m =
        KORB_CLASS_P(start_cls) ? korb_mcache_find(vm, start_cls, mid, &def_class) : NULL;
    if (UNLIKELY(m == NULL)) {
        /* `main` (klass-less): top-level defs live in the global function table,
         * which CRuby exposes as private Object methods reachable via send. */
        /* Kernel builtins (raise/puts/p/format/loop/...) and top-level defs are
         * private Object methods in CRuby, so an implicit-self call reaches them
         * from ANY object (not just the toplevel main).  Try the global table
         * before method_missing. */
        {
            struct korb_method *const gm = korb_method_lookup(vm, mid);
            if (gm) return korb_dispatch_method(c, slots, gm, mid, line, argc, KORB_NIL, block, def_env, captured_self);
        }
        /* user-defined method_missing(name, *args) catches the miss. */
        if (KORB_CLASS_P(start_cls)) {
            const uint32_t mm_mid = korb_intern(vm, "method_missing", 14);
            VALUE mm_def = KORB_NIL;
            struct korb_method *const mm = korb_mcache_find(vm, start_cls, mm_mid, &mm_def);
            if (mm) {                                          /* stage [magic | EP | self | :name | args...] */
                slots[0] = 0;                                  /* base[-3] (magic) */
                slots[1] = 0;                                  /* base[-2] (EP)    */
                slots[2] = self;                               /* base[-1] (self)  */
                slots[3] = ID2SYM(mid);                        /* arg0 = missing method name */
                for (uint32_t j = 0; j < argc; j++) slots[4 + j] = slots[-(korb_sword_t)argc + (korb_sword_t)j];
                return korb_dispatch_method(c, slots + argc + 4, mm, mm_mid, line, argc + 1, mm_def, block, def_env, captured_self);
            }
        }
        slots[0] = self;                                   /* root receiver across the raise + ivar_set allocs */
        char rdbuf[256];
        const char *const rd = korb_recv_desc(c, slots + 2, slots[0], rdbuf, sizeof rdbuf);
        RESULT r = korb_raise(c, slots + 1, KORB_E_NOMETHOD, line,
                              "undefined method '%s' for %s",
                              korb_sym_name(vm, mid), rd);
        if (LIKELY(KORB_EXC_P(r.value))) {                 /* attach #name / #receiver metadata */
            slots[1] = r.value;
            VALUE_REF eref = VALUE_REF_AT(&slots[1]);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__name", 7)), ID2SYM(mid));
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__has_recv", 11)), KORB_TRUE);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(vm, "@__receiver", 11)), slots[0]);
            RESULT ar = korb_ary_new(c, slots + 2, argc);   /* @__args = the args passed to the missing method */
            if (LIKELY(ar.state == KORB_NORMAL)) {
                slots[2] = ar.value;
                VALUE_REF argsref = VALUE_REF_AT(&slots[2]);
                for (uint32_t j = 0; j < argc; j++)
                    korb_ary_push_val(c, slots + 3, argsref, slots[-(korb_sword_t)argc + j]);
                korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(vm, "@__args", 7)), VALUE_REF_GET(argsref));
            }
            r.value = VALUE_REF_GET(eref);
        }
        return r;
    }
    return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, block, def_env, captured_self);
}

RESULT
korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc)
{
    return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);
}

/* Classify a class for `.new` (cached in cls->new_kind; the inputs — the class
 * identity, is_module, members, builtin-base — are all fixed once the class
 * exists, so a one-shot classification stays valid).  1 = plain user class (generic alloc +
 * initialize), 2 = special (Fiber / Struct factory / Struct subclass / a builtin
 * class or subclass / module) that needs korb_send_impl's bespoke handling. */
static uint8_t korb_class_new_kind(CTX *const c, const VALUE cls) {
    struct korb_vm *const vm = c->vm;
    KorbClass *const k = VAL2CLASS(cls);
    if (LIKELY(k->new_kind != 0)) return k->new_kind;
    uint8_t kind = 1;
    if (k->is_module || k->members != KORB_NIL || cls == korb_builtin_class_obj(vm, KORB_C_FIBER) ||
        cls == korb_builtin_class_obj(vm, KORB_C_THREAD) ||
        cls == korb_builtin_class_obj(vm, KORB_C_MUTEX) || cls == korb_builtin_class_obj(vm, KORB_C_CONDVAR) ||
        cls == korb_const_get(vm, vm->name_struct) || cls == korb_const_get(vm, vm->name_module) ||
        cls == korb_builtin_class_obj(vm, KORB_C_CLASS)  ||   /* Class.new / Module.new → real class, not a generic object */
        cls == korb_builtin_class_obj(vm, KORB_C_ARRAY)  || cls == korb_builtin_class_obj(vm, KORB_C_HASH) ||
        cls == korb_builtin_class_obj(vm, KORB_C_SET)    || cls == korb_builtin_class_obj(vm, KORB_C_STRING) ||
        cls == korb_builtin_class_obj(vm, KORB_C_RANGE) ||   /* Range.new(begin,end[,excl]) → real Range, not a generic object */
        cls == korb_builtin_class_obj(vm, KORB_C_NIL)   || cls == korb_builtin_class_obj(vm, KORB_C_TRUE) ||
        cls == korb_builtin_class_obj(vm, KORB_C_FALSE) || cls == korb_builtin_class_obj(vm, KORB_C_INTEGER) ||
        cls == korb_builtin_class_obj(vm, KORB_C_FLOAT) || cls == korb_builtin_class_obj(vm, KORB_C_SYMBOL) ||
        cls == korb_builtin_class_obj(vm, KORB_C_RATIONAL) || cls == korb_builtin_class_obj(vm, KORB_C_COMPLEX)) {   /* immediate/#new-undefined classes → slow path raises NoMethodError */
        kind = 2;
    } else if (korb_class_exc_etype(vm, cls) >= 0) {   /* exception class → KorbException, not a generic object */
        kind = 2;
    } else if (korb_class_le(cls, korb_const_get(vm, vm->name_module)) &&
               !korb_class_le(cls, korb_builtin_class_obj(vm, KORB_C_CLASS))) {
        kind = 2;                                      /* a Module subclass instantiates a real module */
    } else {
        const enum korb_class base = korb_builtin_base_class(vm, cls);
        if (base == KORB_C_STRING || base == KORB_C_ARRAY || base == KORB_C_HASH || base == KORB_C_SET ||
            base == KORB_C_PROC || base == KORB_C_FIBER || base == KORB_C_RANGE ||
            base == KORB_C_THREAD || base == KORB_C_MUTEX || base == KORB_C_CONDVAR) kind = 2;   /* subclass → real payload */
        else   /* a Struct-derived class with no members of its own (`class X < Struct`)
                * acts as a factory (X.new(...) → new struct class), not an instance */
            for (VALUE sc = cls; KORB_CLASS_P(sc); sc = VAL2CLASS(sc)->superclass)
                if (sc == korb_const_get(vm, vm->name_struct)) { kind = 2; break; }
    }
    if (kind == 1) {   /* a user/builtin `new` singleton method (def self.new / Time.new) overrides the default allocator → route to the smethod path */
        const VALUE sing = korb_dispatch_class(c, cls);
        VALUE mdef = KORB_NIL;
        if (KORB_CLASS_P(sing) && korb_class_find_method(sing, vm->mid_new, &mdef) != NULL)
            kind = 2;
    }
    k->new_kind = kind;
    return kind;
}

/* Per-call-site cached plain send (no block).  A monomorphic site resolves the
 * receiver's dispatch class, and on a serial+class match invokes the cached
 * method directly — skipping korb_send_impl's prologue, special-case probes and
 * the mcache hash.  The send/__send__/public_send family falls through to
 * korb_send_impl; a class receiver doing a plain-user-class `.new` is handled
 * here with an inline-cached `initialize` (the hot Klass.new path), other class
 * receivers fall through.  Lookup misses don't fill the cache, so such sites
 * simply stay on the slow path.  Only normal sites cache. */
/* Explicit-receiver visibility guard: a private method is callable only with an
 * implicit self / `self.foo` (recv == caller's self); a protected method only
 * when the caller's self is a kind of the method's owner.  Returns a RAISE on a
 * violation, else NORMAL.  caller_self == KORB_UNDEF disables the check (internal
 * C dispatch, operators). */
static RESULT korb_m_obj_method_missing(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (builtins/symbol.c): the default raiser */
static RESULT
korb_check_call_vis(CTX *c, VALUE *slots, const struct korb_method *m, uint32_t mid,
                    uint32_t line, VALUE recv, VALUE caller_self, VALUE def_class,
                    uint32_t argc, bool *mm_handled)
{
    bool viol;
    if (m->visibility == 1) {                          /* private */
        viol = recv != caller_self;
    } else {                                           /* protected (visibility == 2) */
        const VALUE owner = KORB_CLASS_P(def_class) ? def_class : korb_dispatch_class(c, recv);
        viol = !korb_class_has_ancestor(korb_dispatch_class(c, caller_self), owner);
    }
    if (LIKELY(!viol)) return RESULT_OK(KORB_NIL);
    /* CRuby routes a visibility violation through method_missing when the
     * receiver has a user-defined one (the callers are block-less sends:
     * recv at slots[-argc-1], args at slots[-argc..]). */
    const VALUE cls = korb_dispatch_class(c, recv);
    if (KORB_CLASS_P(cls)) {
        const uint32_t mm_mid = korb_intern(c->vm, "method_missing", 14);
        VALUE mm_def = KORB_NIL;
        struct korb_method *const mm = korb_mcache_find(c->vm, cls, mm_mid, &mm_def);
        if (mm && !(mm->kind == KORB_METHOD_CFUNC && mm->rfn == korb_m_obj_method_missing)) {   /* user-defined only, not the default raiser */
            /* stage [magic | EP | self | :name | args...] */
            slots[0] = 0;
            slots[1] = 0;
            slots[2] = recv;
            slots[3] = ID2SYM(mid);
            for (uint32_t j = 0; j < argc; j++) slots[4 + j] = slots[-(korb_sword_t)argc + (korb_sword_t)j];
            *mm_handled = true;
            return korb_dispatch_method(c, slots + argc + 4, mm, mm_mid, line, argc + 1, mm_def, NULL, NULL, NULL);
        }
    }
    slots[0] = recv;                                   /* root across the raise + ivar_set allocs */
    char rdbuf[224];
    const char *const rd = korb_recv_desc(c, slots + 2, slots[0], rdbuf, sizeof rdbuf);
    RESULT r = korb_raise(c, slots + 1, KORB_E_NOMETHOD, line, "%s method '%s' called for %s",
                          m->visibility == 1 ? "private" : "protected",
                          korb_sym_name(c->vm, mid), rd);
    if (LIKELY(KORB_EXC_P(r.value))) {                 /* attach #name / #receiver metadata */
        slots[1] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[1]);
        korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__name", 7)), ID2SYM(mid));
        korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
        r.value = VALUE_REF_GET(eref);
    }
    return r;
}
__attribute__((no_stack_protector)) RESULT
korb_send_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                 struct korb_inlcache *ic, VALUE caller_self)
{
    struct korb_vm *const vm = c->vm;
    const VALUE recv = slots[-(korb_sword_t)argc - 1];
    /* class receivers (Klass.new / Fiber.yield / Struct / class methods) and the
     * send/__send__/public_send family need korb_send_impl's special handling. */
    if (UNLIKELY(KORB_CLASS_P(recv) ||
                 mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send)) {
        /* hot path: Klass.new of a plain user class → alloc + cached initialize,
         * skipping korb_send_impl's long mid_new special-case cascade and the
         * uncached korb_class_find_method(initialize) it does on every call. */
        if (mid == vm->mid_new && KORB_CLASS_P(recv) && korb_class_new_kind(c, recv) == 1) {
            struct korb_method *init;
            VALUE idef;
            if (LIKELY(ic->kind == KORB_IC_NEW && ic->serial == vm->method_serial && ic->klass == recv)) {
                init = ic->m; idef = ic->def_class;
            } else {
                idef = KORB_NIL;
                init = korb_class_find_method(recv, vm->mid_initialize, &idef);
                ic->serial = vm->method_serial; ic->klass = recv; ic->m = init; ic->def_class = idef;
                ic->kind = KORB_IC_NEW;
            }
            const VALUE obj = UNWRAP(korb_obj_new(c, slots, recv));   /* may GC (bumps serial → next call re-resolves) */
            if (init) {
                VALUE *const base = slots - argc;
                if (LIKELY(init->is_simple)) {     /* fixed-arity initialize → streamlined invoke (skips kw/opt/rest handling) */
                    base[-1] = obj;                /* stage obj as self at the frame's bottom header */
                    const RESULT ir = korb_invoke_simple(c, slots, init, argc, line, vm->mid_initialize, obj, idef);
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(base[-1]);    /* the (possibly moved) obj */
                }
                base[-1] = obj;                    /* stage obj as self at recv_slot (slots[-argc-1]) */
                /* korb_dispatch_method handles CFUNC (e.g. Random#initialize) as well as
                 * ISEQ; korb_invoke_method is ISEQ-only and would misread a CFUNC's
                 * locals_cnt (SIGBUS). */
                const RESULT ir = korb_dispatch_method(c, slots, init, vm->mid_initialize, line, argc,
                                                       idef, NULL, NULL, NULL);
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_OK(base[-1]);        /* the (possibly moved) obj */
            }
            if (UNLIKELY(argc != 0))
                return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                  "wrong number of arguments (given %u, expected 0)", argc);
            return RESULT_OK(obj);
        }
        /* class/module singleton-method call (e.g. Math.sqrt, a user class
         * method) — cache the resolved method keyed on the receiver class,
         * skipping korb_send_impl's special-case cascade + mcache hash.  yield
         * and [] keep their builtin special cases (Fiber.yield / Array[] etc.);
         * the send family and .new were handled above. */
        if (LIKELY(KORB_CLASS_P(recv) && mid != vm->mid_yield && mid != vm->mid_aref)) {
            if (LIKELY(ic->kind == KORB_IC_SMETHOD && ic->serial == vm->method_serial && ic->klass == recv)) {
                if (UNLIKELY(ic->m->visibility != 0 && caller_self != KORB_UNDEF)) {   /* private_class_method guard */
                    bool mm_handled = false;
                    const RESULT vr = korb_check_call_vis(c, slots, ic->m, mid, line, recv, caller_self, ic->def_class, argc, &mm_handled);
                    if (mm_handled || vr.state != KORB_NORMAL) return vr;
                }
                return korb_dispatch_method(c, slots, ic->m, mid, line, argc, ic->def_class, NULL, NULL, NULL);
            }
            const VALUE start_cls = korb_dispatch_class(c, recv);
            VALUE def_class = KORB_NIL;
            struct korb_method *const m =
                KORB_CLASS_P(start_cls) ? korb_mcache_find(vm, start_cls, mid, &def_class) : NULL;
            if (LIKELY(m != NULL)) {
                if (UNLIKELY(m->visibility != 0 && caller_self != KORB_UNDEF)) {   /* private_class_method guard */
                    bool mm_handled = false;
                    const RESULT vr = korb_check_call_vis(c, slots, m, mid, line, recv, caller_self, def_class, argc, &mm_handled);
                    if (mm_handled || vr.state != KORB_NORMAL) return vr;
                }
                ic->serial = vm->method_serial; ic->klass = recv; ic->m = m;
                ic->def_class = def_class; ic->kind = KORB_IC_SMETHOD;
                return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, NULL, NULL, NULL);
            }
            /* miss (method_missing / NoMethodError) → korb_send_impl formats it */
        }
        return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);
    }

    /* receiver class: a plain user instance (no singleton override) reads its
     * klass inline; everything else (override / builtin / exception / main)
     * goes through korb_dispatch_class. */
    VALUE klass;
    if (LIKELY(KORB_OBJECT_P(recv) &&
               !(((const AroObjectHeader *)(uintptr_t)recv)->flags & KORB_FL_HAS_KLASS) &&
               (klass = VAL2OBJ(recv)->klass) != KORB_NIL)) {
        /* plain user instance — klass set above */
    } else if (FIXNUM_P(recv)) {
        klass = korb_builtin_class_obj(vm, KORB_C_INTEGER);   /* immediate: skip the dispatch_class + class_of PLT pair */
    } else if (FLONUM_P(recv)) {
        klass = korb_builtin_class_obj(vm, KORB_C_FLOAT);     /* (numeric kernels send to_i/abs/coerce on these per-iteration) */
    } else {
        klass = korb_dispatch_class(c, recv);
    }
    if (LIKELY((ic->kind == KORB_IC_INSTANCE || ic->kind == KORB_IC_INSTANCE_VIS) &&
               ic->serial == vm->method_serial && ic->klass == klass)) {
        struct korb_method *const m = ic->m;
        if (UNLIKELY(ic->kind == KORB_IC_INSTANCE_VIS && caller_self != KORB_UNDEF)) {   /* cached private/protected — guard the cached entry (no re-lookup) */
            bool mm_handled = false;
            const RESULT vr = korb_check_call_vis(c, slots, m, mid, line, recv, caller_self, ic->def_class, argc, &mm_handled);
            if (mm_handled || vr.state != KORB_NORMAL) return vr;
        }
        if (LIKELY(m->kind == KORB_METHOD_ISEQ && m->is_simple))   /* hot path: inlines invoke_simple, skips dispatch_method PLT */
            return korb_invoke_simple(c, slots, m, argc, line, mid, recv, ic->def_class);
        if (m->kind == KORB_METHOD_ATTR_R)                          /* attr/struct reader: inline ivar load, skip dispatch_method PLT */
            return RESULT_OK(korb_ivar_get(c, recv, ID2SYM(m->attr_ivar)));
        if (m->kind == KORB_METHOD_CFUNC && !m->uses_block &&       /* builtin (Array#<</[], String#..) — inline the CFUNC call, skip dispatch_method */
            LIKELY(m->params_cnt < 0 || (uint32_t)m->params_cnt == argc)) {
            RESULT r = m->rfn(c, slots, VALUE_REF_AT(&slots[-(korb_sword_t)argc - 1]),
                              VALUE_SLICE_MAKE(&slots[-(korb_sword_t)argc], argc));
            if (UNLIKELY(r.state == KORB_RAISE) && KORB_EXC_P(r.value)) {
                KorbException *e = VAL2EXC(r.value);
                korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
                e->line = line;
            }
            return r;
        }
        return korb_dispatch_method(c, slots, m, mid, line, argc, ic->def_class, NULL, NULL, NULL);
    }

    VALUE def_class = KORB_NIL;
    struct korb_method *const m =
        KORB_CLASS_P(klass) ? korb_mcache_find(vm, klass, mid, &def_class) : NULL;
    if (UNLIKELY(m == NULL))   /* NoMethodError (rare) — let korb_send_impl format/raise */
        return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);
    ic->serial = vm->method_serial; ic->klass = klass; ic->m = m; ic->def_class = def_class;
    if (UNLIKELY(m->visibility != 0)) {   /* private/protected: cache as _VIS (resolved) — node_send's inline fast path won't match it, so it always routes here to be guarded */
        ic->kind = KORB_IC_INSTANCE_VIS;
        if (caller_self != KORB_UNDEF) {
            bool mm_handled = false;
            const RESULT vr = korb_check_call_vis(c, slots, m, mid, line, recv, caller_self, def_class, argc, &mm_handled);
            if (mm_handled || vr.state != KORB_NORMAL) return vr;
        }
        return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, NULL, NULL, NULL);
    }
    ic->kind = KORB_IC_INSTANCE;
    return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, NULL, NULL, NULL);
}

RESULT
korb_send_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              uint32_t argc, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    RESULT r = korb_send_impl(c, slots, mid, line, argc, block, def_env, captured_self);
    if (r.state == KORB_BREAK && korb_break_owned(c, block, def_env)) r.state = KORB_NORMAL;   /* `break [v]` in the block = call's value (only if this site gave the block) */
    return r;
}

/* ---- integer formatting (to_s / chr helpers) ----------------------------- */

static uint32_t
korb_fmt_int(korb_sword_t n, int base, char *buf)
{
    char tmp[80];
    int ti = 0;
    bool neg = n < 0;
    uintptr_t u = neg ? (uintptr_t)(-(n + 1)) + 1u : (uintptr_t)n;
    if (u == 0) tmp[ti++] = '0';
    while (u) { int d = (int)(u % (uintptr_t)base); tmp[ti++] = d < 10 ? (char)('0'+d) : (char)('a'+d-10); u /= (uintptr_t)base; }
    uint32_t len = 0;
    if (neg) buf[len++] = '-';
    while (ti) buf[len++] = tmp[--ti];
    return len;
}

/* strict string→integer parse (defined below; declared in node.h so node_bignum
 * can build a beyond-Fixnum literal). */

/* Kernel#Float(x) — strict parse (handles String/Integer/Float).  Defined far
 * below but used by Float#coerce in builtins/float.c and String#% in string_ext.c. */
static RESULT korb_bi_float(CTX *c, VALUE *slots, VALUE_SLICE args);
static RESULT korb_bi_integer(CTX *c, VALUE *slots, VALUE_SLICE args);   /* Kernel#Integer — used by String#% */

/* fwd decls: ArithmeticSequence helpers are defined in arithseq.c (included after
 * array*.c) but used by Integer#upto(∞) and zip's element pull. */
static RESULT korb_arithseq_new(CTX *c, VALUE *slots, VALUE recv, VALUE a0, VALUE a1, uint8_t nargs, uint8_t is_pct);
static void korb_aseq_params(const KorbArithSeq *as, VALUE *beginv, VALUE *limv, VALUE *stepv, bool *excl);
static RESULT korb_srcloc_result(CTX *c, VALUE *slots, const struct Node *body);   /* fwd (defined near require) */
static void korb_aseq_params(const KorbArithSeq *as, VALUE *beginv, VALUE *limv, VALUE *stepv, bool *excl);   /* fwd (arithseq.c) */

static RESULT korb_time_make(CTX *c, VALUE *slots, VALUE cls, double epoch, bool utc);   /* fwd (time.c) — used by File::Stat */
static RESULT korb_coerce_to_int(CTX *c, VALUE *slots, VALUE *v);   /* fwd (string.c) — used by integer.c / float.c */
/* Regexp-pattern String operations live in builtins/regexp.c (included after
 * string.c); forward-declare the entry points string.c delegates to. */
RESULT korb_re_str_gsub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, VALUE re, bool global, bool in_place, NODE *block, VALUE *def_env, VALUE *cself);
RESULT korb_re_str_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long limit);
RESULT korb_re_str_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, VALUE group_or_nil);
RESULT korb_re_str_span(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, VALUE group_or_nil, bool *found, uint32_t *bs, uint32_t *be, bool write);
RESULT korb_re_str_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long startc, bool bytes);
RESULT korb_re_str_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE re, long stop, bool bytes, bool have_stop);
RESULT korb_re_literal_regexp(CTX *c, VALUE *slots, VALUE pv, VALUE *out);   /* String → escaped literal Regexp */
/* Kernel#raise-style args → fully-built exception, OK(exc) = deliverable /
 * RAISE = argument error (defined below; shared by Thread#raise / Fiber#raise). */
static RESULT korb_exc_build_with_cause(CTX *c, VALUE *slots, VALUE_SLICE args);
/* transcode.c is included after integer.c but Integer#chr needs it */
static uint32_t korb_tc_encode_name(const char *enc, uint32_t cp, unsigned char *out);
static uint32_t korb_tc_bytes_chr(const char *enc, uint32_t v, unsigned char *out, bool *unicode);
#include "builtins/bignum.c"
#include "builtins/integer.c"
#include "builtins/float.c"
#include "builtins/string.c"
#include "builtins/transcode.c"
/* node_eval.c-visible wrapper: GC-safe copy of a whole String (string.c's is static). */
RESULT korb_str_dup_pub(CTX *c, VALUE *slots, VALUE *src) {
    return korb_str_slice_new(c, slots, VALUE_REF_AT(src), 0, VAL2STR(*src)->len);
}
#include "builtins/symbol.c"
#include "builtins/enumerator.c"
#include "builtins/set.c"
#include "builtins/math.c"
#include "builtins/regexp.c"
#include "builtins/file.c"
#include "builtins/env.c"
#include "builtins/zlib.c"
static RESULT korb_puts_one_to(CTX *c, VALUE *slots, VALUE v, struct KorbIORep *rep);   /* defined below; io.c IO#puts uses it */
/* IO の readiness 系は blop 層 (builtins/thread.c、io.c より後に include) 実装 */
static RESULT korb_m_io_wait_readable(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_io_wait_writable(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_io_s_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_io_poll_raw(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
/* park the current green thread until `fd` is ready (thread.c, included later) */
static RESULT korb_blop_poll_wait(CTX *c, VALUE *slots, struct pollfd *fds, nfds_t nfds,
                                  double timeout_sec, ssize_t *out_ready);
#include "builtins/io.c"
#ifdef KORB_WASI
#  include "wasi/wasi_stubs.c"      /* WASI: プロセスもソケットも無い */
#else
#  include "builtins/process.c"
#  include "builtins/socket.c"
#endif
#include "builtins/time.c"
#include "builtins/random.c"
#include "builtins/array.c"
#include "builtins/hash.c"
#include "builtins/array_enum.c"
#include "builtins/range.c"
#include "builtins/array_int_ext.c"
#include "builtins/array_ext.c"
#include "builtins/int_float_ext.c"
#include "builtins/fiber.c"
#include "builtins/thread.c"
#include "builtins/arithseq.c"
#include "builtins/string_ext.c"
korb_register_core_methods(CTX *c)
{
    /* Integer */
    /* Integer#=== / Float#=== are aliases of #== (which resolves to Comparable#==);
     * registering the same fn makes instance_method(:===) == instance_method(:==)
     * and keeps case/when (`5 === x`) behaviourally identical. */
    korb_def_cmethod(c, KORB_C_INTEGER, "==", korb_m_num_eq, 1);   /* reflexive for a non-numeric other (overrides Comparable#==) */
    korb_def_cmethod(c, KORB_C_INTEGER, "===", korb_m_num_eq, 1);  /* #=== is the same method as #== */
    korb_def_cmethod(c, KORB_C_FLOAT,   "==", korb_m_num_eq, 1);
    korb_def_cmethod(c, KORB_C_FLOAT,   "===", korb_m_num_eq, 1);
    korb_def_cmethod(c, KORB_C_FLOAT,   "===", korb_m_cmpbl_eq, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL,  "===", korb_m_cmpbl_eq, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "abs", korb_m_int_abs, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "-@", korb_m_int_uminus, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "magnitude", korb_m_int_abs, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "succ", korb_m_int_succ, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "next", korb_m_int_succ, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "pred", korb_m_int_pred, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "zero?", korb_m_int_zero, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "nonzero?", korb_m_int_nonzero, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "even?", korb_m_int_even, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "odd?", korb_m_int_odd, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "positive?", korb_m_int_pos, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "negative?", korb_m_int_neg, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_f", korb_m_int_to_f, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_r", korb_m_int_to_r, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "quo", korb_m_int_quo, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "rationalize", korb_m_int_rationalize, -1);   /* 0..1 args (eps ignored) */
    korb_def_cmethod(c, KORB_C_INTEGER, "numerator", korb_m_int_numerator, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "denominator", korb_m_int_denominator, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "real", korb_m_num_real, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "imaginary", korb_m_num_imag, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "imag", korb_m_num_imag, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "arg", korb_m_num_angle, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "angle", korb_m_num_angle, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "phase", korb_m_num_angle, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_i", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_int", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "ord", korb_m_int_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "integer?", korb_m_true_lit, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_s", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "inspect", korb_m_int_to_s, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "chr", korb_m_int_chr, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "**", korb_m_int_pow, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "pow", korb_m_int_pow, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "divmod", korb_m_int_divmod, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "div", korb_m_int_div, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "modulo", korb_m_num_mod, 1);   /* #modulo IS #% (same UnboundMethod) */
    korb_def_cmethod(c, KORB_C_INTEGER, "remainder", korb_m_int_remainder, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "+", korb_m_num_add, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "-", korb_m_num_sub, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "*", korb_m_num_mul, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "/", korb_m_num_div, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "%", korb_m_num_mod, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "<", korb_m_num_lt, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "<=", korb_m_num_le, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, ">", korb_m_num_gt, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, ">=", korb_m_num_ge, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "gcd", korb_m_int_gcd, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "lcm", korb_m_int_lcm, 1);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_INTEGER), "sqrt", korb_m_integer_sqrt, 1);   /* Integer.sqrt class method */
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "step", korb_m_num_step, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "finite?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "fdiv", korb_m_int_fdiv, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "ceildiv", korb_m_int_ceildiv, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "coerce", korb_m_int_coerce, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "<=>", korb_m_int_cmp, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "between?", korb_m_int_between, 2);
    korb_def_cmethod(c, KORB_C_INTEGER, "clamp", korb_m_int_clamp, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "digits", korb_m_int_digits, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "size", korb_m_int_size, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "bit_length", korb_m_int_bit_length, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "<<", korb_m_int_lshift, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, ">>", korb_m_int_rshift, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "[]", korb_m_int_bitref, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "&", korb_m_int_and, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "|", korb_m_int_or, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "^", korb_m_int_xor, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "~", korb_m_int_inv, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "remainder", korb_m_int_remainder, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "truncate", korb_m_int_truncate, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "floor", korb_m_int_floor, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "ceil", korb_m_int_ceil, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "round", korb_m_int_round, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "dup", korb_m_int_self2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "clone", korb_m_immed_clone, -1);   /* honours freeze: (freeze:false → can't unfreeze) */
    /* the other always-frozen immediates share the same #clone (return self, reject freeze:false) */
    korb_def_cmethod(c, KORB_C_FLOAT,    "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL,   "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_NIL,      "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_TRUE,     "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_FALSE,    "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_COMPLEX,  "clone", korb_m_immed_clone, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "abs2", korb_m_int_abs2, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "nobits?", korb_m_int_nobits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "anybits?", korb_m_int_anybits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "allbits?", korb_m_int_allbits, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "gcdlcm", korb_m_int_gcdlcm, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "+@", korb_m_int_self2, 0);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "times", korb_m_int_times, 0);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "upto", korb_m_int_upto, 1);
    korb_def_cmethod_blk(c, KORB_C_INTEGER, "downto", korb_m_int_downto, 1);

    /* String */
    korb_def_cmethod(c, KORB_C_STRING, "initialize", korb_m_str_initialize, -1);
    { struct korb_method *const mi = korb_cmethod_slot(c->vm, KORB_C_STRING, "initialize"); if (mi) mi->visibility = 1; }   /* #initialize is private */
    korb_def_cmethod(c, KORB_C_STRING, "length", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "size", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "bytesize", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "empty?", korb_m_str_empty, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_s", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_str", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "+@", korb_m_str_plus_at, 0);
    korb_def_cmethod(c, KORB_C_STRING, "-@", korb_m_str_uminus, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_i", korb_m_str_to_i, -1);
    korb_def_cmethod(c, KORB_C_STRING, "hex", korb_m_str_hex, 0);
    korb_def_cmethod(c, KORB_C_STRING, "oct", korb_m_str_oct, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_r", korb_m_str_to_r, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_c", korb_m_str_to_c, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_sym", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "intern", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase", korb_m_str_upcase, -1);
    korb_def_cmethod(c, KORB_C_STRING, "downcase", korb_m_str_downcase, -1);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize", korb_m_str_capitalize, -1);
    korb_def_cmethod(c, KORB_C_STRING, "reverse", korb_m_str_reverse, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<<", korb_m_str_ltlt, 1);
    korb_def_cmethod(c, KORB_C_STRING, "concat", korb_m_str_concat, -1);
    korb_def_cmethod(c, KORB_C_STRING, "replace", korb_m_str_replace, 1);
    korb_def_cmethod(c, KORB_C_STRING, "prepend", korb_m_str_prepend, -1);
    korb_def_cmethod(c, KORB_C_STRING, "clear", korb_m_str_clear, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase!", korb_m_str_upcase_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "downcase!", korb_m_str_downcase_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize!", korb_m_str_capitalize_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase!", korb_m_str_swapcase_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "reverse!", korb_m_str_reverse_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "[]=", korb_m_str_aset, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice!", korb_m_str_slice_bang, -1);
    korb_def_cmethod(c, KORB_C_STRING, "strip!", korb_m_str_strip_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "lstrip!", korb_m_str_lstrip_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rstrip!", korb_m_str_rstrip_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chomp!", korb_m_str_chomp_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chop!", korb_m_str_chop_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "count", korb_m_str_count, -1);
    korb_def_cmethod(c, KORB_C_STRING, "sum", korb_m_str_sum, -1);
    korb_def_cmethod(c, KORB_C_STRING, "squeeze", korb_m_str_squeeze, -1);
    korb_def_cmethod(c, KORB_C_STRING, "squeeze!", korb_m_str_squeeze_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "append_as_bytes", korb_m_str_append_as_bytes, -1);
    korb_def_cmethod(c, KORB_C_STRING, "ascii_only?", korb_m_str_ascii_only, 0);
    korb_def_cmethod(c, KORB_C_STRING, "delete_prefix", korb_m_str_delete_prefix, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_suffix", korb_m_str_delete_suffix, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_prefix!", korb_m_str_delete_prefix_b, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_suffix!", korb_m_str_delete_suffix_b, 1);
    korb_def_cmethod(c, KORB_C_STRING, "dump", korb_m_str_dump, 0);
    korb_def_cmethod(c, KORB_C_STRING, "undump", korb_m_str_undump, 0);
    korb_def_cmethod(c, KORB_C_STRING, "between?", korb_m_str_between, 2);
    korb_def_cmethod(c, KORB_C_STRING, "clamp", korb_m_str_clamp, -1);
    korb_def_cmethod(c, KORB_C_STRING, "delete", korb_m_str_delete, -1);
    korb_def_cmethod(c, KORB_C_STRING, "delete!", korb_m_str_delete_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "tr", korb_m_str_tr, 2);
    korb_def_cmethod(c, KORB_C_STRING, "tr_s", korb_m_str_tr_s, 2);
    korb_def_cmethod(c, KORB_C_STRING, "tr_s!", korb_m_str_tr_s_bang, 2);
    korb_def_cmethod_blk(c, KORB_C_STRING, "gsub", korb_m_str_gsub, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "sub", korb_m_str_sub, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "gsub!", korb_m_str_gsub_b, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "sub!", korb_m_str_sub_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rpartition", korb_m_str_rpartition, 1);
    korb_def_cmethod(c, KORB_C_STRING, "partition", korb_m_str_partition, 1);
    korb_def_cmethod(c, KORB_C_STRING, "to_f", korb_m_str_to_f, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "scrub", korb_m_str_scrub, -1);
    korb_def_cmethod(c, KORB_C_STRING, "scrub!", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "include?", korb_m_str_include, 1);
    korb_def_cmethod(c, KORB_C_STRING, "start_with?", korb_m_str_start_with, -1);
    korb_def_cmethod(c, KORB_C_STRING, "end_with?", korb_m_str_end_with, -1);
    korb_def_cmethod(c, KORB_C_STRING, "index", korb_m_str_index, -1);
    korb_def_cmethod(c, KORB_C_STRING, "strip", korb_m_str_strip, -1);
    korb_def_cmethod(c, KORB_C_STRING, "lstrip", korb_m_str_lstrip, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rstrip", korb_m_str_rstrip, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chomp", korb_m_str_chomp, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chop", korb_m_str_chop, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "split", korb_m_str_split, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "chars", korb_m_str_chars_b, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "codepoints", korb_m_str_codepoints_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "succ", korb_m_str_succ, 0);
    korb_def_cmethod(c, KORB_C_STRING, "next", korb_m_str_succ, 0);
    korb_def_cmethod(c, KORB_C_STRING, "succ!", korb_m_str_succ_bang, 0);
    korb_def_cmethod(c, KORB_C_STRING, "next!", korb_m_str_succ_bang, 0);
    korb_def_cmethod(c, KORB_C_STRING, "tr!", korb_m_str_tr_bang, 2);
    korb_def_cmethod_blk(c, KORB_C_STRING, "grapheme_clusters", korb_m_str_chars_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<=>", korb_m_str_cmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "===", korb_m_cmpbl_eq, 1);   /* #=== is an alias of #== (both resolve to the Comparable rfn → instance_method ==) */
    korb_def_cmethod(c, KORB_C_STRING, "%", korb_m_str_format, 1);
    korb_def_cmethod(c, KORB_C_STRING, "*", korb_m_str_mul, 1);
    korb_def_cmethod(c, KORB_C_STRING, "+", korb_m_str_plus, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp", korb_m_str_casecmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp?", korb_m_str_casecmp_p, 1);
    korb_def_cmethod(c, KORB_C_STRING, "byteslice", korb_m_str_byteslice, -1);
    korb_def_cmethod(c, KORB_C_STRING, "getbyte", korb_m_str_getbyte, 1);
    korb_def_cmethod(c, KORB_C_STRING, "bytesplice", korb_m_str_bytesplice, -1);
    korb_def_cmethod(c, KORB_C_STRING, "insert", korb_m_str_insert, 2);
    korb_def_cmethod(c, KORB_C_STRING, "setbyte", korb_m_str_setbyte, 2);
    korb_def_cmethod(c, KORB_C_STRING, "b", korb_m_str_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "dedup", korb_m_str_uminus, 0);   /* #dedup is an alias of #-@ (frozen, deduplicated) */
    korb_def_cmethod(c, KORB_C_STRING, "encode", korb_m_obj_dup, -1);
    korb_def_cmethod(c, KORB_C_STRING, "encode!", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "force_encoding", korb_m_str_force_encoding, 1);
    korb_def_cmethod(c, KORB_C_STRING, "__encoding_tag", korb_m_str_enc_tag, 0);
    korb_def_cmethod(c, KORB_C_STRING, "__encoding_name", korb_m_str_enc_name, 0);
    korb_def_cmethod(c, KORB_C_STRING, "__set_encoding_tag", korb_m_str_set_enc_tag, 1);
    korb_def_cmethod(c, KORB_C_STRING, "valid_encoding?", korb_m_str_valid_encoding, 0);
    korb_def_cmethod(c, KORB_C_STRING, "byteindex", korb_m_str_byteindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "byterindex", korb_m_str_byterindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chr", korb_m_str_chr, 0);
    korb_def_cmethod(c, KORB_C_STRING, "ord", korb_m_str_ord, 0);
    korb_def_cmethod(c, KORB_C_STRING, "rindex", korb_m_str_rindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase", korb_m_str_swapcase, -1);
    korb_def_cmethod(c, KORB_C_STRING, "ljust", korb_m_str_ljust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rjust", korb_m_str_rjust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "center", korb_m_str_center, -1);
    korb_def_cmethod(c, KORB_C_STRING, "[]", korb_m_str_aref, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice", korb_m_str_aref, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_char", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "upto", korb_m_str_upto, -1);
    korb_def_cmethod(c, KORB_C_STRING, "crypt", korb_m_str_crypt, 1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_grapheme_cluster", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_line", korb_m_str_each_line, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "lines", korb_m_str_lines_b, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_byte", korb_m_str_each_byte, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "bytes", korb_m_str_bytes_b, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_codepoint", korb_m_str_each_codepoint, 0);

    /* Symbol */
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_SYMBOL), "all_symbols", korb_m_sym_all_symbols, 0);   /* Symbol.all_symbols class method */
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_s", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "id2name", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "slice", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "[]", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "succ", korb_m_sym_succ, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "next", korb_m_sym_succ, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "swapcase", korb_m_sym_swapcase, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "upcase", korb_m_sym_upcase, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "downcase", korb_m_sym_downcase, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "capitalize", korb_m_sym_capitalize, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<=>", korb_m_sym_cmp, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<", korb_m_sym_lt, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<=", korb_m_sym_le, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, ">", korb_m_sym_gt, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, ">=", korb_m_sym_ge, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "casecmp", korb_m_sym_casecmp, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "casecmp?", korb_m_sym_casecmp_p, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "between?", korb_m_sym_between, 2);
    korb_def_cmethod(c, KORB_C_SYMBOL, "clamp", korb_m_sym_clamp, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "start_with?", korb_m_sym_start_with, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "end_with?", korb_m_sym_end_with, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "name", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_sym", korb_m_sym_to_sym, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_proc", korb_m_sym_to_proc, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "intern", korb_m_sym_to_sym, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "length", korb_m_sym_len, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "empty?", korb_m_sym_empty, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "size", korb_m_sym_len, 0);

    /* nil */
    /* `=~` with a non-Regexp operand (or non-String receiver) → nil; the shared
     * String#=~ helper already returns nil unless both sides are Regexp+String. */
    korb_def_cmethod(c, KORB_C_NIL, "=~", korb_m_str_match_op, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "=~", korb_m_str_match_op, 1);
    korb_def_cmethod(c, KORB_C_NIL, "to_s", korb_m_nil_to_s, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_i", korb_m_nil_to_i, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_a", korb_m_nil_to_a, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_r", korb_m_nil_to_r, 0);
    korb_def_cmethod(c, KORB_C_NIL, "rationalize", korb_m_nil_rationalize, -1);   /* ignores optional eps; >1 arg → ArgumentError */
    korb_def_cmethod(c, KORB_C_NIL, "to_f", korb_m_nil_to_f, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_h", korb_m_nil_to_h, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_c", korb_m_nil_to_c, 0);
    korb_def_cmethod(c, KORB_C_NIL, "nil?", korb_m_nil_nil_q, 0);
    korb_def_cmethod(c, KORB_C_NIL, "&", korb_m_bool_false_and, 1);
    korb_def_cmethod(c, KORB_C_NIL, "|", korb_m_bool_false_or, 1);
    korb_def_cmethod(c, KORB_C_NIL, "^", korb_m_bool_false_or, 1);

    /* true / false */
    korb_def_cmethod(c, KORB_C_TRUE,  "to_s", korb_m_true_to_s, 0);
    korb_def_cmethod(c, KORB_C_TRUE,  "inspect", korb_m_true_to_s, 0);
    korb_def_cmethod(c, KORB_C_TRUE,  "&", korb_m_bool_true_and, 1);
    korb_def_cmethod(c, KORB_C_TRUE,  "|", korb_m_bool_true_or, 1);
    korb_def_cmethod(c, KORB_C_TRUE,  "^", korb_m_bool_true_xor, 1);
    korb_def_cmethod(c, KORB_C_FALSE, "to_s", korb_m_false_to_s, 0);
    korb_def_cmethod(c, KORB_C_FALSE, "inspect", korb_m_false_to_s, 0);
    korb_def_cmethod(c, KORB_C_FALSE, "&", korb_m_bool_false_and, 1);
    korb_def_cmethod(c, KORB_C_FALSE, "|", korb_m_bool_false_or, 1);
    korb_def_cmethod(c, KORB_C_FALSE, "^", korb_m_bool_false_or, 1);

    /* Array */
    korb_def_cmethod(c, KORB_C_ARRAY, "length", korb_m_ary_len, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "size", korb_m_ary_len, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "empty?", korb_m_ary_empty, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "<=>", korb_m_ary_cmp, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "to_a", korb_m_ary_self, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "to_h", korb_m_ary_to_h, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "cycle", korb_m_ary_cycle, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "permutation", korb_m_ary_permutation, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "combination", korb_m_ary_combination, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "repeated_combination", korb_m_ary_repeated_combination, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "repeated_permutation", korb_m_ary_repeated_permutation, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "to_ary", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "entries", korb_m_obj_dup, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort!", korb_m_ary_sort_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "tally", korb_m_ary_tally, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "first", korb_m_ary_first, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "initialize", korb_m_ary_initialize, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "last", korb_m_ary_last, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]", korb_m_ary_aref, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "slice", korb_m_ary_aref, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "at", korb_m_ary_aref, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "values_at", korb_m_ary_values_at, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "fill", korb_m_ary_fill, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "[]=", korb_m_ary_aset, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "<<", korb_m_ary_ltlt, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "push", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "append", korb_m_ary_push, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "pop", korb_m_ary_pop, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "include?", korb_m_ary_include, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "==", korb_m_ary_eq, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "eql?", korb_m_ary_eql, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "sample", korb_m_ary_sample, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shuffle", korb_m_ary_shuffle, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shuffle!", korb_m_ary_shuffle_bang, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse", korb_m_ary_reverse, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse!", korb_m_ary_reverse_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "rotate!", korb_m_ary_rotate_bang, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "product", korb_m_ary_product, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "fetch_values", korb_m_ary_fetch_values, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "one?", korb_m_ary_one, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reverse_each", korb_m_ary_reverse_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "take_while", korb_m_ary_take_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "drop_while", korb_m_ary_drop_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "bsearch", korb_m_ary_bsearch, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "bsearch_index", korb_m_ary_bsearch_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "map!", korb_m_ary_map_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect!", korb_m_ary_map_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_entry", korb_m_ary_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort_by", korb_m_ary_sort_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort_by!", korb_m_ary_sort_by_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "min_by", korb_m_ary_min_by, -1);   /* -1: optional count arg */
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "max_by", korb_m_ary_max_by, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "minmax_by", korb_m_ary_minmax_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter_map", korb_m_ary_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "flat_map", korb_m_ary_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect_concat", korb_m_ary_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "partition", korb_m_ary_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "group_by", korb_m_ary_group_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "grep", korb_m_ary_grep, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "grep_v", korb_m_ary_grep_v, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "clear", korb_m_ary_clear, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "intersect?", korb_m_ary_intersect_q, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "+", korb_m_ary_plus, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "*", korb_m_ary_mul, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "index", korb_m_ary_find_index, -1);   /* #index is a true alias of #find_index */
    korb_def_cmethod(c, KORB_C_ARRAY, "assoc", korb_m_ary_assoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rassoc", korb_m_ary_rassoc, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "count", korb_m_ary_count, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sum", korb_m_ary_sum_b, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "pack", korb_m_ary_pack, -1);   /* template + optional buffer: kwarg */
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "min", korb_m_ary_min, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "max", korb_m_ary_max, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "minmax", korb_m_ary_minmax, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "transpose", korb_m_ary_transpose, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort", korb_m_ary_sort, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "join", korb_m_ary_join, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "compact", korb_m_ary_compact, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "compact!", korb_m_ary_compact_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "slice!", korb_m_ary_slice_bang, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_index", korb_m_ary_each_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "uniq", korb_m_ary_uniq_b, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "uniq!", korb_m_ary_uniq_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "flatten", korb_m_ary_flatten, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "flatten!", korb_m_ary_flatten_b, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "concat", korb_m_ary_concat, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "unshift", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "prepend", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shift", korb_m_ary_shift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "assoc", korb_m_ary_assoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rassoc", korb_m_ary_rassoc, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "fetch", korb_m_ary_fetch, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "dig", korb_m_ary_dig, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "take", korb_m_ary_take, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "drop", korb_m_ary_drop, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "difference", korb_m_ary_difference, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "-", korb_m_ary_difference, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "replace", korb_m_ary_replace, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "delete", korb_m_ary_delete, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "delete_at", korb_m_ary_delete_at, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "rindex", korb_m_ary_rindex, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "member?", korb_m_ary_include, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rotate", korb_m_ary_rotate, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "zip", korb_m_ary_zip, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "deconstruct", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "insert", korb_m_ary_insert, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "|", korb_m_ary_union, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "union", korb_m_ary_union, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "&", korb_m_ary_intersect, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "intersection", korb_m_ary_intersect, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each", korb_m_ary_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_with_index", korb_m_ary_each_wi, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_slice", korb_m_ary_each_slice, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_cons", korb_m_ary_each_cons, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "chunk_while", korb_m_ary_chunk_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "chunk", korb_m_ary_chunk, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "slice_when", korb_m_ary_slice_when, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "map", korb_m_ary_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "collect", korb_m_ary_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "select", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find_all", korb_m_ary_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reject", korb_m_ary_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find", korb_m_ary_find, -1);     /* find([ifnone]) */
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "rfind", korb_m_ary_rfind, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "detect", korb_m_ary_find, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find_index", korb_m_ary_find_index, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "any?", korb_m_ary_any, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "all?", korb_m_ary_all, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "none?", korb_m_ary_none, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reduce", korb_m_ary_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "inject", korb_m_ary_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "each_with_object", korb_m_ary_each_with_object, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "delete_if", korb_m_ary_delete_if, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "reject!", korb_m_ary_reject_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "keep_if", korb_m_ary_keep_if, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "select!", korb_m_ary_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "filter!", korb_m_ary_select_bang, 0);

    /* Hash */
    korb_def_cmethod(c, KORB_C_HASH, "__kwargs_marked?", korb_m_hash_kwmarked, 0);
    korb_def_cmethod(c, KORB_C_HASH, "__kwargs_mark!", korb_m_hash_kwmark, 0);
    korb_def_cmethod(c, KORB_C_HASH, "[]", korb_m_hash_aref, 1);
    korb_def_cmethod(c, KORB_C_HASH, "[]=", korb_m_hash_aset, 2);
    korb_def_cmethod(c, KORB_C_HASH, "store", korb_m_hash_aset, 2);
    korb_def_cmethod(c, KORB_C_HASH, "size", korb_m_hash_size, 0);
    korb_def_cmethod(c, KORB_C_HASH, "compare_by_identity", korb_m_hash_cmp_by_id, 0);
    korb_def_cmethod(c, KORB_C_HASH, "compare_by_identity?", korb_m_hash_cmp_by_id_q, 0);
    korb_def_cmethod(c, KORB_C_HASH, "length", korb_m_hash_size, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "count", korb_m_hash_count, -1);
    korb_def_cmethod(c, KORB_C_HASH, "empty?", korb_m_hash_empty, 0);
    korb_def_cmethod(c, KORB_C_HASH, "key?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "has_key?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "include?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "member?", korb_m_hash_key_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "value?", korb_m_hash_value_q, 1);
    korb_def_cmethod(c, KORB_C_HASH, "==", korb_m_hash_eq, 1);
    korb_def_cmethod(c, KORB_C_HASH, "eql?", korb_m_hash_eql, 1);
    korb_def_cmethod(c, KORB_C_HASH, "has_value?", korb_m_hash_value_q, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "fetch", korb_m_hash_fetch, -1);
    korb_def_cmethod(c, KORB_C_HASH, "assoc", korb_m_hash_assoc, 1);
    korb_def_cmethod(c, KORB_C_HASH, "keys", korb_m_hash_keys, 0);
    korb_def_cmethod(c, KORB_C_HASH, "values", korb_m_hash_values, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "delete", korb_m_hash_delete, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "merge", korb_m_hash_merge, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "update", korb_m_hash_update, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "merge!", korb_m_hash_update, -1);
    korb_def_cmethod(c, KORB_C_HASH, "key", korb_m_hash_key, 1);
    korb_def_cmethod(c, KORB_C_HASH, "rassoc", korb_m_hash_rassoc, 1);
    korb_def_cmethod(c, KORB_C_HASH, "<", korb_m_hash_lt, 1);
    korb_def_cmethod(c, KORB_C_HASH, "<=", korb_m_hash_le, 1);
    korb_def_cmethod(c, KORB_C_HASH, ">", korb_m_hash_gt, 1);
    korb_def_cmethod(c, KORB_C_HASH, ">=", korb_m_hash_ge, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "to_h", korb_m_hash_to_h, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_hash", korb_m_hash_self, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_a", korb_m_hash_to_a, 0);
    korb_def_cmethod(c, KORB_C_HASH, "entries", korb_m_hash_to_a, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_entry", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reverse_each", korb_m_hash_reverse_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_with_index", korb_m_hash_each_with_index, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "cycle", korb_m_hash_cycle, -1);
    korb_def_cmethod(c, KORB_C_HASH, "tally", korb_m_hash_tally, -1);
    korb_def_cmethod(c, KORB_C_HASH, "invert", korb_m_hash_invert, 0);
    korb_def_cmethod(c, KORB_C_HASH, "rehash", korb_m_hash_rehash, 0);
    korb_def_cmethod(c, KORB_C_HASH, "replace", korb_m_hash_replace, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "drop_while", korb_m_hash_drop_while, 0);
    korb_def_cmethod(c, KORB_C_HASH, "deconstruct_keys", korb_m_ary_self, 1);   /* pattern-match hook → self; requires exactly 1 arg (keys) */
    korb_def_cmethod(c, KORB_C_HASH, "first", korb_m_hash_first, -1);
    korb_def_cmethod(c, KORB_C_HASH, "take", korb_m_hash_take, 1);
    korb_def_cmethod(c, KORB_C_HASH, "clear", korb_m_hash_clear, 0);
    korb_def_cmethod(c, KORB_C_HASH, "shift", korb_m_hash_shift, 0);
    korb_def_cmethod(c, KORB_C_HASH, "uniq", korb_m_hash_uniq, -1);
    korb_def_cmethod(c, KORB_C_HASH, "flatten", korb_m_hash_flatten, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "sort", korb_m_hash_sort, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "fetch_values", korb_m_hash_fetch_values, -1);
    korb_def_cmethod(c, KORB_C_HASH, "dig", korb_m_hash_dig, -1);
    korb_def_cmethod(c, KORB_C_HASH, "values_at", korb_m_hash_values_at, -1);
    korb_def_cmethod(c, KORB_C_HASH, "slice", korb_m_hash_slice, -1);
    korb_def_cmethod(c, KORB_C_HASH, "except", korb_m_hash_except, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "initialize", korb_m_hash_initialize, -1);
    korb_def_cmethod(c, KORB_C_HASH, "default", korb_m_hash_default, -1);
    korb_def_cmethod(c, KORB_C_HASH, "default=", korb_m_hash_default_set, 1);
    korb_def_cmethod(c, KORB_C_HASH, "compact", korb_m_hash_compact, 0);
    korb_def_cmethod(c, KORB_C_HASH, "minmax", korb_m_hash_minmax, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "transform_values", korb_m_hash_transform_values, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "transform_values!", korb_m_hash_transform_values_b, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "transform_keys", korb_m_hash_transform_keys, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "transform_keys!", korb_m_hash_transform_keys_b, -1);
    korb_def_cmethod(c, KORB_C_HASH, "compact!", korb_m_hash_compact_bang, 0);
    korb_def_cmethod(c, KORB_C_HASH, "default_proc", korb_m_hash_default_proc, 0);
    korb_def_cmethod(c, KORB_C_HASH, "default_proc=", korb_m_hash_default_proc_set, 1);
    /* compare_by_identity / ? are registered above (korb_m_hash_cmp_by_id sets
     * KORB_FL_CMP_BY_ID; the no-op duplicate here used to shadow it, leaving
     * identity hashes comparing array keys by Array#== — 50M calls in optcarrot). */
    korb_def_cmethod(c, KORB_C_HASH, "drop", korb_m_hash_drop, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_pair", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_value", korb_m_hash_each_value, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_key", korb_m_hash_each_key, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "map", korb_m_hash_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "collect", korb_m_hash_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "flat_map", korb_m_hash_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "collect_concat", korb_m_hash_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "select", korb_m_hash_select, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter", korb_m_hash_select, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reject", korb_m_hash_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "any?", korb_m_hash_any, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "all?", korb_m_hash_all, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "none?", korb_m_hash_none, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reduce", korb_m_hash_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "inject", korb_m_hash_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_with_object", korb_m_hash_each_wo, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "sum", korb_m_hash_sum, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "take_while", korb_m_hash_take_while, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "select!", korb_m_hash_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter!", korb_m_hash_select_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "keep_if", korb_m_hash_keep_if, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reject!", korb_m_hash_reject_bang, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "delete_if", korb_m_hash_delete_if, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "one?", korb_m_hash_one, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "sort_by", korb_m_hash_sort_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "min_by", korb_m_hash_min_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "max_by", korb_m_hash_max_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "minmax_by", korb_m_hash_minmax_by, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "filter_map", korb_m_hash_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "partition", korb_m_hash_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find", korb_m_hash_find, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "detect", korb_m_hash_find, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find_all", korb_m_hash_find_all, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "find_index", korb_m_hash_find_index, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "group_by", korb_m_hash_group_by, 0);
    korb_def_cmethod(c, KORB_C_HASH, "max", korb_m_hash_max, -1);
    korb_def_cmethod(c, KORB_C_HASH, "min", korb_m_hash_min, -1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "zip", korb_m_hash_zip, -1);
    korb_def_cmethod(c, KORB_C_HASH, "grep", korb_m_hash_grep, 1);
    korb_def_cmethod(c, KORB_C_HASH, "grep_v", korb_m_hash_grep_v, 1);

    /* Range */
    korb_def_cmethod(c, KORB_C_RANGE, "begin", korb_m_range_begin, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "initialize", korb_m_range_initialize, -1);
    { struct korb_method *const mi = korb_cmethod_slot(c->vm, KORB_C_RANGE, "initialize"); if (mi) mi->visibility = 1; }
    korb_def_cmethod(c, KORB_C_RANGE, "first", korb_m_range_first, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "take", korb_m_range_take, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "end", korb_m_range_end, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "last", korb_m_range_last, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "exclude_end?", korb_m_range_exclude, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "size", korb_m_range_size, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "count", korb_m_range_count, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "include?", korb_m_range_include, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "member?", korb_m_range_include, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "cover?", korb_m_range_cover, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "===", korb_m_range_cover, 1);   /* CRuby: Range#=== uses #cover?, not succ-based #include? */
    korb_def_cmethod_blk(c, KORB_C_RANGE, "min", korb_m_range_min_cmp, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "max", korb_m_range_max_cmp, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "sum", korb_m_range_sum, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "frozen?", korb_m_range_frozen, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "to_a", korb_m_range_to_a, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "==", korb_m_range_eq, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "eql?", korb_m_range_eql, 1);
    /* NB: no Range#to_ary — CRuby has none, and defining it makes a Range
     * silently splat (`[1] + (1..2)`, `a, b = (1..2)`) where CRuby does not. */
    korb_def_cmethod(c, KORB_C_RANGE, "entries", korb_m_range_to_a, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each", korb_m_range_each, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_entry", korb_m_range_each, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_with_index", korb_m_range_each_wi, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "filter_map", korb_m_range_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "grep", korb_m_range_grep, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "grep_v", korb_m_range_grep_v, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "group_by", korb_m_range_group_by, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "map", korb_m_range_map, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "overlap?", korb_m_range_overlap, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_with_object", korb_m_range_each_with_object, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "minmax", korb_m_range_minmax_cmp, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "chunk_while", korb_m_range_chunk_while, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "slice_when", korb_m_range_slice_when, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_cons", korb_m_range_each_cons, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "chunk", korb_m_range_chunk, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "uniq", korb_m_range_uniq, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "sort", korb_m_range_sort_cmp, 0);   /* int range already ascending */
    korb_def_cmethod(c, KORB_C_RANGE, "compact", korb_m_range_to_a, 0); /* no nils in an int range */
    korb_def_cmethod_blk(c, KORB_C_RANGE, "sort_by", korb_m_range_sort_by, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "reverse_each", korb_m_range_reverse_each, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_slice", korb_m_range_each_slice, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "bsearch", korb_m_range_bsearch, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "min_by", korb_m_range_min_by, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "max_by", korb_m_range_max_by, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "minmax_by", korb_m_range_minmax_by, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "partition", korb_m_range_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "flat_map", korb_m_range_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "collect_concat", korb_m_range_flat_map, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "tally", korb_m_range_tally, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "collect", korb_m_range_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "step", korb_m_range_step, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "%", korb_m_range_pct, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "reduce", korb_m_range_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "inject", korb_m_range_reduce, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "select", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "filter", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find_all", korb_m_range_select, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "reject", korb_m_range_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find", korb_m_range_find, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "detect", korb_m_range_find, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "drop", korb_m_range_drop, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "zip", korb_m_range_zip, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "one?", korb_m_range_one, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "find_index", korb_m_range_find_index, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "drop_while", korb_m_range_drop_while, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "take_while", korb_m_range_take_while, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "any?", korb_m_range_any, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "all?", korb_m_range_all, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "none?", korb_m_range_none, -1);

    /* Object (universal fallback) */
    korb_def_cmethod(c, KORB_C_OBJECT, "nil?", korb_m_obj_nil_q, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "method_missing", korb_m_obj_method_missing, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "==", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "!",  korb_m_obj_not, 0);   /* node_not handles `!x` inline; this is for reflection + send */
    korb_def_cmethod(c, KORB_C_OBJECT, "===", korb_m_obj_case_eq, 1);   /* default: self == other, honouring an overridden #== (Class/Range/Regexp/Set override) */
    korb_def_cmethod(c, KORB_C_OBJECT, "!=", korb_m_obj_neq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "equal?", korb_m_obj_equal, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "eql?", korb_m_obj_eql, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "itself", korb_m_obj_itself, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "hash", korb_m_obj_hash, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_set", korb_m_obj_ivar_set, 2);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_get", korb_m_obj_ivar_get, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_defined?", korb_m_obj_ivar_defined, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "remove_instance_variable", korb_m_obj_remove_ivar, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variables", korb_m_obj_ivars, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "method", korb_m_obj_method, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "public_method", korb_m_obj_public_method, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "freeze", korb_m_obj_freeze, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_frozen_q, 0);
    korb_def_cmethod_blk(c, KORB_C_METHOD, "call", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "to_proc", korb_m_meth_to_proc, 0);
    korb_def_cmethod_blk(c, KORB_C_METHOD, "[]", korb_m_meth_call, -1);
    korb_def_cmethod_blk(c, KORB_C_METHOD, "===", korb_m_meth_call, -1);
    korb_def_cmethod_blk(c, KORB_C_PROC, "call", korb_m_proc_call, -1);
    korb_def_cmethod_blk(c, KORB_C_PROC, "[]", korb_m_proc_call, -1);
    korb_def_cmethod_blk(c, KORB_C_PROC, "()", korb_m_proc_call, -1);
    korb_def_cmethod_blk(c, KORB_C_PROC, "yield", korb_m_proc_call, -1);
    korb_def_cmethod_blk(c, KORB_C_PROC, "===", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "lambda?", korb_m_proc_lambda_q, 0);
    korb_def_cmethod(c, KORB_C_PROC, "binding", korb_m_proc_binding, 0);
    korb_def_cmethod(c, KORB_C_PROC, "arity", korb_m_proc_arity, 0);
    korb_def_cmethod(c, KORB_C_PROC, "parameters", korb_m_proc_parameters, -1);
    korb_def_cmethod(c, KORB_C_PROC, "source_location", korb_m_proc_source_location, 0);
    korb_def_cmethod(c, KORB_C_PROC, "==",  korb_m_proc_eq, 1);     /* #eql? is an alias of #== (same rfn) */
    korb_def_cmethod(c, KORB_C_PROC, "eql?", korb_m_proc_eq, 1);
    korb_def_cmethod(c, KORB_C_PROC, "to_s", korb_m_proc_to_s, 0);   /* #inspect is an alias of #to_s */
    korb_def_cmethod(c, KORB_C_PROC, "inspect", korb_m_proc_to_s, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "receiver", korb_m_meth_recv, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "name", korb_m_meth_name, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "original_name", korb_m_meth_original_name, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "arity", korb_m_meth_arity, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "owner", korb_m_meth_owner, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "super_method", korb_m_meth_super_method, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "==", korb_m_meth_eq, 1);
    korb_def_cmethod(c, KORB_C_METHOD, "eql?", korb_m_meth_eq, 1);
    korb_def_cmethod(c, KORB_C_METHOD, "hash", korb_m_meth_hash, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "unbind", korb_m_meth_unbind, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "bind", korb_m_meth_bind, 1);
    korb_def_cmethod(c, KORB_C_METHOD, "bind_call", korb_m_meth_bind_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "parameters", korb_m_meth_parameters, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "source_location", korb_m_meth_source_location, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "to_s", korb_m_obj_to_s, 0);      /* #inspect is an alias of #to_s (same rfn → == ) */
    korb_def_cmethod(c, KORB_C_METHOD, "inspect", korb_m_obj_to_s, 0);
    /* UnboundMethod: same reflection surface as Method minus call/receiver;
     * the shared korb_m_meth_* fns already branch on the ->unbound flag. */
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "name", korb_m_meth_name, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "original_name", korb_m_meth_original_name, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "arity", korb_m_meth_arity, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "owner", korb_m_meth_owner, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "super_method", korb_m_meth_super_method, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "==", korb_m_meth_eq, 1);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "eql?", korb_m_meth_eq, 1);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "hash", korb_m_meth_hash, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "bind", korb_m_meth_bind, 1);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "bind_call", korb_m_meth_bind_call, -1);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "parameters", korb_m_meth_parameters, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "source_location", korb_m_meth_source_location, 0);
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "to_s", korb_m_obj_to_s, 0);   /* #inspect is an alias of #to_s */
    korb_def_cmethod(c, KORB_C_UNBOUND_METHOD, "inspect", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_BINDING, "local_variable_get", korb_m_bind_lvget, 1);
    korb_def_cmethod(c, KORB_C_BINDING, "local_variable_set", korb_m_bind_lvset, 2);
    korb_def_cmethod(c, KORB_C_BINDING, "local_variable_defined?", korb_m_bind_lvdefined, 1);
    korb_def_cmethod(c, KORB_C_BINDING, "local_variables", korb_m_bind_lvars, 0);
    korb_def_cmethod(c, KORB_C_BINDING, "receiver", korb_m_bind_recv, 0);
    korb_def_cmethod(c, KORB_C_BINDING, "source_location", korb_m_bind_source_location, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "inherited", korb_m_lit_nil, 1);   /* default no-op hook (so user inherited can call super) */
    korb_def_cmethod(c, KORB_C_CLASS, "method_added", korb_m_lit_nil, 1);   /* default no-op (so user method_added can call super) */
    korb_def_cmethod(c, KORB_C_CLASS, "instance_method", korb_m_class_instance_method, 1);
    korb_def_cmethod(c, KORB_C_FIBER, "resume", korb_m_fiber_resume, -1);
    korb_def_cmethod(c, KORB_C_FIBER, "raise", korb_m_fiber_raise, -1);
    korb_def_cmethod(c, KORB_C_FIBER, "transfer", korb_m_fiber_transfer, -1);
    korb_def_cmethod(c, KORB_C_FIBER, "kill", korb_m_fiber_kill, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "alive?", korb_m_fiber_alive, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "inspect", korb_m_fiber_inspect, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "to_s", korb_m_fiber_inspect, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "storage", korb_m_fiber_storage, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "storage=", korb_m_fiber_storage_set, 1);
    korb_def_cmethod(c, KORB_C_FIBER, "blocking?", korb_m_fiber_blocking_p, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "join", korb_m_thread_join, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "value", korb_m_thread_value, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "alive?", korb_m_thread_alive, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "status", korb_m_thread_status, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "name", korb_m_thread_name, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "name=", korb_m_thread_name_set, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "[]", korb_m_thread_aref, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "[]=", korb_m_thread_aset, 2);
    korb_def_cmethod(c, KORB_C_THREAD, "key?", korb_m_thread_key_p, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "thread_variable_get", korb_m_thread_tvar_get, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "thread_variable_set", korb_m_thread_tvar_set, 2);
    korb_def_cmethod(c, KORB_C_THREAD, "thread_variable?", korb_m_thread_tvar_p, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "native_thread_id", korb_m_thread_native_thread_id, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "__group", korb_m_thread_group_raw, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "__set_group", korb_m_thread_group_set, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "__defer_ints_begin", korb_m_thread_defer_begin, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "__defer_ints_end", korb_m_thread_defer_end, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "kill", korb_m_thread_kill, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "exit", korb_m_thread_kill, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "terminate", korb_m_thread_kill, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "raise", korb_m_thread_raise, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "wakeup", korb_m_thread_wakeup, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "run", korb_m_thread_wakeup, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "stop?", korb_m_thread_stop_p, 0);
    korb_def_cmethod_blk(c, KORB_C_THREAD, "fetch", korb_m_thread_fetch, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "keys", korb_m_thread_keys, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "thread_variables", korb_m_thread_tvars, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "priority", korb_m_thread_priority, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "priority=", korb_m_thread_priority_set, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "report_on_exception", korb_m_thread_roe, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "report_on_exception=", korb_m_thread_roe_set, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "abort_on_exception", korb_m_thread_aoe, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "abort_on_exception=", korb_m_thread_aoe_set, 1);
    korb_def_cmethod(c, KORB_C_THREAD, "backtrace", korb_m_thread_backtrace, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "backtrace_locations", korb_m_thread_backtrace, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "pending_interrupt?", korb_m_thread_pending_interrupt_p, -1);
    korb_def_cmethod(c, KORB_C_THREAD, "to_s", korb_m_thread_to_s, 0);
    korb_def_cmethod(c, KORB_C_THREAD, "inspect", korb_m_thread_to_s, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "lock", korb_m_mutex_lock, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "unlock", korb_m_mutex_unlock, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "try_lock", korb_m_mutex_try_lock, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "locked?", korb_m_mutex_locked_p, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "owned?", korb_m_mutex_owned_p, 0);
    korb_def_cmethod_blk(c, KORB_C_MUTEX, "synchronize", korb_m_mutex_synchronize, 0);
    korb_def_cmethod(c, KORB_C_MUTEX, "sleep", korb_m_mutex_sleep, -1);
    korb_def_cmethod(c, KORB_C_CONDVAR, "wait", korb_m_condvar_wait, -1);
    korb_def_cmethod(c, KORB_C_CONDVAR, "signal", korb_m_condvar_signal, 0);
    korb_def_cmethod(c, KORB_C_CONDVAR, "broadcast", korb_m_condvar_broadcast, 0);
    korb_def_cmethod(c, KORB_C_CONDVAR, "__num_waiting", korb_m_condvar_num_waiting, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "current", korb_m_thread_s_current, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "main", korb_m_thread_s_main, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "pass", korb_m_thread_s_pass, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "list", korb_m_thread_s_list, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "stop", korb_m_thread_s_stop, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "pending_interrupt?", korb_m_thread_pending_interrupt_p, -1);
    korb_class_def_cfn_blk(c, korb_builtin_class_obj(c->vm, KORB_C_THREAD), "initialize", korb_thread_init_body, -1);   /* subclass の super 到達先 */
    { VALUE tsing = korb_obj_singleton(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_THREAD)).value;
      korb_class_def_cfn_blk(c, tsing, "start", korb_m_thread_s_start, -1);   /* #initialize を経由しない (CRuby) */
      korb_class_def_cfn_blk(c, tsing, "fork",  korb_m_thread_s_start, -1);
      korb_class_def_cfn(c, tsing, "abort_on_exception",  korb_m_thread_s_aoe, 0);
      korb_class_def_cfn(c, tsing, "abort_on_exception=", korb_m_thread_s_aoe_set, 1); }
    korb_def_cmethod(c, KORB_C_RANDOM, "initialize", korb_m_random_init, -1);
    korb_def_cmethod(c, KORB_C_RANDOM, "rand", korb_m_random_rand, -1);
    korb_def_cmethod(c, KORB_C_RANDOM, "seed", korb_m_random_seed, 0);
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_RANDOM), "urandom", korb_m_random_urandom, 1);   /* Random.urandom class method */
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_RANDOM), "bytes", korb_m_random_urandom, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "<=>", korb_m_obj_cmp, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "initialize", korb_m_lit_nil, 0);   /* default no-op (so `super()` in a user #initialize resolves) */
    korb_def_cmethod(c, KORB_C_OBJECT, "class", korb_m_obj_class, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "object_id", korb_m_obj_object_id, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "__id__", korb_m_obj_object_id, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "is_a?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "kind_of?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "extend", korb_m_obj_extend, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "singleton_class", korb_m_obj_singleton_class, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "initialize_copy", korb_m_obj_initialize_copy, 1);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "define_singleton_method", korb_m_obj_define_singleton_method, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attached_object", korb_m_class_attached_object, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "subclasses", korb_m_class_subclasses, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "respond_to?", korb_m_obj_respond_to, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "methods", korb_m_obj_methods, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "public_methods", korb_m_obj_public_methods, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "private_methods", korb_m_obj_private_methods, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "protected_methods", korb_m_obj_protected_methods, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "singleton_methods", korb_m_obj_singleton_methods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "===", korb_m_class_case_eq, 1);
    korb_def_cmethod_blk(c, KORB_C_CLASS, "define_method", korb_m_define_method, -1);
    /* class_eval/module_eval (block) rebind self=class, so `def` inside targets the
     * class as an instance method (node_def uses self); class_exec/module_exec also
     * forward block args.  Same self-rebind logic as instance_eval/instance_exec. */
    korb_def_cmethod_blk(c, KORB_C_CLASS, "class_eval", korb_m_mod_class_eval, -1);
    korb_def_cmethod_blk(c, KORB_C_CLASS, "module_eval", korb_m_mod_class_eval, -1);
    korb_def_cmethod_blk(c, KORB_C_CLASS, "class_exec", korb_m_mod_class_exec, -1);
    korb_def_cmethod_blk(c, KORB_C_CLASS, "module_exec", korb_m_mod_class_exec, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "alias_method", korb_m_class_alias_method, 2);
    korb_def_cmethod(c, KORB_C_CLASS, "superclass", korb_m_class_superclass, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "allocate", korb_m_class_allocate, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "name", korb_m_class_name, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "set_temporary_name", korb_m_module_set_temp_name, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "constants", korb_m_mod_constants, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "to_s", korb_m_class_to_s, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "inspect", korb_m_class_to_s, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "ancestors", korb_m_class_ancestors, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "instance_methods", korb_m_class_instance_methods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public_instance_methods", korb_m_class_public_imethods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "private_instance_methods", korb_m_class_private_imethods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "protected_instance_methods", korb_m_class_protected_imethods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "method_defined?", korb_m_class_method_defined, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public_method_defined?", korb_m_class_public_method_defined, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "private_method_defined?", korb_m_class_private_method_defined, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "protected_method_defined?", korb_m_class_protected_method_defined, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "include?", korb_m_class_include_q, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "include", korb_m_class_include, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "prepend", korb_m_class_prepend, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_set", korb_m_class_const_set, 2);
    korb_def_cmethod(c, KORB_C_CLASS, "remove_method", korb_m_class_remove_method, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "undef_method", korb_m_class_undef_method, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "undefined_instance_methods", korb_m_class_undefined_imethods, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "<",  korb_m_class_lt, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "<=", korb_m_class_le, 1);
    korb_def_cmethod(c, KORB_C_CLASS, ">",  korb_m_class_gt, 1);
    korb_def_cmethod(c, KORB_C_CLASS, ">=", korb_m_class_ge, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "<=>", korb_m_class_cmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "=~", korb_m_str_match_op, 1);
    korb_def_cmethod(c, KORB_C_STRING, "match?", korb_m_str_match_q, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "match", korb_m_str_match, -1);
    korb_def_cmethod_blk(c, KORB_C_SYMBOL, "match", korb_m_str_match, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "match?", korb_m_str_match_q, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "scan", korb_m_str_scan, 1);
    korb_def_cmethod(c, KORB_C_STRING, "unpack", korb_m_str_unpack, -1);
    korb_def_cmethod(c, KORB_C_STRING, "unpack1", korb_m_str_unpack1, -1);
    korb_def_cmethod(c, KORB_C_REGEXP, "=~", korb_m_re_match_op, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "match?", korb_m_re_match_q, -1);
    korb_def_cmethod(c, KORB_C_REGEXP, "===", korb_m_re_case_eq, 1);
    korb_def_cmethod_blk(c, KORB_C_REGEXP, "match", korb_m_re_match, -1);
    korb_def_cmethod(c, KORB_C_REGEXP, "source", korb_m_re_source, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "__enc_hint", korb_m_re_enc_hint, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "to_s", korb_m_re_to_s, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "inspect", korb_m_re_inspect, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "options", korb_m_re_options, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "casefold?", korb_m_re_casefold, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "names", korb_m_re_names, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "named_captures", korb_m_re_named_captures, 0);
    korb_def_cmethod(c, KORB_C_REGEXP, "==", korb_m_re_eq, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "eql?", korb_m_re_eq, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "hash", korb_m_re_hash, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "[]", korb_m_md_aref, -1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "to_s", korb_m_md_to_s, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "to_a", korb_m_md_to_a, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "captures", korb_m_md_captures, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "named_captures", korb_m_md_named_captures, -1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "names", korb_m_md_names, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "byteoffset", korb_m_md_byteoffset, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "match", korb_m_md_match, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "match_length", korb_m_md_match_length, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "==", korb_m_md_eq, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "eql?", korb_m_md_eq, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "hash", korb_m_md_hash, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "deconstruct_keys", korb_m_md_deconstruct_keys, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "values_at", korb_m_md_values_at, -1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "pre_match", korb_m_md_pre, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "post_match", korb_m_md_post, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "begin", korb_m_md_begin, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "end", korb_m_md_end, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "bytebegin", korb_m_md_bytebegin, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "byteend", korb_m_md_byteend, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "offset", korb_m_md_offset, 1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "size", korb_m_md_size, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "length", korb_m_md_size, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "string", korb_m_md_string, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "regexp", korb_m_md_regexp, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "private", korb_m_private, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public", korb_m_public, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "protected", korb_m_protected, -1);
    /* NOT on Class: CRuby undefines Module#module_function there (a Class has no
     * module functions).  The Module-side registration below is the only one. */
    /* top-level `private`/`public`/... (self = main, an Object) are also no-ops. */
    korb_def_cmethod(c, KORB_C_OBJECT, "private", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "public", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "protected", korb_m_visibility_noop, -1);
    /* `refine(Klass) { ... }` (refinements) — koruby has no refinement scoping;
     * treat as a no-op returning a fresh module so the call site doesn't raise. */
    korb_def_cmethod(c, KORB_C_CLASS,  "refine", korb_m_lit_nil, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "refine", korb_m_lit_nil, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "using",  korb_m_lit_nil, -1);
    korb_def_cmethod(c, KORB_C_CLASS,  "using",  korb_m_lit_nil, -1);
    /* constant/method visibility — koruby tracks no visibility, so these are
     * no-ops returning their argument (matches `private`/`public`). */
    korb_def_cmethod(c, KORB_C_CLASS, "private_constant", korb_m_mod_private_constant, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public_constant", korb_m_mod_public_constant, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "deprecate_constant", korb_m_deprecate_constant, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "private_class_method", korb_m_private_class_method, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public_class_method", korb_m_public_class_method, -1);
    /* Module#autoload / #autoload? live in the prelude (they record a per-module
     * table honoured from #const_missing); only the top-level forms are wired
     * here, delegating to Object. */
    /* caller / caller_locations: no walkable call stack → empty Array (stub). */
    korb_def_cmethod(c, KORB_C_OBJECT, "caller",           korb_m_empty_ary, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "caller_locations", korb_m_empty_ary, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_get", korb_m_class_const_get, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_source_location", korb_m_mod_const_source_location, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "__lexical_parent", korb_m_mod_lexical_parent, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "remove_const", korb_m_class_remove_const, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_defined?", korb_m_class_const_defined, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "class_variable_get", korb_m_class_cvar_get, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "class_variable_set", korb_m_class_cvar_set, 2);
    korb_def_cmethod(c, KORB_C_CLASS, "remove_class_variable", korb_m_class_remove_cvar, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "class_variable_defined?", korb_m_class_cvar_defined, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "class_variables", korb_m_class_cvars, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_reader", korb_m_class_attr_reader, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_writer", korb_m_class_attr_writer, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_accessor", korb_m_class_attr_accessor, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr", korb_m_class_attr1, -1);
    /* The above are registered on Class, but Module-level methods must also be on
     * Module (Class < Module) so runtime *module values* (m.name, mods.map(&:name),
     * m.ancestors, m.module_eval, …) dispatch them — not just const-ref `M.name`.
     * Re-register the shared (non-Class-specific) ones on the Module class object.
     * Class-only (new/superclass/allocate/inherited/attached_object) stay on Class. */
    #define MOD_CFN(nm, fn, ar)     korb_class_def_cfn(c, korb_const_get(c->vm, c->vm->name_module), nm, fn, ar)
    #define MOD_CFN_BLK(nm, fn, ar) korb_class_def_cfn_blk(c, korb_const_get(c->vm, c->vm->name_module), nm, fn, ar)
    MOD_CFN("name", korb_m_class_name, 0);
    MOD_CFN("set_temporary_name", korb_m_module_set_temp_name, -1);
    MOD_CFN("constants", korb_m_mod_constants, -1);
    MOD_CFN("to_s", korb_m_class_to_s, 0);
    MOD_CFN("inspect", korb_m_class_to_s, 0);
    MOD_CFN("ancestors", korb_m_class_ancestors, 0);
    MOD_CFN("===", korb_m_class_case_eq, 1);
    MOD_CFN("instance_method", korb_m_class_instance_method, 1);
    MOD_CFN("instance_methods", korb_m_class_instance_methods, -1);
    MOD_CFN("public_instance_methods", korb_m_class_public_imethods, -1);
    MOD_CFN("private_instance_methods", korb_m_class_private_imethods, -1);
    MOD_CFN("protected_instance_methods", korb_m_class_protected_imethods, -1);
    MOD_CFN("method_defined?", korb_m_class_method_defined, -1);
    MOD_CFN("public_method_defined?", korb_m_class_public_method_defined, -1);
    MOD_CFN("private_method_defined?", korb_m_class_private_method_defined, -1);
    MOD_CFN("protected_method_defined?", korb_m_class_protected_method_defined, -1);
    MOD_CFN("include?", korb_m_class_include_q, 1);
    MOD_CFN("include", korb_m_class_include, -1);
    MOD_CFN("prepend", korb_m_class_prepend, -1);
    MOD_CFN("append_features", korb_m_mod_append_features, 1);
    MOD_CFN("prepend_features", korb_m_mod_prepend_features, 1);
    MOD_CFN("const_set", korb_m_class_const_set, 2);
    MOD_CFN("const_get", korb_m_class_const_get, -1);
    MOD_CFN("const_source_location", korb_m_mod_const_source_location, -1);
    MOD_CFN("__lexical_parent", korb_m_mod_lexical_parent, 0);
    MOD_CFN("const_defined?", korb_m_class_const_defined, -1);
    MOD_CFN("class_variable_get", korb_m_class_cvar_get, 1);
    MOD_CFN("class_variable_set", korb_m_class_cvar_set, 2);
    MOD_CFN("class_variable_defined?", korb_m_class_cvar_defined, 1);
    MOD_CFN("class_variables", korb_m_class_cvars, -1);
    MOD_CFN("remove_class_variable", korb_m_class_remove_cvar, 1);
    MOD_CFN("remove_const", korb_m_class_remove_const, 1);
    MOD_CFN("remove_method", korb_m_class_remove_method, -1);
    MOD_CFN("undef_method", korb_m_class_undef_method, -1);
    MOD_CFN("undefined_instance_methods", korb_m_class_undefined_imethods, 0);
    MOD_CFN("alias_method", korb_m_class_alias_method, 2);
    MOD_CFN("<",  korb_m_class_lt, 1);
    MOD_CFN("<=", korb_m_class_le, 1);
    MOD_CFN(">",  korb_m_class_gt, 1);
    MOD_CFN(">=", korb_m_class_ge, 1);
    MOD_CFN("<=>", korb_m_class_cmp, 1);
    MOD_CFN("attr_reader", korb_m_class_attr_reader, -1);
    MOD_CFN("attr_writer", korb_m_class_attr_writer, -1);
    MOD_CFN("attr_accessor", korb_m_class_attr_accessor, -1);
    MOD_CFN("attr", korb_m_class_attr1, -1);
    MOD_CFN("private", korb_m_private, -1);
    MOD_CFN("public", korb_m_public, -1);
    MOD_CFN("protected", korb_m_protected, -1);
    MOD_CFN("module_function", korb_m_module_function, -1);
    MOD_CFN("private_constant", korb_m_mod_private_constant, -1);
    MOD_CFN("deprecate_constant", korb_m_deprecate_constant, -1);
    MOD_CFN("public_constant", korb_m_mod_public_constant, -1);
    /* default no-op callbacks so a module (Kernel, `module M`) responds to the
     * hooks and user overrides can `super` (Class also has method_added/inherited). */
    MOD_CFN("method_added", korb_m_lit_nil, 1);
    {   /* BasicObject#singleton_method_added / _removed / _undefined: private
         * no-op defaults, so `super` works and #private_instance_methods lists them. */
        const VALUE bo = korb_const_get(c->vm, korb_intern(c->vm, "BasicObject", 11));
        if (KORB_CLASS_P(bo)) {
            static const char *const smh[] = { "singleton_method_added", "singleton_method_removed", "singleton_method_undefined" };
            for (size_t i = 0; i < 3; i++) {
                korb_class_def_cfn(c, bo, smh[i], korb_m_lit_nil, 1);
                struct korb_method *const m = korb_class_find_method(bo, korb_intern(c->vm, smh[i], (uint32_t)strlen(smh[i])), NULL);
                if (m) m->visibility = 1;
            }
        }
    }
    MOD_CFN("method_removed", korb_m_lit_nil, 1);
    MOD_CFN("method_undefined", korb_m_lit_nil, 1);
    MOD_CFN("included", korb_m_lit_nil, 1);
    MOD_CFN("prepended", korb_m_lit_nil, 1);
    MOD_CFN("extended", korb_m_lit_nil, 1);
    MOD_CFN_BLK("define_method", korb_m_define_method, -1);
    MOD_CFN("private_class_method", korb_m_private_class_method, -1);
    MOD_CFN("public_class_method", korb_m_public_class_method, -1);
    MOD_CFN_BLK("class_eval", korb_m_mod_class_eval, -1);
    MOD_CFN_BLK("module_eval", korb_m_mod_class_eval, -1);
    MOD_CFN_BLK("class_exec", korb_m_mod_class_exec, -1);
    MOD_CFN_BLK("module_exec", korb_m_mod_class_exec, -1);
    #undef MOD_CFN
    #undef MOD_CFN_BLK
    {   /* CRuby undefines the module-only definition methods on Class, so
         * Class.private_instance_methods doesn't list them and an UnboundMethod
         * rebound to a Class raises. */
        static const char *const class_undefs[] = {
            "append_features", "prepend_features", "extend_object", "module_function", "refine",
        };
        const VALUE clsc = korb_builtin_class_obj(c->vm, KORB_C_CLASS);
        if (KORB_CLASS_P(clsc))
            for (size_t i = 0; i < sizeof class_undefs / sizeof class_undefs[0]; i++)
                korb_class_undef_slot(VAL2CLASS(clsc), clsc,
                                      korb_intern(c->vm, class_undefs[i], (uint32_t)strlen(class_undefs[i])));
    }
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "then", korb_m_obj_then, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "yield_self", korb_m_obj_then, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "tap", korb_m_obj_tap, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "lambda", korb_m_kernel_lambda, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "proc", korb_m_kernel_proc, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "instance_exec", korb_m_obj_instance_exec, -1);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "instance_eval", korb_m_obj_instance_eval, -1);
    /* instance_eval/instance_exec are BasicObject instance methods in CRuby, so a
     * bare BasicObject (which does not inherit Object/Kernel) can still use them. */
    { const VALUE bo = korb_const_get(c->vm, korb_intern(c->vm, "BasicObject", 11));
      if (KORB_CLASS_P(bo)) {
          korb_class_def_cfn_blk(c, bo, "instance_eval", korb_m_obj_instance_eval, -1);
          korb_class_def_cfn_blk(c, bo, "instance_exec", korb_m_obj_instance_exec, -1);
          korb_class_def_cfn(c, bo, "equal?", korb_m_obj_equal, 1);      /* BasicObject-level: a bare BasicObject responds to these */
          korb_class_def_cfn(c, bo, "__id__", korb_m_obj_object_id, 0);
          korb_class_def_cfn(c, bo, "==", korb_m_obj_equal, 1);          /* #== is identity — the same rfn as #equal? (alias) */
          korb_class_def_cfn(c, bo, "!=", korb_m_obj_neq, 1);
      } }
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "loop", korb_m_loop, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "catch", korb_m_catch, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "throw", korb_m_throw, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_of?", korb_m_obj_instance_of, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_false, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "dup", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "clone", korb_m_obj_clone, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "frozen?", korb_m_true_lit2, 0);   /* all Numeric instances are frozen (Bignum via Integer) */
    korb_def_cmethod(c, KORB_C_COMPLEX,  "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_NIL,    "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_TRUE,   "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_FALSE,  "frozen?", korb_m_true_lit2, 0);

    /* Exception */
    korb_def_cmethod(c, KORB_C_EXCEPTION, "backtrace", korb_m_exc_backtrace, 0);    /* nil until first raised/rescued */
    { const VALUE ne = korb_const_get(c->vm, korb_intern(c->vm, "NameError", 9));   /* NameError#name/#receiver (NoMethodError inherits) */
      if (KORB_CLASS_P(ne)) {
          korb_class_def_cfn(c, ne, "name", korb_m_exc_name, 0);
          korb_class_def_cfn(c, ne, "receiver", korb_m_exc_receiver, 0);
      } }
    { const VALUE nme = korb_const_get(c->vm, korb_intern(c->vm, "NoMethodError", 13));   /* NoMethodError#args */
      if (KORB_CLASS_P(nme)) korb_class_def_cfn(c, nme, "args", korb_m_nme_args, 0); }
    korb_def_cmethod(c, KORB_C_EXCEPTION, "set_backtrace", korb_m_exc_set_backtrace, -1);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "cause", korb_m_exc_cause, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "backtrace_locations", korb_m_exc_backtrace_locations, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "message", korb_m_exc_message_via_to_s, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "to_s", korb_m_exc_message, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "__raw_mesg", korb_m_exc_raw_mesg, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "inspect", korb_m_exc_inspect, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "initialize", korb_m_exc_initialize, -1);

    /* Float */
    korb_def_cmethod(c, KORB_C_FLOAT, "to_f", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_r", korb_m_flt_to_r, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "rationalize", korb_m_flt_rationalize, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "numerator", korb_m_flt_numerator, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "denominator", korb_m_flt_denominator, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_i", korb_m_flt_to_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_int", korb_m_flt_to_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "truncate", korb_m_flt_truncate, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "floor", korb_m_flt_floor, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "ceil", korb_m_flt_ceil, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "round", korb_m_flt_round, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "abs", korb_m_flt_abs, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "magnitude", korb_m_flt_abs, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "abs2", korb_m_flt_abs2, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "**", korb_m_flt_pow, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "-@", korb_m_flt_uminus, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "pow", korb_m_flt_pow, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "angle", korb_m_flt_angle, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "arg", korb_m_flt_angle, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "phase", korb_m_flt_angle, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "real", korb_m_num_real, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "imaginary", korb_m_num_imag, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "imag", korb_m_num_imag, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "dup", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "+@", korb_m_flt_to_f, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "between?", korb_m_flt_between, 2);
    korb_def_cmethod(c, KORB_C_FLOAT, "clamp", korb_m_flt_clamp, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "zero?", korb_m_flt_zero, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "nonzero?", korb_m_flt_nonzero, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "nan?", korb_m_flt_nan, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "infinite?", korb_m_flt_inf, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "<=>", korb_m_flt_cmp, 1);
    korb_def_cmethod_blk(c, KORB_C_FLOAT, "step", korb_m_num_step, -1);
    korb_def_cmethod(c, KORB_C_FLOAT, "+", korb_m_num_add, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "-", korb_m_num_sub, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "*", korb_m_num_mul, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "/", korb_m_num_div, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "%", korb_m_flt_modulo, 1);   /* shares modulo's impl: ZeroDivisionError + alias-identity */
    korb_def_cmethod(c, KORB_C_FLOAT, "<", korb_m_num_lt, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "<=", korb_m_num_le, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, ">", korb_m_num_gt, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, ">=", korb_m_num_ge, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "fdiv", korb_m_flt_fdiv, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "quo", korb_m_flt_fdiv, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "div", korb_m_flt_div, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "modulo", korb_m_flt_modulo, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "remainder", korb_m_flt_remainder, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "integer?", korb_m_lit_false, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "coerce", korb_m_flt_coerce, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "finite?", korb_m_flt_finite, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "next_float", korb_m_flt_next, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "prev_float", korb_m_flt_prev, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "divmod", korb_m_flt_divmod, 1);
    korb_def_cmethod(c, KORB_C_FLOAT, "negative?", korb_m_flt_neg_q, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "positive?", korb_m_flt_pos_q, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_s", korb_m_flt_to_s, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "inspect", korb_m_flt_to_s, 0);

    /* Rational */
    korb_def_cmethod(c, KORB_C_RATIONAL, "numerator", korb_m_rat_num, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "denominator", korb_m_rat_den, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_f", korb_m_rat_to_f, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_i", korb_m_rat_to_i, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_int", korb_m_rat_to_i, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_c", korb_m_num_to_c, 0);   /* Rational#to_c → Complex(self, 0) */
    korb_def_cmethod(c, KORB_C_RATIONAL, "truncate", korb_m_rat_truncate, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "floor", korb_m_rat_floor, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "ceil", korb_m_rat_ceil, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "round", korb_m_rat_round, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "zero?", korb_m_rat_zero, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "integer?", korb_m_rat_integerp, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "div", korb_m_rat_divfloor, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "divmod", korb_m_rat_divmod, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "%", korb_m_rat_mod, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "modulo", korb_m_rat_mod, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "marshal_dump", korb_m_rat_marshal_dump, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "**", korb_m_rat_pow, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "pow", korb_m_rat_pow, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_r", korb_m_rat_self, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "rationalize", korb_m_rat_rationalize, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "abs", korb_m_rat_abs, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "magnitude", korb_m_rat_abs, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "-@", korb_m_rat_neg, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "+", korb_m_rat_add, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "-", korb_m_rat_sub, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "*", korb_m_rat_mul, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "/", korb_m_rat_div, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "quo", korb_m_rat_div, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "<=>", korb_m_rat_cmp_m, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "==", korb_m_rat_eq, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "<", korb_m_num_lt, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "<=", korb_m_num_le, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, ">", korb_m_num_gt, 1);
    korb_def_cmethod(c, KORB_C_RATIONAL, ">=", korb_m_num_ge, 1);

    /* Complex */
    korb_def_cmethod(c, KORB_C_COMPLEX, "real", korb_m_cpx_real, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "imaginary", korb_m_cpx_imag, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "imag", korb_m_cpx_imag, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "conjugate", korb_m_cpx_conj, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "conj", korb_m_cpx_conj, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "abs", korb_m_cpx_abs, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "arg", korb_m_cpx_arg, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "angle", korb_m_cpx_arg, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "phase", korb_m_cpx_arg, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "polar", korb_m_cpx_to_polar, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "fdiv", korb_m_cpx_fdiv, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "magnitude", korb_m_cpx_abs, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "to_c", korb_m_cpx_self, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "+", korb_m_cpx_add, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "-", korb_m_cpx_sub, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "*", korb_m_cpx_mul, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "/", korb_m_cpx_div, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "quo", korb_m_cpx_div, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "<=>", korb_m_cpx_cmp, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "**", korb_m_cpx_pow, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "eql?", korb_m_cpx_eql, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "marshal_dump", korb_m_cpx_rect, 0);   /* [re, im] */
    korb_def_cmethod(c, KORB_C_COMPLEX, "to_r", korb_m_cpx_to_r, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "==", korb_m_cpx_eq, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "inspect", korb_m_obj_inspect, 0);

    /* Enumerator (eager) */
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "to_a", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "entries", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "force", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "size", korb_m_enum_size, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "__set_size", korb_m_enum_set_size, 1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "to_s", korb_m_enum_inspect, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "inspect", korb_m_enum_inspect, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "initialize", korb_m_enum_initialize, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each", korb_m_enum_each, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "__each_orig", korb_m_enum_each, -1);   /* prelude Enumerator#each falls back to this */
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "map", korb_m_enum_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "collect", korb_m_enum_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "with_index", korb_m_enum_with_index, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_with_index", korb_m_enum_with_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "with_object", korb_m_enum_with_object, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_with_object", korb_m_enum_with_object, 1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "__enum_mode", korb_m_enum_mode, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "next", korb_m_enum_next, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "peek", korb_m_enum_peek, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "rewind", korb_m_enum_rewind, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "next_values", korb_m_enum_next_values, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "peek_values", korb_m_enum_peek_values, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "first", korb_m_enum_first, -1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "take", korb_m_enum_take_l, 1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "drop", korb_m_enum_drop, 1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "drop_while", korb_m_enum_drop_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "select", korb_m_enum_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "find_all", korb_m_enum_select, 0);   /* find_all is an alias for select (lazy-aware) */
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "filter", korb_m_enum_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "reject", korb_m_enum_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "filter_map", korb_m_enum_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "take_while", korb_m_enum_take_while, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "compact", korb_m_enum_compact, 0);   /* Enumerator::Lazy#compact */
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "grep", korb_m_enum_grep, -1);     /* lazy-aware grep / grep_v */
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "grep_v", korb_m_enum_grep_v, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "flat_map", korb_m_enum_flat_map, 0);   /* lazy-aware flat_map */
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "collect_concat", korb_m_enum_flat_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_slice", korb_m_enum_each_slice, 1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_cons", korb_m_enum_each_cons, 1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "group_by", korb_m_enum_group_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "partition", korb_m_enum_partition, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "minmax", korb_m_enum_minmax, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "uniq", korb_m_enum_uniq, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "zip", korb_m_enum_zip, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "find_index", korb_m_enum_find_index, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "chunk_while", korb_m_enum_chunk_while, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "slice_when", korb_m_enum_slice_when, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "chunk", korb_m_enum_chunk, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "tally", korb_m_enum_tally, -1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "lazy", korb_m_to_lazy, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "lazy", korb_m_to_lazy, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "lazy", korb_m_to_lazy, 0);
    korb_def_cmethod(c, KORB_C_HASH,  "lazy", korb_m_to_lazy, 0);

    /* Enumerator::ArithmeticSequence (step / %) */
    korb_def_cmethod_blk(c, KORB_C_ARITHSEQ, "each", korb_m_aseq_each, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "to_a", korb_m_aseq_to_a, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "entries", korb_m_aseq_to_a, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "size", korb_m_aseq_size, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "first", korb_m_aseq_first, -1);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "take", korb_m_aseq_first, 1);   /* take(n) = first n elements */
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "last", korb_m_aseq_last, -1);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "begin", korb_m_aseq_begin, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "end", korb_m_aseq_end, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "step", korb_m_aseq_step_acc, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "exclude_end?", korb_m_aseq_exclude_end, 0);

    /* Set */
    korb_def_cmethod(c, KORB_C_SET, "to_a", korb_m_set_to_a, 0);
    korb_def_cmethod(c, KORB_C_SET, "to_set", korb_m_set_self, 0);
    korb_def_cmethod(c, KORB_C_SET, "size", korb_m_set_size, 0);
    korb_def_cmethod(c, KORB_C_SET, "length", korb_m_set_size, 0);
    korb_def_cmethod(c, KORB_C_SET, "count", korb_m_set_size, 0);
    korb_def_cmethod(c, KORB_C_SET, "empty?", korb_m_set_empty, 0);
    korb_def_cmethod(c, KORB_C_SET, "include?", korb_m_set_include, 1);
    korb_def_cmethod(c, KORB_C_SET, "member?", korb_m_set_include, 1);
    korb_def_cmethod(c, KORB_C_SET, "disjoint?", korb_m_set_disjoint, 1);
    korb_def_cmethod(c, KORB_C_SET, "intersect?", korb_m_set_intersect, 1);
    korb_def_cmethod(c, KORB_C_SET, "===", korb_m_set_include, 1);
    korb_def_cmethod(c, KORB_C_SET, "add", korb_m_set_add, 1);
    korb_def_cmethod(c, KORB_C_SET, "<<", korb_m_set_add, 1);
    korb_def_cmethod(c, KORB_C_SET, "add?", korb_m_set_add_q, 1);
    korb_def_cmethod(c, KORB_C_SET, "delete", korb_m_set_delete, 1);
    korb_def_cmethod(c, KORB_C_SET, "delete?", korb_m_set_delete_q, 1);
    korb_def_cmethod_blk(c, KORB_C_SET, "each", korb_m_set_each, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "map", korb_m_set_map, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "collect", korb_m_set_map, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "select", korb_m_set_select, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "filter", korb_m_set_select, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "reject", korb_m_set_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "find", korb_m_set_find, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "detect", korb_m_set_find, 0);
    korb_def_cmethod_blk(c, KORB_C_SET, "sort", korb_m_set_sort, 0);
    korb_def_cmethod(c, KORB_C_SET, "sum", korb_m_set_sum, -1);
    korb_def_cmethod_blk(c, KORB_C_SET, "minmax", korb_m_set_minmax, 0);
    korb_def_cmethod(c, KORB_C_SET, "|", korb_m_set_union, 1);
    korb_def_cmethod(c, KORB_C_SET, "union", korb_m_set_union, 1);
    korb_def_cmethod(c, KORB_C_SET, "+", korb_m_set_union, 1);
    korb_def_cmethod(c, KORB_C_SET, "merge", korb_m_set_merge, -1);
    korb_def_cmethod(c, KORB_C_SET, "join", korb_m_set_join, -1);
    korb_def_cmethod(c, KORB_C_SET, "&", korb_m_set_inter, 1);
    korb_def_cmethod(c, KORB_C_SET, "intersection", korb_m_set_inter, 1);
    korb_def_cmethod(c, KORB_C_SET, "-", korb_m_set_diff, 1);
    korb_def_cmethod(c, KORB_C_SET, "difference", korb_m_set_diff, 1);
    korb_def_cmethod(c, KORB_C_SET, "^", korb_m_set_xor, 1);
    korb_def_cmethod(c, KORB_C_SET, "subset?", korb_m_set_subset, 1);
    korb_def_cmethod(c, KORB_C_SET, "<=", korb_m_set_subset, 1);
    korb_def_cmethod(c, KORB_C_SET, "superset?", korb_m_set_superset, 1);
    korb_def_cmethod(c, KORB_C_SET, ">=", korb_m_set_superset, 1);
    korb_def_cmethod(c, KORB_C_SET, "<", korb_m_set_psubset, 1);
    korb_def_cmethod(c, KORB_C_SET, ">", korb_m_set_psuperset, 1);
    korb_def_cmethod(c, KORB_C_SET, "proper_subset?", korb_m_set_psubset, 1);
    korb_def_cmethod(c, KORB_C_SET, "proper_superset?", korb_m_set_psuperset, 1);
    korb_def_cmethod(c, KORB_C_SET, "==", korb_m_set_eq, 1);
    korb_def_cmethod(c, KORB_C_SET, "<=>", korb_m_set_cmp, 1);
    korb_def_cmethod(c, KORB_C_SET, "compare_by_identity", korb_m_set_cbi, 0);
    korb_def_cmethod(c, KORB_C_SET, "compare_by_identity?", korb_m_set_cbi_p, 0);
    korb_def_cmethod(c, KORB_C_SET, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_SET, "inspect", korb_m_obj_to_s, 0);   /* #inspect is an alias of #to_s (same rfn → ==) */
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "to_set", korb_m_ary_to_set, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_set", korb_m_hash_to_set, 0);
    /* Array/Hash: #to_s is an alias of #inspect (same rfn → instance_method ==), and
     * both are owned by Array/Hash (not Object).  Container path renders "[1, 2]" / "{a: 1}". */
    korb_def_cmethod(c, KORB_C_ARRAY, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "to_s", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_HASH, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_s", korb_m_obj_inspect, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "to_set", korb_m_range_to_set, 0);

    /* Integer/Float → Complex helpers */
    korb_def_cmethod(c, KORB_C_INTEGER, "i", korb_m_int_i, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "conj", korb_m_num_conj_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "conjugate", korb_m_num_conj_self, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "real?", korb_m_num_real_p, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "to_c", korb_m_num_to_c, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "polar", korb_m_num_polar, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "rect", korb_m_num_rect, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "rectangular", korb_m_num_rect, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "infinite?", korb_m_lit_nil, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "i", korb_m_int_i, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "conj", korb_m_num_conj_self, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "conjugate", korb_m_num_conj_self, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "real?", korb_m_num_real_p, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "to_c", korb_m_num_to_c, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "polar", korb_m_num_polar, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "rect", korb_m_num_rect, 0);
    korb_def_cmethod(c, KORB_C_FLOAT, "rectangular", korb_m_num_rect, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "real?", korb_m_lit_false, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "integer?", korb_m_lit_false, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "abs2", korb_m_cpx_abs2, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "infinite?", korb_m_cpx_infinite, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "numerator", korb_m_cpx_numerator, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "denominator", korb_m_cpx_denominator, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "rationalize", korb_m_cpx_rationalize, -1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "rect", korb_m_cpx_rect, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "rectangular", korb_m_cpx_rect, 0);
}

/* ---------------------------------------------------------------------------
 * Printing (CRuby-compatible to_s / inspect, written directly — no GC).
 * ------------------------------------------------------------------------- */

/* Inspect-quote a byte run: "..." with CRuby's escape set. */
static void
korb_fprint_quoted_enc(FILE *fp, const char *bytes, uint32_t len, bool binary)
{
    fputc('"', fp);
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)bytes[i];
        switch (ch) {
          case '"':  fputs("\\\"", fp); break;
          case '\\': fputs("\\\\", fp); break;
          case '\n': fputs("\\n", fp); break;
          case '\t': fputs("\\t", fp); break;
          case '\r': fputs("\\r", fp); break;
          case '\f': fputs("\\f", fp); break;
          case '\v': fputs("\\v", fp); break;
          case '\b': fputs("\\b", fp); break;
          case '\a': fputs("\\a", fp); break;
          case 27:   fputs("\\e", fp); break;
          case '#':
            if (i + 1 < len && (bytes[i+1] == '{' || bytes[i+1] == '$' || bytes[i+1] == '@'))
                fputs("\\#", fp);
            else
                fputc('#', fp);
            break;
          default:
            if (binary) {         /* ASCII-8BIT: printable ASCII as-is, everything else \xHH */
                if (ch < 0x20 || ch >= 0x7f) fprintf(fp, "\\x%02X", ch);
                else fputc(ch, fp);
            }
            else if (ch < 0x20 || ch == 0x7f) { fprintf(fp, "\\u%04X", ch); }
            else if (ch < 0x80) { fputc(ch, fp); }
            else {                /* >= 0x80: pass valid UTF-8 through, escape invalid bytes as \xHH */
                int n = (ch >= 0xF0) ? 3 : (ch >= 0xE0) ? 2 : (ch >= 0xC0) ? 1 : -1;
                bool valid = n >= 0 && i + (uint32_t)n < len;
                for (int k = 1; valid && k <= n; k++) if (((unsigned char)bytes[i+k] & 0xC0) != 0x80) valid = false;
                if (valid) { fputc(ch, fp); for (int k = 1; k <= n; k++) fputc(bytes[i+k], fp); i += (uint32_t)n; }
                else fprintf(fp, "\\x%02X", ch);
            }
        }
    }
    fputc('"', fp);
}
static void korb_fprint_quoted(FILE *fp, const char *bytes, uint32_t len) { korb_fprint_quoted_enc(fp, bytes, len, false); }

/* Can a symbol name appear bare as a hash label `name:`?  CRuby 3.4+: an
 * identifier optionally ending in ? or ! (not =, not empty, not operator). */
/* identifier chars: ASCII letters/digits/_ plus any non-ASCII byte (>=0x80),
 * so Unicode identifiers like :äöü print bare (no quoting), matching CRuby. */
static inline bool korb_id_start(unsigned char ch) { return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch >= 0x80; }
static inline bool korb_id_cont(unsigned char ch)  { return korb_id_start(ch) || (ch >= '0' && ch <= '9'); }
static bool
korb_sym_label_bare(const char *nm)
{
    if (!korb_id_start((unsigned char)*nm)) return false;
    const char *p = nm + 1;
    while (korb_id_cont((unsigned char)*p)) p++;
    if (*p == '?' || *p == '!') p++;
    return *p == '\0';
}
/* Symbol prints bare in inspect (`:foo`, `:+`) vs quoted (`:"a b"`, `:"123"`). */
static bool
korb_sym_inspect_bare(const char *nm)
{
    if (korb_sym_label_bare(nm)) return true;
    {   /* @ivar / @@cvar / $global names print bare */
        const char *p = nm;
        if (*p == '$') p++;
        else if (*p == '@') { p++; if (*p == '@') p++; }
        if (p != nm && (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            const char *q = p + 1;
            while (*q == '_' || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')) q++;
            if (*q == '\0') return true;
        }
    }
    if (nm[0] == '$' && nm[1] != '\0') {                  /* special global-variable symbols print bare */
        const char *p = nm + 1;
        if (*p >= '0' && *p <= '9') {                     /* $0, $1, $23 — program name / match groups */
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '\0') return true;
        }
        if (p[0] == '-' && p[1] != '\0' && p[2] == '\0') return true;   /* $-w, $-I — one-char flag */
        if (p[0] != '\0' && p[1] == '\0' && strchr("!@&`'+~=/\\,;.<>*$?:\"", p[0])) return true;   /* $! $~ $+ $: ... */
    }
    if (nm[0] == '_' || (nm[0] >= 'a' && nm[0] <= 'z') || (nm[0] >= 'A' && nm[0] <= 'Z')) {
        const char *p = nm + 1;                      /* identifier with trailing '=' (setter) */
        while (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) p++;
        if (*p == '=' && p[1] == '\0') return true;
    }
    static const char *const ops[] = {
        "+","-","*","/","%","**","==","!=","<","<=",">",">=","<=>","<<",">>",
        "&","|","^","~","!","[]","[]=","+@","-@","===","=~","!~","`", NULL };
    for (int i = 0; ops[i]; i++) if (strcmp(nm, ops[i]) == 0) return true;
    return false;
}

/* Range: "1..5" / "1...5"; endpoints to_s for to_s, inspect for inspect.
 * For inspect, an unbounded (nil) endpoint is elided — CRuby prints beg unless
 * (beg nil && end non-nil), and end unless (end nil && beg non-nil): so
 * (1..) => "1..", (..5) => "..5", but (nil..nil) => "nil..nil".  to_s renders
 * both endpoints via to_s, where nil.to_s == "" (so (nil..nil) => ".."). */
static void
korb_fprint_range(CTX *c, FILE *fp, VALUE v, bool insp)
{
    const KorbRange *r = VAL2RANGE(v);
    const bool beg_nil = (r->rbegin == KORB_NIL), end_nil = (r->rend == KORB_NIL);
    if (insp) {
        if (!beg_nil || end_nil) korb_fprint_inspect(c, fp, r->rbegin);
        fputs(r->exclude_end ? "..." : "..", fp);
        if (!end_nil || beg_nil) korb_fprint_inspect(c, fp, r->rend);
    }
    else {
        korb_fprint_to_s(c, fp, r->rbegin);
        fputs(r->exclude_end ? "..." : "..", fp);
        korb_fprint_to_s(c, fp, r->rend);
    }
}

/* Self-referential containers (`a=[]; a<<a; a.inspect`) would recurse forever.
 * Element rendering carries a depth; once it crosses KORB_PRINT_DEPTH_MAX the
 * container collapses to its `...` marker (CRuby shows the marker at the exact
 * cycle point; a depth cap reaches it a few levels deeper — both contain the
 * "[...]" / "{...}" / "Set[...]" substring the specs check, and neither loops). */
#define KORB_PRINT_DEPTH_MAX 48
static void korb_fprint_inspect_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth);

/* Render a plain user object via its (possibly overridden) #inspect.  Returns
 * true if it dispatched and wrote the result; false if the caller should use the
 * default "#<Class>" form.  `slots` must be a rooted region (>= 4 slots); NULL
 * disables dispatch (the slots-less formatter callers keep the default).  The
 * dispatch can GC, so callers re-read any container they are iterating. */
static bool
korb_fprint_user_inspect(CTX *c, VALUE *slots, FILE *fp, VALUE v)
{
    if (slots == NULL || !KORB_OBJECT_P(v) || VAL2OBJ(v)->klass == KORB_NIL) return false;
    slots[0] = v;                                                /* root across the dispatch */
    RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "inspect", 7), 0, 0, NULL, NULL, NULL);
    if (r.state != KORB_NORMAL) return false;
    if (!KORB_STRING_P(r.value)) {                               /* non-String #inspect → coerce via #to_s (never #to_str) */
        slots[0] = r.value;
        RESULT r2 = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
        if (r2.state != KORB_NORMAL || !KORB_STRING_P(r2.value)) return false;   /* → default representation */
        fwrite(korb_strbuf_data(VAL2STR(r2.value)->buf), 1, VAL2STR(r2.value)->len, fp);
        return true;
    }
    fwrite(korb_strbuf_data(VAL2STR(r.value)->buf), 1, VAL2STR(r.value)->len, fp);
    return true;
}

/* Array renders identically for to_s and inspect: "[1, 2, \"x\"]" — elements
 * always use inspect form.  `slots` (or NULL) threads through for element
 * #inspect dispatch; the array is re-read each step (dispatch may move it). */
static void
korb_fprint_ary_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth)
{
    if (UNLIKELY(VAL2ARY(v)->head.flags & KORB_FL_JOIN_VISITING)) { fputs("[...]", fp); return; }   /* recursive → [...] */
    if (UNLIKELY(depth >= KORB_PRINT_DEPTH_MAX)) { fputs("[...]", fp); return; }
    if (slots) slots[0] = v;                                     /* root the array across element dispatch */
    (slots ? VAL2ARY(slots[0]) : VAL2ARY(v))->head.flags |= KORB_FL_JOIN_VISITING;
    fputc('[', fp);
    for (uint32_t i = 0; i < (slots ? VAL2ARY(slots[0]) : VAL2ARY(v))->len; i++) {
        if (i) fputs(", ", fp);
        korb_fprint_inspect_d(c, slots ? slots + 1 : NULL, fp,korb_items_data((slots ? VAL2ARY(slots[0]) : VAL2ARY(v))->items)[i], depth + 1);
    }
    fputc(']', fp);
    (slots ? VAL2ARY(slots[0]) : VAL2ARY(v))->head.flags &= ~KORB_FL_JOIN_VISITING;   /* re-deref: element dispatch may have moved it */
}
static void korb_fprint_ary(CTX *c, VALUE *slots, FILE *fp, VALUE v) { korb_fprint_ary_d(c, slots, fp, v, 0); }

/* Hash inspect (== to_s), CRuby 4.0 form: symbol keys as `name: v` (quoted if
 * not a bare label), other keys as `k => v`. */
static void
korb_fprint_hash_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth)
{
    if (UNLIKELY(VAL2HASH(v)->head.flags & KORB_FL_JOIN_VISITING)) { fputs("{...}", fp); return; }   /* recursive → {...} */
    if (UNLIKELY(depth >= KORB_PRINT_DEPTH_MAX)) { fputs("{...}", fp); return; }
    if (slots) slots[0] = v;                                     /* root the hash across k/v dispatch */
    (slots ? VAL2HASH(slots[0]) : VAL2HASH(v))->head.flags |= KORB_FL_JOIN_VISITING;
    fputc('{', fp);
    for (uint32_t i = 0; i < (slots ? VAL2HASH(slots[0]) : VAL2HASH(v))->len; i++) {
        if (i) fputs(", ", fp);
        const KorbHash *h = slots ? VAL2HASH(slots[0]) : VAL2HASH(v);   /* re-read each step */
        VALUE k = korb_items_data(h->items)[2 * i];
        if (SYMBOL_P(k)) {
            const char *nm = korb_sym_name(c->vm, SYM2ID(k));
            if (korb_sym_label_bare(nm)) fputs(nm, fp);
            else korb_fprint_quoted(fp, nm, (uint32_t)strlen(nm));
            fputs(": ", fp);
        } else {
            korb_fprint_inspect_d(c, slots ? slots + 1 : NULL, fp, k, depth + 1);
            fputs(" => ", fp);
        }
        h = slots ? VAL2HASH(slots[0]) : VAL2HASH(v);            /* re-read after key dispatch */
        korb_fprint_inspect_d(c, slots ? slots + 1 : NULL, fp, korb_items_data(h->items)[2 * i + 1], depth + 1);
    }
    fputc('}', fp);
    (slots ? VAL2HASH(slots[0]) : VAL2HASH(v))->head.flags &= ~KORB_FL_JOIN_VISITING;
}
static void korb_fprint_hash(CTX *c, VALUE *slots, FILE *fp, VALUE v) { korb_fprint_hash_d(c, slots, fp, v, 0); }

/* Set[a, b, c] (elements via inspect), depth-guarded like Array/Hash. */
static void
korb_fprint_set_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth)
{
    if (UNLIKELY(depth >= KORB_PRINT_DEPTH_MAX)) { fputs("Set[...]", fp); return; }
    if (slots) slots[0] = v;
    fputs("Set[", fp);
    for (uint32_t i = 0; i < (slots ? VAL2ARY(VAL2SET(slots[0])->elems) : VAL2ARY(VAL2SET(v)->elems))->len; i++) {
        if (i) fputs(", ", fp);
        const KorbArray *el = slots ? VAL2ARY(VAL2SET(slots[0])->elems) : VAL2ARY(VAL2SET(v)->elems);
        korb_fprint_inspect_d(c, slots ? slots + 1 : NULL, fp, korb_items_data(el->items)[i], depth + 1);
    }
    fputc(']', fp);
}

/* Element-rendering dispatcher: cycle-prone containers carry the depth; every
 * other value renders normally (it cannot contain a back-reference, so no loop). */
static void
korb_fprint_inspect_d(CTX *c, VALUE *slots, FILE *fp, VALUE v, int depth)
{
    if (AROH_IS_GC_OBJECT(v)) {
        switch (KORB_OBJ_TYPE(v)) {
          case KORB_OBJ_ARRAY: korb_fprint_ary_d(c, slots, fp, v, depth);  return;
          case KORB_OBJ_HASH:  korb_fprint_hash_d(c, slots, fp, v, depth); return;
          case KORB_OBJ_SET:   korb_fprint_set_d(c, slots, fp, v, depth);  return;
          case KORB_OBJ_OBJECT: {                                   /* Struct/Data element → "#<struct Name f=v, …>" */
            const VALUE klass = VAL2OBJ(v)->klass;
            if (klass != KORB_NIL && KORB_ARRAY_P(VAL2CLASS(klass)->members)) {
                const KorbClass *const k = VAL2CLASS(klass);
                if (UNLIKELY(depth >= KORB_PRINT_DEPTH_MAX)) {      /* cycle safety net (direct self-refs are handled precisely upstream) */
                    fputs(k->is_data ? "#<data ...>" : "#<struct ...>", fp); return;
                }
                fputc('#', fp); fputc('<', fp); fputs(k->is_data ? "data" : "struct", fp);
                if (k->name_sym) { fputc(' ', fp); fputs(korb_sym_name(c->vm, k->name_sym), fp); }
                const KorbArray *const mem = VAL2ARY(k->members);    /* no GC below (formatter only writes to fp) */
                for (uint32_t i = 0; i < mem->len; i++) {
                    const VALUE msym = korb_items_data(mem->items)[i];
                    const VALUE mval = korb_ivar_get(c, v, korb_member_ivar_sym(c->vm, msym));
                    fputs(i == 0 ? " " : ", ", fp);
                    fputs(korb_sym_name(c->vm, SYM2ID(msym)), fp);
                    fputc('=', fp);
                    korb_fprint_inspect_d(c, slots ? slots + 1 : NULL, fp, mval, depth + 1);
                }
                fputc('>', fp);
                return;
            }
            if (korb_fprint_user_inspect(c, slots, fp, v)) return;   /* plain object → its (overridable) #inspect */
            break;
          }
          default: break;
        }
    }
    korb_fprint_inspect_s(c, slots, fp, v);
}

void korb_fprint_to_s(CTX *c, FILE *fp, VALUE v);   /* wrapper (slots-less → no element dispatch); defined below */
static void
korb_fprint_to_s_s(CTX *c, VALUE *slots, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))           { fprintf(fp, "%lld", (long long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)         { return; }                     /* "" */
    if (v == KORB_TRUE)        { fputs("true", fp); return; }
    if (v == KORB_FALSE)       { fputs("false", fp); return; }
    if (SYMBOL_P(v))           { fputs(korb_sym_name(c->vm, SYM2ID(v)), fp); return; }
    if (KORB_FLOAT_P(v))       { char b[40]; korb_float_to_s(korb_float_val(v), b); fputs(b, fp); return; }   /* flonum or heap */
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_BIGNUM: {
        char *s = korb_mp_get_str(NULL, 10, VAL2BIG(v)->z);   /* GMP-malloc'd */
        fputs(s, fp); free(s);
        return;
      }
      case KORB_OBJ_STRING: {
        const KorbString *s = VAL2STR(v);
        fwrite(korb_strbuf_data(s->buf), 1, s->len, fp);
        return;
      }
      case KORB_OBJ_ARRAY:
        korb_fprint_ary(c, slots, fp, v);
        return;
      case KORB_OBJ_HASH:
        korb_fprint_hash(c, slots, fp, v);
        return;
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, false);
        return;
      case KORB_OBJ_OBJECT: {
        const KorbObject *o = VAL2OBJ(v);
        if (o->klass == KORB_NIL) { fputs("main", fp); return; }       /* top-level self */
        /* a user #to_s wins over the C printer (Encoding#to_s, and any object a
         * program renders with `print`); only possible when we have slots */
        if (slots != NULL) {
            VALUE def = KORB_NIL;
            const struct korb_method *const m = korb_mcache_find(c->vm, o->klass, korb_intern(c->vm, "to_s", 4), &def);
            if (m != NULL && m->kind != KORB_METHOD_CFUNC) {   /* a CFUNC to_s is the default → it calls back here */
                slots[0] = v;
                const RESULT tsr = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
                if (tsr.state == KORB_NORMAL && KORB_STRING_P(tsr.value)) {
                    const KorbString *const ts = VAL2STR(tsr.value);
                    fwrite(korb_strbuf_data(ts->buf), 1, ts->len, fp);
                    return;
                }
            }
        }
        fputs("#<", fp);
        if (!korb_fprint_class_qname(c, fp, o->klass)) fputs("Class", fp);   /* qualified (M::C); anonymous fallback */
        fprintf(fp, ":0x%016lx>", (unsigned long)(uintptr_t)v);   /* to_s: "#<Foo:0x…>" (no ivars) */
        return;
      }
      case KORB_OBJ_CLASS:
        korb_fprint_class_tostr(c, fp, v);   /* qualified name (M::C), `#<Class:obj>` for a singleton, else `#<Class:0x…>` */
        return;
      case KORB_OBJ_FLOAT: {
        char fb[40];
        korb_float_to_s(korb_float_val(v), fb);
        fputs(fb, fp);
        return;
      }
      case KORB_OBJ_RATIONAL:                            /* to_s: n/d (n,d may be Bignum) */
        korb_fprint_to_s(c, fp, VAL2RAT(v)->num); fputc('/', fp); korb_fprint_to_s(c, fp, VAL2RAT(v)->den);
        return;
      case KORB_OBJ_COMPLEX: {                          /* to_s: re±|im|i */
        const KorbComplex *x = VAL2CPX(v);
        korb_fprint_to_s(c, fp, x->re);
        char *ib = NULL; size_t isz = 0; FILE *ims = open_memstream(&ib, &isz);
        if (ims) { korb_fprint_to_s(c, ims, x->im); fclose(ims); }
        const char *const isuf = (KORB_FLOAT_P(x->im) && !isfinite(korb_float_val(x->im))) ? "*i" : "i";   /* Infinity/NaN → *i (parseable) */
        if (ib && ib[0] == '-') fprintf(fp, "-%s%s", ib + 1, isuf);
        else                    fprintf(fp, "+%s%s", ib ? ib : "0", isuf);
        free(ib);
        return;
      }
      case KORB_OBJ_EXCEPTION: {
        const KorbException *e = VAL2EXC(v);
        if (e->msg != KORB_NIL) fwrite(korb_strbuf_data(VAL2STR(e->msg)->buf), 1, VAL2STR(e->msg)->len, fp);
        else fputs(korb_etype_name(e->etype), fp);
        return;
      }
      case KORB_OBJ_ENUMERATOR: {
        const KorbEnumerator *e = VAL2ENUM(v);
        if (KORB_STRING_P(e->desc)) fwrite(korb_strbuf_data(VAL2STR(e->desc)->buf), 1, VAL2STR(e->desc)->len, fp);
        else fputs("#<Enumerator>", fp);
        return;
      }
      case KORB_OBJ_SET:                               /* Set[a, b, c] (elements via inspect) */
        korb_fprint_set_d(c, slots, fp, v, 0);
        return;
      case KORB_OBJ_PROC: {                            /* #<Proc:0x.. file:line (lambda)> */
        const KorbProc *const p = VAL2PROC(v);
        const char *const lam = p->is_lambda ? " (lambda)" : "";
        uint32_t fsym, line;
        if (p->iseq && korb_get_srcloc(c->vm, p->iseq, &fsym, &line))
            fprintf(fp, "#<Proc:0x%016lx %s:%u%s>", (unsigned long)(uintptr_t)v, korb_sym_name(c->vm, fsym), line, lam);
        else if (p->iseq == NULL && p->self == KORB_NIL)   /* Symbol#to_proc → #<Proc:0x..(&:name) (lambda)> */
            fprintf(fp, "#<Proc:0x%016lx(&:%s)%s>", (unsigned long)(uintptr_t)v, korb_sym_name(c->vm, p->sym_mid), lam);
        else
            fprintf(fp, "#<Proc:0x%016lx%s>", (unsigned long)(uintptr_t)v, lam);
        return;
      }
      case KORB_OBJ_METHOD: {                          /* #<Method: Recv(DefiningModule)#name> */
        const KorbMethod *const m = (const KorbMethod *)(uintptr_t)v;
        const VALUE recv_cls = m->unbound ? m->recv : korb_class_obj_of(c, m->recv);
        fprintf(fp, "#<%s: ", m->unbound ? "UnboundMethod" : "Method");
        if (KORB_CLASS_P(recv_cls)) korb_fprint_class_qname(c, fp, recv_cls); else fputs("Object", fp);
        /* the module/class that actually defines the method, if different from the receiver class */
        VALUE def_cls = KORB_NIL;
        const struct korb_method *km = KORB_CLASS_P(recv_cls) ? korb_class_find_method(recv_cls, m->mid, &def_cls) : NULL;
        if (km == NULL) km = korb_method_lookup(c->vm, m->mid);   /* top-level (global function) method */
        if (KORB_CLASS_P(def_cls) && def_cls != recv_cls) { fputc('(', fp); korb_fprint_class_qname(c, fp, def_cls); fputc(')', fp); }
        fprintf(fp, "#%s", korb_sym_name(c->vm, m->mid));
        if (km != NULL && km->orig_mid && km->orig_mid != km->mid)   /* aliased → #renamed(original) */
            fprintf(fp, "(%s)", korb_sym_name(c->vm, km->orig_mid));
        /* parameter signature + source location for a Ruby-defined method (CRuby shape) */
        if (km != NULL && km->kind == KORB_METHOD_ISEQ) {
            const struct korb_param_info *const pi = (const struct korb_param_info *)km->param_info;
            fputc('(', fp);
            for (uint32_t i = 0; pi != NULL && i < pi->n; i++) {
                if (i) fputs(", ", fp);
                const struct korb_param_entry *const e = &pi->e[i];
                const char *const nm = e->name ? korb_sym_name(c->vm, e->name) : "";
                switch (e->kind) {
                  case 0: fputs(nm, fp); break;                 /* required */
                  case 1: fprintf(fp, "%s=...", nm); break;     /* optional */
                  case 2: fprintf(fp, "*%s", nm); break;        /* rest */
                  case 3: fprintf(fp, "%s:", nm); break;        /* keyreq */
                  case 4: fprintf(fp, "%s: ...", nm); break;    /* key */
                  case 5: fprintf(fp, "**%s", nm); break;       /* keyrest */
                  case 6: fprintf(fp, "&%s", nm); break;        /* block */
                }
            }
            fputc(')', fp);
            uint32_t fsym, line;
            if (km->body && korb_get_srcloc(c->vm, km->body, &fsym, &line))
                fprintf(fp, " %s:%u", korb_sym_name(c->vm, fsym), line);
        }
        fputc('>', fp);
        return;
      }
    }
    fputs("#<Object>", fp);
}
void korb_fprint_to_s(CTX *c, FILE *fp, VALUE v) { korb_fprint_to_s_s(c, NULL, fp, v); }

void
korb_fprint_inspect_s(CTX *c, VALUE *slots, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))     { fprintf(fp, "%lld", (long long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)   { fputs("nil", fp); return; }
    if (v == KORB_TRUE)  { fputs("true", fp); return; }
    if (v == KORB_FALSE) { fputs("false", fp); return; }
    if (SYMBOL_P(v)) {
        const char *nm = korb_sym_name(c->vm, SYM2ID(v));
        if (korb_sym_inspect_bare(nm)) fprintf(fp, ":%s", nm);
        else { fputc(':', fp); korb_fprint_quoted(fp, nm, (uint32_t)strlen(nm)); }
        return;
    }
    if (KORB_FLOAT_P(v)) { char b[40]; korb_float_to_s(korb_float_val(v), b); fputs(b, fp); return; }   /* flonum or heap */
    switch (KORB_OBJ_TYPE(v)) {
      case KORB_OBJ_STRING: {
        const KorbString *s = VAL2STR(v);
        /* Non-UTF-8 (ASCII-8BIT or US-ASCII) inspects control/high bytes as \xNN;
         * UTF-8 passes multibyte through / uses \uNNNN. */
        bool binary = KORB_STR_ENC(v) != KORB_ENC_UTF8;   /* non-UTF-8 → \xNN escaping */
        korb_fprint_quoted_enc(fp, korb_strbuf_data(s->buf), s->len, binary);
        return;
      }
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, true);   /* inspect endpoints */
        return;
      case KORB_OBJ_REGEXP: {                            /* inspect: /source/opts */
        const KorbRegexp *const re = VAL2RE(v);
        const KorbString *const src = KORB_STRING_P(re->source) ? VAL2STR(re->source) : NULL;
        fputc('/', fp);
        if (src) {   /* an unescaped '/' in the source is escaped in the literal form */
            const char *const d = korb_strbuf_data(src->buf);
            for (uint32_t i = 0; i < src->len; i++) {
                if (d[i] == '/' && (i == 0 || d[i - 1] != '\\')) fputc('\\', fp);
                fputc(d[i], fp);
            }
        }
        fputc('/', fp);
        if (re->flags & 16u) fputc('m', fp);             /* prism: MULTI_LINE */
        if (re->flags & 4u)  fputc('i', fp);             /* IGNORE_CASE */
        if (re->flags & 8u)  fputc('x', fp);             /* EXTENDED */
        return;
      }
      case KORB_OBJ_RATIONAL:                            /* inspect: (n/d) */
        fputc('(', fp); korb_fprint_to_s(c, fp, VAL2RAT(v)->num); fputc('/', fp); korb_fprint_to_s(c, fp, VAL2RAT(v)->den); fputc(')', fp);
        return;
      case KORB_OBJ_MATCHDATA: {                        /* inspect: #<MatchData "g0" 1:"g1" 2:"g2"> */
        const KorbMatchData *md = VAL2MD(v);
        const KorbString *subj = VAL2STR(md->subject);
        const KorbArray *off = VAL2ARY(md->offsets);
        fputs("#<MatchData ", fp);
        for (uint32_t i = 0; 2 * i + 1 < off->len; i++) {
            const long b = FIX2LONG(korb_items_data(off->items)[2 * i]), e = FIX2LONG(korb_items_data(off->items)[2 * i + 1]);
            if (i) fprintf(fp, " %u:", i);
            if (b >= 0) korb_fprint_quoted_enc(fp, korb_strbuf_data(subj->buf) + b, (uint32_t)(e - b), false);
            else fputs("nil", fp);
        }
        fputc('>', fp);
        return;
      }
      case KORB_OBJ_COMPLEX: {                          /* inspect: (re±|im|i); compound (Rational) parts get parens + *i */
        const KorbComplex *x = VAL2CPX(v);
        const bool re_comp = KORB_RATIONAL_P(x->re) || KORB_COMPLEX_P(x->re);
        const bool im_comp = KORB_RATIONAL_P(x->im) || KORB_COMPLEX_P(x->im);
        fputc('(', fp);
        if (re_comp) fputc('(', fp);
        korb_fprint_to_s(c, fp, x->re);
        if (re_comp) fputc(')', fp);
        char *ib = NULL; size_t isz = 0; FILE *ims = open_memstream(&ib, &isz);
        if (ims) { korb_fprint_to_s(c, ims, x->im); fclose(ims); }
        const bool neg = ib && ib[0] == '-';
        const char *mag = neg ? ib + 1 : (ib ? ib : "0");
        fputc(neg ? '-' : '+', fp);
        if (im_comp) { fputc('(', fp); fputs(mag, fp); fputs(")*i", fp); }
        else fprintf(fp, "%s%s", mag, (KORB_FLOAT_P(x->im) && !isfinite(korb_float_val(x->im))) ? "*i" : "i");
        free(ib);
        fputc(')', fp);
        return;
      }
      case KORB_OBJ_ARITHSEQ: {                         /* inspect: (recv.step(args)) / (recv.%(arg)) */
        const KorbArithSeq *as = VAL2ASEQ(v);
        const bool rng = KORB_RANGE_P(as->recv);
        fputc('(', fp);
        if (rng) fputc('(', fp);
        korb_fprint_inspect(c, fp, as->recv);
        if (rng) fputc(')', fp);
        fputs(as->is_pct ? ".%" : ".step", fp);
        if (as->nargs >= 1) {
            fputc('(', fp);
            korb_fprint_inspect(c, fp, as->a0);
            if (as->nargs >= 2) { fputs(", ", fp); korb_fprint_inspect(c, fp, as->a1); }
            fputc(')', fp);
        }
        fputc(')', fp);
        return;
      }
      case KORB_OBJ_EXCEPTION: {                         /* inspect: #<Class: message> (bare Class when empty) */
        const KorbException *const e = VAL2EXC(v);
        const char *const cn = (e->exc_class != KORB_NIL && KORB_CLASS_P(e->exc_class))
                                   ? korb_sym_name(c->vm, VAL2CLASS(e->exc_class)->name_sym)
                                   : korb_etype_name(e->etype);
        const char *msg; size_t mlen;
        if (e->msg != KORB_NIL && KORB_STRING_P(e->msg)) { msg = korb_strbuf_data(VAL2STR(e->msg)->buf); mlen = VAL2STR(e->msg)->len; }
        else { msg = cn; mlen = strlen(cn); }            /* default message = class name */
        if (mlen == 0) fputs(cn, fp);
        else { fprintf(fp, "#<%s: ", cn); fwrite(msg, 1, mlen, fp); fputc('>', fp); }
        return;
      }
    }
    korb_fprint_to_s_s(c, slots, fp, v);
}
void korb_fprint_inspect(CTX *c, FILE *fp, VALUE v) { korb_fprint_inspect_s(c, NULL, fp, v); }

/* ---------------------------------------------------------------------------
 * Builtins.
 * ------------------------------------------------------------------------- */

/* puts one value, newline-terminated; arrays flatten recursively (each element
 * on its own line), matching CRuby.  An empty array prints nothing.  User
 * objects dispatch their to_s method (slots-threaded, may GC). */
static RESULT
korb_puts_one_to(CTX *c, VALUE *slots, VALUE v, struct KorbIORep *rep)
{
    if (KORB_ARRAY_P(v)) {
        if (VAL2ARY(v)->head.flags & KORB_FL_JOIN_VISITING) {   /* recursive array → CRuby prints "[...]" */
            CHECK(korb_io_wr_checked(c, slots, rep, "[...]\n", 6));
            return RESULT_OK(KORB_NIL);
        }
        slots[0] = v;                                   /* root across to_s GC in recursion */
        VAL2ARY(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
        RESULT r = RESULT_OK(KORB_NIL);
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            r = korb_puts_one_to(c, slots + 1, korb_items_data(VAL2ARY(slots[0])->items)[i], rep);
            if (UNLIKELY(r.state != KORB_NORMAL)) break;
        }
        VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;   /* re-deref: source may have moved */
        return r;
    }
    if (KORB_OBJECT_P(v) && !KORB_STRING_P(v)) {
        /* CRuby asks a non-String for #to_ary first (printing the elements one
         * per line) and only then falls back to #to_s. */
        VALUE av = v;
        slots[0] = v;                                   /* root across the coercion dispatch */
        RESULT ar = korb_coerce_to_ary(c, slots + 1, &av);
        if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        if (ar.value == KORB_TRUE) return korb_puts_one_to(c, slots + 1, av, rep);
        slots[0] = v;
        RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_STRING_P(r.value)) {                  /* a #to_s that is not a String → the default "#<Class:0x…>" */
            char b[160];
            const int bn = snprintf(b, sizeof b, "#<%s:0x%016lx>\n", korb_coerce_name(c, slots[0]), (unsigned long)(uintptr_t)slots[0]);
            return korb_io_wr_checked(c, slots + 1, rep, b, (size_t)bn);
        }
        v = r.value;                                    /* fall through to print the string */
    }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        CHECK(korb_io_wr_checked(c, slots, rep, korb_strbuf_data(s->buf), s->len));
        if (s->len == 0 || korb_strbuf_data(s->buf)[s->len - 1] != '\n')
            CHECK(korb_io_wr_checked(c, slots, rep, "\n", 1));
        return RESULT_OK(KORB_NIL);
    }
    /* Everything else renders through the FILE*-based printer, which builds a
     * string in memory (open_memstream — no descriptor involved) before the one
     * write below.  Keeping that split means the printers stay untouched. */
    char *buf = NULL; size_t sz = 0;
    FILE *const ms = open_memstream(&buf, &sz);
    if (!ms) return RESULT_OK(KORB_NIL);
    korb_fprint_to_s(c, ms, v);
    fputc('\n', ms);
    fclose(ms);
    const RESULT wr = korb_io_wr_checked(c, slots, rep, buf, sz);
    free(buf);
    return wr;
}

/* Evaluate `src` as a top-level program (fresh `main` self, shared globals /
 * constants / methods), like Kernel#eval with no binding.  Used by require/load. */
static RESULT
korb_eval_toplevel_wrap(CTX *c, VALUE *slots, const char *src, size_t len, const char *fname, VALUE *wrapp)
{
    const uint32_t repo_before = code_repo_count();               /* bodies this file adds: [repo_before, count) */
    NODE *ast = koruby_parse_source(c, src, len, fname, false);   /* immortal AST; no GC */
    if (UNLIKELY(ast == NULL)) {
        RESULT sr = korb_raise(c, slots, KORB_E_SYNTAX, 0, "syntax error in %s", fname);
        if (LIKELY(KORB_EXC_P(sr.value))) {               /* SyntaxError#path is the file we were reading */
            slots[0] = sr.value;
            slots[1] = UNWRAP(korb_str_new(c, slots + 1, fname, (uint32_t)strlen(fname)));
            korb_exc_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[0]), ID2SYM(korb_intern(c->vm, "@__path", 7)), slots[1]);
            sr.value = slots[0];
        }
        return sr;
    }
    korb_load_time_specialize(ast, repo_before, fname);          /* AOT: bind (+compile when producing) at load */
    const uint32_t locals = koruby_toplevel_locals_cnt;
    slots[0] = 0; slots[1] = 0; slots[2] = 0;          /* frame meta: fb[-3]=magic, fb[-2]=EP, fb[-1]=self */
    VALUE *const fb = slots + 3;
    VALUE *const cur = fb + locals;
    memset(fb, 0, (size_t)locals * sizeof(VALUE));
    RESULT mr = korb_obj_new(c, cur, KORB_NIL);        /* fresh `main` self */
    if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
    fb[-1] = mr.value;
    /* Park the wrap module in the CTX (a GC root) so nothing here holds a bare
     * VALUE across the extend / EVAL below — a moving GC would strand it. */
    const VALUE saved_definee = c->def_definee;
    const VALUE saved_cref = c->eval_cref;
    const VALUE wrap = wrapp ? *wrapp : KORB_NIL;      /* read AFTER the allocs above: the caller's slot is scanned */
    c->def_definee = KORB_CLASS_P(wrap) ? wrap : KORB_NIL;   /* `def` / constants land in the wrap module */
    c->eval_cref    = KORB_CLASS_P(wrap) ? wrap : KORB_NIL;
    if (KORB_CLASS_P(c->eval_cref)) {                  /* load(file, wrap): self extends the module */
        cur[0] = fb[-1]; cur[1] = c->eval_cref;
        RESULT er = korb_send_impl(c, cur + 2, korb_intern(c->vm, "extend", 6), 0, 1, NULL, NULL, NULL);
        if (UNLIKELY(er.state != KORB_NORMAL)) { c->def_definee = saved_definee; c->eval_cref = saved_cref; return er; }
        fb[-1] = cur[0];
        cur[0] = 0; cur[1] = 0;
    }
    /* a required/loaded file starts at the real top level even when the require
     * ran inside instance_eval/class_eval: its `def`s are global functions, not
     * methods of the surrounding definee. */
    uint8_t saved_vis = 0;
    if (KORB_CLASS_P(c->eval_cref)) {                  /* toplevel `def` is private there (CRuby) */
        saved_vis = VAL2CLASS(c->eval_cref)->cur_visibility;
        VAL2CLASS(c->eval_cref)->cur_visibility = 1;
    }
    RESULT r = EVAL(c, ast, cur);
    /* the file's frame goes away here, so a proc/define_method block that
     * captured its locals must have the env closed (heap-copied) first */
    if (UNLIKELY(korb_frame_escaped(fb))) r = korb_close_ret(c, cur, fb, r);
    if (KORB_CLASS_P(c->eval_cref)) VAL2CLASS(c->eval_cref)->cur_visibility = saved_vis;   /* re-read: it may have moved */
    c->def_definee = saved_definee;
    c->eval_cref = saved_cref;
    return r;
}
/* source_location: register a def/block body NODE → (file, line) at parse time. */
/* CRuby warns on every constant reassignment, naming where the previous one was
 * (which is exactly what the const-location table records). */
void
korb_warn_const_redef_at(CTX *c, VALUE *slots, uint32_t name_sym, VALUE owner,
                         const char *file, uint32_t line0)
{
    if (korb_const_get(c->vm, korb_intern(c->vm, "$VERBOSE", 8)) == KORB_NIL) return;
    const char *const nm = korb_sym_name(c->vm, name_sym);
    char qual[256];
    if (KORB_CLASS_P(owner) && VAL2CLASS(owner)->name_sym &&
        owner != korb_builtin_class_obj(c->vm, KORB_C_OBJECT)) {  /* CRuby names it Owner::CONST (Object is implicit) */
        char obuf[192]; korb_class_qname_into(c, owner, obuf, sizeof obuf);
        snprintf(qual, sizeof qual, "%s::%s", obuf, nm);
    } else snprintf(qual, sizeof qual, "%s", nm);
    /* CRuby prefixes both lines with the position they happen at.  korb_warn_at
     * builds a String, so park the owner across it — a bare VALUE would go
     * stale under the moving GC and korb_const_get_loc would read freed memory. */
    slots[0] = owner;
    korb_warn_at(c, slots + 1, file, line0, "already initialized constant %s", qual);
    uint32_t fsym = 0, line = 0;
    if (korb_const_get_loc(c->vm, name_sym, slots[0], &fsym, &line))
        korb_warn_at(c, slots + 1, korb_sym_name(c->vm, fsym), line, "previous definition of %s was here", nm);
}
void
korb_warn_const_redef(CTX *c, VALUE *slots, uint32_t name_sym, VALUE owner)
{
    korb_warn_const_redef_at(c, slots, name_sym, owner, NULL, 0);
}

/* Where a constant was assigned, for Module#const_source_location.  Keyed by
 * (name, owner) like the constant table itself; append-only with the newest
 * entry winning, and only written from the two nodes that know a position
 * (a constant assignment and a class/module body), so a constant defined in C
 * has no entry — which is exactly the empty result CRuby reports for those. */
void
korb_const_reg_loc(struct korb_vm *vm, uint32_t name_sym, VALUE owner, uint32_t file_sym, uint32_t line)
{
    /* the prelude stands in for CRuby's C code: those constants have no source
     * location (Module#const_source_location answers []) */
    if (strcmp(korb_sym_name(vm, file_sym), "<prelude>") == 0) return;
    const uint32_t oser = korb_const_owner_serial(vm, owner);
    for (uint32_t i = 0; i < vm->constloc_cnt; i++)
        if (vm->constlocs[i].name == name_sym && vm->constlocs[i].owner_serial == oser) {
            vm->constlocs[i].file_sym = file_sym; vm->constlocs[i].line = line; return;
        }
    if (vm->constloc_cnt == vm->constloc_capa) {
        vm->constloc_capa = vm->constloc_capa ? vm->constloc_capa * 2 : 128;
        vm->constlocs = realloc(vm->constlocs, sizeof(*vm->constlocs) * vm->constloc_capa);
        if (!vm->constlocs) abort();
    }
    vm->constlocs[vm->constloc_cnt].name = name_sym;
    vm->constlocs[vm->constloc_cnt].owner_serial = oser;
    vm->constlocs[vm->constloc_cnt].file_sym = file_sym;
    vm->constlocs[vm->constloc_cnt].line = line;
    vm->constloc_cnt++;
}
/* false when the constant has no recorded position (defined in C). */
bool
korb_const_get_loc(const struct korb_vm *vm, uint32_t name_sym, VALUE owner, uint32_t *file_sym, uint32_t *line)
{
    const uint32_t oser = korb_const_owner_serial(vm, owner);
    for (uint32_t i = vm->constloc_cnt; i-- > 0; )
        if (vm->constlocs[i].name == name_sym && vm->constlocs[i].owner_serial == oser) {
            *file_sym = vm->constlocs[i].file_sym; *line = vm->constlocs[i].line; return true;
        }
    return false;
}

void
korb_reg_srcloc(struct korb_vm *vm, struct Node *node, uint32_t file_sym, uint32_t line)
{
    if (!node) return;
    if (vm->srcloc_cnt == vm->srcloc_capa) {
        vm->srcloc_capa = vm->srcloc_capa ? vm->srcloc_capa * 2 : 256;
        vm->srclocs = realloc(vm->srclocs, sizeof(*vm->srclocs) * vm->srcloc_capa);
        if (!vm->srclocs) abort();
    }
    vm->srclocs[vm->srcloc_cnt].node = node;
    vm->srclocs[vm->srcloc_cnt].file_sym = file_sym;
    vm->srclocs[vm->srcloc_cnt].line = line;
    vm->srcloc_cnt++;
}
/* look up a body NODE's source location; false if never registered. */
bool
korb_get_srcloc(struct korb_vm *vm, const struct Node *node, uint32_t *file_sym, uint32_t *line)
{
    if (!node) return false;
    for (uint32_t i = vm->srcloc_cnt; i-- > 0; )   /* newest-first: a redefinition shadows */
        if (vm->srclocs[i].node == node) { *file_sym = vm->srclocs[i].file_sym; *line = vm->srclocs[i].line; return true; }
    return false;
}
/* [file, line] for a body NODE, or nil.  Result rooted by caller's slots. */
static RESULT
korb_srcloc_result(CTX *c, VALUE *slots, const struct Node *body)
{
    uint32_t fsym, line;
    if (!korb_get_srcloc(c->vm, body, &fsym, &line)) return RESULT_OK(KORB_NIL);
    const char *fname = korb_sym_name(c->vm, fsym);
    slots[0] = UNWRAP(korb_str_new(c, slots, fname, (uint32_t)strlen(fname)));
    slots[1] = LONG2FIX((korb_sword_t)line);
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
/* remember an absolute path as loaded (libc side; no GC). */
static bool korb_mark_loaded(struct korb_vm *vm, const char *abspath) {
    for (uint32_t i = 0; i < vm->loaded_cnt; i++)
        if (strcmp(vm->loaded_files[i], abspath) == 0) return false;   /* already loaded */
    if (vm->loaded_cnt == vm->loaded_capa) {
        vm->loaded_capa = vm->loaded_capa ? vm->loaded_capa * 2 : 16;
        vm->loaded_files = realloc(vm->loaded_files, sizeof(char *) * vm->loaded_capa);
        if (!vm->loaded_files) abort();
    }
    vm->loaded_files[vm->loaded_cnt++] = strdup(abspath);
    return true;
}
static RESULT korb_raise_load_error(CTX *c, VALUE *slots, const char *path);   /* fwd (defined with require) */

/* Drop the last `abspath` entry from the C-side loaded list (a load that raised). */
static void korb_unmark_loaded(struct korb_vm *vm, const char *abspath)
{
    for (uint32_t i = vm->loaded_cnt; i-- > 0; )
        if (strcmp(vm->loaded_files[i], abspath) == 0) {
            free(vm->loaded_files[i]);
            memmove(&vm->loaded_files[i], &vm->loaded_files[i + 1], sizeof(char *) * (vm->loaded_cnt - i - 1));
            vm->loaded_cnt--;
            return;
        }
}

/* Append `path` to $LOADED_FEATURES (Ruby code — autoload?, `require`-guards —
 * reads it; the C-side loaded list is not visible from Ruby). */
static void korb_loaded_features_push(CTX *c, VALUE *slots, const char *path)
{
    const VALUE lf = korb_const_get(c->vm, korb_intern(c->vm, "$LOADED_FEATURES", 16));
    if (!KORB_ARRAY_P(lf)) return;
    slots[0] = lf;
    const RESULT sr = korb_str_new(c, slots + 1, path, (uint32_t)strlen(path));
    if (UNLIKELY(sr.state != KORB_NORMAL)) return;
    slots[1] = sr.value;
    (void)korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1]);
}

/* Remove `path` from $LOADED_FEATURES again (its load raised). */
static void korb_loaded_features_pop(CTX *c, VALUE *slots, const char *path)
{
    const VALUE lf = korb_const_get(c->vm, korb_intern(c->vm, "$LOADED_FEATURES", 16));
    if (!KORB_ARRAY_P(lf)) return;
    const uint32_t plen = (uint32_t)strlen(path);
    KorbArray *const a = VAL2ARY(lf);
    for (uint32_t i = a->len; i-- > 0; ) {
        const VALUE e = korb_items_data(a->items)[i];
        if (!KORB_STRING_P(e) || VAL2STR(e)->len != plen) continue;
        if (memcmp(korb_strbuf_data(VAL2STR(e)->buf), path, plen) != 0) continue;
        slots[0] = lf; slots[1] = LONG2FIX((korb_sword_t)i);
        (void)korb_send(c, slots + 2, korb_intern(c->vm, "delete_at", 9), 0, 1);
        return;
    }
}

/* Has `path` already been required?  $LOADED_FEATURES is the authority when it
 * exists — Ruby code (mspec's load-path fixtures, `require`-guards) manipulates
 * that array and expects the answer to follow; the C-side list only covers the
 * bootstrap window before the global is created. */
static bool korb_feature_loaded_p(CTX *c, const char *path)
{
    const VALUE lf = korb_const_get(c->vm, korb_intern(c->vm, "$LOADED_FEATURES", 16));
    if (KORB_ARRAY_P(lf)) {
        const uint32_t plen = (uint32_t)strlen(path);
        const KorbArray *const a = VAL2ARY(lf);
        for (uint32_t i = 0; i < a->len; i++) {
            const VALUE e = korb_items_data(a->items)[i];
            if (KORB_STRING_P(e) && VAL2STR(e)->len == plen &&
                memcmp(korb_strbuf_data(VAL2STR(e)->buf), path, plen) == 0) return true;
        }
        /* a bare feature name (no '/') can also be one the interpreter pre-marked
         * as built in before $LOADED_FEATURES existed — "set" is core-loaded, so
         * `require "set"` is false.  Real files always arrive as absolute paths,
         * so this never overrides a $LOADED_FEATURES the program has edited. */
        if (strchr(path, '/') != NULL) return false;
    }
    for (uint32_t i = 0; i < c->vm->loaded_cnt; i++)
        if (strcmp(c->vm->loaded_files[i], path) == 0) return true;
    return false;
}

/* Does $LOADED_FEATURES carry `stem` — exactly, or as a path whose basename
 * (with or without ".rb") is `stem`?  The provided-features seed stores
 * "<internal:koruby>/<stem>.rb" pseudo paths; a later require of the bare
 * name must see them as already loaded. */
static bool
korb_feature_basename_loaded_p(CTX *c, const char *stem)
{
    if (korb_feature_loaded_p(c, stem)) return true;
    const VALUE lf = korb_const_get(c->vm, korb_intern(c->vm, "$LOADED_FEATURES", 16));
    if (!KORB_ARRAY_P(lf)) return false;
    const size_t sl = strlen(stem);
    const KorbArray *const a = VAL2ARY(lf);
    for (uint32_t i = 0; i < a->len; i++) {
        const VALUE e = korb_items_data(a->items)[i];
        if (!KORB_STRING_P(e)) continue;
        const char *b = korb_strbuf_data(VAL2STR(e)->buf);
        const uint32_t bl = VAL2STR(e)->len;
        const char *slash = NULL;
        for (uint32_t j = 0; j < bl; j++) if (b[j] == '/') slash = b + j;
        const char *base = slash ? slash + 1 : b;
        const size_t rest = (size_t)(bl - (uint32_t)(base - b));
        if (rest == sl && strncmp(base, stem, sl) == 0) return true;
        if (rest == sl + 3 && strncmp(base, stem, sl) == 0 && strncmp(base + sl, ".rb", 3) == 0) return true;
    }
    return false;
}

/* CRuby ships a set of features pre-required (require of them returns false
 * from the start): complex/enumerator/fiber/rational/thread/ruby2_keywords
 * (+ set/pathname since 4.0).  fiber and pathname have real lib .rb files —
 * load them so their stdlib surface exists; the rest are built into the
 * runtime and get "<internal:koruby>/<name>.rb" pseudo entries. */
RESULT korb_require_feature(CTX *c, VALUE *slots, const char *name);   /* fwd */
void
korb_seed_provided_features(CTX *c, VALUE *slots)
{
    /* The preload requires PARSE their files, which clobbers the toplevel
     * frame globals the caller derived its cursor from (parse of any file
     * overwrites koruby_toplevel_locals_cnt / _local_syms).  Restore them —
     * the crash mode is the user program running on pathname.rb's counts. */
    const uint32_t saved_cnt  = koruby_toplevel_locals_cnt;
    const uint32_t *saved_sym = koruby_toplevel_local_syms;
    const uint32_t saved_scnt = koruby_toplevel_local_cnt;
    static const char *const preload[] = { "fiber", "pathname", NULL };
    for (uint32_t i = 0; preload[i]; i++)
        (void)korb_require_feature(c, slots, preload[i]);   /* pushes the real path */
    koruby_toplevel_locals_cnt = saved_cnt;
    koruby_toplevel_local_syms = saved_sym;
    koruby_toplevel_local_cnt  = saved_scnt;
    static const char *const pseudo[] =
        { "complex", "enumerator", "rational", "thread", "set", "ruby2_keywords", NULL };
    for (uint32_t i = 0; pseudo[i]; i++) {
        char path[128];
        snprintf(path, sizeof path, "<internal:koruby>/%s.rb", pseudo[i]);
        korb_loaded_features_push(c, slots, path);
    }
}

/* Loading-claim table: which green thread is mid-load of a feature. */
static struct korb_thread *
korb_loading_owner(struct korb_vm *vm, const char *abspath)
{
    for (uint32_t i = 0; i < vm->loading_cnt; i++)
        if (strcmp(vm->loading[i].path, abspath) == 0) return vm->loading[i].owner;
    return NULL;
}
static void
korb_loading_add(struct korb_vm *vm, const char *abspath, struct korb_thread *owner)
{
    if (vm->loading_cnt == vm->loading_capa) {
        vm->loading_capa = vm->loading_capa ? vm->loading_capa * 2 : 8;
        vm->loading = realloc(vm->loading, sizeof(*vm->loading) * vm->loading_capa);
        if (!vm->loading) abort();
    }
    vm->loading[vm->loading_cnt].path = strdup(abspath);
    vm->loading[vm->loading_cnt].owner = owner;
    vm->loading_cnt++;
}
static void
korb_loading_remove(struct korb_vm *vm, const char *abspath)
{
    for (uint32_t i = 0; i < vm->loading_cnt; i++)
        if (strcmp(vm->loading[i].path, abspath) == 0) {
            free((void *)(uintptr_t)vm->loading[i].path);
            vm->loading[i] = vm->loading[--vm->loading_cnt];
            break;
        }
    /* Wake every thread parked on this feature (they re-check and either see
     * it loaded → false, or claim the retry after a raised load). */
    for (struct korb_thread *t = vm->thread_list; t; t = t->next) {
        if (t->state == KORB_TH_PENDED && t->waiting_feature &&
            strcmp(t->waiting_feature, abspath) == 0) {
            t->waiting_feature = NULL;
            t->state = KORB_TH_READY;
            korb_thread_runq_push(vm, t);
        }
    }
}

/* Load `abspath` (read + eval at top level), tracking it as a required feature.
 * dedup: if true (require), a second require of the same path returns false. */
static RESULT
korb_load_abspath_wrap(CTX *c, VALUE *slots, const char *abspath, bool dedup, VALUE *out, VALUE *wrapp)
{
    struct korb_vm *const vm = c->vm;
    if (dedup) {
        /* Another green thread mid-load of this feature: wait for it (yield
         * the scheduler) rather than trusting the pre-eval loaded mark.  The
         * same thread falls through — the mark answers circular requires. */
        for (;;) {
            struct korb_thread *const owner = korb_loading_owner(vm, abspath);
            if (owner == NULL || owner == vm->cur_thread || owner->state == KORB_TH_DEAD) break;
            struct korb_thread *const me = vm->cur_thread;
            if (me == NULL) break;                 /* threads not booted: nothing to wait for */
            /* Park for real (PENDED): a busy re-queue loop would keep the runq
             * non-empty forever and starve the blop pump — a loader sleeping on
             * a timer would then never wake (livelock).  korb_loading_remove
             * wakes us. */
            me->blocked_in = "require";            /* observable: #stop? / #backtrace */
            me->waiting_feature = abspath;
            me->state = KORB_TH_PENDED;
            RESULT yr = korb_thread_yield_cpu(c, slots);
            me->waiting_feature = NULL;
            me->blocked_in = NULL;
            if (UNLIKELY(yr.state != KORB_NORMAL)) return yr;
            RESULT ci = korb_thread_check_ints(c, slots);
            if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
        }
    }
    if (dedup && korb_feature_loaded_p(c, abspath)) { *out = KORB_FALSE; return RESULT_OK(KORB_FALSE); }
    size_t got = 0;
    char *buf = korb_file_slurp(abspath, &got);
    if (!buf) return korb_raise_load_error(c, slots, abspath);
    /* record before eval so a circular require returns false rather than reloading. */
    if (dedup) {
        korb_mark_loaded(vm, abspath);
        korb_loaded_features_push(c, slots, abspath);   /* keep $LOADED_FEATURES in step */
        korb_loading_add(vm, abspath, vm->cur_thread);  /* claim for cross-thread waiters */
    }
    const char *const saved = vm->cur_load_file;
    char *const abscopy = strdup(abspath);             /* stable across the eval (fname baked into AST) */
    vm->cur_load_file = abscopy;
    RESULT r = korb_eval_toplevel_wrap(c, slots, buf, got, abscopy, wrapp);
    vm->cur_load_file = saved;
    free(buf);
    if (dedup) korb_loading_remove(vm, abspath);
    if (UNLIKELY(r.state != KORB_NORMAL)) {
        /* a load that raised did not happen: CRuby leaves neither the C-side
         * record nor $LOADED_FEATURES behind, so the next require retries */
        if (dedup) { korb_unmark_loaded(vm, abspath); korb_loaded_features_pop(c, slots, abspath); }
        return r;
    }
    *out = KORB_TRUE;
    return RESULT_OK(KORB_TRUE);
}
static RESULT
korb_load_abspath(CTX *c, VALUE *slots, const char *abspath, bool dedup, VALUE *out)
{
    return korb_load_abspath_wrap(c, slots, abspath, dedup, out, NULL);
}
/* Resolve `name` (adding ".rb" if absent) against `base_dir` into `out` (abs),
 * returning true if the file exists. */
/* Lexically normalise a path: collapse "//", drop "." and resolve ".." without
 * touching the filesystem (CRuby reports the cleaned path in a LoadError even
 * when the file does not exist). */
static void korb_path_lexnorm(const char *in, char *out, size_t outsz) {
    const char *seg = in;
    size_t o = 0;
    const bool abs = (in[0] == '/');
    if (abs && outsz > 1) out[o++] = '/';
    while (*seg) {
        while (*seg == '/') seg++;
        const char *end = strchr(seg, '/');
        const size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if (n == 0) break;
        if (n == 1 && seg[0] == '.') { seg += n; continue; }
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (o > (abs ? 1u : 0u)) {                    /* pop the previous component */
                size_t k = o - 1;
                if (out[k] == '/') k--;
                while (k > (abs ? 1u : 0u) && out[k - 1] != '/') k--;
                if (!(k == 0 && !abs && o >= 3 && out[0] == '.' && out[1] == '.')) {
                    o = k;
                    if (o > (abs ? 1u : 0u) && out[o - 1] == '/') o--;   /* drop the separator too */
                    seg += n; continue;
                }
            }
        }
        if (o > (abs ? 1u : 0u) && o + 1 < outsz) out[o++] = '/';
        for (size_t i = 0; i < n && o + 1 < outsz; i++) out[o++] = seg[i];
        seg += n;
    }
    if (o == 0 && outsz > 1) out[o++] = abs ? '/' : '.';
    out[o < outsz ? o : outsz - 1] = '\0';
}
static bool korb_resolve_load(const char *base_dir, const char *name, char *out, size_t outsz) {
    char cand[4096];
    const bool has_rb = (strlen(name) >= 3 && strcmp(name + strlen(name) - 3, ".rb") == 0);
    if (name[0] == '/') snprintf(cand, sizeof cand, "%s%s", name, has_rb ? "" : ".rb");
    else                snprintf(cand, sizeof cand, "%s/%s%s", base_dir, name, has_rb ? "" : ".rb");
    /* $LOADED_FEATURES keeps the path as WRITTEN (expand_path, not realpath):
     * CRuby does not canonicalize symlinks there. */
    if (cand[0] != '/') {                                 /* make it absolute first, like expand_path */
        char cwd[4096], abs[4096];
        if (getcwd(cwd, sizeof cwd)) { snprintf(abs, sizeof abs, "%s/%s", cwd, cand); snprintf(cand, sizeof cand, "%s", abs); }
    }
    korb_path_lexnorm(cand, out, outsz);
    struct stat st; return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}
/* __method__ / __callee__ reached by an explicit send / eval: the parser bakes
 * the lexical answer, so this only sees the define_method case (else nil). */
static RESULT
korb_bi_method_name(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots; (void)args;
    return RESULT_OK(c->dm_entry ? ID2SYM(c->dm_entry->mid) : KORB_NIL);
}
/* Kernel#__dir__ → the directory of the current source file (realpath'd), or nil
 * when there is no file (e.g. -e).  Uses the file being loaded / the main script. */
static RESULT
korb_bi_dir(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)args;
    const char *f = c->vm->cur_load_file ? c->vm->cur_load_file : c->vm->script_name;
    if (!f || strcmp(f, "-e") == 0 || strcmp(f, "-") == 0 || strcmp(f, "(eval)") == 0)
        return RESULT_OK(KORB_NIL);
    char real[4096];
    if (!realpath(f, real)) { if ((size_t)snprintf(real, sizeof real, "%s", f) >= sizeof real) return RESULT_OK(KORB_NIL); }
    char *slash = strrchr(real, '/');
    if (slash) *slash = '\0'; else snprintf(real, sizeof real, ".");
    return korb_str_new(c, slots, real, (uint32_t)strlen(real));
}
/* Raise LoadError("cannot load such file -- <path>") carrying #path, as CRuby's
 * require / require_relative / load do. */
static RESULT
korb_raise_load_error(CTX *c, VALUE *slots, const char *path)
{
    RESULT r = korb_raise(c, slots, KORB_E_LOADERR, 0, "cannot load such file -- %s", path);
    if (LIKELY(KORB_EXC_P(r.value))) {
        slots[0] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[0]);
        const RESULT pr = korb_str_new(c, slots + 1, path, (uint32_t)strlen(path));
        if (LIKELY(pr.state == KORB_NORMAL)) {
            slots[1] = pr.value;
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__path", 7)), slots[1]);
        }
        r.value = VALUE_REF_GET(eref);
    }
    return r;
}

/* The path argument of require / require_relative / load: a String as is,
 * otherwise #to_path then #to_str (CRuby's FilePathValue). */
static RESULT
korb_load_path_arg(CTX *c, VALUE *slots, VALUE *v)
{
    if (LIKELY(KORB_STRING_P(*v))) return RESULT_OK(KORB_TRUE);
    const char *const cls = korb_type_name(*v);              /* capture before dispatch */
    static const char *const conv[2] = { "to_path", "to_str" };
    static const uint32_t convlen[2] = { 7, 6 };
    for (int i = 0; i < 2; i++) {
        VALUE recv = *v;
        const uint32_t mid = korb_intern(c->vm, conv[i], convlen[i]);
        if (!(KORB_OBJECT_P(recv) && korb_responds_to_coerce_p(c, slots, &recv, mid))) continue;
        slots[0] = recv;
        const RESULT pr = korb_send(c, slots + 1, mid, 0, 0);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        if (KORB_STRING_P(pr.value)) { *v = pr.value; return RESULT_OK(KORB_TRUE); }
        *v = pr.value;                                       /* #to_path may hand on a #to_str-able */
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", cls);
}

/* require(name): search CWD / $LOAD_PATH; load once (false if already loaded). */
static RESULT
korb_bi_require(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(nv))) {
        CHECK(korb_load_path_arg(c, slots, &nv));
        slots[0] = nv;
        return korb_bi_require(c, slots + 1, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    char namebuf[4096]; uint32_t nl = VAL2STR(nv)->len;
    if (nl >= sizeof namebuf) nl = sizeof namebuf - 1;
    memcpy(namebuf, korb_strbuf_data(VAL2STR(nv)->buf), nl); namebuf[nl] = '\0';
    char abspath[4096];
    if (namebuf[0] == '~') {                               /* shell-style tilde expansion (CRuby expand_path) */
        const char *home = getenv("HOME");
        if (home && (namebuf[1] == '/' || namebuf[1] == '\0')) {
            char expanded[4096];
            if ((size_t)snprintf(expanded, sizeof expanded, "%s%s", home, namebuf + 1) < sizeof expanded)
                snprintf(namebuf, sizeof namebuf, "%s", expanded);
        }
    }
    /* Only an explicitly relative ("./x", "../x") or absolute name is resolved
     * against the working directory; a bare feature name comes from $LOAD_PATH
     * alone (CRuby dropped "." from the load path in 1.9.2, and searching it
     * would let a stray ./stringio.rb shadow the real feature). */
    if ((namebuf[0] == '/' || namebuf[0] == '.') &&
        korb_resolve_load(".", namebuf, abspath, sizeof abspath)) {
        VALUE out; return korb_load_abspath(c, slots, abspath, true, &out);
    }
    if (namebuf[0] != '/' && namebuf[0] != '.') {          /* search each $LOAD_PATH dir (re-read per iter: #to_path may run Ruby) */
        const uint32_t lp_sym = korb_intern(c->vm, "$LOAD_PATH", 10);
        for (uint32_t i = 0; ; i++) {
            const VALUE lp = korb_const_get(c->vm, lp_sym);
            if (!KORB_ARRAY_P(lp) || i >= VAL2ARY(lp)->len) break;
            VALUE e = korb_items_data(VAL2ARY(lp)->items)[i];
            if (!KORB_STRING_P(e)) {                       /* a #to_path / #to_str entry (CRuby FilePathValue) */
                RESULT pr = korb_load_path_arg(c, slots, &e);
                if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
                if (!KORB_STRING_P(e)) continue;
            }
            char dir[4096]; uint32_t dl = VAL2STR(e)->len; if (dl >= sizeof dir) dl = sizeof dir - 1;
            memcpy(dir, korb_strbuf_data(VAL2STR(e)->buf), dl); dir[dl] = '\0';
            if (korb_resolve_load(dir, namebuf, abspath, sizeof abspath)) {
                VALUE out; return korb_load_abspath(c, slots, abspath, true, &out);
            }
        }
    }
    /* the exact name already in $LOADED_FEATURES → false, even when no such
     * file exists any more (CRuby) */
    if (korb_feature_loaded_p(c, namebuf)) return RESULT_OK(KORB_FALSE);
    /* Not on disk.  A handful of stdlib features are built into koruby (no .rb
     * on disk): a require of one of those succeeds as a no-op.  Any other
     * missing feature is a LoadError, matching CRuby. */
    static const char *const builtin_features[] = { "set", "stringio", "enumerator", "comparable", "rbconfig", "pp", "prettyprint", "date", "delegate", "complex", "rational", "thread", "ruby2_keywords", NULL };
    const char *stem = namebuf; if (strncmp(stem, "./", 2) == 0) stem += 2;
    for (uint32_t i = 0; builtin_features[i]; i++)
        if (strcmp(stem, builtin_features[i]) == 0) {                 /* built-in: load-once contract by feature name */
            if (korb_feature_basename_loaded_p(c, stem)) return RESULT_OK(KORB_FALSE);
            (void)korb_mark_loaded(c->vm, stem);
            korb_loaded_features_push(c, slots, stem);
            return RESULT_OK(KORB_TRUE);
        }
    return korb_raise_load_error(c, slots, namebuf);
}
/* -I DIR: prepend to $LOAD_PATH (CRuby order: the last -I ends up first). */
void korb_load_path_unshift(CTX *c, VALUE *slots, const char *dir)
{
    const VALUE lp = korb_const_get(c->vm, korb_intern(c->vm, "$LOAD_PATH", 10));
    if (!KORB_ARRAY_P(lp)) return;
    slots[0] = lp;
    const RESULT sr = korb_str_new(c, slots + 1, dir, (uint32_t)strlen(dir));
    if (UNLIKELY(sr.state != KORB_NORMAL)) return;
    slots[1] = sr.value;
    (void)korb_send(c, slots + 2, korb_intern(c->vm, "unshift", 7), 0, 1);
}

/* -r LIB: require it the same way Ruby code would. */
RESULT korb_require_feature(CTX *c, VALUE *slots, const char *name)
{
    const RESULT sr = korb_str_new(c, slots, name, (uint32_t)strlen(name));
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[0] = sr.value;
    return korb_bi_require(c, slots + 1, VALUE_SLICE_MAKE(&slots[0], 1));
}

/* Method-shaped wrappers for require / require_relative / load.  They go on the
 * Kernel module (which Object includes), so a Ruby-level `def require` on Object
 * — mspec installs one — finds them via `super` instead of hitting BasicObject.
 * Defining them on Object itself would be replaced by that very override. */
static RESULT korb_bi_require(CTX *c, VALUE *slots, VALUE_SLICE args);
static RESULT korb_bi_require_relative(CTX *c, VALUE *slots, VALUE_SLICE args);
static RESULT korb_bi_load(CTX *c, VALUE *slots, VALUE_SLICE args);
static RESULT korb_m_kernel_require(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_bi_require(c, slots, a);
}
static RESULT korb_m_kernel_require_relative(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_bi_require_relative(c, slots, a);
}
static RESULT korb_m_kernel_load(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_bi_load(c, slots, a);
}

/* require_relative(name): resolve against the current file's directory. */
static RESULT
korb_bi_require_relative(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(nv))) { CHECK(korb_load_path_arg(c, slots, &nv)); slots[0] = nv; }
    char namebuf[4096]; uint32_t nl = VAL2STR(nv)->len;
    if (nl >= sizeof namebuf) nl = sizeof namebuf - 1;
    memcpy(namebuf, korb_strbuf_data(VAL2STR(nv)->buf), nl); namebuf[nl] = '\0';
    /* base = dirname of the current load file (or the main script). */
    const char *base = c->vm->cur_load_file ? c->vm->cur_load_file : (c->vm->script_name ? c->vm->script_name : ".");
    char basedir[4096]; snprintf(basedir, sizeof basedir, "%s", base);
    char *slash = strrchr(basedir, '/'); if (slash) *slash = '\0'; else snprintf(basedir, sizeof basedir, ".");
    char abspath[4096];
    if (korb_resolve_load(basedir, namebuf, abspath, sizeof abspath)) {
        VALUE out; return korb_load_abspath(c, slots, abspath, true, &out);
    }
    {   /* CRuby names the resolved absolute path (minus any ".rb" it appended) */
        char full[4096], norm[4096];
        if (namebuf[0] == '/')      snprintf(full, sizeof full, "%s", namebuf);   /* an absolute argument is already the path */
        else if (basedir[0] == '/') snprintf(full, sizeof full, "%s/%s", basedir, namebuf);
        else {                                            /* make it absolute, like CRuby */
            char cwd[4096];
            if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
            snprintf(full, sizeof full, "%s/%s/%s", cwd, basedir, namebuf);
        }
        korb_path_lexnorm(full, norm, sizeof norm);
        return korb_raise_load_error(c, slots, norm);
    }
}
/* load(name): always (re)load; returns true. */
static RESULT
korb_bi_load(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(nv))) { CHECK(korb_load_path_arg(c, slots, &nv)); slots[0] = nv; }
    char namebuf[4096]; uint32_t nl = VAL2STR(nv)->len;
    if (nl >= sizeof namebuf) nl = sizeof namebuf - 1;
    memcpy(namebuf, korb_strbuf_data(VAL2STR(nv)->buf), nl); namebuf[nl] = '\0';
    char abspath[4096];
    /* load() takes a path verbatim (no .rb auto-append); try relative to CWD. */
    if (namebuf[0] == '/') snprintf(abspath, sizeof abspath, "%s", namebuf);
    else if (!realpath(namebuf, abspath)) {
        /* not under the working directory: a relative name is looked up in
         * $LOAD_PATH too (CRuby), including entries with #to_path */
        bool found = false;
        const VALUE lp = korb_const_get(c->vm, korb_intern(c->vm, "$LOAD_PATH", 10));
        if (namebuf[0] != '.' && KORB_ARRAY_P(lp)) {
            slots[1] = lp;                                 /* park: #to_path dispatches */
            for (uint32_t i = 0; i < VAL2ARY(slots[1])->len && !found; i++) {
                slots[2] = korb_items_data(VAL2ARY(slots[1])->items)[i];
                if (!KORB_STRING_P(slots[2])) {
                    if (!korb_responds_to(c, slots[2], korb_intern(c->vm, "to_path", 7))) continue;
                    const RESULT pr = korb_send(c, slots + 3, korb_intern(c->vm, "to_path", 7), 0, 0);
                    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
                    if (!KORB_STRING_P(pr.value)) continue;
                    slots[2] = pr.value;
                }
                char cand[4096];
                snprintf(cand, sizeof cand, "%.*s/%s", (int)VAL2STR(slots[2])->len,
                         korb_strbuf_data(VAL2STR(slots[2])->buf), namebuf);
                if (realpath(cand, abspath)) found = true;
            }
        }
        if (!found) snprintf(abspath, sizeof abspath, "%s", namebuf);
    }
    VALUE wrap = KORB_NIL;
    if (VALUE_SLICE_LEN(args) >= 2) {                  /* load(file, true) / load(file, Module) */
        const VALUE w = VALUE_SLICE_GET(args, 1);
        if (KORB_CLASS_P(w) && VAL2CLASS(w)->is_module) wrap = w;
        else if (KORB_TRUTHY(w)) {                     /* true → a fresh anonymous module */
            slots[0] = UNWRAP(korb_class_new(c, slots + 1, 0, KORB_NIL));
            VAL2CLASS(slots[0])->is_module = 1;
            wrap = slots[0];
        }
    }
    slots[1] = wrap;                                   /* root it across the load */
    VALUE out; return korb_load_abspath_wrap(c, slots + 2, abspath, false, &out, &slots[1]);
}

/* Kernel#exit([status]) / exit! — terminate the process (true/nil → 0, false → 1,
 * Integer → that code).  Runs the registered at_exit blocks first (reverse order),
 * matching CRuby's exit (exit! skips them). */
int korb_drain_at_exit(CTX *c, VALUE *slots);   /* fwd (defined below; called from main.c) */
/* If `exc` is a SystemExit, its status (>= 0); otherwise -1.  main.c uses this
 * to end the program quietly with the requested code. */
int
korb_system_exit_status(CTX *c, VALUE exc)
{
    if (!KORB_EXC_P(exc)) return -1;
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "SystemExit", 10));
    const VALUE ec = VAL2EXC(exc)->exc_class;
    if (!KORB_CLASS_P(cls) || !KORB_CLASS_P(ec) || !korb_class_le(ec, cls)) return -1;
    const VALUE st = korb_ivar_get(c, exc, ID2SYM(korb_intern(c->vm, "@__status", 9)));
    return FIXNUM_P(st) ? (int)FIX2LONG(st) : 0;
}

/* Kernel#exit — raises SystemExit (rescuable, runs ensure blocks); the process
 * only ends when it reaches the top level uncaught (main.c) .  Returning an
 * exception rather than calling exit(3) is what lets a test framework — or any
 * `begin; exit; rescue SystemExit; end` — intercept it, as CRuby allows. */
RESULT
korb_make_system_exit(CTX *c, VALUE *slots, int code)
{
    RESULT r = korb_raise(c, slots, KORB_E_RUNTIME, 0, "exit");
    if (LIKELY(KORB_EXC_P(r.value))) {
        slots[0] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[0]);
        const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "SystemExit", 10));
        if (KORB_CLASS_P(cls)) {
            KorbException *const e = VAL2EXC(VALUE_REF_GET(eref));
            ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->exc_class, cls);
        }
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern(c->vm, "@__status", 9)), LONG2FIX(code));
        r.value = VALUE_REF_GET(eref);
    }
    return r;
}
/* The status argument of exit / exit!: true → 0, false → 1, a Float truncates,
 * anything else goes through #to_int (nil / String / Array are TypeErrors). */
static RESULT korb_exit_status_arg(CTX *c, VALUE *slots, VALUE_SLICE args, int *out) {
    *out = 0;
    if (VALUE_SLICE_LEN(args) < 1) return RESULT_OK(KORB_TRUE);
    VALUE s = VALUE_SLICE_GET(args, 0);
    if (s == KORB_TRUE)  { *out = 0; return RESULT_OK(KORB_TRUE); }
    if (s == KORB_FALSE) { *out = 1; return RESULT_OK(KORB_TRUE); }
    if (FIXNUM_P(s))     { *out = (int)FIX2LONG(s); return RESULT_OK(KORB_TRUE); }
    if (KORB_FLOAT_P(s)) { *out = (int)korb_float_val(s); return RESULT_OK(KORB_TRUE); }
    {
        const char *const cls = korb_type_name(s);
        VALUE t = s;
        if (KORB_OBJECT_P(t)) {
            const RESULT cr = korb_coerce_to_int(c, slots, &t);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value == KORB_TRUE && FIXNUM_P(t)) { *out = (int)FIX2LONG(t); return RESULT_OK(KORB_TRUE); }
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer",
                          s == KORB_NIL ? "nil" : cls);
    }
}
static RESULT
korb_bi_exit(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    int code = 0;
    CHECK(korb_exit_status_arg(c, slots, args, &code));
    return korb_make_system_exit(c, slots, code);
}
static RESULT
korb_bi_exit_bang(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    int code = 0;
    CHECK(korb_exit_status_arg(c, slots, args, &code));
    korb_io_flush_std(c->vm);   /* _exit skips at_exit, but buffered output is still ours to deliver */
    _exit(code);
}
/* Kernel#abort([msg]) — write msg to stderr (if given) and exit(1). */
static VALUE korb_out_target(CTX *c, const char *gv, uint32_t gvlen, bool *is_default);   /* fwd */
static RESULT korb_out_emit(CTX *c, VALUE *slots, VALUE out, uint32_t stdidx, const char *data, size_t len);   /* fwd */
static RESULT
korb_bi_abort(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    bool has_msg = VALUE_SLICE_LEN(args) >= 1;
    if (has_msg && !KORB_STRING_P(VALUE_SLICE_GET(args, 0))) {   /* #to_str, else TypeError */
        VALUE mv = VALUE_SLICE_GET(args, 0);
        const char *const cls = (mv == KORB_NIL) ? "nil" : korb_type_name(mv);
        bool ok = false;
        if (KORB_OBJECT_P(mv) && korb_responds_to_coerce_p(c, slots, &mv, korb_intern(c->vm, "to_str", 6))) {
            slots[0] = mv;
            const RESULT sr = korb_send(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) { ((VALUE *)args.p)[0] = sr.value; ok = true; }
        }
        if (!ok)
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", cls);
    }
    if (has_msg) {
        /* The message goes through $stderr (which a test harness may replace),
         * not straight to fd 2. */
        slots[0] = VALUE_SLICE_GET(args, 0);
        bool def; const VALUE out = korb_out_target(c, "$stderr", 7, &def);
        const KorbString *const s = VAL2STR(slots[0]);
        if (def) {
            KorbIORep *const er = korb_io_std_rep(c->vm, 2);
            (void)korb_io_wr(er, korb_strbuf_data(s->buf), s->len);
            (void)korb_io_wr(er, "\n", 1);
        } else {
            char *buf = NULL; size_t sz = 0; FILE *const ms = open_memstream(&buf, &sz);
            if (ms) { fwrite(korb_strbuf_data(s->buf), 1, s->len, ms); fputc('\n', ms); fclose(ms); }
            const RESULT er = korb_out_emit(c, slots + 1, out, 2, buf ? buf : "", sz);
            free(buf);
            if (UNLIKELY(er.state != KORB_NORMAL)) return er;
        }
    }
    RESULT r = korb_make_system_exit(c, slots + 1, 1);
    if (has_msg && KORB_EXC_P(r.value)) {   /* CRuby: the SystemExit carries the message */
        slots[1] = r.value;
        ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->msg, VALUE_SLICE_GET(args, 0));
        r.value = slots[1];
    }
    return r;
}
/* Run at_exit blocks (reverse order); guarded so an at_exit block calling exit
 * doesn't re-enter.  Shared by main.c's post-run drain and Kernel#exit.
 * Returns the exit status a handler asked for (-1 = none): a SystemExit raised
 * in a handler decides the process status, and any other exception is reported
 * and makes it 1 — the remaining handlers still run, seeing it as $!. */
int
korb_drain_at_exit(CTX *c, VALUE *slots)
{
    static bool draining = false;
    if (draining) return -1;
    draining = true;
    int status = -1;
    const VALUE ax = korb_const_get(c->vm, korb_intern(c->vm, "$__at_exit", 10));
    if (!KORB_ARRAY_P(ax)) { draining = false; return -1; }
    slots[0] = ax;
    /* pop from the end: reverse registration order, and a handler registering
     * another runs it right after itself (CRuby's nesting order) */
    while (VAL2ARY(slots[0])->len > 0) {
        const uint32_t last = VAL2ARY(slots[0])->len - 1;
        slots[1] = korb_items_data(VAL2ARY(slots[0])->items)[last];
        VAL2ARY(slots[0])->len = last;
        RESULT er = korb_send(c, slots + 2, korb_intern(c->vm, "call", 4), 0, 0);
        if (er.state == KORB_RAISE) {
            const int st = korb_system_exit_status(c, er.value);
            if (st >= 0) { status = st; continue; }        /* `exit` in a handler: status only */
            korb_report_uncaught(c, er.value);
            korb_errinfo_push(c, er.value);                /* later handlers see it as $! */
            status = 1;
        }
    }
    draining = false;
    return status;
}

/* Kernel#warn(*msgs) — write each message + newline to stderr (a trailing
 * keyword Hash, e.g. uplevel:/category:, is ignored). */
static VALUE korb_out_target(CTX *c, const char *gv, uint32_t gvlen, bool *is_default);   /* fwd (defined below) */
static RESULT korb_out_emit(CTX *c, VALUE *slots, VALUE out, uint32_t stdidx, const char *data, size_t len);   /* fwd */
/* Kernel#gets / #readline — read a line from $stdin (forwarding any sep/limit). */
static RESULT korb_bi_gets_impl(CTX *c, VALUE *slots, VALUE_SLICE args, const char *meth, uint32_t mlen) {
    const VALUE in = korb_const_get(c->vm, korb_intern(c->vm, "$stdin", 6));
    if (UNLIKELY(!KORB_OBJECT_P(in))) return RESULT_OK(KORB_NIL);
    const uint32_t n = VALUE_SLICE_LEN(args);
    slots[0] = in;
    for (uint32_t i = 0; i < n; i++) slots[1 + i] = VALUE_SLICE_GET(args, i);
    return korb_send(c, slots + 1 + n, korb_intern(c->vm, meth, mlen), 0, n);
}
static RESULT korb_bi_gets(CTX *c, VALUE *slots, VALUE_SLICE args)     { return korb_bi_gets_impl(c, slots, args, "gets", 4); }
static RESULT korb_bi_readline(CTX *c, VALUE *slots, VALUE_SLICE args) { return korb_bi_gets_impl(c, slots, args, "readline", 8); }
/* Kernel#global_variables — every defined $-global as a Symbol.  Globals reuse the
 * const table with a $-prefixed name (see parse.c), so scan it for those. */
/* Assign a global variable by name, bypassing the parse-time read-only check —
 * the prelude has to seed $< (ARGF), which user code may not assign. */
static RESULT korb_bi_set_gvar(CTX *c, VALUE *slots, VALUE_SLICE args) {
    const VALUE nv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(nv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "global variable name must be a String");
    korb_const_define(c, korb_intern(c->vm, korb_strbuf_data(VAL2STR(nv)->buf), VAL2STR(nv)->len),
                      VALUE_SLICE_GET(args, 1));
    return RESULT_OK(VALUE_SLICE_GET(args, 1));
}
static RESULT korb_bi_global_variables(CTX *c, VALUE *slots, VALUE_SLICE args) {
    (void)args;
    struct korb_vm *const vm = c->vm;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < vm->const_cnt; i++) {
        const char *const nm = korb_sym_name(vm, vm->const_names[i]);   /* const_names is append-only; re-read each iter */
        if (nm[0] != '$') continue;
        CHECK(korb_ary_push_val(c, slots + 1, arr, ID2SYM(vm->const_names[i])));
    }
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT
korb_bi_warn(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) {      /* uplevel:/category: kwargs */
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t ci = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "category", 8)));
        if (ci >= 0 && korb_items_data(h->items)[2 * ci + 1] == ID2SYM(korb_intern(c->vm, "deprecated", 10)))
            return RESULT_OK(KORB_NIL);                            /* deprecation warnings are off by default */
        n--;
    }
    if (n == 0) return RESULT_OK(KORB_NIL);
    bool def; const VALUE out = korb_out_target(c, "$stderr", 7, &def);
    KorbIORep mem = KORB_IO_MEM_SINK;
    KorbIORep *const sink = def ? korb_io_std_rep(c->vm, 2) : &mem;
    for (uint32_t i = 0; i < n; i++) {
        RESULT r = korb_puts_one_to(c, slots, VALUE_SLICE_GET(args, i), sink);
        if (UNLIKELY(r.state != KORB_NORMAL)) { free(mem.wbuf); return r; }
    }
    if (def) return RESULT_OK(KORB_NIL);
    RESULT er = korb_out_emit(c, slots, out, 2, mem.wbuf, mem.wlen);
    free(mem.wbuf);
    return er;
}

/* The current $stdout/$stderr object; *is_default = it's still the init-time IO
 * (fast direct-fwrite path applies).  gv = "$stdout"/"$stderr". */
static VALUE korb_out_target(CTX *c, const char *gv, uint32_t gvlen, bool *is_default) {
    const VALUE o = korb_const_get(c->vm, korb_intern(c->vm, gv, gvlen));
    *is_default = KORB_OBJECT_P(o) && (((const AroObjectHeader *)(uintptr_t)o)->flags & KORB_FL_DEFAULT_IO);
    return o;
}
/* Emit a byte run to a reassigned/mocked output object via #write. */
static RESULT korb_out_emit(CTX *c, VALUE *slots, VALUE out, uint32_t stdidx, const char *data, size_t len) {
    if (UNLIKELY(!KORB_OBJECT_P(out))) { if (len) (void)korb_io_wr(korb_io_std_rep(c->vm, stdidx), data, len); return RESULT_OK(KORB_NIL); }
    slots[0] = out;
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, data, (uint32_t)len));
    RESULT r = korb_send(c, slots + 2, korb_intern(c->vm, "write", 5), 0, 1);
    return (r.state == KORB_NORMAL) ? RESULT_OK(KORB_NIL) : r;
}
/* Emit `warning: ...\n` to $stderr, suppressed only when $VERBOSE is nil (-W0);
 * routed through $stderr so mspec's `complain` matcher (which reassigns $stderr)
 * captures it.  Fire-and-forget: any write error from the sink is ignored. */
/* rb_warn with a source position: "file:line: warning: msg" (CRuby's shape). */
void korb_warn_at(CTX *c, VALUE *slots, const char *file, uint32_t line, const char *fmt, ...) {
    if (korb_const_get(c->vm, korb_intern(c->vm, "$VERBOSE", 8)) == KORB_NIL) return;
    char body[240];
    va_list ap; va_start(ap, fmt);
    const int bl = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (bl < 0) return;
    char msg[512];
    const int ml = file ? snprintf(msg, sizeof msg, "%s:%u: warning: %s\n", file, line, body)
                        : snprintf(msg, sizeof msg, "warning: %s\n", body);
    if (ml < 0) return;
    bool def; const VALUE out = korb_out_target(c, "$stderr", 7, &def);
    (void)korb_out_emit(c, slots, out, 2, msg, (size_t)(ml < (int)sizeof msg ? ml : (int)sizeof msg - 1));
}
/* like korb_warn, but for category warnings whose own flag is the gate */
void korb_warn_ignore_verbose(CTX *c, VALUE *slots, const char *fmt, ...) {
    char body[240];
    va_list ap; va_start(ap, fmt);
    const int bl = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (bl < 0) return;
    char msg[256];
    const int ml = snprintf(msg, sizeof msg, "warning: %s\n", body);
    if (ml < 0) return;
    bool def; const VALUE out = korb_out_target(c, "$stderr", 7, &def);
    (void)korb_out_emit(c, slots, out, 2, msg, (size_t)(ml < (int)sizeof msg ? ml : (int)sizeof msg - 1));
}
void korb_warn(CTX *c, VALUE *slots, const char *fmt, ...) {
    if (korb_const_get(c->vm, korb_intern(c->vm, "$VERBOSE", 8)) == KORB_NIL) return;
    char body[240];
    va_list ap; va_start(ap, fmt);
    const int bl = vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (bl < 0) return;
    char msg[256];
    const int ml = snprintf(msg, sizeof msg, "warning: %s\n", body);
    if (ml < 0) return;
    bool def; const VALUE out = korb_out_target(c, "$stderr", 7, &def);
    (void)korb_out_emit(c, slots, out, 2, msg, (size_t)(ml < (int)sizeof msg ? ml : (int)sizeof msg - 1));
}
static RESULT
korb_bi_puts(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool def; (void)korb_out_target(c, "$stdout", 7, &def);
    if (def) {                                           /* default $stdout → straight to the descriptor */
        KorbIORep *const rep = korb_io_std_rep(c->vm, 1);
        if (n == 0) return korb_io_wr_checked(c, slots, rep, "\n", 1);
        for (uint32_t i = 0; i < n; i++) CHECK(korb_puts_one_to(c, slots, VALUE_SLICE_GET(args, i), rep));
        return RESULT_OK(KORB_NIL);
    }
    /* redirected → capture, then $stdout.write (NOT $stdout.puts, which is
     * Kernel#puts on a plain object and would recurse forever). */
    KorbIORep mem = KORB_IO_MEM_SINK;
    if (n == 0) (void)korb_io_wr(&mem, "\n", 1);
    else for (uint32_t i = 0; i < n; i++) {
        RESULT pr = korb_puts_one_to(c, slots, VALUE_SLICE_GET(args, i), &mem);
        if (UNLIKELY(pr.state != KORB_NORMAL)) { free(mem.wbuf); return pr; }
    }
    bool def2; const VALUE out = korb_out_target(c, "$stdout", 7, &def2);   /* re-read: buffering may have GC'd */
    RESULT er = korb_out_emit(c, slots, out, 1, mem.wbuf, mem.wlen); free(mem.wbuf);
    return er;
}

/* Integer/Bignum/Rational/Float/String → canonical mpq.  false = unconvertible. */
/* Can this value be a Rational() operand at all?  (Used to name the offending
 * argument in the TypeError.) */
static bool korb_arg_rational_ok(VALUE v) {
    return FIXNUM_P(v) || KORB_BIGNUM_P(v) || KORB_RATIONAL_P(v) || KORB_FLOAT_P(v) ||
           KORB_STRING_P(v) || KORB_COMPLEX_P(v);
}
static bool korb_arg_to_mpq(VALUE v, korb_mq_t out) {
    if (FIXNUM_P(v)) { korb_mq_set_si(out, (long long)FIX2LONG(v), 1); return true; }
    if (KORB_BIGNUM_P(v)) { korb_mp_t z; korb_to_mpz(v, z); korb_mq_set_z(out, z); korb_mp_clear(z); return true; }
    if (KORB_RATIONAL_P(v)) { korb_mp_t zn, zd; korb_to_mpz(VAL2RAT(v)->num, zn); korb_to_mpz(VAL2RAT(v)->den, zd);
                              korb_mq_set_num(out, zn); korb_mq_set_den(out, zd); korb_mp_clear(zn); korb_mp_clear(zd); korb_mq_canonicalize(out); return true; }
    if (KORB_FLOAT_P(v)) { double d = korb_float_val(v); if (!isfinite(d)) return false; korb_mq_set_d(out, d); return true; }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v); char buf[512];
        if (s->len >= sizeof(buf)) return false;
        uint32_t b = 0, e = s->len;
        while (b < e && isspace((unsigned char)korb_strbuf_data(s->buf)[b])) b++;
        while (e > b && isspace((unsigned char)korb_strbuf_data(s->buf)[e - 1])) e--;
        uint32_t m = 0; for (uint32_t i = b; i < e; i++) buf[m++] = korb_strbuf_data(s->buf)[i]; buf[m] = 0;
        if (m == 0) return false;
        char *dot = strchr(buf, '.'), *slash = strchr(buf, '/');
        if (dot && !slash) {                                  /* decimal "X.Y" → (XY)/(10^|Y|) */
            const uint32_t fraclen = (uint32_t)(m - (uint32_t)(dot + 1 - buf));
            char numbuf[512]; uint32_t k = 0;
            for (uint32_t i = 0; i < m; i++) if (buf[i] != '.') numbuf[k++] = buf[i];
            numbuf[k] = 0;
            korb_mp_t zn, zd; korb_mp_init(zd);
            if (korb_mp_init_set_str(zn, numbuf, 10) != 0) { korb_mp_clear(zn); korb_mp_clear(zd); return false; }
            korb_mp_ui_pow_ui(zd, 10, fraclen);
            korb_mq_set_num(out, zn); korb_mq_set_den(out, zd); korb_mp_clear(zn); korb_mp_clear(zd);
            korb_mq_canonicalize(out); return true;
        }
        if (korb_mq_set_str(out, buf, 10) != 0) return false;     /* "a/b" or plain integer */
        if (korb_mq_sgn(out) == 0 && buf[0] != '0' && !(buf[0] == '-' && buf[1] == '0') && strchr(buf, '0') == NULL) return false;
        korb_mq_canonicalize(out); return true;
    }
    return false;
}
static RESULT
korb_bi_rational(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool exc = true;                                          /* trailing exception: kwarg */
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t kx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
        if (kx >= 0) {
            const VALUE ev = korb_items_data(h->items)[2 * kx + 1];   /* only true / false (CRuby) */
            if (UNLIKELY(ev != KORB_TRUE && ev != KORB_FALSE)) {
                char *ib = NULL; size_t il = 0;
                FILE *ims = open_memstream(&ib, &il);
                if (ims) { korb_fprint_inspect(c, ims, ev); fclose(ims); }
                RESULT er = korb_raise(c, slots, KORB_E_ARGUMENT, 0, "expected true or false as exception: %s", ib ? ib : "");
                free(ib);
                return er;
            }
            exc = (ev == KORB_TRUE); n--;
        }
    }
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (KORB_RATIONAL_P(nv) && n < 2) return RESULT_OK(nv);
    /* fast path: both Integer (preserves Bignum, no GMP rounding) */
    if (KORB_INTEGER_P(nv) && (n < 2 || KORB_INTEGER_P(VALUE_SLICE_GET(args, 1))))
        return korb_rat_new_v(c, slots, nv, n >= 2 ? VALUE_SLICE_GET(args, 1) : LONG2FIX(1));
    {
        korb_mq_t q0, q1; korb_mq_init(q0);
        if (!korb_arg_to_mpq(nv, q0)) { korb_mq_clear(q0); goto bad; }
        if (n >= 2) {
            korb_mq_init(q1);
            if (!korb_arg_to_mpq(VALUE_SLICE_GET(args, 1), q1)) { korb_mq_clear(q0); korb_mq_clear(q1); goto bad; }
            if (korb_mq_sgn(q1) == 0) { korb_mq_clear(q0); korb_mq_clear(q1); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
            korb_mq_div(q0, q0, q1); korb_mq_clear(q1);
        }
        korb_mq_canonicalize(q0);
        slots[0] = UNWRAP(korb_big_from_mpz(c, slots, korb_mq_numref(q0)));     /* num */
        slots[1] = UNWRAP(korb_big_from_mpz(c, slots + 1, korb_mq_denref(q0))); /* den */
        korb_mq_clear(q0);
        return korb_rat_new_v(c, slots + 2, slots[0], slots[1]);
    }
bad:
    /* A Complex operand (either position) is handled by Complex arithmetic:
     * Rational(3, Complex(2,0)) is 3/2, and a non-real denominator yields a
     * Complex quotient — exactly `numerator / denominator` in Ruby. */
    if (KORB_COMPLEX_P(nv) || (n >= 2 && KORB_COMPLEX_P(VALUE_SLICE_GET(args, 1)))) {
        slots[0] = nv;
        slots[1] = (n >= 2) ? VALUE_SLICE_GET(args, 1) : LONG2FIX(1);
        RESULT dr = korb_cpx_arith(c, slots + 2, slots[0], slots[1], 3);   /* division */
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        slots[2] = dr.value;
        if (KORB_COMPLEX_P(slots[2])) {          /* a real-valued quotient collapses to its real part */
            double imd;
            if (!korb_num_to_d(VAL2CPX(slots[2])->im, &imd) || imd != 0.0) return RESULT_OK(slots[2]);
            slots[2] = VAL2CPX(slots[2])->re;
        }
        if (KORB_INTEGER_P(slots[2])) return korb_rat_new_v(c, slots + 3, slots[2], LONG2FIX(1));
        return RESULT_OK(slots[2]);
    }
    /* #to_r, else #to_int (CRuby tries both before giving up) */
    if (n == 1 && KORB_OBJECT_P(nv)) {
        static const char *const conv[2] = { "to_r", "to_int" };
        static const uint32_t convlen[2] = { 4, 6 };
        for (int i = 0; i < 2; i++) {
            const uint32_t mid = korb_intern(c->vm, conv[i], convlen[i]);
            if (!korb_responds_to(c, nv, mid)) continue;
            slots[0] = nv;
            RESULT rr = korb_send_impl(c, slots + 1, mid, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
            if (KORB_RATIONAL_P(rr.value)) return RESULT_OK(rr.value);
            if (KORB_INTEGER_P(rr.value)) return korb_rat_new_v(c, slots + 1, rr.value, LONG2FIX(1));
            nv = slots[0];
        }
    }
    if (!exc) return RESULT_OK(KORB_NIL);
    {   /* CRuby names the offending operand: nil/true/false read as literals */
        const VALUE bad = (n >= 2 && !korb_arg_rational_ok(VALUE_SLICE_GET(args, 1)))
                            ? VALUE_SLICE_GET(args, 1) : VALUE_SLICE_GET(args, 0);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Rational",
                          bad == KORB_NIL ? "nil" : bad == KORB_TRUE ? "true" : bad == KORB_FALSE ? "false"
                                                  : korb_type_name(bad));
    }
}

bool korb_str_to_int(CTX *c, VALUE *slots, const char *s, uint32_t len, int base, VALUE *out);   /* defined below */

/* Parse one numeric component of a Complex literal (s[lo..hi)) into an
 * Integer / Float / Rational VALUE.  Empty or malformed → false. */
static bool korb_cpx_component(CTX *c, VALUE *scratch, const char *s, uint32_t lo, uint32_t hi, VALUE *out) {
    if (lo >= hi) return false;
    bool has_dot = false, has_slash = false, has_e = false;
    for (uint32_t k = lo; k < hi; k++) {
        const char ch = s[k];
        if (ch == '.') has_dot = true;
        else if (ch == '/') has_slash = true;
        else if ((ch == 'e' || ch == 'E') && k > lo) has_e = true;
    }
    if (has_slash) {                                          /* Rational "n/d" */
        uint32_t sl = lo; while (sl < hi && s[sl] != '/') sl++;
        VALUE nv, dv;
        if (!korb_str_to_int(c, scratch, s + lo, sl - lo, 10, &nv)) return false;
        scratch[0] = nv;                                      /* root num across den parse / rat alloc */
        if (!korb_str_to_int(c, scratch + 1, s + sl + 1, hi - sl - 1, 10, &dv)) return false;
        scratch[1] = dv;
        RESULT r = korb_rat_new_v(c, scratch + 2, scratch[0], scratch[1]);
        if (r.state != KORB_NORMAL) return false;
        *out = r.value; return true;
    }
    if (has_dot || has_e) {                                   /* Float */
        char buf[64]; if (hi - lo >= sizeof buf) return false;
        memcpy(buf, s + lo, hi - lo); buf[hi - lo] = '\0';
        char *ep; errno = 0; double d = strtod(buf, &ep);
        if (ep != buf + (hi - lo)) return false;
        RESULT r = korb_float_new(c, scratch, d);
        if (r.state != KORB_NORMAL) return false;
        *out = r.value; return true;
    }
    return korb_str_to_int(c, scratch, s + lo, hi - lo, 10, out);   /* Integer */
}

/* Parse a Complex literal string into slots[0] (real) and slots[1] (imag).
 * Supports "a", "bi", "a+bi", "a-bi", "+bi", "-bi", "i", "-i" (with 'j'/'J' as
 * the engineers' spelling of the imaginary unit) and the polar form "m@a", with
 * Integer / Float / Rational components.  Returns false on any malformation. */
static bool korb_parse_cpx(CTX *c, VALUE *slots, const char *s, uint32_t len) {
    uint32_t lo = 0, hi = len;
    while (lo < hi && isspace((unsigned char)s[lo])) lo++;
    while (hi > lo && isspace((unsigned char)s[hi - 1])) hi--;
    if (lo >= hi) return false;
    /* polar "modulus@argument" → modulus * (cos a + i sin a) */
    for (uint32_t k = lo + 1; k + 1 < hi; k++) {
        if (s[k] != '@') continue;
        VALUE mv, av;
        if (!korb_cpx_component(c, slots + 2, s, lo, k, &mv)) return false;
        slots[0] = mv;                                        /* root across the next parse's allocs */
        if (!korb_cpx_component(c, slots + 2, s, k + 1, hi, &av)) return false;
        double m, arg;
        if (!korb_num_to_d(slots[0], &m) || !korb_num_to_d(av, &arg)) return false;
        const RESULT rr = korb_float_new(c, slots + 2, m * cos(arg));
        if (rr.state != KORB_NORMAL) return false;
        slots[0] = rr.value;
        const RESULT ir = korb_float_new(c, slots + 2, m * sin(arg));
        if (ir.state != KORB_NORMAL) return false;
        slots[1] = ir.value;
        return true;
    }
    const bool last_i = (s[hi - 1] == 'i' || s[hi - 1] == 'I' || s[hi - 1] == 'j' || s[hi - 1] == 'J');
    const uint32_t end = last_i ? hi - 1 : hi;
    int32_t split = -1;                                       /* last +/- that is not an exponent sign and not the leading sign */
    for (uint32_t k = lo + 1; k < end; k++)
        if ((s[k] == '+' || s[k] == '-') && !(s[k - 1] == 'e' || s[k - 1] == 'E')) split = (int32_t)k;
    slots[0] = LONG2FIX(0); slots[1] = LONG2FIX(0);
    if (!last_i) {                                            /* pure real: no internal sign allowed ("1+2" invalid) */
        if (split >= 0) return false;
        return korb_cpx_component(c, slots + 2, s, lo, end, &slots[0]);
    }
    uint32_t ilo, ihi;                                       /* imaginary substring range */
    if (split >= 0) {                                        /* real = [lo,split), imag = [split,end) */
        if (!korb_cpx_component(c, slots + 2, s, lo, (uint32_t)split, &slots[0])) return false;
        ilo = (uint32_t)split; ihi = end;
    } else { ilo = lo; ihi = end; }
    if (ihi == ilo) { slots[1] = LONG2FIX(1); return true; }                       /* "i" */
    if (ihi - ilo == 1 && (s[ilo] == '+' || s[ilo] == '-')) {                       /* "+i" / "-i" */
        slots[1] = (s[ilo] == '-') ? LONG2FIX(-1) : LONG2FIX(1); return true;
    }
    return korb_cpx_component(c, slots + 2, s, ilo, ihi, &slots[1]);
}

static RESULT
korb_bi_complex(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool exc = true;                                          /* trailing exception: kwarg */
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t kx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
        if (kx >= 0) {
            const VALUE ev = korb_items_data(h->items)[2 * kx + 1];   /* only true / false (CRuby) */
            if (UNLIKELY(ev != KORB_TRUE && ev != KORB_FALSE)) {
                char *ib = NULL; size_t il = 0;
                FILE *ims = open_memstream(&ib, &il);
                if (ims) { korb_fprint_inspect(c, ims, ev); fclose(ims); }
                RESULT er = korb_raise(c, slots, KORB_E_ARGUMENT, 0, "expected true or false as exception: %s", ib ? ib : "");
                free(ib);
                return er;
            }
            exc = (ev == KORB_TRUE); n--;
        }
    }
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const VALUE re = VALUE_SLICE_GET(args, 0);
    const VALUE im = (n >= 2) ? VALUE_SLICE_GET(args, 1) : LONG2FIX(0);
    /* Complex(a, b) == a + b*i.  Evaluating via full complex arithmetic (rather
     * than picking components directly) reproduces CRuby's Float promotion of the
     * cross terms, e.g. Complex(c(1.5,2), c(-5,6.3)) → (-4.8-3.0i) not (...-3i). */
    VALUE ar, ai, br, bi;
    if (korb_cpx_parts(re, &ar, &ai) && korb_cpx_parts(im, &br, &bi)) {
        if (!KORB_COMPLEX_P(re) && !KORB_COMPLEX_P(im))               /* both plain reals → store as-is (no Float promotion) */
            return korb_cpx_new(c, slots, re, im);
        slots[0] = re; slots[1] = im;                                 /* root across allocs */
        slots[2] = UNWRAP(korb_cpx_new(c, slots + 2, LONG2FIX(0), LONG2FIX(1)));   /* i */
        slots[3] = UNWRAP(korb_cpx_arith(c, slots + 3, slots[1], slots[2], 2));    /* b * i */
        return korb_cpx_arith(c, slots + 4, slots[0], slots[3], 0);                /* a + b*i */
    }
    /* single String arg → parse a complex literal ("1+2i", "123", ...).  Copy to
     * a stack buffer first: korb_parse_cpx allocates (the components), which may
     * move the String and invalidate its data pointer mid-parse. */
    if (n == 1 && KORB_STRING_P(re)) {
        const KorbString *s = VAL2STR(re);
        if (UNLIKELY(!korb_enc_ascii_compat_idx(c->vm, KORB_STR_ENC(re)))) {   /* checked before parsing */
            char m[96];
            snprintf(m, sizeof m, "ASCII incompatible encoding: %s", korb_enc_name_of(c->vm, KORB_STR_ENC(re)));
            return korb_raise_enc_compat_msg(c, slots, m);
        }
        for (uint32_t i = 0; i < s->len; i++)
            if (UNLIKELY(korb_strbuf_data(s->buf)[i] == '\0')) {
                if (!exc) return RESULT_OK(KORB_NIL);
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "string contains null byte");
            }
        char buf[128];
        if (s->len < sizeof buf) {
            memcpy(buf, korb_strbuf_data(s->buf), s->len);
            if (korb_parse_cpx(c, slots, buf, s->len))
                return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
        }
        if (!exc) return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid value for convert(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
    }
    /* a Numeric that is not one of the builtin kinds: CRuby asks #real? — a real
     * one becomes the real component, a complex one is returned as-is, and with
     * two of them the answer is n1 + n2 * Complex(0, 1) through their own #+/#*. */
    if (korb_obj_is_numeric(c, re)) {
        const uint32_t real_p = korb_intern(c->vm, "real?", 5);
        slots[0] = re; slots[1] = im;                      /* parked: every send below allocates */
        bool re_real = true, im_real = true;
        if (korb_responds_to(c, slots[0], real_p)) {
            slots[2] = slots[0];                           /* receiver at cursor[-1] */
            const RESULT rr = korb_send(c, slots + 3, real_p, 0, 0);
            if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
            re_real = KORB_TRUTHY(rr.value);
        }
        if (n >= 2 && korb_obj_is_numeric(c, slots[1]) && korb_responds_to(c, slots[1], real_p)) {
            slots[2] = slots[1];
            const RESULT ir = korb_send(c, slots + 3, real_p, 0, 0);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            im_real = KORB_TRUTHY(ir.value);
        }
        if (n == 1 && !re_real) return RESULT_OK(slots[0]);           /* already complex */
        if (n == 1) return korb_cpx_new(c, slots + 2, slots[0], LONG2FIX(0));
        if (!re_real || !im_real) {                                    /* n1 + n2 * Complex(0,1) */
            slots[2] = UNWRAP(korb_cpx_new(c, slots + 4, LONG2FIX(0), LONG2FIX(1)));
            slots[3] = slots[1]; slots[4] = slots[2];
            const RESULT mr = korb_send(c, slots + 5, korb_intern(c->vm, "*", 1), 0, 1);
            if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
            slots[3] = slots[0]; slots[4] = mr.value;
            return korb_send(c, slots + 5, korb_intern(c->vm, "+", 1), 0, 1);
        }
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    /* non-numeric arg (Symbol, nil, multi-arg String, ...) */
    if (n == 1 && KORB_OBJECT_P(re) && korb_responds_to(c, re, korb_intern(c->vm, "to_c", 4))) {
        slots[0] = re;
        RESULT cr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_c", 4), 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (KORB_COMPLEX_P(cr.value)) return RESULT_OK(cr.value);
    }
    if (!exc) return RESULT_OK(KORB_NIL);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Complex", korb_coerce_name(c, VALUE_SLICE_GET(args, 0)));
}

static RESULT
korb_bi_p(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) return RESULT_OK(KORB_NIL);              /* p() → nil, no output */
    bool def; const VALUE outobj = korb_out_target(c, "$stdout", 7, &def);
    /* Always render into memory first: the inspect printers are FILE*-based, but
     * the bytes have to reach the same sink Kernel#puts uses, or the two
     * buffers would deliver a program's output out of order. */
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (!ms) return RESULT_OK(KORB_NIL);
    FILE *const out = ms;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE v = VALUE_SLICE_GET(args, i);
        /* user-class instance → honour a (possibly user-defined) #inspect via
         * dispatch; the default Object#inspect yields the same "#<Class>" form as
         * korb_fprint_inspect, so non-overriding objects are unaffected. */
        if (KORB_OBJECT_P(v) && VAL2OBJ(v)->klass != KORB_NIL) {
            slots[0] = v;
            RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "inspect", 7), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { fclose(ms); free(buf); return r; }
            if (LIKELY(KORB_STRING_P(r.value))) fwrite(korb_strbuf_data(VAL2STR(r.value)->buf), 1, VAL2STR(r.value)->len, out);
            else korb_fprint_inspect(c, out, r.value);
        } else {
            korb_fprint_inspect_s(c, slots, out, v);   /* containers dispatch element #inspect */
        }
        fputc('\n', out);
    }
    fclose(ms);
    if (def) { const RESULT wr = korb_io_wr_checked(c, slots, korb_io_std_rep(c->vm, 1), buf, sz); if (UNLIKELY(wr.state != KORB_NORMAL)) { free(buf); return wr; } }
    else { RESULT er = korb_out_emit(c, slots, outobj, 1, buf, sz); if (UNLIKELY(er.state != KORB_NORMAL)) { free(buf); return er; } }
    free(buf);
    /* M0: p(a) → a; p() → nil; p(a, b, ...) returns an Array in CRuby —
     * arrays land in M1, return the first arg until then. */
    return RESULT_OK(VALUE_SLICE_GET(args, 0));
}

/* Parse + run `str` (a String) as a top-level program with `self_val` as self.
 * Shared by Kernel#eval (self = a throwaway main) and instance/class/module_eval's
 * String form (self = the receiver, so `def` in class_eval attaches to the class).
 * The eval'd code sees its own locals only (no caller binding). */
static RESULT
korb_eval_str_self(CTX *c, VALUE *slots, VALUE str, VALUE self_val, const char *fname, int32_t line, VALUE cref)
{
    const KorbString *const s = VAL2STR(str);
    NODE *ast = koruby_parse_source_at(c, korb_strbuf_data(s->buf), s->len, fname, line, false);   /* immortal AST; no GC */
    if (UNLIKELY(ast == NULL)) return korb_raise_syntax_at(c, slots, "syntax error in eval string", fname);
    const uint32_t locals = koruby_toplevel_locals_cnt;
    slots[0] = 0; slots[1] = 0; slots[2] = 0;          /* eval frame meta: fb[-3]=magic, fb[-2]=EP, fb[-1]=self */
    VALUE *const fb = slots + 3;                        /* base (bottom header: fb[-2]=EP) */
    VALUE *const cur = fb + locals;                     /* the eval program's body cursor */
    memset(fb, 0, (size_t)locals * sizeof(VALUE));      /* zero its locals */
    fb[-1] = self_val;                                  /* self cell (base[-1]) */
    return korb_eval_run(c, slots, ast, cur, fname, cref);   /* raises report the caller's filename */
}

/* Kernel#eval(string) — parse + run the string as a program in a fresh frame
 * (self = a throwaway `main`).  No caller-binding/lvar access (M0 minimal): the
 * eval'd code sees its own locals + a fresh self, which suffices for the common
 * literal/expression eval (e.g. eval("(1..)")). */
/* Run a parsed eval program with the named file in effect, so a raise inside it
 * is reported against that file (the backtrace is snapshotted before the name is
 * restored).  `slots` is scratch for the snapshot; `cur` is the program cursor. */
static RESULT
korb_eval_run(CTX *c, VALUE *slots, NODE *ast, VALUE *cur, const char *fname, VALUE cref)
{
    const char *const saved = c->vm->script_name;
    const VALUE saved_definee = c->def_definee;
    const VALUE saved_cref = c->eval_cref;
    c->def_definee = cref;                              /* instance_eval/class_eval(String): `def` lands here */
    c->eval_cref = cref;                                /* … and so do constants */
    c->vm->script_name = fname;
    /* require_relative / __dir__ inside the eval resolve against the name the
     * eval was given (its 2nd argument), like CRuby. */
    const char *const saved_load = c->vm->cur_load_file;
    c->vm->cur_load_file = fname;
    RESULT r = EVAL(c, ast, cur);
    c->vm->cur_load_file = saved_load;
    c->def_definee = saved_definee;
    c->eval_cref = saved_cref;
    if (UNLIKELY(r.state == KORB_RAISE)) {
        slots[0] = r.value;                             /* park: capture allocates */
        (void)korb_capture_backtrace(c, slots);
        r.value = slots[0];
    }
    c->vm->script_name = saved;
    return r;
}

/* `fname` (may be NULL) becomes SyntaxError#path — the file the parse was told
 * it was reading, which for eval is its 3rd argument. */
static RESULT
korb_raise_syntax_at(CTX *c, VALUE *slots, const char *generic, const char *fname)
{
    const char *const detail = c->vm->last_syntax_msg;
    c->vm->last_syntax_msg = NULL;
    RESULT r = korb_raise(c, slots, KORB_E_SYNTAX, 0, "%s", detail ? detail : generic);
    if (fname != NULL && LIKELY(KORB_EXC_P(r.value))) {
        slots[0] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[0]);
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, fname, (uint32_t)strlen(fname)));
        korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__path", 7)), slots[1]);
        r.value = slots[0];
    }
    return r;
}
static RESULT
korb_raise_syntax(CTX *c, VALUE *slots, const char *generic)
{
    return korb_raise_syntax_at(c, slots, generic, NULL);
}

/* Eval `*src_slot` under the binding at `*bind_slot`: declared-scope parse,
 * seed locals from the binding, run, write back new/changed locals.  Both
 * pointers must be rooted slots (re-read across GC).  `self_slot` (rooted, may
 * be NULL) overrides the eval frame's self (instance_eval); `cref` other than
 * KORB_UNDEF installs the def/const scope for the duration. */
static RESULT
korb_eval_binding_core(CTX *c, VALUE *slots, VALUE *src_slot, VALUE *bind_slot,
                       const char *fname, int32_t eline, VALUE *self_slot, VALUE cref)
{
    if (strcmp(fname, "(eval)") == 0) {                 /* CRuby 3.3+: default is "(eval at FILE:LINE)" (callsite ≈ the binding's site) */
        const struct Node *const sn = VAL2BIND(*bind_slot)->src_node;
        uint32_t fsym, ln;
        if (sn && korb_get_srcloc(c->vm, sn, &fsym, &ln)) {
            char buf[512];
            const int n = snprintf(buf, sizeof buf, "(eval at %s:%u)", korb_sym_name(c->vm, fsym), ln);
            if (n > 0 && (size_t)n < sizeof buf)
                fname = korb_sym_name(c->vm, korb_intern(c->vm, buf, (uint32_t)n));
        }
    }
    const KorbBinding *b0 = VAL2BIND(*bind_slot);
    /* declare every binding local (frame names + dynamically-added `extra`
     * names) so the eval code recognises them as locals (no GC during parse). */
    const uint32_t ecnt = (b0->extra != KORB_NIL) ? VAL2HASH(b0->extra)->len : 0;
    const uint32_t declc = b0->name_cnt + ecnt;
    uint32_t *decl = declc ? malloc(sizeof(uint32_t) * declc) : NULL;
    for (uint32_t i = 0; i < b0->name_cnt; i++) decl[i] = KORB_BIND_TRIPLE(b0, i)[0];
    for (uint32_t i = 0; i < ecnt; i++) decl[b0->name_cnt + i] = SYM2ID(korb_items_data(VAL2HASH(b0->extra)->items)[2 * i]);
    const KorbString *const s = VAL2STR(*src_slot);
    NODE *entry = koruby_parse_binding_eval(c, korb_strbuf_data(s->buf), s->len, fname, eline, decl, declc);
    free(decl);
    if (UNLIKELY(entry == NULL)) return korb_raise_syntax(c, slots, "syntax error in eval string");
    const uint32_t L = koruby_toplevel_locals_cnt;
    const uint32_t ncnt = koruby_toplevel_local_cnt;
    const uint32_t *const nsyms = koruby_toplevel_local_syms;   /* stable malloc'd array; capture before EVAL */
    slots[0] = 0; slots[1] = 0; slots[2] = 0;       /* eval frame meta: fb[-3]=magic, fb[-2]=EP, fb[-1]=self(step2) */
    VALUE *const fb = slots + 3;                    /* eval frame base (bottom header: fb[-2]=EP) */
    VALUE *const cur = fb + L;
    memset(fb, 0, (size_t)L * sizeof(VALUE));
    for (uint32_t i = 0; i < ncnt; i++) {           /* seed: copy binding values into the eval frame's locals */
        const KorbBinding *b = VAL2BIND(*bind_slot);
        const int j = korb_bind_find(b, nsyms[i]);
        if (j >= 0) fb[i] = korb_bind_env_get(b, (uint32_t)j);
        else if (b->extra != KORB_NIL) { const int32_t hi = korb_hash_find(VAL2HASH(b->extra), ID2SYM(nsyms[i])); if (hi >= 0) fb[i] = korb_items_data(VAL2HASH(b->extra)->items)[2 * hi + 1]; }
    }
    fb[-1] = self_slot ? *self_slot : VAL2BIND(*bind_slot)->self;   /* self cell (base[-1]) */
    const VALUE saved_definee = c->def_definee;
    const VALUE saved_cref = c->eval_cref;
    const char *const saved_name = c->vm->script_name;
    if (cref != KORB_UNDEF) { c->def_definee = cref; c->eval_cref = cref; }
    c->vm->script_name = fname;                     /* raises inside report the eval's filename */
    const char *const saved_load = c->vm->cur_load_file;   /* require_relative / __dir__ resolve against it too */
    c->vm->cur_load_file = fname;
    /* the eval'd string INHERITS the body's bare-`private` default but its own
     * changes stay inside (CRuby scopes it to the eval's cref) */
    const uint8_t saved_vis = KORB_CLASS_P(fb[-1]) ? VAL2CLASS(fb[-1])->cur_visibility : 0xffu;
    RESULT er = EVAL(c, entry, cur);
    if (saved_vis != 0xffu && KORB_CLASS_P(fb[-1])) VAL2CLASS(fb[-1])->cur_visibility = saved_vis;
    if (cref != KORB_UNDEF) { c->def_definee = saved_definee; c->eval_cref = saved_cref; }
    if (UNLIKELY(er.state == KORB_RAISE)) {
        cur[0] = er.value;                          /* park: capture allocates */
        (void)korb_capture_backtrace(c, cur);
        er.value = cur[0];
    }
    c->vm->cur_load_file = saved_load;
    c->vm->script_name = saved_name;
    if (UNLIKELY(er.state != KORB_NORMAL)) return er;
    fb[L] = er.value;                               /* park result across writeback allocs */
    /* Re-read the binding from the rooted slot each step — the hash allocs
     * below trigger GC, which moves the binding (a cached VALUE goes stale). */
    #define EVAL_BIND VAL2BIND(*bind_slot)
    for (uint32_t i = 0; i < ncnt; i++) {           /* write back: existing → env, new → extra hash */
        const int j = korb_bind_find(EVAL_BIND, nsyms[i]);
        if (j >= 0) { korb_bind_env_set(c, EVAL_BIND, (uint32_t)j, fb[i]); continue; }
        fb[L + 1] = EVAL_BIND->extra;
        if (fb[L + 1] == KORB_NIL) { fb[L + 1] = UNWRAP(korb_hash_new(c, fb + L + 2, 4)); ARO_STORE(c, EVAL_BIND, (VALUE *)(uintptr_t)&EVAL_BIND->extra, fb[L + 1]); }
        fb[L + 2] = ID2SYM(nsyms[i]); fb[L + 3] = fb[i];
        CHECK(korb_hash_set(c, fb + L + 4, VALUE_REF_AT(&fb[L + 1]), VALUE_REF_AT(&fb[L + 2]), fb[L + 3]));
    }
    #undef EVAL_BIND
    return RESULT_OK(fb[L]);
}

static RESULT
korb_bi_eval(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    VALUE sv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(sv))) {                    /* coerce a non-String source via #to_str */
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(sv) && korb_responds_to_coerce_p(c, slots, &sv, to_str)) {
            slots[0] = sv;                                 /* root receiver across the dispatch */
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) { VALUE_REF_SET(VALUE_SLICE_REF(args, 0), sr.value); sv = sr.value; }
        }
        if (UNLIKELY(!KORB_STRING_P(sv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    }
    /* a non-nil 2nd arg must be a Binding (a Proc is no longer accepted) */
    if (UNLIKELY(VALUE_SLICE_LEN(args) >= 2 && VALUE_SLICE_GET(args, 1) != KORB_NIL &&
                 !KORB_BINDING_P(VALUE_SLICE_GET(args, 1))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Binding)",
                          korb_type_name(VALUE_SLICE_GET(args, 1)));
    const bool have_bind = VALUE_SLICE_LEN(args) >= 2 && KORB_BINDING_P(VALUE_SLICE_GET(args, 1));
    /* optional 3rd/4th args: the filename and first line the source is reported
     * as (CRuby uses them for __FILE__ / __LINE__ / backtraces) */
    const char *fname = "(eval)"; int32_t eline = 1;
    if (VALUE_SLICE_LEN(args) >= 3 && VALUE_SLICE_GET(args, 2) != KORB_NIL) {
        slots[0] = VALUE_SLICE_GET(args, 2);
        if (!KORB_STRING_P(slots[0])) {
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (!KORB_OBJECT_P(slots[0]) || !korb_responds_to(c, slots[0], to_str))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
            const RESULT fr = korb_send(c, slots + 1, to_str, 0, 0);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            if (!KORB_STRING_P(fr.value))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
            slots[0] = fr.value;
        }
        const KorbString *const fs = VAL2STR(slots[0]);
        fname = korb_sym_name(c->vm, korb_intern(c->vm, korb_strbuf_data(fs->buf), fs->len));   /* interned → outlives the frame */
        sv = VALUE_SLICE_GET(args, 0);                 /* the dispatch above may have moved the source */
    }
    if (VALUE_SLICE_LEN(args) >= 4 && VALUE_SLICE_GET(args, 3) != KORB_NIL) {
        korb_sword_t l = 1;
        if (!korb_to_index(VALUE_SLICE_GET(args, 3), &l))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, VALUE_SLICE_GET(args, 3)));
        eline = (int32_t)l;
    }
    const KorbString *s = VAL2STR(sv);
    if (have_bind) {                                    /* eval(str, binding) — seed the eval frame from the binding, run, write back */
        slots[0] = sv; slots[1] = VALUE_SLICE_GET(args, 1);
        return korb_eval_binding_core(c, slots + 2, &slots[0], &slots[1], fname, eline, NULL, KORB_UNDEF);
    }
    NODE *ast = koruby_parse_source_at(c, korb_strbuf_data(s->buf), s->len, fname, eline, false);   /* immortal AST; no GC */
    if (UNLIKELY(ast == NULL)) return korb_raise_syntax_at(c, slots, "syntax error in eval string", fname);
    const uint32_t locals = koruby_toplevel_locals_cnt;
    slots[0] = 0; slots[1] = 0; slots[2] = 0;          /* eval frame meta: fb[-3]=magic, fb[-2]=EP, fb[-1]=self(step2) */
    VALUE *const fb = slots + 3;                        /* base (bottom header: fb[-2]=EP) */
    VALUE *const cur = fb + locals;                     /* the eval program's body cursor */
    memset(fb, 0, (size_t)locals * sizeof(VALUE));      /* zero its locals */
    RESULT mr = korb_obj_new(c, cur, KORB_NIL);         /* fresh `main` self */
    if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
    fb[-1] = mr.value;                          /* self cell (base[-1]) */
    return korb_eval_run(c, slots, ast, cur, fname, KORB_NIL);
}

/* strict string→integer parse for Integer():  optional surrounding whitespace,
 * sign, base prefix (0x/0b/0o/0d, leading-0 octal when base auto), and `_`
 * digit separators (single, between digits).  base==0 = auto-detect.  Returns
 * false on any malformation; on success *out is a Fixnum or (on overflow, with
 * GMP) a Bignum.  May allocate (the Bignum), so takes the slots cursor. */
bool
korb_str_to_int(CTX *c, VALUE *slots, const char *s, uint32_t len, int base, VALUE *out)
{
    uint32_t i = 0, end = len;
    while (i < end && isspace((unsigned char)s[i])) i++;
    while (end > i && isspace((unsigned char)s[end - 1])) end--;
    if (i >= end) return false;
    int sign = 1;
    if (s[i] == '+' || s[i] == '-') { if (s[i] == '-') sign = -1; i++; }
    if (i < end && s[i] == '0' && i + 1 < end) {           /* prefix? */
        char p = s[i + 1] | 0x20;
        int pb = p == 'x' ? 16 : p == 'b' ? 2 : p == 'o' ? 8 : p == 'd' ? 10 : 0;
        if (pb && (base == 0 || base == pb)) { base = pb; i += 2; }
        else if (base == 0) base = 8;                      /* leading 0 → octal */
    }
    if (base == 0) base = 10;
    korb_sword_t acc = 0; bool any = false, prev_us = false;
    bool big = false; korb_mp_t z;
    for (; i < end; i++) {
        char ch = s[i];
        if (ch == '_') { if (!any || prev_us) goto bad; prev_us = true; continue; }
        prev_us = false;
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') d = (ch | 0x20) - 'a' + 10;
        else goto bad;
        if (d >= base) goto bad;
        any = true;
        if (big) { korb_mp_mul_ui(z, z, (unsigned long)base); korb_mp_add_ui(z, z, (unsigned long)d); continue; }
        korb_sword_t nn;
        if (UNLIKELY(__builtin_mul_overflow(acc, (korb_sword_t)base, &nn) || __builtin_add_overflow(nn, (korb_sword_t)d, &nn))) {
            korb_mp_init_set_si(z, acc); korb_mp_mul_ui(z, z, (unsigned long)base); korb_mp_add_ui(z, z, (unsigned long)d);
            big = true; continue;
        }
        acc = nn;
    }
    if (!any || prev_us) goto bad;
    if (big) {
        if (sign < 0) korb_mp_neg(z, z);
        RESULT r = korb_big_from_mpz(c, slots, z);
        korb_mp_clear(z);
        if (UNLIKELY(r.state != KORB_NORMAL)) return false;
        *out = r.value;
        return true;
    }
    acc *= sign;
    if (UNLIKELY(!FIXABLE(acc))) {   /* fixnum-overflow but int64-fit (e.g. 2^62) → Bignum, not a wrapped Fixnum */
        korb_mp_t z2; korb_mp_init_set_si(z2, (long)acc);
        RESULT r = korb_big_from_mpz(c, slots, z2);
        korb_mp_clear(z2);
        if (UNLIKELY(r.state != KORB_NORMAL)) return false;
        *out = r.value;
        return true;
    }
    *out = LONG2FIX(acc);   /* in-range Fixnum (larger values promoted to Bignum above) */
    return true;
  bad:
    if (big) korb_mp_clear(z);
    return false;
}

/* Integer(arg[, base]) — Kernel conversion.  Integer→itself, Float→truncate
 * toward zero, String→strict parse (ArgumentError on garbage). */
static RESULT
korb_bi_integer(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    /* trailing `exception:` keyword (default true) — when false, a conversion
     * failure returns nil instead of raising (matching Kernel#Integer). */
    bool exc = true;
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t kx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
        if (kx >= 0) {
            const VALUE ev = korb_items_data(h->items)[2 * kx + 1];   /* only true / false (CRuby) */
            if (UNLIKELY(ev != KORB_TRUE && ev != KORB_FALSE)) {
                char *ib = NULL; size_t il = 0;
                FILE *ims = open_memstream(&ib, &il);
                if (ims) { korb_fprint_inspect(c, ims, ev); fclose(ims); }
                RESULT er = korb_raise(c, slots, KORB_E_ARGUMENT, 0, "expected true or false as exception: %s", ib ? ib : "");
                free(ib);
                return er;
            }
            exc = (ev == KORB_TRUE); n--;
        }
    }
#define INT_FAIL(...) do { return exc ? korb_raise(c, slots, __VA_ARGS__) : RESULT_OK(KORB_NIL); } while (0)
    if (UNLIKELY(n < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    /* A base (2nd arg) is only meaningful for a String value → reject numerics. */
    if (n >= 2 && VALUE_SLICE_GET(args, 1) != KORB_NIL &&
        (FIXNUM_P(a0) || KORB_FLOAT_P(a0) || KORB_BIGNUM_P(a0) || KORB_RATIONAL_P(a0)))
        INT_FAIL(KORB_E_ARGUMENT, 0, "base specified for non string value");
    if (FIXNUM_P(a0)) return RESULT_OK(a0);
    if (KORB_BIGNUM_P(a0)) return RESULT_OK(a0);
    if (KORB_FLOAT_P(a0)) {
        double d = korb_float_val(a0);
        if (UNLIKELY(!isfinite(d)))
            INT_FAIL(KORB_E_FLOAT_DOMAIN, 0, "%s", isnan(d) ? "NaN" : (d < 0 ? "-Infinity" : "Infinity"));
        if (UNLIKELY(!FIXABLE((korb_sword_t)d) || fabs(d) >= 9.0e18)) {   /* large finite Float → Bignum (trunc) */
            korb_mp_t z; korb_mp_init_set_d(z, d);                 /* korb_mp_set_d truncates toward zero */
            RESULT r = korb_big_from_mpz(c, slots, z);
            korb_mp_clear(z);
            return r;
        }
        return RESULT_OK(LONG2FIX((korb_sword_t)d));           /* trunc toward zero */
    }
    if (KORB_RATIONAL_P(a0))                                /* Integer(Rational) → truncate toward zero */
        return korb_rat_intdiv(c, slots, VAL2RAT(a0)->num, VAL2RAT(a0)->den, 2);
    /* Optional base (2nd arg): valid only with a String (or #to_str) value;
     * coerced via #to_int; range 2..36 (0 = auto-detect). */
    int base = 0; bool base_given = (n >= 2);
    if (base_given) {
        VALUE b = VALUE_SLICE_GET(args, 1);
        if (b == KORB_NIL) base_given = false;             /* explicit nil base == none */
        else {
            korb_sword_t bi;
            if (FIXNUM_P(b)) bi = FIX2LONG(b);
            else if (KORB_OBJECT_P(b)) {                    /* #to_int on the base */
                VALUE bv = b; RESULT bc = korb_coerce_to_int(c, slots, &bv);
                if (UNLIKELY(bc.state != KORB_NORMAL)) return bc;
                if (bc.value != KORB_TRUE || !FIXNUM_P(bv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(b));
                bi = FIX2LONG(bv);
            }
            else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(b));
            base = (int)bi;
        }
    }
    if (KORB_STRING_P(a0)) {
        if (base_given && base != 0 && (base < 2 || base > 36))
            INT_FAIL(KORB_E_ARGUMENT, 0, "invalid radix %d", base);
        const KorbString *s = VAL2STR(a0);
        VALUE v;
        if (UNLIKELY(!korb_str_to_int(c, slots, korb_strbuf_data(s->buf), s->len, base, &v)))
            INT_FAIL(KORB_E_ARGUMENT, 0, "invalid value for Integer(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
        return RESULT_OK(v);
    }
    /* A base with a non-String value is an error (checked after String above). */
    if (base_given && !KORB_OBJECT_P(a0))
        INT_FAIL(KORB_E_ARGUMENT, 0, "base specified for non string value");
    if (a0 == KORB_NIL)
        INT_FAIL(KORB_E_TYPE, 0, "can't convert nil into Integer");
    if (KORB_OBJECT_P(a0)) {                               /* user object → #to_int, then #to_i, then #to_str (CRuby) */
        VALUE v = a0;
        RESULT ci = korb_coerce_to_int(c, slots, &v);      /* dispatches #to_int */
        if (UNLIKELY(ci.state != KORB_NORMAL)) return exc ? ci : RESULT_OK(KORB_NIL);
        if (ci.value == KORB_TRUE) return RESULT_OK(v);
        if (KORB_BIGNUM_P(v)) return RESULT_OK(v);          /* #to_int returned a Bignum (out of the index range) */
        slots[0] = a0;
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (korb_responds_to_coerce(c, slots + 1, slots[0], to_str)) {   /* #to_str → parse as a String (honours base) */
            RESULT r = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return exc ? r : RESULT_OK(KORB_NIL);
            if (KORB_STRING_P(r.value)) {
                if (base_given && base != 0 && (base < 2 || base > 36)) INT_FAIL(KORB_E_ARGUMENT, 0, "invalid radix %d", base);
                slots[0] = r.value; const KorbString *s = VAL2STR(slots[0]); VALUE iv;
                if (UNLIKELY(!korb_str_to_int(c, slots + 1, korb_strbuf_data(s->buf), s->len, base, &iv)))
                    INT_FAIL(KORB_E_ARGUMENT, 0, "invalid value for Integer(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
                return RESULT_OK(iv);
            }
        }
        const uint32_t to_i = korb_intern(c->vm, "to_i", 4);
        slots[0] = a0;
        if (korb_responds_to_coerce(c, slots + 1, slots[0], to_i)) {   /* honors respond_to_missing? (proxies/mocks) */
            slots[0] = a0;                                       /* re-root (coerce used slots+1 scratch) */
            RESULT r = korb_send_impl(c, slots + 1, to_i, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return exc ? r : RESULT_OK(KORB_NIL);
            korb_sword_t tmp;
            if (korb_to_index(r.value, &tmp) || KORB_BIGNUM_P(r.value)) return RESULT_OK(r.value);
        }
    }
    INT_FAIL(KORB_E_TYPE, 0, "can't convert %s into Integer", korb_type_name(a0));
#undef INT_FAIL
}

/* Kernel#format / sprintf(fmt, *args) — delegate to String#% with the rest
 * args collected into an Array (the form korb_m_str_format expects). */
static RESULT
korb_bi_printf(CTX *c, VALUE *slots, VALUE_SLICE args);   /* fwd */
static RESULT
korb_bi_format(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no format string given");
    VALUE fmt = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(fmt))) {                  /* coerce the format via #to_str */
        slots[0] = fmt;
        if (korb_responds_to(c, fmt, korb_intern(c->vm, "to_str", 6))) {
            RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (UNLIKELY(!KORB_STRING_P(r.value)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(args, 0)));
            fmt = r.value;
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(fmt));
    }
    slots[0] = fmt;                                       /* root format string */
    const uint32_t n = VALUE_SLICE_LEN(args) - 1;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, n));     /* arr at slots[1], scratch from slots+2 */
    VALUE_REF arr = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_ary_push_val(c, slots + 2, arr, VALUE_SLICE_GET(args, i + 1)));
    return korb_m_str_format(c, slots + 2, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(&slots[1], 1));
}
/* Kernel#printf(fmt, *args) — write sprintf(fmt, *args) to stdout, return nil.
 * (The printf(io, fmt, ...) form and $stdout redirection are not supported.) */
static RESULT
korb_bi_printf(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) return RESULT_OK(KORB_NIL);
    const VALUE first = VALUE_SLICE_GET(args, 0);
    VALUE target; bool def = false;
    RESULT fr;
    if (KORB_STRING_P(first)) {                           /* printf(format, *args) → $stdout */
        target = korb_out_target(c, "$stdout", 7, &def);
        fr = korb_bi_format(c, slots + 2, args);
    } else {                                              /* printf(io, format, *args) → io.write(...) */
        target = first;
        fr = korb_bi_format(c, slots + 2, VALUE_SLICE_MAKE(args.p + 1, n - 1));
    }
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    if (def || target == KORB_NIL || !KORB_OBJECT_P(target)) {   /* default $stdout → raw stdout */
        if (KORB_STRING_P(fr.value)) { const KorbString *const s = VAL2STR(fr.value); CHECK(korb_io_wr_checked(c, slots, korb_io_std_rep(c->vm, 1), korb_strbuf_data(s->buf), s->len)); }
        return RESULT_OK(KORB_NIL);
    }
    slots[0] = target; slots[1] = fr.value;               /* target.write(formatted) */
    RESULT wr = korb_send(c, slots + 2, korb_intern(c->vm, "write", 5), 0, 1);
    if (UNLIKELY(wr.state != KORB_NORMAL)) return wr;
    return RESULT_OK(KORB_NIL);
}

/* Kernel#Array(arg) — nil→[], Array→itself, else [arg]. */
static RESULT
korb_bi_array(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (a0 == KORB_NIL) return korb_ary_new(c, slots, 0);
    if (KORB_ARRAY_P(a0)) return RESULT_OK(a0);
    slots[0] = a0;                                        /* root across to_ary/to_a dispatch + alloc */
    const uint32_t conv[2] = { korb_intern(c->vm, "to_ary", 6), korb_intern(c->vm, "to_a", 4) };
    const char *const cname[2] = { "to_ary", "to_a" };
    for (int k = 0; k < 2; k++) {                         /* CRuby: to_ary, then to_a; an Array result wins */
        if (korb_responds_to(c, slots[0], conv[k])) {
            slots[1] = slots[0];                          /* receiver */
            const char *const onm = korb_type_name(slots[0]);
            const RESULT r = korb_send_impl(c, slots + 2, conv[k], 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_ARRAY_P(r.value)) return RESULT_OK(r.value);
            if (r.value != KORB_NIL)                      /* non-Array, non-nil result → TypeError */
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array (%s#%s gives %s)", onm, onm, cname[k], korb_type_name(r.value));
        }
    }
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, 1));     /* otherwise wrap: [a0] */
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[1]);
}
/* Kernel#Hash(arg): nil/[] → {}, Hash → itself, #to_hash → its result, else TypeError. */
static RESULT
korb_bi_hash(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (a0 == KORB_NIL) return korb_hash_new(c, slots, 0);
    if (KORB_HASH_P(a0)) return RESULT_OK(a0);
    if (KORB_ARRAY_P(a0) && VAL2ARY(a0)->len == 0) return korb_hash_new(c, slots, 0);   /* Hash([]) → {} */
    const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
    if (KORB_OBJECT_P(a0) && korb_responds_to_coerce_p(c, slots, &a0, to_hash)) {
        slots[0] = a0;                                    /* receiver */
        const RESULT r = korb_send_impl(c, slots + 1, to_hash, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_HASH_P(r.value)) return RESULT_OK(r.value);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Hash", korb_type_name(a0));
}

/* Kernel#String(arg) — arg.to_s (String → itself). */
static RESULT
korb_bi_string(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (KORB_STRING_P(a0)) return RESULT_OK(a0);
    slots[0] = a0;                                        /* root across dispatch */
    if (!korb_responds_to(c, a0, korb_intern(c->vm, "to_s", 4)))   /* CRuby: no #to_s → TypeError, not NoMethodError */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into String", korb_type_name(a0));
    RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (UNLIKELY(!KORB_STRING_P(r.value)))               /* #to_s must return a String */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to String (%s#to_s gives %s)",
                          korb_coerce_name(c, slots[0]), korb_coerce_name(c, slots[0]), korb_type_name(r.value));
    return r;
}

/* Float(arg) — Kernel conversion.  Float→itself, Integer→to f, String→strict. */
static RESULT
korb_bi_float(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool exc = true;                                          /* trailing exception: kwarg */
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t kx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
        if (kx >= 0) {
            const VALUE ev = korb_items_data(h->items)[2 * kx + 1];   /* only true / false (CRuby) */
            if (UNLIKELY(ev != KORB_TRUE && ev != KORB_FALSE)) {
                char *ib = NULL; size_t il = 0;
                FILE *ims = open_memstream(&ib, &il);
                if (ims) { korb_fprint_inspect(c, ims, ev); fclose(ims); }
                RESULT er = korb_raise(c, slots, KORB_E_ARGUMENT, 0, "expected true or false as exception: %s", ib ? ib : "");
                free(ib);
                return er;
            }
            exc = (ev == KORB_TRUE); n--;
        }
    }
#define FLT_FAIL(...) do { return exc ? korb_raise(c, slots, __VA_ARGS__) : RESULT_OK(KORB_NIL); } while (0)
    if (UNLIKELY(n < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (KORB_FLOAT_P(a0)) return RESULT_OK(a0);
    if (FIXNUM_P(a0)) return korb_float_new(c, slots, (double)FIX2LONG(a0));
    if (KORB_BIGNUM_P(a0)) return korb_float_new(c, slots, korb_big_to_d(a0));
    if (KORB_RATIONAL_P(a0)) { double d; (void)korb_num_to_d(a0, &d); return korb_float_new(c, slots, d); }
    if (KORB_STRING_P(a0)) {
        const KorbString *s = VAL2STR(a0);
        char buf[64];
        if (s->len >= sizeof(buf)) FLT_FAIL(KORB_E_ARGUMENT, 0, "invalid value for Float(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
        for (uint32_t k = 0; k < s->len; k++)                                /* an embedded NUL is invalid (CRuby raises) */
            if (korb_strbuf_data(s->buf)[k] == '\0') FLT_FAIL(KORB_E_ARGUMENT, 0, "string contains null byte");
        /* Copy, dropping a Ruby-legal `_` between two digits (decimal digits for
         * a decimal float, hex digits for a 0x float — `0x1_0`, `0x1_0p10`);
         * leading/trailing/double `_` and `_` adjacent to a non-digit stay so
         * strtod then rejects the whole string. */
        const char *sp = korb_strbuf_data(s->buf); uint32_t sl = s->len;
        while (sl && isspace((unsigned char)*sp)) { sp++; sl--; }             /* skip leading ws for the hex test */
        const bool hex = sl >= 2 && sp[0] == '0' && (sp[1] == 'x' || sp[1] == 'X');
        uint32_t bl = 0;
        for (uint32_t k = 0; k < s->len; k++) {
            const char ch = korb_strbuf_data(s->buf)[k];
            if (ch == '_' && k > 0 && k + 1 < s->len) {
                const unsigned char pv = (unsigned char)korb_strbuf_data(s->buf)[k - 1], nx = (unsigned char)korb_strbuf_data(s->buf)[k + 1];
                if (hex ? (isxdigit(pv) && isxdigit(nx)) : (isdigit(pv) && isdigit(nx))) continue;   /* valid digit separator */
            }
            buf[bl++] = ch;
        }
        buf[bl] = '\0';
        char *endp; errno = 0;
        double d = strtod(buf, &endp);
        if (UNLIKELY(endp == buf))                                           /* no digits consumed (empty / whitespace-only) */
            FLT_FAIL(KORB_E_ARGUMENT, 0, "invalid value for Float(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
        while (*endp && isspace((unsigned char)*endp)) endp++;
        if (UNLIKELY(*endp != '\0'))
            FLT_FAIL(KORB_E_ARGUMENT, 0, "invalid value for Float(): \"%.*s\"", (int)s->len, korb_strbuf_data(s->buf));
        return korb_float_new(c, slots, d);
    }
    if (KORB_COMPLEX_P(a0)) {                                                 /* real-only Complex → its real part; else RangeError */
        const VALUE im = VAL2CPX(a0)->im;
        if (im == LONG2FIX(0) || (KORB_FLOAT_P(im) && korb_float_val(im) == 0.0)) {
            double d; (void)korb_num_to_d(VAL2CPX(a0)->re, &d); return korb_float_new(c, slots, d);
        }
        FLT_FAIL(KORB_E_RANGE, 0, "can't convert %s into Float", korb_type_name(a0));
    }
    if (KORB_OBJECT_P(a0)) {                                  /* object with #to_f → use it (nil/true/false excluded: not objects) */
        const uint32_t to_f = korb_intern(c->vm, "to_f", 4);
        if (korb_responds_to_coerce(c, slots + 1, a0, to_f)) {   /* honors respond_to_missing? (proxies/mocks) */
            const char *const cls = korb_type_name(a0);       /* capture before dispatch (a0 may move) */
            slots[0] = a0;
            RESULT r = korb_send_impl(c, slots + 1, to_f, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (UNLIKELY(!KORB_FLOAT_P(r.value))) FLT_FAIL(KORB_E_TYPE, 0, "can't convert %s into Float (%s#to_f gives %s)", cls, cls, korb_type_name(r.value));
            return RESULT_OK(r.value);
        }
    }
    FLT_FAIL(KORB_E_TYPE, 0, "can't convert %s into Float", korb_type_name(a0));
#undef FLT_FAIL
}

/* __binread(path) — read a whole file as a binary String (the one file-I/O
 * primitive; the `File` class methods are layered on top in Ruby). */
static RESULT
korb_bi_binread(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    VALUE pv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    const KorbString *ps = VAL2STR(pv);
    char path[4096];
    if (UNLIKELY(ps->len >= sizeof(path)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "path too long");
    memcpy(path, korb_strbuf_data(ps->buf), ps->len); path[ps->len] = '\0';
    size_t rd = 0;
    char *const buf = korb_file_slurp(path, &rd);
    if (UNLIKELY(!buf))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory - %s", path);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)rd);
    free(buf);
    return r;
}

/* __gc_stat_raw() — [count, minor, major, total_bytes, total_ns] (backs GC.stat). */
static RESULT
korb_bi_gc_stat_raw(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)args;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 5));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)aro_gc_count(c))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)aro_gc_minor_count(c))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)aro_gc_major_count(c))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)aro_gc_total_bytes(c))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)(aro_gc_total_seconds(c) * 1e9))));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* __gc_start() — force a full collection (backs GC.start). */
static RESULT
korb_bi_gc_start(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots; (void)args;
    aro_gc_collect(c);
    return RESULT_OK(KORB_NIL);
}
/* __clock_gettime() — monotonic seconds as Float (backs Process.clock_gettime). */
static RESULT
korb_bi_clock_gettime(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)args;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return korb_float_new(c, slots, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static RESULT
korb_bi_print(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool def; (void)korb_out_target(c, "$stdout", 7, &def);
    /* Rendering goes through the FILE*-based printer into memory (no descriptor
     * involved); the bytes then make one trip to the sink. */
    char *buf = NULL; size_t sz = 0; FILE *const ms = open_memstream(&buf, &sz);
    if (!ms) return RESULT_OK(KORB_NIL);
    for (uint32_t i = 0; i < n; i++) korb_fprint_to_s_s(c, slots, ms, VALUE_SLICE_GET(args, i));   /* slots: a user #to_s is dispatched */
    fclose(ms);
    if (def) {                                           /* default $stdout → straight to the descriptor */
        const RESULT wr = korb_io_wr_checked(c, slots, korb_io_std_rep(c->vm, 1), buf, sz);
        free(buf);
        return wr;
    }
    /* redirected → $stdout.write (NOT $stdout.print, which is Kernel#print on a
     * plain object and would recurse forever). */
    bool def2; const VALUE out = korb_out_target(c, "$stdout", 7, &def2);   /* re-read: buffering may have GC'd */
    RESULT er = korb_out_emit(c, slots, out, 1, buf, sz); free(buf);
    return er;
}

/* Build (do not deliver) the exception described by Kernel#raise-style
 * positional args — the shared engine behind Kernel#raise / Thread#raise /
 * Fiber#raise.  OK(exc) is the fully-formed exception (message / #exception
 * protocol / user #initialize / backtrace arg applied); RAISE is a genuine
 * argument error (TypeError etc.) that belongs to the calling context. */
static RESULT
korb_exc_build(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) {   /* bare `raise` → re-raise $!, else fresh RuntimeError("") */
        const VALUE cur = korb_errinfo_top(c);
        if (KORB_EXC_P(cur)) return RESULT_OK(cur);
        const RESULT r = korb_raise(c, slots, KORB_E_RUNTIME, 0, "%s", "");
        return RESULT_OK(r.value);
    }
    const VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (KORB_STRING_P(a0) && n == 1) {               /* raise "msg" → RuntimeError; `raise "m", extra` is a TypeError (String is not a class) */
        const KorbString *const s = VAL2STR(a0);
        const RESULT r = korb_raise(c, slots, KORB_E_RUNTIME, 0, "%.*s", (int)s->len, korb_strbuf_data(s->buf));
        return RESULT_OK(r.value);
    }
    if (UNLIKELY(n > 3))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..3)", n);
    if (KORB_EXC_P(a0) && n == 1) return RESULT_OK(a0);   /* re-raise an exception object (backtrace preserved) */
    if (KORB_CLASS_P(a0)) {                          /* raise SomeError[, msg[, backtrace]] */
        /* a user-defined singleton `exception` (not Exception's own) drives the
         * build, as CRuby's rb_make_exception does */
        const uint32_t exc_mid0 = korb_intern(c->vm, "exception", 9);
        const VALUE sing = korb_dispatch_class(c, a0);
        VALUE exc_owner = KORB_NIL;
        if (KORB_CLASS_P(sing) && korb_class_find_method(sing, exc_mid0, &exc_owner) != NULL &&
            KORB_CLASS_P(exc_owner) && exc_owner != korb_dispatch_class(c, korb_builtin_class_obj(c->vm, KORB_C_EXCEPTION))) {
            slots[0] = a0;
            uint32_t xargc = 0;
            if (n >= 2) { slots[1] = VALUE_SLICE_GET(args, 1); xargc = 1; }
            RESULT xr = korb_send(c, slots + 1 + xargc, exc_mid0, 0, xargc);
            if (UNLIKELY(xr.state != KORB_NORMAL)) return xr;
            if (UNLIKELY(!KORB_EXC_P(xr.value)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "exception object expected");
            slots[0] = xr.value;
            if (n >= 3) {
                slots[1] = VALUE_SLICE_GET(args, 2);
                RESULT br = korb_send(c, slots + 2, korb_intern(c->vm, "set_backtrace", 13), 0, 1);
                if (UNLIKELY(br.state != KORB_NORMAL)) return br;
            }
            return RESULT_OK(slots[0]);
        }
        /* nearest builtin-exception ancestor supplies the etype; a user
         * subclass is remembered in exc_class (for #class and rescue). */
        int et = -1;
        for (VALUE cc = a0; KORB_CLASS_P(cc); cc = VAL2CLASS(cc)->superclass)
            if (VAL2CLASS(cc)->exc_etype >= 0) { et = VAL2CLASS(cc)->exc_etype; break; }
        if (et < 0) {
            /* abstract base (Exception/StandardError, et -1) or a user subclass
             * of one: carry a generic etype — exc_class drives rescue/#class. */
            const VALUE exc_base = korb_builtin_class_obj(c->vm, KORB_C_EXCEPTION);
            if (KORB_CLASS_P(exc_base) && korb_class_le(a0, exc_base)) et = KORB_E_RUNTIME;
            else return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
        }
        slots[0] = a0;                               /* root the class across korb_raise's allocs */
        RESULT r;
        if (n >= 2 && KORB_STRING_P(VALUE_SLICE_GET(args, 1))) {
            const KorbString *const s = VAL2STR(VALUE_SLICE_GET(args, 1));
            r = korb_raise(c, slots + 1, (unsigned)et, 0, "%.*s", (int)s->len, korb_strbuf_data(s->buf));
        } else {
            char cbuf[256];                          /* anonymous class → its #to_s ("#<Class:0x...>") */
            const char *cname;
            if (VAL2CLASS(slots[0])->name_sym != 0) cname = korb_sym_name(c->vm, VAL2CLASS(slots[0])->name_sym);
            else {
                slots[1] = slots[0];
                const RESULT tr = korb_send(c, slots + 2, korb_intern(c->vm, "to_s", 4), 0, 0);
                if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
                uint32_t tl = KORB_STRING_P(tr.value) ? VAL2STR(tr.value)->len : 0;
                if (tl >= sizeof cbuf) tl = sizeof cbuf - 1;
                if (tl) memcpy(cbuf, korb_strbuf_data(VAL2STR(tr.value)->buf), tl);
                cbuf[tl] = '\0';
                cname = cbuf;
            }
            r = korb_raise(c, slots + 1, (unsigned)et, 0, "%s", cname);
            if (n >= 2 && VALUE_SLICE_GET(args, 1) != KORB_NIL && KORB_EXC_P(r.value))   /* non-String message → store the object; #message #to_s's it */
                ARO_STORE(c, VAL2EXC(r.value), &VAL2EXC(r.value)->msg, VALUE_SLICE_GET(args, 1));
        }
        slots[1] = r.value;                          /* root the exception */
        if (VAL2CLASS(slots[0])->exc_etype < 0)      /* user subclass → tag the instance with it */
            ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->exc_class, slots[0]);
        /* A user-defined #initialize runs so its `super` can set a custom
         * message (the default msg above is the fallback / class name). */
        const uint32_t init_mid = korb_intern(c->vm, "initialize", 10);
        VALUE idef = KORB_NIL;
        struct korb_method *const uinit = korb_class_find_method(slots[0], init_mid, &idef);
        if (uinit != NULL && uinit->kind != KORB_METHOD_CFUNC) {
            /* CRuby passes only the message to the constructor — a third arg is
             * the backtrace, applied below, never an #initialize argument. */
            const uint32_t iargc = (n >= 2) ? 1 : 0;
            if (iargc) slots[2] = VALUE_SLICE_GET(args, 1);
            VALUE *const icur = slots + 2 + iargc;
            RESULT ir = korb_invoke_method(c, icur, uinit, iargc, 0, init_mid, slots[1], idef, NULL, NULL, KORB_NIL);
            if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
            slots[1] = (icur - iargc)[-1];           /* the (moved) exception */
        }
        if (n >= 3) {                                /* raise Class, msg, backtrace */
            slots[2] = VALUE_SLICE_GET(args, 2);
            RESULT br = korb_send(c, slots + 3, korb_intern(c->vm, "set_backtrace", 13), 0, 1);
            if (UNLIKELY(br.state != KORB_NORMAL)) return br;
        }
        return RESULT_OK(slots[1]);
    }
    {   /* raise obj[, msg[, backtrace]] — the #exception protocol; also covers
         * `raise exc_instance, msg` (a fresh exception carrying msg). */
        const uint32_t exc_mid = korb_intern(c->vm, "exception", 9);
        VALUE recv = a0;
        if (UNLIKELY(!(KORB_EXC_P(recv) || (KORB_OBJECT_P(recv) && korb_responds_to_coerce_p(c, slots, &recv, exc_mid)))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
        slots[1] = recv;
        uint32_t argc = 0;
        if (n >= 2) { slots[2] = VALUE_SLICE_GET(args, 1); argc = 1; }
        RESULT er = korb_send(c, slots + 2 + argc, exc_mid, 0, argc);
        if (UNLIKELY(er.state != KORB_NORMAL)) return er;
        if (UNLIKELY(!KORB_EXC_P(er.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "exception object expected");
        slots[1] = er.value;
        if (n >= 3) {
            slots[2] = VALUE_SLICE_GET(args, 2);
            RESULT br = korb_send(c, slots + 3, korb_intern(c->vm, "set_backtrace", 13), 0, 1);
            if (UNLIKELY(br.state != KORB_NORMAL)) return br;
        }
        return RESULT_OK(slots[1]);
    }
}
/* korb_exc_build plus the trailing `cause:` keyword (the full Kernel#raise
 * argument list).  OK(exc) = ready to deliver; RAISE = argument error. */
static RESULT
korb_exc_build_with_cause(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    bool has_cause_kw = false; VALUE cause_val = KORB_NIL;
    /* Only a trailing hash marked as kwargs is the cause keyword; an explicit
     * `raise("m", {cause: e})` stays a positional argument (TypeError). */
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1)) &&
        (((const AroObjectHeader *)(uintptr_t)VALUE_SLICE_GET(args, n - 1))->flags & KORB_FL_KWARGS)) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(args, n - 1));
        const int32_t ci = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "cause", 5)));
        if (ci >= 0 && h->len == 1) { cause_val = korb_items_data(h->items)[2 * ci + 1]; has_cause_kw = true; n--; }
    }
    if (has_cause_kw) {
        if (cause_val != KORB_NIL && !KORB_EXC_P(cause_val))     /* cause must be an Exception or nil */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "exception object expected");
        if (n == 0)                                             /* raise(cause: …) with no exception */
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "only cause is given with no arguments");
    }
    slots[0] = cause_val;                                       /* root across the build's allocs */
    RESULT rr = korb_exc_build(c, slots + 1, VALUE_SLICE_MAKE(args.p, n));
    if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
    if (has_cause_kw && rr.value != slots[0]) {                 /* cause == exc → silently not set (CRuby) */
        slots[1] = rr.value;                                    /* the built exception (rooted) */
        for (VALUE cc = slots[0]; KORB_EXC_P(cc); cc = VAL2EXC(cc)->cause)   /* reject a circular cause chain */
            if (cc == slots[1]) return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "circular causes");
        ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->cause, slots[0]);   /* override (incl. explicit nil) */
        return RESULT_OK(slots[1]);
    }
    return rr;
}
/* Kernel#raise — build, then deliver in the calling context. */
static RESULT
korb_bi_raise(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const RESULT r = korb_exc_build_with_cause(c, slots, args);
    return (r.state == KORB_NORMAL) ? RESULT_RAISE_(r.value) : r;
}

/* ---------------------------------------------------------------------------
 * CTX creation (main.c entry helper).
 * ------------------------------------------------------------------------- */

CTX *
korb_ctx_new(void)
{
    CTX *c = calloc(1, sizeof(CTX));
    if (!c) { fprintf(stderr, "koruby_precise: out of memory (CTX)\n"); abort(); }

    /* slots: virtual reservation, lazy commit, guard page at the end
     * (v2_design §3.5).  The buffer address is fixed for the CTX lifetime. */
    size_t bytes = (size_t)8 << 20;                 /* 8 MiB default */
    const char *env = getenv("KORUBY_SLOTS_BYTES");
    if (env && *env) {
        long long req = atoll(env);
        if (req > 0) bytes = (size_t)req;
    }
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    bytes = (bytes + page - 1) & ~(page - 1);

#ifdef KORB_WASI
    /* WASI の mmap エミュレーションは MAP_NORESERVE も PROT_NONE のガード
     * ページも扱えない。素の確保で代用する = **値スタックの溢れがガード
     * ページで捕まらない**ので、そこはインタプリタ側の深さ検査に頼る。 */
    /* wasm の線形メモリは memory.grow で増えた分が仕様上ゼロなので calloc は
     * 要らない (mmap(MAP_ANONYMOUS) のゼロ保証と同じ理屈)。calloc だと memset が
     * そのまま RSS になる。 */
    char *base = malloc(bytes + page);
    if (base == NULL) { perror("koruby_precise: alloc slots"); abort(); }
#else
    char *base = mmap(NULL, bytes + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("koruby_precise: mmap slots"); abort(); }
    if (mprotect(base + bytes, page, PROT_NONE) != 0) {
        perror("koruby_precise: mprotect guard");
        abort();
    }
#endif

    /* Leading slack cells: every frame's EP is base[-2] (self at base[-1]); the
     * toplevel frame sits at c->slots so its EP cell is c->slots[-2].  Reserve two
     * VALUEs before the logical
     * base (AROH_VISIT_ROOTS scans from c->slots-2 to include them). */
    c->slots = (VALUE *)base + 2;
    c->slots[-1] = 0;                                  /* toplevel self cell (base[-1]; populated in step 2) */
    korb_ep_set(c->slots, 0);                          /* toplevel EP (base[-2]): no open env yet */
    c->slots_top = c->slots;
    c->slots_limit = (VALUE *)(base + bytes);
    c->slots_high_water = c->slots;

    /* Native C-stack floor: the AST walker recurses on the C stack, which
     * overflows long before the slots reservation.  Margin must cover one
     * deepest expression chain + the raise/unwind path. */
    {
        pthread_attr_t attr;
        void *stack_addr = NULL;
        size_t stack_size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            pthread_attr_getstack(&attr, &stack_addr, &stack_size);
            pthread_attr_destroy(&attr);
        }
        if (stack_addr && stack_size > 0) {
            size_t margin = stack_size / 8;
            if (margin < (size_t)512 << 10) margin = (size_t)512 << 10;
            c->cstack_limit = (const char *)stack_addr + margin;
            korb_re_sync_floor(c);
        }
        else {
            char here;
            c->cstack_limit = &here - ((size_t)6 << 20);   /* fallback: ~6 MiB below */
    korb_re_sync_floor(c);
        }
    }

    c->vm = calloc(1, sizeof(struct korb_vm));
    if (!c->vm) { fprintf(stderr, "koruby_precise: out of memory (VM)\n"); abort(); }
    c->vm->mcache = calloc(KORB_MCACHE_N, sizeof(*c->vm->mcache));
    if (!c->vm->mcache) { fprintf(stderr, "koruby_precise: out of memory (mcache)\n"); abort(); }
    c->vm->shapes = calloc(64, sizeof(*c->vm->shapes));   /* [0]=unused, [1]=root */
    if (!c->vm->shapes) { fprintf(stderr, "koruby_precise: out of memory (shapes)\n"); abort(); }
    c->vm->shape_capa = 64;
    c->vm->shape_cnt = 2;
    c->vm->shapes[1].edge_sym = 0xFFFFFFFFu;             /* root: no ivars, no edge */

    aro_gc_init(c);

    korb_builtin_define(c, "puts",  korb_bi_puts,  -1);
    korb_builtin_define(c, "gets",  korb_bi_gets,  -1);
    korb_builtin_define(c, "readline", korb_bi_readline, -1);
    korb_builtin_define(c, "p",     korb_bi_p,     -1);
    korb_builtin_define(c, "print", korb_bi_print, -1);
    korb_builtin_define(c, "raise", korb_bi_raise, -1);
    korb_builtin_define(c, "fail",  korb_bi_raise, -1);   /* Kernel#fail — alias of raise */
    /* the raw writer: Kernel#warn itself is Ruby (prelude/exception.rb) so it can
       delegate to Warning.warn and introspect its arity */
    korb_builtin_define(c, "__warn_raw", korb_bi_warn, -1);
    korb_builtin_define(c, "global_variables", korb_bi_global_variables, 0);
    korb_builtin_define(c, "__set_gvar", korb_bi_set_gvar, 2);
    korb_mark_loaded(c->vm, "set");   /* Set is core-loaded in modern Ruby: require 'set' ⇒ false */
    korb_builtin_define(c, "__dir__", korb_bi_dir, 0);
    korb_builtin_define(c, "__method__", korb_bi_method_name, 0);
    korb_builtin_define(c, "__callee__", korb_bi_method_name, 0);
    korb_builtin_define(c, "require", korb_bi_require, -1);
    korb_builtin_define(c, "require_relative", korb_bi_require_relative, -1);
    korb_builtin_define(c, "load", korb_bi_load, -1);
    korb_builtin_define(c, "eval",  korb_bi_eval,  -1);
    korb_builtin_define(c, "__transcode",    korb_bi_transcode,  -1);
    korb_builtin_define(c, "__transcodable?", korb_bi_transcodable, 1);
    korb_builtin_define(c, "sleep", korb_bi_sleep, -1);   /* TIMER blop (thread.c) — 他 green thread は走れる */
    korb_builtin_define(c, "rand",  korb_bi_rand,  -1);
    korb_builtin_define(c, "srand", korb_bi_srand, -1);
    korb_builtin_define(c, "__binread", korb_bi_binread, 1);
    korb_builtin_define(c, "__clock_gettime", korb_bi_clock_gettime, -1);
    korb_builtin_define(c, "__gc_stat_raw", korb_bi_gc_stat_raw, 0);
    korb_builtin_define(c, "__gc_start", korb_bi_gc_start, 0);
    korb_builtin_define(c, "Integer", korb_bi_integer, -1);
    korb_builtin_define(c, "Float", korb_bi_float, -1);
    korb_builtin_define(c, "Array", korb_bi_array, -1);
    korb_builtin_define(c, "Hash", korb_bi_hash, -1);
    korb_builtin_define(c, "String", korb_bi_string, -1);
    korb_builtin_define(c, "format", korb_bi_format, -1);
    korb_builtin_define(c, "sprintf", korb_bi_format, -1);
    korb_builtin_define(c, "printf", korb_bi_printf, -1);
    korb_builtin_define(c, "exit", korb_bi_exit, -1);
    korb_builtin_define(c, "exit!", korb_bi_exit_bang, -1);
    korb_builtin_define(c, "abort", korb_bi_abort, -1);
    korb_builtin_define(c, "Rational", korb_bi_rational, -1);
    korb_builtin_define(c, "Complex", korb_bi_complex, -1);

    /* Builtin class objects must exist before core methods are registered onto
     * them (korb_def_cmethod attaches CFUNC entries to the class objects). */
    korb_init_builtin_classes(c, c->slots);
    {   /* require / require_relative / load as real Kernel instance methods, so
         * `class X; def require(n); super n; end; end` has a superclass method
         * to reach.  This MUST come after korb_init_builtin_classes — the Kernel
         * module does not exist before it, and an earlier attempt sat above the
         * call and silently did nothing (KORB_CLASS_P(nil) is false). */
        const VALUE kmod = korb_const_get(c->vm, korb_intern(c->vm, "Kernel", 6));
        if (KORB_CLASS_P(kmod)) {
            korb_class_def_cfn(c, kmod, "require",          korb_m_kernel_require,          -1);
            korb_class_def_cfn(c, kmod, "require_relative", korb_m_kernel_require_relative, -1);
            korb_class_def_cfn(c, kmod, "load",             korb_m_kernel_load,             -1);
        }
    }
    korb_init_exception_classes(c, c->slots);
    korb_init_math(c, c->slots);
    korb_init_regexp(c, c->slots);
    korb_init_env(c, c->slots);
    korb_init_zlib(c);
    korb_init_file(c, c->slots);
    korb_init_io(c, c->slots);
    korb_init_process(c, c->slots);
    korb_init_socket(c, c->slots);
    korb_init_time(c, c->slots);
    /* RUBY_* version constants (specs/guards reference these).  Values track the
     * CRuby the differential tests run against so version guards behave the same. */
    {
        static const char *const rc[][2] = {
            {"RUBY_VERSION", "4.0.2"}, {"RUBY_ENGINE", "ruby"},
            {"RUBY_PLATFORM", "x86_64-linux"}, {"RUBY_ENGINE_VERSION", "4.0.2"},
            {"RUBY_DESCRIPTION", KORUBY_RUBY_DESCRIPTION},
            {"RUBY_COPYRIGHT", "ruby - Copyright (C) 1993-2026 Yukihiro Matsumoto"},
            {"RUBY_RELEASE_DATE", "2026-01-01"}, {"RUBY_REVISION", "0000000000000000000000000000000000000000"},
        };
        for (size_t i = 0; i < sizeof(rc) / sizeof(rc[0]); i++) {
            const VALUE s = korb_str_new(c, c->slots, rc[i][1], (uint32_t)strlen(rc[i][1])).value;
            /* CRuby freezes every RUBY_* string constant. */
            ((AroObjectHeader *)(uintptr_t)s)->flags |= KORB_FL_FROZEN;
            korb_const_define(c, korb_intern(c->vm, rc[i][0], (uint32_t)strlen(rc[i][0])), s);
        }
        korb_const_define(c, korb_intern(c->vm, "RUBY_PATCHLEVEL", 15), LONG2FIX(0));
    }
    korb_register_core_methods(c);

    /* resolve dispatch-hot method names once (see struct korb_vm). */
    c->vm->mid_send        = korb_intern(c->vm, "send", 4);
    c->vm->mid___send__    = korb_intern(c->vm, "__send__", 8);
    c->vm->mid_public_send = korb_intern(c->vm, "public_send", 11);
    c->vm->mid_new         = korb_intern(c->vm, "new", 3);
    c->vm->mid_initialize  = korb_intern(c->vm, "initialize", 10);
    c->vm->mid_yield       = korb_intern(c->vm, "yield", 5);
    c->vm->name_fiber      = korb_intern(c->vm, "Fiber", 5);
    c->vm->name_thread     = korb_intern(c->vm, "Thread", 6);
    c->vm->name_struct     = korb_intern(c->vm, "Struct", 6);
    c->vm->mid_aref        = korb_intern(c->vm, "[]", 2);
    c->vm->mid_aset        = korb_intern(c->vm, "[]=", 3);
    c->vm->mid_eqq         = korb_intern(c->vm, "===", 3);
    c->vm->mid_dm_super    = korb_intern(c->vm, "__dm_super__", 12);
    c->vm->mid_band        = korb_intern(c->vm, "&", 1);
    c->vm->mid_bor         = korb_intern(c->vm, "|", 1);
    c->vm->mid_bxor        = korb_intern(c->vm, "^", 1);
    c->vm->mid_shl         = korb_intern(c->vm, "<<", 2);
    c->vm->mid_shr         = korb_intern(c->vm, ">>", 2);
    c->vm->mid_cmp         = korb_intern(c->vm, "<=>", 3);
    c->vm->mid_eq          = korb_intern(c->vm, "==", 2);

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}

/* --- korb_embed_*: --build exe startup helpers ------------------------------
 * Called from the generated _embed.c AST builder to rebuild the parse-built
 * side structures (`void *` operands, symbol arrays) that emit_ast can't bake
 * as plain literals.  Symbol names re-intern here, so the embedded AST is
 * independent of the bake process's intern order.  All allocations are
 * immortal (AST metadata), matching parse. */

static void *
korb_embed_alloc(size_t size)
{
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "koruby_precise: out of memory (embed)\n"); abort(); }
    return p;
}

NODE **
korb_embed_nodes(uint32_t cnt, ...)
{
    NODE **const a = korb_embed_alloc(sizeof(NODE *) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) a[i] = va_arg(ap, NODE *);
    va_end(ap);
    return a;
}

/* (name, len) pairs → interned uint32 array. */
uint32_t *
korb_embed_syms(CTX *c, uint32_t cnt, ...)
{
    uint32_t *const a = korb_embed_alloc(sizeof(uint32_t) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        const char *const name = va_arg(ap, const char *);
        const uint32_t len = va_arg(ap, uint32_t);
        a[i] = korb_intern(c->vm, name, len);
    }
    va_end(ap);
    return a;
}

/* (name, len, slot, deflt NODE*) per entry. */
void *
korb_embed_kw_info(CTX *c, uint32_t count, int32_t kwrest_slot, ...)
{
    struct korb_kw_info *const kw = korb_embed_alloc(sizeof(*kw));
    kw->count = count;
    kw->kwrest_slot = kwrest_slot;
    kw->entries = korb_embed_alloc(sizeof(struct korb_kw_entry) * (count ? count : 1));
    va_list ap;
    va_start(ap, kwrest_slot);
    for (uint32_t i = 0; i < count; i++) {
        const char *const name = va_arg(ap, const char *);
        const uint32_t len = va_arg(ap, uint32_t);
        kw->entries[i].mid = korb_intern(c->vm, name, len);
        kw->entries[i].slot = va_arg(ap, uint32_t);
        kw->entries[i].deflt = va_arg(ap, NODE *);
    }
    va_end(ap);
    return kw;
}

/* (kind, name-or-NULL, len) per entry.  Flexible-array struct. */
void *
korb_embed_param_info(CTX *c, uint32_t n, ...)
{
    struct korb_param_info *const pi =
        korb_embed_alloc(sizeof(*pi) + sizeof(struct korb_param_entry) * n);
    pi->n = n;
    va_list ap;
    va_start(ap, n);
    for (uint32_t i = 0; i < n; i++) {
        pi->e[i].kind = (uint8_t)va_arg(ap, uint32_t);
        const char *const name = va_arg(ap, const char *);
        const uint32_t len = va_arg(ap, uint32_t);
        pi->e[i].name = name ? korb_intern(c->vm, name, len) : 0;
    }
    va_end(ap);
    return pi;
}

void *
korb_embed_u8(uint32_t cnt, ...)
{
    uint8_t *const a = korb_embed_alloc(cnt ? cnt : 1);
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) a[i] = (uint8_t)va_arg(ap, unsigned int);
    va_end(ap);
    return a;
}

void *
korb_embed_u16(uint32_t cnt, ...)
{
    uint16_t *const a = korb_embed_alloc(sizeof(uint16_t) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) a[i] = (uint16_t)va_arg(ap, unsigned int);
    va_end(ap);
    return a;
}

void *
korb_embed_i32(uint32_t cnt, ...)
{
    int32_t *const a = korb_embed_alloc(sizeof(int32_t) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) a[i] = (int32_t)va_arg(ap, int);
    va_end(ap);
    return a;
}

/* (kind, slot, name-or-NULL, len) per target: kind 0 keeps the slot; kinds
 * 1 (@ivar) / 2 (CONST) re-intern the name into `data`. */
void *
korb_embed_het_descs(CTX *c, uint32_t cnt, ...)
{
    struct korb_het_desc { int32_t kind; int32_t data; };
    struct korb_het_desc *const d =
        korb_embed_alloc(sizeof(*d) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        d[i].kind = va_arg(ap, int32_t);
        const int32_t slot = va_arg(ap, int32_t);
        const char *const name = va_arg(ap, const char *);
        const uint32_t len = va_arg(ap, uint32_t);
        d[i].data = name ? (int32_t)korb_intern(c->vm, name, len) : slot;
    }
    va_end(ap);
    return d;
}

/* (mid name, len, ivar name, len, is_writer) per accessor. */
void *
korb_embed_attr_descs(CTX *c, uint32_t cnt, ...)
{
    struct korb_attr_desc *const d =
        korb_embed_alloc(sizeof(*d) * (cnt ? cnt : 1));
    va_list ap;
    va_start(ap, cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        const char *const mn = va_arg(ap, const char *);
        const uint32_t ml = va_arg(ap, uint32_t);
        d[i].mid = korb_intern(c->vm, mn, ml);
        const char *const in = va_arg(ap, const char *);
        const uint32_t il = va_arg(ap, uint32_t);
        d[i].ivar = korb_intern(c->vm, in, il);
        d[i].is_writer = (uint8_t)va_arg(ap, uint32_t);
    }
    va_end(ap);
    return d;
}

/* Binding scope table: L, ns[L], cnt, then (name,len,depth,slot) per entry. */
uint32_t *
korb_embed_binding_scope(CTX *c, uint32_t L, ...)
{
    va_list ap;
    va_start(ap, L);
    uint32_t ns[64];
    for (uint32_t d = 0; d < L && d < 64; d++) ns[d] = va_arg(ap, uint32_t);
    const uint32_t cnt = va_arg(ap, uint32_t);
    uint32_t *tbl = korb_embed_alloc(sizeof(uint32_t) * (1 + L + 3 * (cnt ? cnt : 1)));
    tbl[0] = L;
    for (uint32_t d = 0; d < L; d++) tbl[1 + d] = ns[d];
    for (uint32_t i = 0; i < cnt; i++) {
        const char *const name = va_arg(ap, const char *);
        const uint32_t len = va_arg(ap, uint32_t);
        tbl[1 + L + 3 * i]     = korb_intern(c->vm, name, len);
        tbl[1 + L + 3 * i + 1] = va_arg(ap, uint32_t);
        tbl[1 + L + 3 * i + 2] = va_arg(ap, uint32_t);
    }
    va_end(ap);
    return tbl;
}

/* Recursive pattern descriptor: fixed head, then ecnt sub-pattern pointers,
 * then kcnt key VALUEs (symbols arrive as ID2SYM(korb_intern(...)) results). */
void *
korb_embed_pat(CTX *c, uint32_t kind, int32_t bind_off, NODE *value_node,
               uint32_t n, uint32_t npost, uint32_t ecnt, ...)
{
    (void)c;
    struct korb_pat *const p = korb_embed_alloc(sizeof(*p));
    p->kind = (uint8_t)kind;
    p->bind_off = bind_off;
    p->value_node = value_node;
    p->n = n;
    p->npost = npost;
    p->elems = NULL;
    p->keys = NULL;
    va_list ap;
    va_start(ap, ecnt);
    if (ecnt > 0) {
        p->elems = korb_embed_alloc(sizeof(struct korb_pat *) * ecnt);
        for (uint32_t i = 0; i < ecnt; i++)
            p->elems[i] = va_arg(ap, struct korb_pat *);
    }
    const uint32_t kcnt = va_arg(ap, uint32_t);
    if (kcnt > 0) {
        p->keys = korb_embed_alloc(sizeof(VALUE) * kcnt);
        for (uint32_t i = 0; i < kcnt; i++) p->keys[i] = va_arg(ap, VALUE);
    }
    va_end(ap);
    return p;
}
