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
#include <ctype.h>
#include <errno.h>
#include <ucontext.h>
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
    VALUE v = aro_gc_alloc(c, size);      /* may collect; scans [slots, slots_top) */
    AroObjectHeader *h = (AroObjectHeader *)(uintptr_t)v;
    h->flags = (uint16_t)type;
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
    b->data[len] = '\0';
    return s;
}

RESULT
korb_str_new(CTX *c, VALUE *slots, const char *bytes, uint32_t len)
{
    KorbString *s = korb_str_alloc(c, slots, len);
    memcpy(s->buf->data, bytes, len);
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
    memcpy(nb->data, s->buf->data, s->len);
    nb->data[s->len] = '\0';
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
    memcpy(s->buf->data + s->len, src, n);
    s->len += n;
    s->buf->data[s->len] = '\0';
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

RESULT
korb_float_new(CTX *c, VALUE *slots, double d)
{
    VALUE imm = korb_d2flo(d);                 /* immediate flonum — no heap box */
    if (imm) return RESULT_OK(imm);
    return korb_float_box(c, slots, d);
}

static intptr_t korb_gcd_pos(intptr_t a, intptr_t b) {   /* gcd of |a|,|b| */
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { intptr_t t = a % b; a = b; b = t; }
    return a;
}
/* korb_to_mpz / korb_big_from_mpz live in builtins/bignum.c (included later);
 * forward-declare so the Rational core can reduce Bignum num/den. */
#ifdef KORB_HAVE_GMP
static void korb_to_mpz(VALUE v, mpz_t out);
static RESULT korb_big_from_mpz(CTX *c, VALUE *slots, const mpz_t src);
#endif

/* Make a reduced Rational from VALUE num/den (Fixnum or Bignum); den != 0,
 * normalized den > 0.  Fixnum-fits fast path; Bignum path reduces via mpz_gcd. */
RESULT
korb_rat_new_v(CTX *c, VALUE *slots, VALUE num, VALUE den)
{
    if (LIKELY(FIXNUM_P(num) && FIXNUM_P(den))) {
        intptr_t n = FIX2LONG(num), d = FIX2LONG(den);
        if (UNLIKELY(d == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        if (d < 0) { n = -n; d = -d; }
        intptr_t g = korb_gcd_pos(n, d);
        if (g > 1) { n /= g; d /= g; }
        KorbRational *r = korb_alloc(c, slots, sizeof(KorbRational), KORB_OBJ_RATIONAL);
        ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->num, LONG2FIX(n));
        ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->den, LONG2FIX(d));
        return RESULT_OK((VALUE)r);
    }
#ifdef KORB_HAVE_GMP
    mpz_t zn, zd, zg;
    korb_to_mpz(num, zn); korb_to_mpz(den, zd);
    if (UNLIKELY(mpz_sgn(zd) == 0)) { mpz_clear(zn); mpz_clear(zd); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
    if (mpz_sgn(zd) < 0) { mpz_neg(zn, zn); mpz_neg(zd, zd); }
    mpz_init(zg); mpz_gcd(zg, zn, zd);
    if (mpz_cmp_ui(zg, 1) > 0) { mpz_divexact(zn, zn, zg); mpz_divexact(zd, zd, zg); }
    mpz_clear(zg);
    RESULT nr = korb_big_from_mpz(c, slots, zn); mpz_clear(zn);
    if (UNLIKELY(nr.state != KORB_NORMAL)) { mpz_clear(zd); return nr; }
    slots[0] = nr.value;                                       /* root num across den alloc */
    RESULT dr = korb_big_from_mpz(c, slots + 1, zd); mpz_clear(zd);
    if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
    slots[1] = dr.value;
    KorbRational *r = korb_alloc(c, slots + 2, sizeof(KorbRational), KORB_OBJ_RATIONAL);
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->num, slots[0]);
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->den, slots[1]);
    return RESULT_OK((VALUE)r);
#else
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Bignum Rational not available (no GMP)");
#endif
}
/* intptr → VALUE (Fixnum if FIXABLE, else Bignum — e.g. float_to_rat's 1<<62). */
static RESULT korb_intptr_to_val(CTX *c, VALUE *slots, intptr_t n) {
    if (LIKELY(FIXABLE(n))) return RESULT_OK(LONG2FIX(n));
#ifdef KORB_HAVE_GMP
    mpz_t z; mpz_init_set_si(z, (long)n);
    RESULT r = korb_big_from_mpz(c, slots, z); mpz_clear(z); return r;
#else
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (no GMP)");
#endif
}
/* Legacy intptr entry (some callers pass values up to 1<<62, beyond Fixnum). */
RESULT korb_rat_new(CTX *c, VALUE *slots, intptr_t num, intptr_t den) {
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
    if (UNLIKELY(!korb_as_rat_v(l, &slots[0], &slots[1]) || !korb_as_rat_v(r, &slots[2], &slots[3])))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Rational", korb_type_name(KORB_RATIONAL_P(l) ? r : l));
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
#ifdef KORB_HAVE_GMP
        mpz_t a, b, t; korb_to_mpz(ln, a); korb_to_mpz(rd, t); mpz_mul(a, a, t);
        korb_to_mpz(rn, b); korb_to_mpz(ld, t); mpz_mul(b, b, t);
        int cmp = mpz_cmp(a, b); mpz_clear(a); mpz_clear(b); mpz_clear(t);
        return (cmp > 0) - (cmp < 0);
#endif
    }
    double x, y;
    if (korb_num_to_d(l, &x) && korb_num_to_d(r, &y)) return (x > y) - (x < y);
    return 2;
}
static RESULT korb_m_rat_num(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RAT->num); }
static RESULT korb_m_rat_den(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RAT->den); }
static RESULT korb_m_rat_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; double n, d; korb_num_to_d(SELF_RAT->num, &n); korb_num_to_d(SELF_RAT->den, &d); return korb_float_new(c, slots, n / d); }
static RESULT korb_m_rat_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* integer floor-div of rational num/den (mode: 0 floor, 1 ceil, 2 trunc, 3 round-half-away). */
static RESULT korb_rat_intdiv(CTX *c, VALUE *slots, VALUE num, VALUE den, int mode) {
    if (LIKELY(FIXNUM_P(num) && FIXNUM_P(den))) {
        const intptr_t n = FIX2LONG(num), d = FIX2LONG(den);   /* d > 0 (normalized) */
        intptr_t q = n / d, rem = n % d;
        if (mode == 0)      { if (rem != 0 && n < 0) q--; }
        else if (mode == 1) { if (rem != 0 && n > 0) q++; }
        else if (mode == 3) { const intptr_t ar = rem < 0 ? -rem : rem; if (ar * 2 >= d) q += (n < 0 ? -1 : 1); }
        return RESULT_OK(LONG2FIX(q));   /* trunc (mode 2) = bare q */
    }
#ifdef KORB_HAVE_GMP
    mpz_t zn, zd, zq, zr; korb_to_mpz(num, zn); korb_to_mpz(den, zd); mpz_init(zq); mpz_init(zr);
    if (mode == 0)      mpz_fdiv_qr(zq, zr, zn, zd);
    else if (mode == 1) mpz_cdiv_qr(zq, zr, zn, zd);
    else                mpz_tdiv_qr(zq, zr, zn, zd);
    if (mode == 3) { mpz_t two_ar; mpz_init(two_ar); mpz_abs(two_ar, zr); mpz_mul_ui(two_ar, two_ar, 2);
                     if (mpz_cmp(two_ar, zd) >= 0) { if (mpz_sgn(zn) < 0) mpz_sub_ui(zq, zq, 1); else mpz_add_ui(zq, zq, 1); }
                     mpz_clear(two_ar); }
    mpz_clear(zn); mpz_clear(zd); mpz_clear(zr);
    RESULT r = korb_big_from_mpz(c, slots, zq); mpz_clear(zq);
    return r;
#else
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Bignum Rational not available");
#endif
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
static RESULT korb_m_rat_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_intdiv(c, slots, SELF_RAT->num, SELF_RAT->den, 2); }
static RESULT korb_m_rat_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_intdiv(c, slots, SELF_RAT->num, SELF_RAT->den, 0); }
static RESULT korb_m_rat_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_intdiv(c, slots, SELF_RAT->num, SELF_RAT->den, 1); }
static RESULT korb_m_rat_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_intdiv(c, slots, SELF_RAT->num, SELF_RAT->den, 3); }
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
static RESULT korb_m_rat_cmp_m(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; int r = korb_rat_cmp(VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); return RESULT_OK(r == 2 ? KORB_NIL : LONG2FIX(r)); }
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
    return korb_float_new(c, slots, sqrt(re * re + im * im));
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
static void korb_cpx_nd(VALUE v, intptr_t *num, intptr_t *den) {
    if (KORB_RATIONAL_P(v) && FIXNUM_P(VAL2RAT(v)->num) && FIXNUM_P(VAL2RAT(v)->den)) {
        *num = FIX2LONG(VAL2RAT(v)->num); *den = FIX2LONG(VAL2RAT(v)->den);
    } else if (FIXNUM_P(v)) { *num = FIX2LONG(v); *den = 1; }
    else { *num = 0; *den = 1; }
}
static intptr_t korb_igcd(intptr_t a, intptr_t b) { a = a < 0 ? -a : a; b = b < 0 ? -b : b; while (b) { intptr_t t = a % b; a = b; b = t; } return a ? a : 1; }
static RESULT korb_m_cpx_denominator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* lcm of component denominators */
    (void)c;(void)slots;(void)a;
    intptr_t rn, rd, in, id; korb_cpx_nd(SELF_CPX->re, &rn, &rd); korb_cpx_nd(SELF_CPX->im, &in, &id);
    return RESULT_OK(LONG2FIX(rd / korb_igcd(rd, id) * id));
}
static RESULT korb_m_cpx_numerator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* Complex(re*d/rd, im*d/id) where d=lcm */
    (void)a;
    intptr_t rn, rd, in, id; korb_cpx_nd(SELF_CPX->re, &rn, &rd); korb_cpx_nd(SELF_CPX->im, &in, &id);
    const intptr_t d = rd / korb_igcd(rd, id) * id;
    slots[0] = LONG2FIX(rn * (d / rd)); slots[1] = LONG2FIX(in * (d / id));
    return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
}
static RESULT korb_m_cpx_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* only when imaginary part is 0 */
    double im;
    if (UNLIKELY(!(korb_num_to_d(SELF_CPX->im, &im) && im == 0.0)))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "can't convert %s into Rational", korb_type_name(VALUE_REF_GET(self)));
    const uint32_t argc = VALUE_SLICE_LEN(a);
    slots[0] = SELF_CPX->re;                                 /* recv below the staged args */
    for (uint32_t j = 0; j < argc; j++) slots[1 + j] = VALUE_SLICE_GET(a, j);
    return korb_send(c, slots + 1 + argc, korb_intern(c->vm, "rationalize", 11), 0, argc);
}
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
    if (FIXNUM_P(v) || KORB_FLOAT_P(v) || KORB_RATIONAL_P(v)) { *re = v; *im = LONG2FIX(0); return true; }
    return false;
}
/* Complex arithmetic (op 0+ 1- 2*); returns a Complex. Components combined via
 * korb_num_binop (Int/Float/Rational-aware). Division (op 3) is unsupported. */
RESULT korb_cpx_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op) {
    VALUE lre, lim, rre, rim;
    if (UNLIKELY(!korb_cpx_parts(l, &lre, &lim) || !korb_cpx_parts(r, &rre, &rim)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Complex", korb_type_name(KORB_COMPLEX_P(l) ? r : l));
    if (UNLIKELY(op == 3)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Complex#/ is not implemented");
    slots[0] = lre; slots[1] = lim; slots[2] = rre; slots[3] = rim;   /* root inputs */
    VALUE res_re, res_im;
    if (op == 0 || op == 1) {
        RESULT a = korb_num_binop(c, slots + 4, slots[0], slots[2], op); if (UNLIKELY(a.state != KORB_NORMAL)) return a; slots[4] = a.value;
        RESULT b = korb_num_binop(c, slots + 5, slots[1], slots[3], op); if (UNLIKELY(b.state != KORB_NORMAL)) return b; slots[5] = b.value;
        res_re = slots[4]; res_im = slots[5];
    } else {   /* mul: (lre*rre - lim*rim) + (lre*rim + lim*rre)i */
        RESULT m1 = korb_num_binop(c, slots + 4, slots[0], slots[2], 2); if (UNLIKELY(m1.state != KORB_NORMAL)) return m1; slots[4] = m1.value;
        RESULT m2 = korb_num_binop(c, slots + 5, slots[1], slots[3], 2); if (UNLIKELY(m2.state != KORB_NORMAL)) return m2; slots[5] = m2.value;
        RESULT re = korb_num_binop(c, slots + 6, slots[4], slots[5], 1); if (UNLIKELY(re.state != KORB_NORMAL)) return re; slots[6] = re.value;
        RESULT m3 = korb_num_binop(c, slots + 7, slots[0], slots[3], 2); if (UNLIKELY(m3.state != KORB_NORMAL)) return m3; slots[7] = m3.value;
        RESULT m4 = korb_num_binop(c, slots + 8, slots[1], slots[2], 2); if (UNLIKELY(m4.state != KORB_NORMAL)) return m4; slots[8] = m4.value;
        RESULT im = korb_num_binop(c, slots + 9, slots[7], slots[8], 0); if (UNLIKELY(im.state != KORB_NORMAL)) return im; slots[9] = im.value;
        res_re = slots[6]; res_im = slots[9];
    }
    slots[10] = res_re; slots[11] = res_im;
    return korb_cpx_new(c, slots + 12, slots[10], slots[11]);
}
static RESULT korb_m_cpx_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_cpx_sub(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 1); }
static RESULT korb_m_cpx_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_cpx_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 2); }

bool
korb_num_to_d(VALUE v, double *out)
{
    if (FIXNUM_P(v))     { *out = (double)FIX2LONG(v); return true; }
    if (KORB_FLOAT_P(v)) { *out = korb_float_val(v);     return true; }
    if (KORB_RATIONAL_P(v)) { double n, d; korb_num_to_d(VAL2RAT(v)->num, &n); korb_num_to_d(VAL2RAT(v)->den, &d); *out = n / d; return true; }
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) { *out = korb_big_to_d(v); return true; }
#endif
    return false;
}

/* Index coercion: Integer as-is, Float truncated via to_int (CRuby Array#[] etc). */
static inline bool
korb_to_index(VALUE v, intptr_t *out)
{
    if (FIXNUM_P(v))     { *out = FIX2LONG(v);          return true; }
    if (KORB_FLOAT_P(v)) { *out = (intptr_t)korb_float_val(v); return true; }
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

/* numeric arithmetic with at least one Float operand.  op: 0+ 1- 2* 3/ 4% */
RESULT
korb_num_arith(CTX *c, VALUE *slots, VALUE l, VALUE rhs, int op, uint32_t line)
{
    static const char *const opn[] = { "+", "-", "*", "/", "%" };
    double a = 0.0, b = 0.0;
    if (UNLIKELY(!korb_num_to_d(l, &b)))     /* l not numeric → method missing on l */
        return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '%s' for %s", opn[op], korb_a_type_name(l));
    if (UNLIKELY(!korb_num_to_d(rhs, &b)))   /* rhs not numeric → coercion error */
        return korb_raise(c, slots, KORB_E_TYPE, line, "%s can't be coerced into Float", korb_type_name(rhs));
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

/* a + b — alloc first, then copy through refs (fixup-safe; v2_design §4.3). */
static RESULT
korb_str_plus_ref(CTX *c, VALUE *slots, VALUE_REF a, VALUE_REF b)
{
    uint32_t alen = VAL2STR(VALUE_REF_GET(a))->len;
    uint32_t blen = VAL2STR(VALUE_REF_GET(b))->len;
    KorbString *s = korb_str_alloc(c, slots, alen + blen);
    const KorbString *as = VAL2STR(VALUE_REF_GET(a));   /* re-read: fixed up */
    const KorbString *bs = VAL2STR(VALUE_REF_GET(b));
    memcpy(s->buf->data, as->buf->data, alen);
    memcpy(s->buf->data + alen, bs->buf->data, blen);
    return RESULT_OK((VALUE)s);
}

static RESULT
korb_str_repeat_ref(CTX *c, VALUE *slots, VALUE_REF src, intptr_t cnt, uint32_t line)
{
    if (cnt < 0)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
    uint32_t len = VAL2STR(VALUE_REF_GET(src))->len;
    size_t total = (size_t)len * (size_t)cnt;
    if (total > (size_t)1 << 31)
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "argument too big");
    KorbString *s = korb_str_alloc(c, slots, (uint32_t)total);
    const KorbString *ss = VAL2STR(VALUE_REF_GET(src));
    for (intptr_t i = 0; i < cnt; i++) {
        memcpy(s->buf->data + (size_t)i * len, ss->buf->data, len);
    }
    return RESULT_OK((VALUE)s);
}

static uint32_t korb_fmt_int(intptr_t n, int base, char *buf);   /* defined below */

/* String interpolation step: acc (a String) + to_s(part).  String parts take
 * the direct concat path; other values render through korb_fprint_to_s (which
 * does not allocate, so `part` stays put) into a transient buffer first. */
RESULT
korb_str_interp(CTX *c, VALUE *slots, VALUE_REF aref, VALUE part)
{
    VALUE_REF pref = SLOTS_PUSH(slots, part);            /* root part across GC */
    VALUE p = VALUE_REF_GET(pref);
    if (KORB_STRING_P(p))
        return korb_str_plus_ref(c, slots, aref, pref);
    if (KORB_OBJECT_P(p)) {                              /* user object → its to_s (user or default) */
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
        if (FIXNUM_P(p))          { slen = korb_fmt_int((intptr_t)FIX2LONG(p), 10, sb); src = sb; }
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
    korb_fprint_to_s(c, ms, p);                          /* no GC inside */
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
    ARO_STORE_BULK(c, nit, nit->data, oit->data, (size_t)a->len);   /* new tail is zero-init = nil */
    ARO_STORE(c, a, (VALUE *)(uintptr_t)&a->items, (VALUE)(uintptr_t)nit);
    a->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(aref));
}

RESULT
korb_ary_push_val(CTX *c, VALUE *slots, VALUE_REF aref, VALUE elem)
{
    VALUE_REF eref = SLOTS_PUSH(slots, elem);       /* root elem across the grow GC */
    CHECK(korb_ary_ensure(c, slots, aref, 1));
    KorbArray *a = VAL2ARY(VALUE_REF_GET(aref));
    KorbArrayItems *it = a->items;
    ARO_STORE(c, it, &it->data[a->len], VALUE_REF_GET(eref));
    a->len++;
    return RESULT_OK(VALUE_REF_GET(aref));
}

/* In-place store ary[i] = val (write-barriered).  Caller guarantees i is in
 * range.  Exported for node_aset's fast path (node_eval.c lacks the GC macros). */
void
korb_ary_store_at(CTX *c, VALUE ary, uint32_t i, VALUE val)
{
    KorbArray *const a = VAL2ARY(ary);
    ARO_STORE(c, a->items, &a->items->data[i], val);
}

static bool korb_range_int_bounds(const KorbRange *r, intptr_t *lo, intptr_t *hi);
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
            CHECK(korb_ary_push_val(c, slots + 1, aref, VAL2ARY(VALUE_REF_GET(sref))->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(aref));
    }
    if (KORB_RANGE_P(val)) {
        const KorbRange *r = VAL2RANGE(val);
        intptr_t lo, hi;
        if (korb_range_int_bounds(r, &lo, &hi)) {
            for (intptr_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots, aref, LONG2FIX(i)));
            return RESULT_OK(VALUE_REF_GET(aref));
        }
        slots[0] = val;                                 /* non-int (e.g. String) range → expand via to_a */
        RESULT ta = korb_m_range_to_a(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0));
        if (UNLIKELY(ta.state != KORB_NORMAL)) return ta;
        slots[0] = ta.value;
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++)
            CHECK(korb_ary_push_val(c, slots + 1, aref, VAL2ARY(slots[0])->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(aref));
    }
    if (val == KORB_NIL) return RESULT_OK(VALUE_REF_GET(aref));
    /* `*obj` for an object with a to_a (e.g. Struct) spreads its elements
     * (Ruby `[*x]` / `a, b = *x` semantics); otherwise `*x` is just `[x]`. */
    if (KORB_OBJECT_P(val)) {
        const uint32_t to_a_mid = korb_intern(c->vm, "to_a", 4);
        if (korb_responds_to(c, val, to_a_mid)) {
            VALUE_REF vr = SLOTS_PUSH(slots, val);       /* root recv across the send */
            RESULT ta = korb_send(c, slots, to_a_mid, 0, 0);
            if (UNLIKELY(ta.state != KORB_NORMAL)) return ta;
            if (KORB_ARRAY_P(ta.value)) {
                VALUE_REF_SET(vr, ta.value);             /* reuse the rooted slot for the array */
                uint32_t n = VAL2ARY(VALUE_REF_GET(vr))->len;
                for (uint32_t i = 0; i < n; i++)
                    CHECK(korb_ary_push_val(c, slots, aref, VAL2ARY(VALUE_REF_GET(vr))->items->data[i]));
                return RESULT_OK(VALUE_REF_GET(aref));
            }
            return korb_ary_push_val(c, slots, aref, VALUE_REF_GET(vr));   /* to_a not an Array → [x] */
        }
    }
    return korb_ary_push_val(c, slots, aref, val);
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
        VALUE elem = VAL2ARY(VALUE_REF_GET(lref))->items->data[i];   /* push roots elem first */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    rn = VAL2ARY(VALUE_REF_GET(rref))->len;                          /* re-read (rooted) */
    for (uint32_t i = 0; i < rn; i++) {
        VALUE elem = VAL2ARY(VALUE_REF_GET(rref))->items->data[i];
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
    return FIXNUM_P(v) || SYMBOL_P(v) || KORB_STRING_P(v) ||
           v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE;
}
static uint64_t korb_value_hash(VALUE v) {
    if (FIXNUM_P(v)) { uint64_t x = (uint64_t)v; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 29; return x; }
    if (SYMBOL_P(v)) { uint64_t x = (uint64_t)SYM2ID(v) + 1; x *= 0x9e3779b97f4a7c15ULL; return x ^ (x >> 32); }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        uint64_t h = 1469598103934665603ULL;            /* FNV-1a */
        for (uint32_t i = 0; i < s->len; i++) { h ^= (unsigned char)s->buf->data[i]; h *= 1099511628211ULL; }
        return h;
    }
    if (v == KORB_NIL)  return 0x9e3779b97f4a7c15ULL;
    if (v == KORB_TRUE) return 0x100000001ULL;
    return 0x200000002ULL;                              /* KORB_FALSE (only remaining indexable) */
}

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
    return korb_value_eq(a, b);
}

/* eql? (numeric-type-strict equality) — defined below; Hash keys use it so 1
 * and 1.0 are distinct keys (CRuby Hash uses eql?, not ==). */
static bool korb_value_eql(VALUE a, VALUE b);

/* index of key in the pair array, or -1 */
static int32_t
korb_hash_find(const KorbHash *h, VALUE key)
{
    const VALUE *const d = h->items->data;
    if (LIKELY(h->idx_mask && korb_key_indexable(key))) {   /* O(1) open-addressing probe (CMP_BY_ID never indexes) */
        const uint32_t *const tab = (const uint32_t *)h->index->data;
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

/* Insert pair `pi` into an existing index (no alloc; caller ensures capacity). */
static void korb_hash_index_put(KorbHash *h, uint32_t pi) {
    uint32_t *const tab = (uint32_t *)h->index->data;
    const uint32_t mask = h->idx_mask;
    uint32_t slot = (uint32_t)korb_value_hash(h->items->data[2 * pi]) & mask;
    while (tab[slot]) slot = (slot + 1) & mask;
    tab[slot] = pi + 1;
}
/* (Re)build the O(1) index for an all-indexable hash (load factor ~0.5). */
static RESULT korb_hash_index_build(CTX *c, VALUE *slots, VALUE_REF href) {
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    uint32_t cap = 16; while ((size_t)cap < (size_t)h->len * 2) cap <<= 1;
    KorbStrBuf *idx = korb_alloc(c, slots, sizeof(KorbStrBuf) + (size_t)cap * sizeof(uint32_t), KORB_OBJ_STR_BUF);
    memset(idx->data, 0, (size_t)cap * sizeof(uint32_t));
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
    ARO_STORE_BULK(c, nit, nit->data, oit->data, (size_t)h->len * 2);
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->items, (VALUE)(uintptr_t)nit);
    h->capa = ncapa;
    return RESULT_OK(VALUE_REF_GET(href));
}

/* merge `**src` into href: copy each src pair (Hash) into href.  nil → no-op. */
RESULT
korb_hash_merge_val(CTX *c, VALUE *slots, VALUE_REF href, VALUE src)
{
    if (src == KORB_NIL) return RESULT_OK(VALUE_REF_GET(href));
    if (UNLIKELY(!KORB_HASH_P(src))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(src));
    VALUE_REF sref = SLOTS_PUSH(slots, src);          /* root src across grow GCs */
    uint32_t n = VAL2HASH(VALUE_REF_GET(sref))->len;
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = VAL2HASH(VALUE_REF_GET(sref))->items->data[2*i];      /* key */
        slots[2] = VAL2HASH(VALUE_REF_GET(sref))->items->data[2*i + 1];  /* val */
        CHECK(korb_hash_set(c, slots + 3, href, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(href));
}

RESULT
korb_hash_set(CTX *c, VALUE *slots, VALUE_REF href, VALUE_REF kref, VALUE val)
{
    VALUE_REF vref = SLOTS_PUSH(slots, val);          /* root val across grow GC */
    KorbHash *h = VAL2HASH(VALUE_REF_GET(href));
    int32_t idx = korb_hash_find(h, VALUE_REF_GET(kref));
    if (idx >= 0) {
        KorbArrayItems *it = h->items;
        ARO_STORE(c, it, &it->data[2 * idx + 1], VALUE_REF_GET(vref));
        return RESULT_OK(VALUE_REF_GET(href));
    }
    CHECK(korb_hash_ensure(c, slots, href, 1));
    h = VAL2HASH(VALUE_REF_GET(href));                /* re-read after grow */
    KorbArrayItems *it = h->items;
    uint32_t i = h->len;
    ARO_STORE(c, it, &it->data[2 * i],     VALUE_REF_GET(kref));
    ARO_STORE(c, it, &it->data[2 * i + 1], VALUE_REF_GET(vref));
    h->len++;
    /* O(1) index maintenance.  An ambiguous key (Float/heap) permanently drops
     * the index; otherwise build at the threshold and incrementally fill,
     * rebuilding bigger when the load factor exceeds ~0.7. */
    if (!(h->head.flags & (KORB_FL_HASH_NOINDEX | KORB_FL_CMP_BY_ID))) {
        if (UNLIKELY(!korb_key_indexable(VALUE_REF_GET(kref)))) {
            h->head.flags |= KORB_FL_HASH_NOINDEX;
            h->index = NULL; h->idx_mask = 0;
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

VALUE
korb_ivar_get(CTX *c, VALUE self, VALUE name_sym)
{
    const KorbObject *o = VAL2OBJ(self);
    int32_t idx = korb_shape_index(c->vm, o->shape_id, SYM2ID(name_sym));
    if (idx < 0) return KORB_NIL;
    return o->ivars->data[idx];
}

void
korb_ivar_store_at(CTX *c, KorbObject *o, uint32_t slot, VALUE val)
{
    ARO_STORE(c, o->ivars, &o->ivars->data[slot], val);   /* values-only array */
}

RESULT
korb_ivar_set(CTX *c, VALUE *slots, VALUE_REF selfref, VALUE name_sym, VALUE val)
{
    const uint32_t sym = SYM2ID(name_sym);
    KorbObject *o = VAL2OBJ(VALUE_REF_GET(selfref));
    int32_t idx = korb_shape_index(c->vm, o->shape_id, sym);
    if (idx >= 0) {                                   /* existing ivar: in-place (no GC) */
        ARO_STORE(c, o->ivars, &o->ivars->data[idx], val);
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
        if (o->ivars) ARO_STORE_BULK(c, nit, nit->data, o->ivars->data, (size_t)(ncount - 1));
        ARO_STORE(c, o, (VALUE *)(uintptr_t)&o->ivars, (VALUE)(uintptr_t)nit);
        o->ivar_capa = ncapa;
    }
    o->shape_id = nshape;                             /* commit transition */
    ARO_STORE(c, o->ivars, &o->ivars->data[ncount - 1], VALUE_REF_GET(vref));
    return RESULT_OK(VALUE_REF_GET(vref));
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
    k->exc_etype = -1;                                 /* not an exception class by default */
    if (superclass != KORB_NIL) ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->superclass, VALUE_REF_GET(sref));
    return RESULT_OK((VALUE)k);
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
static VALUE
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
static VALUE
korb_cvar_owner(VALUE cref, VALUE sym, int32_t *idx_out)
{
    for (VALUE k = cref; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass) {
        const VALUE cv = VAL2CLASS(k)->cvars;
        if (cv != KORB_NIL) {
            const int32_t idx = korb_hash_find(VAL2HASH(cv), sym);
            if (idx >= 0) { *idx_out = idx; return k; }
        }
    }
    return KORB_NIL;
}

/* soft: `@@x ||= v` / `&&=` read an undefined cvar as nil instead of raising. */
RESULT
korb_cvar_get(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, uint32_t soft)
{
    const VALUE cref = korb_cvar_cref(self, entry_cell);
    if (!KORB_CLASS_P(cref)) {
        if (soft) return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "class variable access from toplevel");
    }
    const VALUE sym = ID2SYM(sym_id);
    int32_t idx;
    const VALUE owner = korb_cvar_owner(cref, sym, &idx);
    if (owner == KORB_NIL) {
        if (soft) return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_NAME, 0, "uninitialized class variable %s in %s",
                          korb_sym_name(c->vm, sym_id), korb_type_name(cref));
    }
    return RESULT_OK(VAL2HASH(VAL2CLASS(owner)->cvars)->items->data[2 * idx + 1]);
}

RESULT
korb_cvar_set(CTX *c, VALUE *slots, VALUE self, VALUE entry_cell, uint32_t sym_id, VALUE val)
{
    const VALUE cref = korb_cvar_cref(self, entry_cell);
    if (!KORB_CLASS_P(cref))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "class variable assignment from toplevel");
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
static RESULT korb_m_struct_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);                            /* members (rooted, below the alloc scratch) */
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));          /* result */
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        VALUE iv = korb_member_ivar_sym(c->vm, VAL2ARY(slots[0])->items->data[i]);
        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_ivar_get(c, VALUE_REF_GET(self), iv)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_struct_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_hash_new(c, slots + 1, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        slots[2] = VAL2ARY(slots[0])->items->data[i];              /* member sym (key, rooted) */
        VALUE iv = korb_member_ivar_sym(c->vm, slots[2]);
        slots[3] = korb_ivar_get(c, VALUE_REF_GET(self), iv);      /* value (rooted) */
        CHECK(korb_hash_set(c, slots + 4, dst, VALUE_REF_AT(&slots[2]), slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_struct_members(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = STRUCT_MEMBERS(self);
    const uint32_t n = VAL2ARY(slots[0])->len;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(slots[0])->items->data[i]));
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
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(slots[0])->items->data[i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_struct_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE mems = STRUCT_MEMBERS(self);
    const KorbArray *mem = VAL2ARY(mems);
    VALUE k = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(k)) {
        intptr_t i = FIX2LONG(k); if (i < 0) i += mem->len;
        if (UNLIKELY(i < 0 || (uint32_t)i >= mem->len)) return korb_raise(c, slots, KORB_E_INDEX, 0, "offset %ld too large for struct(size:%u)", (long)FIX2LONG(k), mem->len);
        return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, mem->items->data[i])));
    }
    uint32_t sym = SYMBOL_P(k) ? SYM2ID(k) : (KORB_STRING_P(k) ? korb_intern(c->vm, VAL2STR(k)->buf->data, VAL2STR(k)->len) : 0);
    for (uint32_t i = 0; i < mem->len; i++)
        if (SYM2ID(mem->items->data[i]) == sym) return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, mem->items->data[i])));
    return korb_raise(c, slots, KORB_E_NAME, 0, "no member '%s' in struct", SYMBOL_P(k) ? korb_sym_name(c->vm, sym) : "?");
}
static RESULT korb_m_struct_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbArray *mem = VAL2ARY(STRUCT_MEMBERS(self));
    VALUE k = VALUE_SLICE_GET(a, 0), v = VALUE_SLICE_GET(a, 1);
    intptr_t idx = -1;
    if (FIXNUM_P(k)) { intptr_t i = FIX2LONG(k); if (i < 0) i += mem->len; idx = i; }
    else { uint32_t sym = SYMBOL_P(k) ? SYM2ID(k) : (KORB_STRING_P(k) ? korb_intern(c->vm, VAL2STR(k)->buf->data, VAL2STR(k)->len) : 0);
           for (uint32_t i = 0; i < mem->len; i++) if (SYM2ID(mem->items->data[i]) == sym) { idx = i; break; } }
    if (UNLIKELY(idx < 0 || (uint32_t)idx >= mem->len)) return korb_raise(c, slots, KORB_E_NAME, 0, "no such struct member");
    slots[0] = v;
    CHECK(korb_ivar_set(c, slots + 1, self, korb_member_ivar_sym(c->vm, mem->items->data[idx]), slots[0]));
    return RESULT_OK(slots[0]);
}
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);                /* fwd (enumerator.c) */
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
        VALUE iv = korb_member_ivar_sym(c->vm, VAL2ARY(slots[1])->items->data[i]);
        VALUE val = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &val, 1, cself);
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
        VALUE iv = korb_member_ivar_sym(c->vm, VAL2ARY(slots[0])->items->data[i]);
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
        VALUE iv = korb_member_ivar_sym(c->vm, mem->items->data[i]);
        if (!korb_value_eq(korb_ivar_get(c, VALUE_REF_GET(self), iv), korb_ivar_get(c, o, iv))) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_struct_define(CTX *c, VALUE *slots, VALUE_SLICE a, NODE *block, VALUE *def_env) {
    struct korb_vm *const vm = c->vm;
    slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));   /* anon class, super Object */
    VALUE_REF cls = VALUE_REF_AT(&slots[0]);
    bool kwinit = false;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF mem = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_HASH_P(sym)) {                               /* trailing keyword_init: true */
            int32_t ki = korb_hash_find(VAL2HASH(sym), ID2SYM(korb_intern(vm, "keyword_init", 12)));
            if (ki >= 0 && KORB_TRUTHY(VAL2HASH(sym)->items->data[2*ki+1])) kwinit = true;
            continue;
        }
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, VAL2STR(sym)->buf->data, VAL2STR(sym)->len));
        if (!SYMBOL_P(sym)) continue;
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256];
        snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, nm, strlen(nm)), ivar, 0);   /* reader */
        snprintf(buf, sizeof buf, "%s=", nm); korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, buf, strlen(buf)), ivar, 1);  /* writer */
        CHECK(korb_ary_push_val(c, slots + 2, mem, sym));
    }
    ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(cls)), (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(cls))->members, VALUE_REF_GET(mem));
    /* common Struct instance methods (read members + @ivars generically).
     * korb_class_def_cfn interns the name → may GC → re-read the class from the
     * rooted `cls` slot each call (never hold it in a bare C-local across them). */
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_a", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_ary", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "values", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "deconstruct", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_h", korb_m_struct_to_h, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "members", korb_m_struct_members, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "[]", korb_m_struct_aref, 1);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "[]=", korb_m_struct_aset, 2);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "==", korb_m_struct_eq, 1);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "eql?", korb_m_struct_eq, 1);
    korb_class_def_cfn_blk(c, VALUE_REF_GET(cls), "each", korb_m_struct_each, 0);
    korb_class_def_cfn_blk(c, VALUE_REF_GET(cls), "map", korb_m_struct_map, 0);
    korb_class_def_cfn_blk(c, VALUE_REF_GET(cls), "collect", korb_m_struct_map, 0);
    VAL2CLASS(VALUE_REF_GET(cls))->struct_kwinit = kwinit ? 1 : 0;
    /* class-level `Rec.members`: install on the class's singleton (name already
     * interned above → no GC in def_cfn). */
    slots[2] = VALUE_REF_GET(cls);                            /* root class across singleton alloc */
    slots[3] = UNWRAP(korb_obj_singleton(c, slots + 4, slots[2]));
    korb_class_def_cfn(c, slots[3], "members", korb_m_struct_class_members, 0);
    if (block != NULL) {                                      /* Struct.new(...) do ... end → class-body methods */
        slots[2] = VALUE_REF_GET(cls);                       /* root the class as the block's self/cref */
        RESULT br = korb_block_yield(c, slots + 3, block, def_env, NULL, 0, &slots[2]);
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
        VALUE key = h->items->data[2 * i];
        bool found = false;
        for (uint32_t j = 0; j < mem->len; j++) if (mem->items->data[j] == key) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

/* Data#with(**changes) → a fresh instance with the named members replaced. */
static RESULT korb_m_data_with(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VAL2OBJ(VALUE_REF_GET(self))->klass;          /* the data class (rooted) */
    slots[1] = (VALUE_SLICE_LEN(a) >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, 0))) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    slots[2] = UNWRAP(korb_obj_new(c, slots + 2, slots[0]));  /* new instance (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *mem = VAL2ARY(VAL2CLASS(slots[0])->members);   /* re-read (ivar_set may GC) */
        if (i >= mem->len) break;
        VALUE msym = mem->items->data[i];
        VALUE iv = korb_member_ivar_sym(c->vm, msym);
        VALUE val;
        if (slots[1] != KORB_NIL) {
            int32_t hi = korb_hash_find(VAL2HASH(slots[1]), msym);
            val = hi >= 0 ? VAL2HASH(slots[1])->items->data[2 * hi + 1] : korb_ivar_get(c, VALUE_REF_GET(self), iv);
        } else {
            val = korb_ivar_get(c, VALUE_REF_GET(self), iv);
        }
        slots[3] = val;                                       /* root across ivar_set */
        CHECK(korb_ivar_set(c, slots + 4, VALUE_REF_AT(&slots[2]), iv, slots[3]));
    }
    return RESULT_OK(slots[2]);
}

/* Data#inspect → "#<data Name member=val, ...>" (anonymous → no Name). */
static RESULT korb_m_data_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE klass = VAL2OBJ(VALUE_REF_GET(self))->klass;
    const KorbClass *const k = VAL2CLASS(klass);
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    fputs("#<data", ms);
    if (k->name_sym) { fputc(' ', ms); fputs(korb_sym_name(c->vm, k->name_sym), ms); }
    const KorbArray *const mem = VAL2ARY(k->members);
    for (uint32_t i = 0; i < mem->len; i++) {                 /* no GC in this loop (fprint writes to FILE) */
        const VALUE msym = mem->items->data[i];
        const VALUE val = korb_ivar_get(c, VALUE_REF_GET(self), korb_member_ivar_sym(c->vm, msym));
        fputs(i == 0 ? " " : ", ", ms);
        fputs(korb_sym_name(c->vm, SYM2ID(msym)), ms);
        fputc('=', ms);
        korb_fprint_inspect(c, ms, val);
    }
    fputc('>', ms);
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}

/* Data.define(*members [, &block]) → an anonymous immutable value class. */
static RESULT korb_data_define(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)self; (void)cself;
    struct korb_vm *const vm = c->vm;
    slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));   /* anon class, super Object */
    VALUE_REF cls = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF mem = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, VAL2STR(sym)->buf->data, VAL2STR(sym)->len));
        if (!SYMBOL_P(sym)) continue;
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256];
        snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, nm, strlen(nm)), ivar, 0);   /* reader only (immutable) */
        CHECK(korb_ary_push_val(c, slots + 2, mem, sym));
    }
    ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(cls)), (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(cls))->members, VALUE_REF_GET(mem));
    VAL2CLASS(VALUE_REF_GET(cls))->is_data = 1;
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_h", korb_m_struct_to_h, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "deconstruct_keys", korb_m_struct_to_h, -1);   /* arg (keys|nil) ignored → full hash */
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "members", korb_m_struct_members, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_a", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "deconstruct", korb_m_struct_to_a, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "==", korb_m_struct_eq, 1);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "eql?", korb_m_struct_eq, 1);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "with", korb_m_data_with, -1);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "inspect", korb_m_data_inspect, 0);
    korb_class_def_cfn(c, VALUE_REF_GET(cls), "to_s", korb_m_data_inspect, 0);
    slots[2] = VALUE_REF_GET(cls);                            /* root across singleton alloc */
    slots[3] = UNWRAP(korb_obj_singleton(c, slots + 4, slots[2]));
    korb_class_def_cfn(c, slots[3], "members", korb_m_struct_class_members, 0);
    if (block != NULL) {                                      /* Data.define(...) do ... end → class-body methods */
        slots[2] = VALUE_REF_GET(cls);
        RESULT br = korb_block_yield(c, slots + 3, block, def_env, NULL, 0, &slots[2]);
        if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
    }
    return RESULT_OK(VALUE_REF_GET(cls));
}

/* ---- Regexp (matching via the astrogre engine in koruby_regex.so) -------- */
typedef int (*korb_re_fn_t)(const char *, size_t, const char *, size_t, int, long *, long *);
static korb_re_fn_t korb_re_load(struct korb_vm *vm) {
    if (vm->re_fn == NULL) {
        void *h = dlopen(KORUBY_SRC_DIR "/koruby_regex.so", RTLD_NOW | RTLD_LOCAL);
        vm->re_fn = h ? dlsym(h, "koruby_re_search") : NULL;
        if (vm->re_fn == NULL) vm->re_fn = (void *)(intptr_t)-1;   /* mark load failure */
    }
    return vm->re_fn == (void *)(intptr_t)-1 ? NULL : (korb_re_fn_t)vm->re_fn;
}
RESULT korb_regexp_new(CTX *c, VALUE *slots, VALUE source, uint8_t ci) {
    VALUE_REF sref = SLOTS_PUSH(slots, source);          /* root source across alloc */
    KorbRegexp *r = korb_alloc(c, slots, sizeof(KorbRegexp), KORB_OBJ_REGEXP);
    r->ci = ci;
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
    ARO_STORE(c, vi, &vi->data[index], v);
}

/* Find an already-open env for the given frame locals base (so multiple procs
 * capturing the same scope activation share one env → shared mutation). */
static KorbEnv *korb_open_env_find(struct korb_vm *vm, VALUE *loc) {
    for (uint32_t i = 0; i < vm->open_env_cnt; i++) {
        KorbEnv *e = VAL2ENV(vm->open_envs[i]);
        if (!e->closed && e->loc == loc) return e;
    }
    return NULL;
}
/* Register an open env as a GC root + close candidate. */
static void korb_open_env_register(struct korb_vm *vm, VALUE env) {
    if (vm->open_env_cnt == vm->open_env_capa) {
        vm->open_env_capa = vm->open_env_capa ? vm->open_env_capa * 2 : 16;
        vm->open_envs = realloc(vm->open_envs, sizeof(VALUE) * vm->open_env_capa);
        if (!vm->open_envs) { fprintf(stderr, "koruby_precise: out of memory (open_envs)\n"); abort(); }
    }
    vm->open_envs[vm->open_env_cnt++] = env;
}
/* A frame at `frame_base` is returning: close (slots->vals) every open env
 * whose loc lies in [frame_base, top) — i.e. belongs to this frame (inner
 * frames already closed theirs, LIFO).  `slots` is scratch above the frame. */
void korb_close_envs(CTX *c, VALUE *slots, VALUE *frame_base) {
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->open_env_cnt; ) {
        KorbEnv *e = VAL2ENV(vm->open_envs[i]);
        if (e->loc < frame_base) { i++; continue; }       /* outer frame's env: keep open */
        slots[0] = vm->open_envs[i];                       /* root env across vals alloc */
        uint32_t n = VAL2ENV(slots[0])->n;
        KorbArrayItems *vals = korb_alloc(c, slots + 1, sizeof(KorbArrayItems) + (size_t)n * sizeof(VALUE), KORB_OBJ_VALUE_ARRAY);
        KorbEnv *ee = VAL2ENV(slots[0]);                   /* re-read after alloc/GC */
        for (uint32_t j = 0; j < n; j++) ARO_STORE(c, vals, &vals->data[j], ee->loc[j]);
        ARO_STORE(c, ee, (VALUE *)(uintptr_t)&ee->vals, (VALUE)(uintptr_t)vals);
        ee->closed = 1;
        vm->open_envs[i] = vm->open_envs[vm->open_env_cnt - 1];   /* swap-remove */
        vm->open_env_cnt--;
    }
}

/* Cold tail of the frame-return close hook: kept OUT-OF-LINE so the close logic
 * doesn't bloat the always-inlined invoke fast paths.  Even when the guard is
 * false, inlining this code hurt i-cache/regalloc on call/loop-heavy programs
 * that never escape (collatz regressed ~16% before this split). */
RESULT __attribute__((noinline)) korb_close_ret(CTX *c, VALUE *scratch, VALUE *frame_base, RESULT r) {
    scratch[0] = r.value;                              /* root return value across close's alloc */
    korb_close_envs(c, scratch + 1, frame_base);
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
    if (UNLIKELY(entry->head.kind != &kind_node_entry)) return EVAL(c, entry, slots);
    uint32_t depth = entry->u.node_entry.cap_depth;
    slots[0] = self_val;                                 /* root captured self across allocs */
    if (depth == 0) {                                    /* no captured outer locals */
        KorbProc *p = korb_alloc(c, slots + 1, sizeof(KorbProc), KORB_OBJ_PROC);
        p->iseq = entry; p->is_lambda = (uint8_t)is_lambda;
        p->env = (VALUE)((uintptr_t)def_env | 1u);       /* sentinel (unused by body) */
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
        const VALUE pv = bases[k-1][-1];
        if (pv & 1u) { bases[k] = (VALUE *)(uintptr_t)(pv & ~(uintptr_t)1u); nlive++; }
        else { outer_env = pv; break; }                 /* reached an already-materialized KorbEnv */
    }
    /* materialize the live levels outermost -> innermost, grafting onto
     * outer_env; slots[1] holds the current outer env (rooted across each alloc). */
    slots[1] = outer_env;
    for (int k = (int)nlive - 1; k >= 0; k--) {
        KorbEnv *existing = korb_open_env_find(c->vm, bases[k]);
        if (existing) { slots[1] = (VALUE)(uintptr_t)existing; continue; }   /* share */
        KorbEnv *e = korb_alloc(c, slots + 2, sizeof(KorbEnv), KORB_OBJ_ENV);
        e->loc = bases[k];                               /* open: live slots */
        e->n = ns[k];
        e->closed = 0;
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->vals, 0);
        ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->prev, slots[1]);   /* outer (forwarded) */
        slots[1] = (VALUE)(uintptr_t)e;
        korb_open_env_register(c->vm, slots[1]);
    }
    KorbProc *p = korb_alloc(c, slots + 2, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = entry; p->is_lambda = (uint8_t)is_lambda;
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->env, slots[1]);   /* innermost env (even = KorbEnv) */
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, slots[0]);
    return RESULT_OK((VALUE)p);
}
/* `re =~ str` core: returns the match's CHARACTER index (Integer) or nil. */
static RESULT korb_re_match_index(CTX *c, VALUE *slots, VALUE re, VALUE str) {
    if (!KORB_REGEXP_P(re) || !KORB_STRING_P(str)) return RESULT_OK(KORB_NIL);
    korb_re_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    const KorbString *pat = VAL2STR(VAL2RE(re)->source), *s = VAL2STR(str);
    long ms = 0, me = 0;
    int rc = fn(pat->buf->data, pat->len, s->buf->data, s->len, VAL2RE(re)->ci, &ms, &me);
    if (rc != 1) return RESULT_OK(KORB_NIL);
    long cidx = 0;                                        /* byte offset → char index (UTF-8) */
    for (long i = 0; i < ms; i++) if (((unsigned char)s->buf->data[i] & 0xC0) != 0x80) cidx++;
    return RESULT_OK(LONG2FIX(cidx));
}
static RESULT korb_m_str_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* String#=~ */
    return korb_re_match_index(c, slots, VALUE_SLICE_GET(a, 0), VALUE_REF_GET(self));
}
static RESULT korb_m_re_match_op(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {    /* Regexp#=~ */
    return korb_re_match_index(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0));
}
static RESULT korb_m_re_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {     /* Regexp#match? / === */
    RESULT r = korb_re_match_index(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_NIL ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_re_source(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {      /* Regexp#source */
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2RE(VALUE_REF_GET(self))->source);
}
/* String#match?(pat) — pat is a Regexp or a String (literal pattern); true if it
 * matches anywhere.  (Whole-match only; group captures need astrogre — see
 * project_regexp_astrorge.) */
/* type name as Ruby renders it in "wrong argument type ..." (singletons lowercase). */
static const char *korb_re_arg_type(VALUE v) {
    if (v == KORB_NIL) return "nil";
    if (v == KORB_TRUE) return "true";
    if (v == KORB_FALSE) return "false";
    return korb_type_name(v);
}
static RESULT korb_re_match_region(CTX *c, VALUE *slots, VALUE re, VALUE str, long startc, long *ms, long *me);   /* fwd */
static RESULT korb_m_str_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t argc = VALUE_SLICE_LEN(a);
    const VALUE selfv = VALUE_REF_GET(self);
    if (SYMBOL_P(selfv)) { const char *const nm = korb_sym_name(c->vm, SYM2ID(selfv)); slots[0] = UNWRAP(korb_str_new(c, slots + 1, nm, (uint32_t)strlen(nm))); }
    else slots[0] = selfv;                                /* subject string in slots[0] */
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (KORB_REGEXP_P(pv)) slots[1] = pv;
    else if (KORB_STRING_P(pv)) { slots[1] = pv; slots[1] = UNWRAP(korb_regexp_new(c, slots + 2, slots[1], 0)); }
    else return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
    long startc = 0;
    if (argc >= 2) { intptr_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p) && p > 0) startc = (long)p; }
    long ms = 0, me = 0;
    const RESULT mr = korb_re_match_region(c, slots + 2, slots[1], slots[0], startc, &ms, &me);
    if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
    return RESULT_OK(mr.value == KORB_TRUE ? KORB_TRUE : KORB_FALSE);
}
/* allocate a MatchData carrying group 0 (the matched substring). */
static RESULT korb_matchdata_new(CTX *c, VALUE *slots, VALUE matched) {
    VALUE_REF mref = SLOTS_PUSH(slots, matched);         /* root across alloc */
    KorbMatchData *md = korb_alloc(c, slots, sizeof(KorbMatchData), KORB_OBJ_MATCHDATA);
    ARO_STORE(c, md, (VALUE *)(uintptr_t)&md->matched, VALUE_REF_GET(mref));
    return RESULT_OK((VALUE)md);
}
/* run the engine on `str` starting at character index `startc`; on a match,
 * fill the ms/me byte offsets and return KORB_TRUE, else KORB_FALSE.
 * Whole-match only (no group captures — those need astrogre). */
static RESULT korb_re_match_region(CTX *c, VALUE *slots, VALUE re, VALUE str, long startc, long *ms, long *me) {
    if (!KORB_REGEXP_P(re) || !KORB_STRING_P(str)) return RESULT_OK(KORB_FALSE);
    const korb_re_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    const KorbString *const pat = VAL2STR(VAL2RE(re)->source), *const s = VAL2STR(str);
    long boff = 0, cc = 0;                                /* char index startc → byte offset */
    while (cc < startc && boff < (long)s->len) {
        boff++;
        while (boff < (long)s->len && ((unsigned char)s->buf->data[boff] & 0xC0) == 0x80) boff++;
        cc++;
    }
    if (cc < startc) return RESULT_OK(KORB_FALSE);        /* start position past end → no match */
    long lms = 0, lme = 0;
    const int rc = fn(pat->buf->data, pat->len, s->buf->data + boff, (size_t)(s->len - boff), VAL2RE(re)->ci, &lms, &lme);
    if (rc != 1) return RESULT_OK(KORB_FALSE);
    *ms = boff + lms; *me = boff + lme;
    return RESULT_OK(KORB_TRUE);
}
/* String#match / Symbol#match (pat[, pos]) → MatchData or nil; with a block,
 * yields the MatchData on a match (returns its value) else nil. */
static RESULT korb_m_str_match(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const uint32_t argc = VALUE_SLICE_LEN(a);
    const VALUE selfv = VALUE_REF_GET(self);
    if (SYMBOL_P(selfv)) { const char *const nm = korb_sym_name(c->vm, SYM2ID(selfv)); slots[0] = UNWRAP(korb_str_new(c, slots + 1, nm, (uint32_t)strlen(nm))); }
    else slots[0] = selfv;                                /* subject string in slots[0] */
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (KORB_REGEXP_P(pv)) slots[1] = pv;
    else if (KORB_STRING_P(pv)) { slots[1] = pv; slots[1] = UNWRAP(korb_regexp_new(c, slots + 2, slots[1], 0)); }
    else return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
    long startc = 0;
    if (argc >= 2) { intptr_t p = 0; if (korb_to_index(VALUE_SLICE_GET(a, 1), &p) && p > 0) startc = (long)p; }
    long ms = 0, me = 0;
    const RESULT mr = korb_re_match_region(c, slots + 2, slots[1], slots[0], startc, &ms, &me);
    if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
    if (mr.value != KORB_TRUE) return RESULT_OK(KORB_NIL);
    const uint32_t mlen = (uint32_t)(me - ms);
    KorbString *const r = korb_str_alloc(c, slots + 3, mlen);        /* may move slots[0] */
    memcpy(r->buf->data, VAL2STR(slots[0])->buf->data + ms, mlen);   /* re-read subject after the alloc-GC */
    slots[2] = (VALUE)r;                                             /* root the matched substring */
    slots[2] = UNWRAP(korb_matchdata_new(c, slots + 3, slots[2]));   /* MatchData in slots[2] */
    if (block != NULL) return korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);
    return RESULT_OK(slots[2]);
}
/* MatchData#[] (only group 0 supported), #to_a, #to_s. */
static RESULT korb_m_md_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    intptr_t i = 0;
    if (!korb_to_index(VALUE_SLICE_GET(a, 0), &i)) return RESULT_OK(KORB_NIL);
    if (i == 0 || i == -1) return RESULT_OK(VAL2MD(VALUE_REF_GET(self))->matched);
    return RESULT_OK(KORB_NIL);                           /* no captures */
}
static RESULT korb_m_md_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2MD(VALUE_REF_GET(self))->matched);
}
static RESULT korb_m_md_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VAL2MD(VALUE_REF_GET(self))->matched;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[1]);
}
/* String#scan(pat) — array of all (whole) matches.  Group captures unsupported
 * (engine returns whole-match only); no-group patterns are exact. */
static RESULT korb_m_str_scan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    uint8_t ci = 0; VALUE patstr;
    if (KORB_REGEXP_P(pv)) { patstr = VAL2RE(pv)->source; ci = VAL2RE(pv)->ci; }
    else if (KORB_STRING_P(pv)) patstr = pv;
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_re_arg_type(pv));
    korb_re_fn_t fn = korb_re_load(c->vm);
    if (UNLIKELY(fn == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Regexp engine (koruby_regex.so) unavailable");
    slots[0] = patstr;                                   /* root pattern across allocs */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));    /* result */
    VALUE_REF res = VALUE_REF_AT(&slots[1]);
    long off = 0;
    for (;;) {
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));
        const KorbString *p = VAL2STR(slots[0]);
        if (off > (long)s->len) break;
        long ms = 0, me = 0;
        int rc = fn(p->buf->data, p->len, s->buf->data + off, (size_t)((long)s->len - off), ci, &ms, &me);
        if (rc != 1) break;
        long abss = off + ms, abse = off + me;
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, VAL2STR(VALUE_REF_GET(self))->buf->data + abss, (uint32_t)(abse - abss)));
        CHECK(korb_ary_push_val(c, slots + 3, res, slots[2]));
        off = (abse > abss) ? abse : abss + 1;           /* empty match → advance 1 */
    }
    return RESULT_OK(VALUE_REF_GET(res));
}

VALUE
korb_const_get(struct korb_vm *vm, uint32_t name_sym)
{
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) return vm->const_vals[i];
    return KORB_NIL;
}

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

void
korb_const_define(CTX *c, uint32_t name_sym, VALUE val)
{
    struct korb_vm *const vm = c->vm;
    /* Ruby: assigning an anonymous class/module to a constant names it after
     * that constant (the first such assignment wins). */
    if (KORB_CLASS_P(val) && VAL2CLASS(val)->name_sym == 0)
        VAL2CLASS(val)->name_sym = name_sym;
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == name_sym) { vm->const_vals[i] = val; return; }
    if (vm->const_cnt == vm->const_capa) {
        uint32_t nc = vm->const_capa ? vm->const_capa * 2 : 16;
        vm->const_names = realloc(vm->const_names, sizeof(uint32_t) * nc);
        vm->const_vals  = realloc(vm->const_vals,  sizeof(VALUE) * nc);
        if (!vm->const_names || !vm->const_vals) { fprintf(stderr, "koruby_precise: oom (consts)\n"); abort(); }
        vm->const_capa = nc;
    }
    vm->const_names[vm->const_cnt] = name_sym;
    vm->const_vals[vm->const_cnt] = val;     /* root cell (scanned); no WB needed */
    vm->const_cnt++;
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
        m->mid = mid;
        k->methods[k->method_cnt++] = m;
    }
    m->rfn = NULL; m->rbfn = NULL; m->bfn = NULL; m->is_simple = 0; m->dm_proc = KORB_NIL;
    return m;
}

/* Module#define_method(name, &block) / define_method(name, proc).  In a class
 * body self is the class; the block becomes the method body (run with self =
 * receiver).  The captured env is force-closed immediately so it survives past
 * the defining frame.  Returns the method name symbol. */
static RESULT korb_m_define_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE klass = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(klass)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "define_method called on a non-class");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    VALUE nv = VALUE_SLICE_GET(a, 0);
    uint32_t mid;
    if (SYMBOL_P(nv))            mid = SYM2ID(nv);
    else if (KORB_STRING_P(nv))  mid = korb_intern(c->vm, VAL2STR(nv)->buf->data, VAL2STR(nv)->len);
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(nv));
    slots[0] = klass;                                    /* root class across allocs */
    if (block != NULL) {                                 /* block form → a (lambda) proc */
        /* a block-arg's def_env arrives in tagged prev form (base|1); korb_make_proc
         * wants the raw frame base (it reads base[-1] for outer scopes).  The
         * captured open env is shared (korb_open_env_find) and promoted to heap when
         * the defining frame returns — so closures over a shared mutable local work. */
        VALUE *const denv = (VALUE *)((uintptr_t)def_env & ~(uintptr_t)1u);
        slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, denv, KORB_CSELF_VAL(cself), 1));
    } else if (VALUE_SLICE_LEN(a) >= 2 && KORB_PROC_P(VALUE_SLICE_GET(a, 1))) {
        slots[1] = VALUE_SLICE_GET(a, 1);                /* proc form: already self-contained */
    } else {
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create Proc object without a block");
    }
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(slots[0]), mid);
    m->kind = KORB_METHOD_DM;
    m->dm_proc = slots[1];
    m->owner = slots[0];
    m->params_cnt = -1;                                  /* lenient arity (block semantics) */
    m->uses_block = 0; m->rest_slot = -1; m->post_cnt = 0;
    c->vm->method_serial++;
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
    /* Array#[] / Array#[]= redefinition deopts the node_aref/node_aset fast path. */
    if (!vm->aref_redefined && klass == korb_builtin_class_obj(vm, KORB_C_ARRAY) &&
        (mid == vm->mid_aref || mid == vm->mid_aset))
        vm->aref_redefined = true;
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

extern const struct NodeKind kind_node_ivar_get;   /* auto-attr detection */

void
korb_class_def_method(CTX *c, VALUE klass, uint32_t mid, NODE *body,
                      uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                      uint32_t uses_block, struct Node **opt_defaults, void *kw_info)
{
    korb_check_basic_op_redef(c, klass, mid);
    KorbClass *const k = VAL2CLASS(klass);
    struct korb_method *m = korb_class_method_slot(k, mid);
    m->kind = KORB_METHOD_ISEQ;
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
    c->vm->method_serial++;
}

/* lookup mid up the superclass chain; *out_def (if non-NULL) gets the class
 * that defines the found method (for `super`). */
static struct korb_method *
korb_class_find_method(VALUE klass, uint32_t mid, VALUE *out_def)
{
    while (KORB_CLASS_P(klass)) {
        KorbClass *k = VAL2CLASS(klass);
        for (uint32_t i = 0; i < k->method_cnt; i++)
            if (k->methods[i]->mid == mid) { if (out_def) *out_def = klass; return k->methods[i]; }
        /* included modules, most-recently-included first (nearest ancestor) */
        if (k->included != KORB_NIL) {
            const KorbArray *inc = VAL2ARY(k->included);
            for (int32_t j = (int32_t)inc->len - 1; j >= 0; j--) {
                VALUE mod = inc->items->data[j];
                if (!KORB_CLASS_P(mod)) continue;
                KorbClass *mk = VAL2CLASS(mod);
                for (uint32_t i = 0; i < mk->method_cnt; i++)
                    if (mk->methods[i]->mid == mid) { if (out_def) *out_def = mod; return mk->methods[i]; }
            }
        }
        klass = k->superclass;
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
    if (kw && argc >= 1 && KORB_HASH_P(base[argc - 1])) { kwhash = base[argc - 1]; pos_argc = argc - 1; }
    const uint32_t min_pos = m->req_cnt + m->post_cnt;   /* posts are required too */
    if (UNLIKELY(pos_argc < min_pos || (m->rest_slot < 0 && pos_argc > (uint32_t)m->params_cnt))) {
        if (m->rest_slot >= 0)
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %u+)", pos_argc, min_pos);
        if (m->req_cnt == (uint32_t)m->params_cnt)
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)", pos_argc, m->params_cnt);
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %u..%d)", pos_argc, m->req_cnt, m->params_cnt);
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
    }
    if (locals_cnt > pos_argc) memset(base + pos_argc, 0, (locals_cnt - pos_argc) * sizeof(VALUE));
    if (have_rest) base[m->rest_slot] = rest_arr;        /* after memset (rest_slot may be >= pos_argc when no surplus) */
    for (uint32_t i = 0; i < npost; i++) base[m->rest_slot + 1 + i] = postbuf[i];   /* post slots follow rest */
    base[locals_cnt - 1] = self;
    base[locals_cnt - 2] = (VALUE)((uintptr_t)m | 1u);   /* method entry (tagged -> GC skips); super reads owner, __method__ reads mid */
    (void)def_class;
    if (block != NULL && m->uses_block) {
        base[locals_cnt - 5] = (VALUE)((uintptr_t)block | 1u);
        base[locals_cnt - 4] = (VALUE)(uintptr_t)def_env;   /* raw PREV (odd slots / clean KorbEnv) */
        base[locals_cnt - 3] = captured_self;
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
            if (idx >= 0) { base[kw->entries[j].slot] = VAL2HASH(kwhash)->items->data[2*idx+1]; if (j < 64) present |= (1ull << j); }
        }
        for (uint32_t j = 0; j < kw->count; j++) {                  /* pass 2: defaults / required check */
            if (j < 64 && (present & (1ull << j))) continue;
            if (kw->entries[j].deflt) {
                RESULT dr = (*kw->entries[j].deflt->head.dispatcher)(c, kw->entries[j].deflt, base + locals_cnt);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                base[kw->entries[j].slot] = dr.value;
            } else {
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "missing keyword: :%s", korb_sym_name(vm, kw->entries[j].mid));
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
                    VALUE key = VAL2HASH(base[kw->kwrest_slot])->items->data[2*i];
                    bool declared = false;
                    for (uint32_t j = 0; j < kw->count; j++) if (key == ID2SYM(kw->entries[j].mid)) { declared = true; break; }
                    if (declared) continue;
                    cur[1] = key;
                    VALUE val = VAL2HASH(base[kw->kwrest_slot])->items->data[2*i+1];
                    CHECK(korb_hash_set(c, cur + 2, kr, VALUE_REF_AT(&cur[1]), val));
                }
            }
            base[kw->kwrest_slot] = VALUE_REF_GET(kr);
        }
    }
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) {
        /* Consume only a return targeted at this method (NULL = nearest-method,
         * the common case) — a block's `return` aimed at an outer method passes
         * through unchanged. */
        if (c->return_target == NULL || c->return_target == base) {
            r.state = KORB_NORMAL;
            c->return_target = NULL;
        }
    }
    else if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    if (UNLIKELY(c->vm->open_env_cnt)) r = korb_close_ret(c, base + locals_cnt, base, r);
    return r;
}

/* True if `m` can take the hash-free keyword fast path: an ISEQ with keyword
 * params but no **kwrest / *rest / post params (the box(x:,y:,z:) shape). */
static inline bool korb_kw_fast_eligible(const struct korb_method *m) {
    const struct korb_kw_info *kw = (const struct korb_kw_info *)m->kw_info;
    return m->kind == KORB_METHOD_ISEQ && kw != NULL && kw->kwrest_slot < 0 &&
           m->rest_slot < 0 && m->post_cnt == 0;
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
    base[locals_cnt - 1] = self;
    base[locals_cnt - 2] = (VALUE)((uintptr_t)m | 1u);
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
            return korb_raise(c, slots, KORB_E_ARGUMENT, line, "missing keyword: :%s", korb_sym_name(vm, kw->entries[j].mid));
        }
    }
  run_body:;
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) { if (c->return_target == NULL || c->return_target == base) { r.state = KORB_NORMAL; c->return_target = NULL; } }
    else if (UNLIKELY(r.state == KORB_RAISE)) { KorbException *e = VAL2EXC(r.value); korb_bt_append(vm, e->line, korb_sym_name(vm, mid)); e->line = line; }
    if (UNLIKELY(c->vm->open_env_cnt)) r = korb_close_ret(c, base + locals_cnt, base, r);
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
    cur[0] = UNWRAP(korb_hash_new(c, cur, kw_argc));
    VALUE_REF h = VALUE_REF_AT(&cur[0]);
    for (uint32_t p = 0; p < kw_argc; p++) {
        cur[1] = ID2SYM(kw_syms[p]);
        cur[2] = base[pos_argc + p];                      /* re-read each iter (hash_set may GC) */
        CHECK(korb_hash_set(c, cur + 3, h, VALUE_REF_AT(&cur[1]), cur[2]));
    }
    base[pos_argc] = VALUE_REF_GET(h);                    /* trailing hash replaces the kw region head */
    return korb_invoke_method(c, base + pos_argc + 1, m, pos_argc + 1, line, mid, self, def_class, NULL, NULL, KORB_NIL);
}

/* korb_invoke_simple — the streamlined is_simple ISEQ invoke — now lives in
 * node.h as an always_inline so it folds into the code_store SDs too (node_call
 * inlines its own fast path). */

RESULT
korb_class_body(CTX *c, VALUE *slots, uint32_t name_sym, NODE *body_entry, VALUE superclass, int is_module)
{
    if (superclass != KORB_NIL && !KORB_CLASS_P(superclass))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "superclass must be a Class (%s given)", korb_type_name(superclass));
    /* a class with no explicit superclass derives from Object, so its instances'
     * MRO reaches the universal Object methods (==, freeze, method, ...). */
    if (superclass == KORB_NIL && !is_module)
        superclass = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    VALUE cls = korb_const_get(c->vm, name_sym);
    if (!KORB_CLASS_P(cls)) {
        cls = UNWRAP(korb_class_new(c, slots, name_sym, superclass));
        if (is_module) VAL2CLASS(cls)->is_module = 1;
        korb_const_define(c, name_sym, cls);    /* now rooted in the const table */
    }
    slots[0] = cls;                              /* root for the body run + capture */
    return korb_block_yield(c, slots + 1, body_entry, NULL, NULL, 0, &slots[0]);
}

/* `include mod...` in a class/module body: append each module to klass->included
 * (later lookups check most-recently-included first). Returns the class. */
RESULT
korb_do_include(CTX *c, VALUE *slots, VALUE klass, VALUE_SLICE mods)
{
    slots[0] = klass;                            /* root klass across allocs */
    VALUE_REF kref = VALUE_REF_AT(&slots[0]);
    if (VAL2CLASS(klass)->included == KORB_NIL) {
        VALUE arr = UNWRAP(korb_ary_new(c, slots + 1, 4));
        KorbClass *k = VAL2CLASS(VALUE_REF_GET(kref));   /* re-read after GC */
        ARO_STORE(c, k, (VALUE *)(uintptr_t)&k->included, arr);
    }
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(mods); i++) {
        VALUE mod = VALUE_SLICE_GET(mods, i);
        if (UNLIKELY(!KORB_CLASS_P(mod)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(mod));
        slots[1] = VAL2CLASS(VALUE_REF_GET(kref))->included;   /* the array (rooted) */
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), mod));
    }
    return RESULT_OK(VALUE_REF_GET(kref));
}

/* `super` — invoke `mid` starting from def_class's superclass, keeping self.
 * `entry_cell` is the frame's fs-2 cell: the running method's entry (tagged
 * korb_method*); its owner is the def_class to search above. */
RESULT
korb_super(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
           VALUE entry_cell, VALUE self, NODE *block, VALUE *def_env, VALUE captured_self)
{
    const struct korb_method *const cur =
        ((uintptr_t)entry_cell & 1u) ? (const struct korb_method *)((uintptr_t)entry_cell & ~(uintptr_t)1u) : NULL;
    const VALUE def_class = cur ? cur->owner : KORB_NIL;
    VALUE found_def = KORB_NIL;
    struct korb_method *m = NULL;
    /* MRO after def_class: its included modules (nearest first), then superclass. */
    if (KORB_CLASS_P(def_class)) {
        VALUE inc = VAL2CLASS(def_class)->included;
        if (inc != KORB_NIL) {
            const KorbArray *arr = VAL2ARY(inc);
            for (int32_t j = (int32_t)arr->len - 1; j >= 0 && m == NULL; j--) {
                VALUE mod = arr->items->data[j];
                if (!KORB_CLASS_P(mod)) continue;
                KorbClass *mk = VAL2CLASS(mod);
                for (uint32_t i = 0; i < mk->method_cnt; i++)
                    if (mk->methods[i]->mid == mid) { m = mk->methods[i]; found_def = mod; break; }
            }
        }
        if (m == NULL) m = korb_class_find_method(VAL2CLASS(def_class)->superclass, mid, &found_def);
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
        const VALUE_SLICE args = VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc);
        if (m->uses_block) {
            slots[1] = captured_self;             /* park in scanned slot for rbfn cself ptr */
            return m->rbfn(c, slots + 2, recv, args, block, def_env, &slots[1]);
        }
        return m->rfn(c, slots + 1, recv, args);
    }
    return korb_invoke_method(c, slots, m, argc, line, mid, self, found_def, block, def_env, captured_self);
}

/* a descends from (or equals) b */
static bool
korb_class_le(VALUE a, VALUE b)
{
    while (KORB_CLASS_P(a)) {
        if (a == b) return true;
        a = VAL2CLASS(a)->superclass;
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
    if (et >= 16) return false;
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
static VALUE
korb_class_obj_of(CTX *c, VALUE self)
{
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) return VAL2OBJ(self)->klass;
    if (AROH_IS_GC_OBJECT(self) && (((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
        VALUE ov = korb_klass_override_get(c->vm, self);
        while (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) ov = VAL2CLASS(ov)->superclass;  /* singleton is transparent */
        if (ov != KORB_NIL) return ov;
    }
    if (KORB_EXC_P(self)) {
        if (VAL2EXC(self)->exc_class != KORB_NIL) return VAL2EXC(self)->exc_class;   /* user exception subclass */
        uint32_t et = VAL2EXC(self)->etype;
        if (et < 16) return korb_const_get(c->vm, c->vm->exc_name[et]);
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
    if (KORB_EXC_P(self)) {
        if (VAL2EXC(self)->exc_class != KORB_NIL) return VAL2EXC(self)->exc_class;   /* user exception subclass → its MRO */
        const uint32_t et = VAL2EXC(self)->etype;
        if (et < 16 && vm->exc_name[et]) {
            const VALUE k = korb_const_get(vm, vm->exc_name[et]);
            if (KORB_CLASS_P(k)) return k;
        }
    }
    return korb_builtin_class_obj(vm, korb_class_of(self));
}

/* True if `self` responds to `mid` (own MRO incl. inherited builtins). */
bool
korb_responds_to(CTX *c, VALUE self, uint32_t mid)
{
    const VALUE start = korb_dispatch_class(c, self);
    return KORB_CLASS_P(start) && korb_class_find_method(start, mid, NULL) != NULL;
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
        { "MatchData", KORB_C_MATCHDATA },
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

    /* BasicObject = Object's superclass; Kernel = a module mixed into Object.
     * Wiring both makes ancestors / is_a? / superclass reflect the real MRO tail. */
    { uint32_t bo = korb_intern(vm, "BasicObject", 11);
      slots[0] = korb_class_new(c, slots, bo, KORB_NIL).value;          /* super nil */
      korb_const_define(c, bo, slots[0]);
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
    { uint32_t s = korb_intern(vm, "Struct", 6); korb_const_define(c, s, korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value); }
    /* Data factory class — `Data.define(*members)` builds anonymous immutable value subclasses. */
    { uint32_t s = korb_intern(vm, "Data", 4);
      slots[0] = korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value;
      korb_const_define(c, s, slots[0]);
      slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;    /* Data's singleton holds `define` */
      korb_class_def_cfn_blk(c, slots[1], "define", korb_data_define, -1); }
    { uint32_t s = korb_intern(vm, "Module", 6); vm->name_module = s; korb_const_define(c, s, korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value); }

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
    /* Numeric: a tag module on the numeric types so is_a?(Numeric) / `Integer <
     * Numeric` answer correctly (the arithmetic methods live on the concretes). */
    uint32_t num_sym = korb_intern(vm, "Numeric", 7);
    korb_const_define(c, num_sym, KORB_NIL);
    { VALUE num = korb_class_new(c, slots, num_sym, KORB_NIL).value; VAL2CLASS(num)->is_module = 1; korb_const_define(c, num_sym, num); }
    { slots[0] = korb_const_get(vm, comp_sym);                   /* Numeric includes Comparable (CRuby) */
      VALUE num = korb_const_get(vm, num_sym);
      (void)korb_do_include(c, slots + 1, num, VALUE_SLICE_MAKE(&slots[0], 1)); }
    static const int num_in[] = { KORB_C_INTEGER, KORB_C_FLOAT, KORB_C_RATIONAL, KORB_C_COMPLEX };
    for (size_t i = 0; i < sizeof(num_in)/sizeof(num_in[0]); i++) {
        slots[0] = korb_const_get(vm, num_sym);
        VALUE k = korb_const_get(vm, vm->class_name[num_in[i]]);
        (void)korb_do_include(c, slots + 1, k, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    static const int comp_in[] = { KORB_C_INTEGER, KORB_C_FLOAT, KORB_C_STRING, KORB_C_SYMBOL, KORB_C_RATIONAL };
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
        { "TypeError",           KORB_E_TYPE,       "StandardError" },
        { "ArgumentError",       KORB_E_ARGUMENT,   "StandardError" },
        { "ZeroDivisionError",   KORB_E_ZERODIV,    "StandardError" },
        { "LocalJumpError",      KORB_E_LOCALJUMP,  "StandardError" },
        { "RangeError",          KORB_E_RANGE,      "StandardError" },
        { "IndexError",          KORB_E_INDEX,      "StandardError" },
        { "KeyError",            KORB_E_KEY,        "IndexError" },
        { "NoMethodError",       KORB_E_NOMETHOD,   "NameError" },
        { "NotImplementedError", KORB_E_NOTIMPL,    "ScriptError" },
        { "SystemStackError",    KORB_E_SYSSTACK,   "Exception" },
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
    VALUE_REF eref = SLOTS_PUSH(slots, end);          /* root end across the alloc */
    KorbRange *r = korb_alloc(c, slots, sizeof(KorbRange), KORB_OBJ_RANGE);
    r->exclude_end = exclude_end;
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rbegin, VALUE_REF_GET(bref));
    ARO_STORE(c, r, (VALUE *)(uintptr_t)&r->rend,   VALUE_REF_GET(eref));
    return RESULT_OK((VALUE)r);
}

/* ---------------------------------------------------------------------------
 * Type names for messages.
 * ------------------------------------------------------------------------- */

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
    }
    return "an instance of Object";
}

/* ---------------------------------------------------------------------------
 * Equality / comparison.
 * ------------------------------------------------------------------------- */

/* spaceship for sort/min/max: -1/0/1, or 2 if incomparable.  Integers compare
 * numerically, strings byte-lexicographically. */
static int
korb_cmp_values(VALUE a, VALUE b)
{
    if ((FIXNUM_P(a) || KORB_FLOAT_P(a)) && (FIXNUM_P(b) || KORB_FLOAT_P(b))) {
        double x = 0, y = 0; korb_num_to_d(a, &x); korb_num_to_d(b, &y);
        return (x > y) - (x < y);
    }
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        uint32_t m = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->buf->data, y->buf->data, m);
        if (c) return c < 0 ? -1 : 1;
        return (x->len > y->len) - (x->len < y->len);
    }
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
            int r = korb_cmp_full(c, x->items->data[i], y->items->data[i]);
            if (r != 0) return r;
        }
        return (x->len > y->len) - (x->len < y->len);
    }
    return korb_cmp_values(a, b);
}

bool
korb_value_eq(VALUE a, VALUE b)
{
    if (a == b) return true;    /* fixnum / symbol / singletons / identity */
    if (SYMBOL_P(a)) return false;   /* a Symbol is eql? only to the identical Symbol — skip the type cascade (hot in symbol-keyed Hash / kwargs scans) */
#ifdef KORB_HAVE_GMP
    if (KORB_INTEGER_P(a) && KORB_INTEGER_P(b)) return korb_int_cmp(a, b) == 0;   /* Bignum/Fixnum */
#endif
    if (KORB_STRING_P(a) && KORB_STRING_P(b)) {
        const KorbString *x = VAL2STR(a), *y = VAL2STR(b);
        return x->len == y->len && memcmp(x->buf->data, y->buf->data, x->len) == 0;
    }
    if (KORB_ARRAY_P(a) && KORB_ARRAY_P(b)) {         /* Array#==: same length, element-wise == */
        const KorbArray *x = VAL2ARY(a), *y = VAL2ARY(b);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++)
            if (!korb_value_eq(x->items->data[i], y->items->data[i])) return false;
        return true;
    }
    if (KORB_SET_P(a) && KORB_SET_P(b)) {             /* Set#==: same members (order-independent) */
        const KorbArray *x = VAL2ARY(VAL2SET(a)->elems), *y = VAL2ARY(VAL2SET(b)->elems);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++) {
            bool found = false;
            for (uint32_t j = 0; j < y->len; j++) if (korb_value_eq(x->items->data[i], y->items->data[j])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    if (KORB_HASH_P(a) && KORB_HASH_P(b)) {           /* Hash#==: same pairs (order-independent) */
        const KorbHash *x = VAL2HASH(a), *y = VAL2HASH(b);
        if (x->len != y->len) return false;
        for (uint32_t i = 0; i < x->len; i++) {
            int32_t j = korb_hash_find(y, x->items->data[2*i]);
            if (j < 0 || !korb_value_eq(x->items->data[2*i+1], y->items->data[2*j+1])) return false;
        }
        return true;
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
    double da, db;              /* numeric ==: 1 == 1.0, 1.0 == 1.0 */
    if ((KORB_FLOAT_P(a) || KORB_FLOAT_P(b)) && korb_num_to_d(a, &da) && korb_num_to_d(b, &db))
        return da == db;
    return false;
}

/* eql? semantics (Array#uniq/&/|/-, Set, hash membership): like ==, but numerics
 * are type-strict — 1 is NOT eql? 1.0 / (1/1).  Non-numeric → identical to ==. */
static bool korb_value_eql(VALUE a, VALUE b) {
    int ta = FIXNUM_P(a) ? 1 : KORB_FLOAT_P(a) ? 2 : KORB_RATIONAL_P(a) ? 3 : 0;
    int tb = FIXNUM_P(b) ? 1 : KORB_FLOAT_P(b) ? 2 : KORB_RATIONAL_P(b) ? 3 : 0;
    if ((ta || tb) && ta != tb) return false;        /* mixed numeric types → not eql? */
    return korb_value_eq(a, b);
}

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
    if (KORB_CLASS_P(pat)) {
        if (pat == korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT])) return true;
        VALUE cls = korb_class_obj_of(c, val);
        while (KORB_CLASS_P(cls)) { if (cls == pat) return true; cls = VAL2CLASS(cls)->superclass; }
        return false;
    }
    if (KORB_REGEXP_P(pat)) {                             /* Regexp#=== : whole-match against a String */
        if (!KORB_STRING_P(val)) return false;
        const korb_re_fn_t fn = korb_re_load(c->vm);
        if (UNLIKELY(fn == NULL)) return false;
        const KorbString *const p = VAL2STR(VAL2RE(pat)->source), *const s = VAL2STR(val);
        long ms = 0, me = 0;
        return fn(p->buf->data, p->len, s->buf->data, s->len, VAL2RE(pat)->ci, &ms, &me) == 1;
    }
    return korb_value_eq(pat, val);
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
        return RESULT_OK(korb_case_eq(c, pv.value, VALUE_REF_GET(subjref)) ? KORB_TRUE : KORB_FALSE);
      }
      case 2: {                                          /* array pattern [e0..en) */
        if (!KORB_ARRAY_P(VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE);
        if (VAL2ARY(VALUE_REF_GET(subjref))->len != p->n) return RESULT_OK(KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {
            cur[0] = VAL2ARY(VALUE_REF_GET(subjref))->items->data[i];   /* element, rooted at cur[0] */
            RESULT r = korb_pat_match(c, base, cur + 1, VALUE_REF_AT(&cur[0]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(KORB_TRUE);
      }
      case 3: {                                          /* hash pattern {k: e ...} */
        if (!KORB_HASH_P(VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {
            const int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(subjref)), p->keys[i]);
            if (idx < 0) return RESULT_OK(KORB_FALSE);
            cur[0] = VAL2HASH(VALUE_REF_GET(subjref))->items->data[2 * idx + 1];
            RESULT r = korb_pat_match(c, base, cur + 1, VALUE_REF_AT(&cur[0]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(KORB_TRUE);
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
      case 6: {                                          /* array w/ rest: [pre..., *rest, post...] */
        if (!KORB_ARRAY_P(VALUE_REF_GET(subjref))) return RESULT_OK(KORB_FALSE);
        const uint32_t len = VAL2ARY(VALUE_REF_GET(subjref))->len;
        if (len < p->n + p->npost) return RESULT_OK(KORB_FALSE);
        for (uint32_t i = 0; i < p->n; i++) {            /* pre */
            cur[0] = VAL2ARY(VALUE_REF_GET(subjref))->items->data[i];
            RESULT r = korb_pat_match(c, base, cur + 1, VALUE_REF_AT(&cur[0]), p->elems[i]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_TRUE) return RESULT_OK(KORB_FALSE);
        }
        if (p->bind_off != INT32_MIN) {                  /* bind *rest to a fresh sub-array (subj stays rooted) */
            const uint32_t rest_len = len - p->n - p->npost;
            cur[0] = UNWRAP(korb_ary_new(c, cur, rest_len));
            VALUE_REF rest = VALUE_REF_AT(&cur[0]);
            for (uint32_t i = 0; i < rest_len; i++) {
                cur[1] = VAL2ARY(VALUE_REF_GET(subjref))->items->data[p->n + i];   /* re-read (push may GC) */
                CHECK(korb_ary_push_val(c, cur + 2, rest, cur[1]));
            }
            base[p->bind_off] = VALUE_REF_GET(rest);
        }
        for (uint32_t i = 0; i < p->npost; i++) {        /* post (from the tail) */
            cur[0] = VAL2ARY(VALUE_REF_GET(subjref))->items->data[len - p->npost + i];
            RESULT r = korb_pat_match(c, base, cur + 1, VALUE_REF_AT(&cur[0]), p->elems[p->n + i]);
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
    if (FIXNUM_P(v))          snprintf(buf, cap, "%ld", (long)FIX2LONG(v));
    else if (v == KORB_NIL)   snprintf(buf, cap, "nil");
    else if (v == KORB_TRUE)  snprintf(buf, cap, "true");
    else if (v == KORB_FALSE) snprintf(buf, cap, "false");
    else                      snprintf(buf, cap, "%s", korb_type_name(v));
}

static bool korb_hash_is_subset(const KorbHash *sub, const KorbHash *sup);
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
        intptr_t x = FIX2LONG(l), y = FIX2LONG(r);
        bool t = op == 0 ? x < y : op == 1 ? x <= y : op == 2 ? x > y : x >= y;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
#ifdef KORB_HAVE_GMP
    if (KORB_INTEGER_P(l) && KORB_INTEGER_P(r)) {        /* at least one Bignum */
        int cmp = korb_int_cmp(l, r);
        bool t = op == 0 ? cmp < 0 : op == 1 ? cmp <= 0 : op == 2 ? cmp > 0 : cmp >= 0;
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
#endif
    if (KORB_HASH_P(l) && KORB_HASH_P(r)) {              /* subset/superset comparison */
        const KorbHash *me = VAL2HASH(l), *other = VAL2HASH(r);
        bool t;
        switch (op) {
          case 0:  t = me->len <  other->len && korb_hash_is_subset(me, other); break;
          case 1:  t = me->len <= other->len && korb_hash_is_subset(me, other); break;
          case 2:  t = me->len >  other->len && korb_hash_is_subset(other, me); break;
          default: t = me->len >= other->len && korb_hash_is_subset(other, me); break;
        }
        return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
    }
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(r)) {     /* exact rational/int compare */
        int cmp = korb_rat_cmp(l, r);
        if (cmp != 2) {
            bool t = (op == 0) ? cmp < 0 : (op == 1) ? cmp <= 0 : (op == 2) ? cmp > 0 : cmp >= 0;
            return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
        }
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
        int cmp = memcmp(x->buf->data, y->buf->data, min);
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
        if (korb_responds_to(c, l, mid)) {
            slots[0] = l; slots[1] = r;
            return korb_send_impl(c, slots + 2, mid, 0, 1, NULL, NULL, KORB_NIL);
        }
    }
    if (FIXNUM_P(l) || KORB_STRING_P(l) || SYMBOL_P(l)) {
        char rdesc[64];
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
    if (KORB_CLASS_P(l) || KORB_OBJECT_P(l)) {
        uint32_t mid = korb_intern(c->vm, op, (uint32_t)strlen(op));
        if (korb_responds_to(c, l, mid)) {
            slots[0] = l; slots[1] = rhs; *handled = true;
            return korb_send_impl(c, slots + 2, mid, 0, 1, NULL, NULL, KORB_NIL);
        }
    }
    *handled = false;
    return RESULT_OK(KORB_NIL);
}

RESULT
korb_plus_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_COMPLEX_P(l) || KORB_COMPLEX_P(rhs)) return korb_cpx_arith(c, slots, l, rhs, 0);
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(rhs)) return korb_num_arith(c, slots, l, rhs, 0, line);
    if (KORB_RATIONAL_P(l) || KORB_RATIONAL_P(rhs)) return korb_rat_arith(c, slots, l, rhs, 0);
    if (KORB_SET_P(l)) { slots[0] = rhs; return korb_m_set_union(c, slots + 1, lhs, VALUE_SLICE_MAKE(&slots[0], 1)); }   /* Set + → union */
    if (KORB_STRING_P(l) && KORB_STRING_P(rhs)) {
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_str_plus_ref(c, slots, lhs, r);
    }
    if (KORB_ARRAY_P(l)) {
        if (!KORB_ARRAY_P(rhs))
            return korb_raise(c, slots, KORB_E_TYPE, line,
                              "no implicit conversion of %s into Array", korb_type_name(rhs));
        VALUE_REF r = SLOTS_PUSH(slots, rhs);   /* root rhs before allocating */
        return korb_ary_plus_ref(c, slots, lhs, r);
    }
    if (FIXNUM_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "%s can't be coerced into Integer", korb_type_name(rhs));
    if (KORB_STRING_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into String", korb_type_name(rhs));
    { bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "+", &h); if (h) return ur; }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                      "undefined method '+' for %s", korb_a_type_name(l));
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
        intptr_t cnt;
        if (UNLIKELY(!korb_to_index(rhs, &cnt))) return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion of %s into Integer", korb_type_name(rhs));
        return korb_str_repeat_ref(c, slots, lhs, cnt, line);
    }
    if (KORB_ARRAY_P(l) && (FIXNUM_P(rhs) || KORB_FLOAT_P(rhs))) {   /* Array * n → repeated array (Float coerced via to_int) */
        intptr_t cnt = FIXNUM_P(rhs) ? FIX2LONG(rhs) : (intptr_t)korb_float_val(rhs);
        if (cnt < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative argument");
        uint32_t len = VAL2ARY(l)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)cnt * len)));
        for (intptr_t r = 0; r < cnt; r++)
            for (uint32_t i = 0; i < len; i++)
                CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(VALUE_REF_GET(lhs))->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_ARRAY_P(l) && KORB_STRING_P(rhs)) {     /* Array * sep → join */
        slots[0] = rhs;
        return korb_m_ary_join(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
    }
    if (KORB_ARRAY_P(l))                             /* Array * non-int/str */
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "no implicit conversion of %s into Integer", korb_type_name(rhs));
    if (FIXNUM_P(l))
        return korb_raise(c, slots, KORB_E_TYPE, line,
                          "%s can't be coerced into Integer", korb_type_name(rhs));
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

uint32_t
korb_intern(struct korb_vm *vm, const char *name, size_t len)
{
    for (uint32_t i = 0; i < vm->sym_cnt; i++) {
        if (strlen(vm->sym_names[i]) == len && memcmp(vm->sym_names[i], name, len) == 0)
            return i;
    }
    if (vm->sym_cnt == vm->sym_capa) {
        vm->sym_capa = vm->sym_capa ? vm->sym_capa * 2 : 64;
        vm->sym_names = realloc(vm->sym_names, sizeof(char *) * vm->sym_capa);
        if (!vm->sym_names) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    }
    char *copy = malloc(len + 1);
    if (!copy) { fprintf(stderr, "koruby_precise: out of memory (symbols)\n"); abort(); }
    memcpy(copy, name, len);
    copy[len] = '\0';
    vm->sym_names[vm->sym_cnt] = copy;
    return vm->sym_cnt++;
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
    m->mid = mid;
    vm->methods[vm->method_cnt++] = m;
    return m;
}

void
korb_method_define(CTX *c, uint32_t mid, NODE *body,
                   uint32_t params_cnt, uint32_t req_cnt, uint32_t post_cnt, int32_t rest_slot, uint32_t locals_cnt,
                   uint32_t uses_block, struct Node **opt_defaults, void *kw_info)
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
      default:              return "RuntimeError";
    }
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
    const char *cls = korb_etype_name(e->etype);
    const char *msg = (e->msg != KORB_NIL) ? VAL2STR(e->msg)->buf->data : cls;

    if (vm->bt_cnt > 0) {
        fprintf(stderr, "%s:%u:in '%s': %s (%s)\n", file, vm->bt[0].line, vm->bt[0].name, msg, cls);
        /* elide the middle of very deep unwinds (SystemStackError) */
        uint32_t head = vm->bt_cnt, tail = 0;
        if (vm->bt_cnt > 20) { head = 12; tail = 4; }
        for (uint32_t i = 1; i < head; i++) {
            fprintf(stderr, "\tfrom %s:%u:in '%s'\n", file, vm->bt[i].line, vm->bt[i].name);
        }
        if (tail) {
            fprintf(stderr, "\t ... %u levels...\n", vm->bt_cnt - head - tail);
            for (uint32_t i = vm->bt_cnt - tail; i < vm->bt_cnt; i++) {
                fprintf(stderr, "\tfrom %s:%u:in '%s'\n", file, vm->bt[i].line, vm->bt[i].name);
            }
        }
        fprintf(stderr, "\tfrom %s:%u:in '<main>'\n", file, e->line);
    }
    else {
        fprintf(stderr, "%s:%u:in '<main>': %s (%s)\n", file, e->line, msg, cls);
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

/* Invoke a resolved method `m` on the staged receiver (send layout: recv at
 * slots[-argc-1], args at slots[-argc..]).  Handles every method kind, so all
 * receiver dispatch funnels through one place. */
static __attribute__((no_stack_protector)) RESULT
korb_dispatch_method(CTX *c, VALUE *slots, struct korb_method *m, uint32_t mid,
                     uint32_t line, uint32_t argc, VALUE def_class,
                     NODE *block, VALUE *def_env, VALUE *captured_self)
{
    VALUE *const recv_slot = &slots[-(intptr_t)argc - 1];
    const VALUE self = *recv_slot;
    switch (m->kind) {
      case KORB_METHOD_ATTR_R:
        return RESULT_OK(korb_ivar_get(c, self, ID2SYM(m->attr_ivar)));
      case KORB_METHOD_ATTR_W: {
        if (UNLIKELY(argc != 1))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected 1)", argc);
        const VALUE v = slots[-(intptr_t)argc];
        CHECK(korb_ivar_set(c, slots, VALUE_REF_AT(recv_slot), ID2SYM(m->attr_ivar), v));
        return RESULT_OK(slots[-(intptr_t)argc]);
      }
      case KORB_METHOD_CFUNC: {
        if (UNLIKELY(m->params_cnt >= 0 && (uint32_t)m->params_cnt != argc))
            return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                              "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
        const VALUE_REF recv = VALUE_REF_AT(recv_slot);
        const VALUE_SLICE args = VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc);
        RESULT r = m->uses_block ? m->rbfn(c, slots, recv, args, block, def_env, captured_self)
                                 : m->rfn(c, slots, recv, args);
        if (UNLIKELY(r.state == KORB_RAISE)) {
            KorbException *e = VAL2EXC(r.value);
            korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid));
            e->line = line;
        }
        return r;
      }
      case KORB_METHOD_DM: {   /* define_method: run the (env-pre-closed) Proc body with self = receiver */
        const KorbProc *const p = VAL2PROC(m->dm_proc);
        RESULT r = korb_block_yield(c, slots, p->iseq, (VALUE *)(uintptr_t)p->env,
                                    &slots[-(intptr_t)argc], argc, recv_slot);   /* captured_self = receiver slot */
        if (r.state == KORB_RETURN) { r.state = KORB_NORMAL; c->return_target = NULL; }   /* return-from-method */
        else if (UNLIKELY(r.state == KORB_RAISE)) {
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
static RESULT
korb_call_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
               struct korb_callcache *cc, uint32_t argc,
               VALUE self, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    struct korb_vm *const vm = c->vm;

    /* implicit-self send / __send__ / public_send → re-dispatch with self as the
     * receiver (korb_send_impl shifts arg0 = the target method name). */
    if (UNLIKELY(argc >= 1 && (mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send))) {
        for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(intptr_t)argc + j];
        slots[0] = self;                            /* recv below the args */
        return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
    }

    /* implicit self-call on a user instance → dispatch through its class chain
     * (a miss falls through to the global function table). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_mcache_find(vm, VAL2OBJ(self)->klass, mid, &def_class);
        if (um) {
            if (um->kind == KORB_METHOD_ATTR_R)
                return RESULT_OK(korb_ivar_get(c, self, ID2SYM(um->attr_ivar)));
            if (um->kind == KORB_METHOD_ATTR_W) {
                if (UNLIKELY(argc != 1))
                    return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                                      "wrong number of arguments (given %u, expected 1)", argc);
                slots[0] = self;                       /* root self for the set */
                VALUE v = slots[-(intptr_t)argc];
                CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(um->attr_ivar), v));
                return RESULT_OK(slots[-(intptr_t)argc]);
            }
            if (um->kind == KORB_METHOD_ISEQ) {
                if (LIKELY(um->is_simple))
                    return korb_invoke_simple(c, slots, um, argc, line, mid, self, def_class);
                return korb_invoke_method(c, slots, um, argc, line, mid, self, def_class, block, def_env, KORB_CSELF_VAL(captured_self));
            }
            /* CFUNC (inherited builtin, e.g. implicit `freeze`) → re-dispatch as send */
            for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(intptr_t)argc + j];
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
    if (AROH_IS_GC_OBJECT(self) &&
        (!(KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL) || block != NULL)) {
        /* `main` (klass=nil) normally keeps globals, but a block-bearing call
         * (e.g. top-level `loop do … end`, `tap { }`) must reach the Object
         * method — top-level user defs are globals and won't respond here. */
        if (korb_responds_to(c, self, mid)) {
            for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(intptr_t)argc + j];
            slots[0] = self;                            /* recv below the args */
            return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
        }
    }

    /* `include Mod...` inside a class/module body (self is the class) */
    if (KORB_CLASS_P(self) && argc >= 1 && strcmp(korb_sym_name(vm, mid), "include") == 0) {
        return korb_do_include(c, slots, self, VALUE_SLICE_MAKE(slots - argc, argc));
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
                for (uint32_t j = 0; j < argc; j++) slots[1 + j] = slots[-(intptr_t)argc + j];
                slots[0] = self;
                return korb_send_impl(c, slots + 1 + argc, mid, line, argc, block, def_env, captured_self);
            }
            return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                              "undefined method '%s' for %s", korb_sym_name(vm, mid),
                              (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL)
                                  ? "main" : korb_a_type_name(self));
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
        if (UNLIKELY(r.state == KORB_RAISE)) {
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
        slots[0] = self;
        { VALUE v = slots[-(intptr_t)argc];
          RESULT chk = korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(m->attr_ivar), v);
          if (UNLIKELY(chk.state != KORB_NORMAL)) { *out = chk; return true; } }
        *out = RESULT_OK(slots[-(intptr_t)argc]);
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
    return korb_call(c, base + pos_argc + 1, mid, line, cc, pos_argc + 1, self);
}

RESULT
korb_call_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              struct korb_callcache *cc, uint32_t argc,
              VALUE self, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    RESULT r = korb_call_impl(c, slots, mid, line, cc, argc, self, block, def_env, captured_self);
    if (r.state == KORB_BREAK) r.state = KORB_NORMAL;   /* `break [v]` in the block = call's value */
    return r;
}

/* ---- node_entry accessors + yield ----------------------------------------- */

static RESULT korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                             NODE *block, VALUE *def_env, VALUE *captured_self);

uint32_t korb_entry_params_cnt(NODE *entry) { return entry->u.node_entry.params_cnt; }
uint32_t korb_entry_locals_cnt(NODE *entry) { return entry->u.node_entry.locals_cnt; }
static uint32_t korb_entry_destructure_n(NODE *entry) { return entry->u.node_entry.destructure_n; }
static int32_t  korb_entry_rest_slot(NODE *entry) { return entry->u.node_entry.rest_slot; }   /* -1 = no rest param */
static struct Node **korb_entry_opt_defaults(NODE *entry) { return (struct Node **)entry->u.node_entry.opt_defaults; }
static uint32_t korb_entry_req_cnt(NODE *entry) { return entry->u.node_entry.req_cnt; }
static const struct korb_kw_info *korb_entry_kw_info(NODE *entry) { return (const struct korb_kw_info *)entry->u.node_entry.kw_info; }
static const uint8_t *korb_entry_destructure_spec(NODE *entry) { return (const uint8_t *)entry->u.node_entry.destructure_spec; }
NODE    *korb_entry_body(NODE *entry)       { return entry->u.node_entry.body; }

/* Core block invocation: lay out the block frame at cursor `slots` and
 * dispatch the entry.  Args come from `argv` (argv[i] copied into block
 * params; extra dropped, missing → nil — CRuby semantics).  argv may alias the
 * cursor region (node_yield passes &slots[-argc]); copies happen before any
 * GC, so raw VALUEs in argv are safe.  A stack-overflow check returns RAISE. */
__attribute__((no_stack_protector)) RESULT
korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                 const VALUE *argv, uint32_t argc, VALUE *captured_self)
{
    /* A block whose params we couldn't compile (e.g. `|&b|`) is a node_unsupported
     * placeholder, not a node_entry — running it raises NotImplementedError instead
     * of dereferencing node_entry fields off the wrong union member (→ SEGV). */
    if (UNLIKELY(block->head.kind != &kind_node_entry)) return EVAL(c, block, slots);
    /* &block forward: re-read prev (proc->env) from the rooted Proc slot each
     * call so a GC-moved escaped env is never stale. */
    const bool fwd = (def_env == KORB_BLK_FWD);
    const VALUE prev = fwd ? VAL2PROC(*captured_self)->env : (VALUE)(uintptr_t)def_env;
    const uint32_t blocals = korb_entry_locals_cnt(block);   /* incl. self cell */
    /* block frame: bf[0]=PREV (tagged-odd slots handle, or even KorbEnv* for an
     * escaped Proc), bf[1..1+blocals)=block locals, self cell at bf[blocals]. */
    VALUE *const bf = slots;
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    }
    bf[0] = prev;
    /* keyword params: a trailing Hash is consumed as kwargs (like methods), so
     * the positional binding below sees only the positional args. */
    const struct korb_kw_info *const kw = korb_entry_kw_info(block);
    const uint32_t orig_argc = argc;
    const bool has_kw_hash = (kw && argc >= 1 && KORB_HASH_P(argv[argc - 1]));
    if (has_kw_hash) argc--;   /* positional binding below sees only positionals */
    const uint8_t *spec = korb_entry_destructure_spec(block);
    uint32_t dn = korb_entry_destructure_n(block);
    if (spec != NULL) {                                 /* mixed scalar + destructuring params, e.g. |a, (k, v)| */
        const uint32_t np = korb_entry_params_cnt(block);   /* logical param count */
        const VALUE *src = argv; uint32_t srcn = argc;
        if (np > 1 && argc == 1 && KORB_ARRAY_P(argv[0])) {  /* auto-splat one yielded Array across params */
            const KorbArray *ar = VAL2ARY(argv[0]); src = ar->items->data; srcn = ar->len;
        }
        uint32_t loc = 0;
        for (uint32_t p = 0; p < np; p++) {
            VALUE pv = (p < srcn) ? src[p] : KORB_NIL;
            uint8_t m = spec[p];
            if (m == 0) { bf[1 + loc] = pv; loc++; continue; }
            if (KORB_ARRAY_P(pv)) {                     /* destructure this param into m locals */
                const KorbArray *ar2 = VAL2ARY(pv);
                for (uint32_t j = 0; j < m; j++) bf[1 + loc + j] = j < ar2->len ? ar2->items->data[j] : KORB_NIL;
            } else {
                bf[1 + loc] = pv;
                for (uint32_t j = 1; j < m; j++) bf[1 + loc + j] = KORB_NIL;
            }
            loc += m;
        }
        for (uint32_t i = loc; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (dn > 0) {                                /* |(a, b, ...)| — splat the array arg */
        VALUE arr = (argc >= 1) ? argv[0] : KORB_NIL;
        if (KORB_ARRAY_P(arr)) {
            const KorbArray *ar = VAL2ARY(arr);
            for (uint32_t i = 0; i < dn; i++) bf[1 + i] = i < ar->len ? ar->items->data[i] : KORB_NIL;
        } else {
            bf[1] = arr;                               /* non-array → first target, rest nil */
            for (uint32_t i = 1; i < dn; i++) bf[1 + i] = KORB_NIL;
        }
        for (uint32_t i = dn; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (korb_entry_rest_slot(block) >= 0) {     /* |front..., *rest[, post...]| */
        const uint32_t np = korb_entry_params_cnt(block);
        const uint32_t rs = (uint32_t)korb_entry_rest_slot(block);
        const uint32_t npost = (np > rs + 1) ? (np - rs - 1) : 0;
        const bool splat = (np > 1 && argc == 1 && KORB_ARRAY_P(argv[0]));   /* auto-splat one Array */
        const uint32_t srcn = splat ? VAL2ARY(argv[0])->len : argc;
        /* Copy the source args into block-frame scratch FIRST (rooted): argv may
         * point at non-scanned C-locals (builtin yielders pass &elem) that the
         * rest-array alloc's GC would leave stale.  No alloc during this copy. */
        VALUE *const stage = bf + 1 + blocals;
        for (uint32_t i = 0; i < srcn; i++)
            stage[i] = splat ? VAL2ARY(argv[0])->items->data[i] : argv[i];
        const uint32_t surplus = (srcn > rs + npost) ? (srcn - rs - npost) : 0;
        for (uint32_t i = 0; i < rs; i++) bf[1 + i] = (i < srcn) ? stage[i] : KORB_NIL;   /* front */
        VALUE *const rcur = stage + srcn;                                    /* alloc above the staged source */
        rcur[0] = UNWRAP(korb_ary_new(c, rcur, surplus ? surplus : 4));
        VALUE_REF rarr = VALUE_REF_AT(&rcur[0]);
        for (uint32_t i = 0; i < surplus; i++)
            CHECK(korb_ary_push_val(c, rcur + 1, rarr, stage[rs + i]));      /* stage rooted below rcur */
        bf[1 + rs] = VALUE_REF_GET(rarr);
        for (uint32_t j = 0; j < npost; j++)                                 /* trailing post params */
            bf[1 + rs + 1 + j] = (rs + surplus + j < srcn) ? stage[rs + surplus + j] : KORB_NIL;
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else if (korb_entry_opt_defaults(block) != NULL) {   /* |req..., opt=default...| (no rest) */
        const uint32_t np = korb_entry_params_cnt(block);
        const uint32_t reqc = korb_entry_req_cnt(block);
        struct Node **const opts = korb_entry_opt_defaults(block);
        const bool splat = (np > 1 && argc == 1 && KORB_ARRAY_P(argv[0]));
        const uint32_t srcn = splat ? VAL2ARY(argv[0])->len : argc;
        for (uint32_t i = 0; i < np; i++) {
            if (i < srcn) bf[1 + i] = splat ? VAL2ARY(argv[0])->items->data[i] : argv[i];   /* provided (read before any alloc) */
            else if (i >= reqc) {                          /* optional → eval default in block scope */
                RESULT dr = EVAL(c, opts[i - reqc], bf + 1 + blocals);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                bf[1 + i] = dr.value;
            } else bf[1 + i] = KORB_NIL;                    /* missing required → nil (block-lenient) */
        }
        for (uint32_t i = np; i < blocals; i++) bf[1 + i] = KORB_NIL;
    } else {
        const uint32_t np = korb_entry_params_cnt(block);   /* np <= blocals - 1 */
        if (np > 1 && argc == 1 && KORB_ARRAY_P(argv[0])) {  /* auto-splat: |a,b| yielded one Array */
            const KorbArray *ar = VAL2ARY(argv[0]);
            for (uint32_t i = 0; i < np; i++) bf[1 + i] = i < ar->len ? ar->items->data[i] : KORB_NIL;
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
            if (idx >= 0) { bf[1 + kw->entries[j].slot] = VAL2HASH(kwhash)->items->data[2 * idx + 1]; if (j < 64) present |= (1ull << j); }
        }
        for (uint32_t j = 0; j < kw->count; j++) {      /* pass 2: defaults / required check */
            if (j < 64 && (present & (1ull << j))) continue;
            if (kw->entries[j].deflt) {
                RESULT dr = (*kw->entries[j].deflt->head.dispatcher)(c, kw->entries[j].deflt, kcur);
                if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
                bf[1 + kw->entries[j].slot] = dr.value;
            } else {
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "missing keyword: :%s", korb_sym_name(c->vm, kw->entries[j].mid));
            }
        }
    }
    bf[blocals] = fwd ? VAL2PROC(*captured_self)->self : *captured_self;   /* block's lexical self (re-read fresh) */

    RESULT r = (*block->head.dispatcher)(c, block, bf + 1 + blocals);
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;   /* `next [v]` = block value */
    if (UNLIKELY(c->vm->open_env_cnt))                 /* B3: escaped envs of this frame close now */
        r = korb_close_ret(c, bf + 1 + blocals, bf, r);
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
        const VALUE h = node[-1];
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
        const VALUE h = node[-1];
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
          case KORB_OBJ_METHOD: return KORB_C_METHOD;
          case KORB_OBJ_FIBER:  return KORB_C_FIBER;
          case KORB_OBJ_PROC:   return KORB_C_PROC;
          case KORB_OBJ_MATCHDATA: return KORB_C_MATCHDATA;
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
      case KORB_C_FIBER:  return "Fiber";
      case KORB_C_PROC:   return "Proc";
      case KORB_C_ARITHSEQ: return "Enumerator::ArithmeticSequence";
      case KORB_C_MATCHDATA: return "MatchData";
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
static RESULT
korb_send_impl(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
               NODE *block, VALUE *def_env, VALUE *captured_self)
{
    struct korb_vm *const vm = c->vm;
    VALUE *const recv_slot = &slots[-(intptr_t)argc - 1];
    VALUE self = *recv_slot;

    /* send / __send__ / public_send: redispatch by the symbol/string name in arg0.
     * Shift recv into arg0's slot so [recv | arg1..] forms an argc-1 call. */
    if (UNLIKELY(argc >= 1 && (mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send))) {
        {
            VALUE name = slots[-(intptr_t)argc];           /* arg0 */
            uint32_t rmid;
            if (SYMBOL_P(name)) rmid = SYM2ID(name);
            else if (KORB_STRING_P(name)) rmid = korb_intern(vm, VAL2STR(name)->buf->data, VAL2STR(name)->len);
            else return korb_raise(c, slots, KORB_E_TYPE, line, "%s is not a symbol nor a string", korb_type_name(name));
            /* public_send cannot reach top-level defs: CRuby exposes them as
             * private Object methods (send/__send__ bypass privacy, public_send
             * does not). */
            if (mid == vm->mid_public_send &&
                KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL &&
                !korb_responds_to(c, self, rmid) && korb_method_lookup(vm, rmid) != NULL)
                return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                                  "private method '%s' called for main", korb_sym_name(vm, rmid));
            slots[-(intptr_t)argc] = self;                 /* recv → arg0 slot; args shift down by one */
            return korb_send_impl(c, slots, rmid, line, argc - 1, block, def_env, captured_self);
        }
    }

    /* user instance → dispatch through its class chain (miss falls to Object). */
    if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL) {
        VALUE def_class = KORB_NIL;
        struct korb_method *um = korb_mcache_find(vm, VAL2OBJ(self)->klass, mid, &def_class);
        if (um) return korb_dispatch_method(c, slots, um, mid, line, argc, def_class, block, def_env, captured_self);
    }
    /* class receiver → Klass.new (allocate + initialize). */
    else if (KORB_CLASS_P(self) && mid == vm->mid_yield && VAL2CLASS(self)->name_sym == vm->name_fiber) {
        return korb_m_fiber_yield(c, slots, VALUE_REF_AT(recv_slot), VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc));
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref &&
             VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_SET]) {       /* Set[a, b, ...] */
        VALUE *const base = &slots[-(intptr_t)argc];
        slots[0] = UNWRAP(korb_ary_new(c, slots, argc));
        VALUE_REF arr = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 1, arr, base[i]));
        return korb_set_from_array(c, slots + 1, arr);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_aref &&
             (VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_ARRAY] ||
              VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_HASH])) {
        VALUE *const base = &slots[-(intptr_t)argc];
        if (VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_ARRAY]) {   /* Array[a, b, ...] → [a, b, ...] */
            slots[0] = UNWRAP(korb_ary_new(c, slots, argc));
            VALUE_REF dst = VALUE_REF_AT(&slots[0]);
            for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, base[i]));
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        /* Hash[k,v,k,v,...] | Hash[[[k,v],...]] | Hash[{...}] */
        slots[0] = UNWRAP(korb_hash_new(c, slots, argc));
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        if (argc == 1 && KORB_HASH_P(base[0])) {                          /* copy an existing Hash */
            const uint32_t n = VAL2HASH(base[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const KorbHash *src = VAL2HASH(base[0]);                   /* re-read: base[0] rooted, may move */
                slots[1] = src->items->data[2*i];                         /* key + val into rooted slots */
                slots[2] = src->items->data[2*i+1];
                CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[1]), slots[2]));
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (argc == 1 && KORB_ARRAY_P(base[0])) {                         /* array of [k,v] pairs */
            const uint32_t n = VAL2ARY(base[0])->len;
            for (uint32_t i = 0; i < n; i++) {
                const VALUE pr = VAL2ARY(base[0])->items->data[i];        /* re-read */
                if (UNLIKELY(!KORB_ARRAY_P(pr) || VAL2ARY(pr)->len < 1)) continue;
                slots[1] = VAL2ARY(pr)->items->data[0];                   /* key (rooted) */
                slots[2] = VAL2ARY(pr)->len >= 2 ? VAL2ARY(pr)->items->data[1] : KORB_NIL;
                CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[1]), slots[2]));
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (UNLIKELY(argc & 1u)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "odd number of arguments for Hash");
        for (uint32_t i = 0; i < argc; i += 2)
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&base[i]), base[i+1]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    else if (KORB_CLASS_P(self) && VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_ARRAY] &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* Array.try_convert(obj) */
        const VALUE arg = argc >= 1 ? slots[-(intptr_t)argc] : KORB_NIL;
        return RESULT_OK(KORB_ARRAY_P(arg) ? arg : KORB_NIL);              /* no to_ary coercion of arbitrary objects */
    }
    else if (KORB_CLASS_P(self) && VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_INTEGER] &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* Integer.try_convert(obj) → obj/to_int/nil */
        const VALUE arg = argc >= 1 ? slots[-(intptr_t)argc] : KORB_NIL;
        if (KORB_INTEGER_P(arg)) return RESULT_OK(arg);
        const uint32_t to_int = korb_intern(vm, "to_int", 6);
        if (korb_responds_to(c, arg, to_int)) {
            slots[0] = arg;
            return korb_send_impl(c, slots + 1, to_int, line, 0, NULL, NULL, NULL);
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && VAL2CLASS(self)->name_sym == vm->class_name[KORB_C_STRING] &&
             mid == korb_intern(vm, "try_convert", 11)) {                  /* String.try_convert(obj) → obj/to_str/nil */
        const VALUE arg = argc >= 1 ? slots[-(intptr_t)argc] : KORB_NIL;
        if (KORB_STRING_P(arg)) return RESULT_OK(arg);
        const uint32_t to_str = korb_intern(vm, "to_str", 6);
        if (korb_responds_to(c, arg, to_str)) {
            slots[0] = arg;
            return korb_send_impl(c, slots + 1, to_str, line, 0, NULL, NULL, NULL);
        }
        return RESULT_OK(KORB_NIL);
    }
    else if (KORB_CLASS_P(self) && mid == vm->mid_new) {
        uint32_t cname = VAL2CLASS(self)->name_sym;
        if (cname == vm->name_fiber)
            return korb_fiber_new(c, slots, block, def_env, captured_self);
        if (cname == vm->class_name[KORB_C_CLASS] || cname == vm->name_module) {   /* Class.new([super]) / Module.new [do…end] */
            const bool is_mod = (cname == vm->name_module);
            slots[0] = (!is_mod && argc >= 1) ? slots[-(intptr_t)argc] : korb_builtin_class_obj(vm, KORB_C_OBJECT);   /* super (rooted) */
            if (UNLIKELY(!is_mod && !KORB_CLASS_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, line, "superclass must be a Class (%s given)", korb_type_name(slots[0]));
            slots[1] = UNWRAP(korb_class_new(c, slots + 1, 0, is_mod ? KORB_NIL : slots[0]));   /* anonymous (name_sym 0) */
            if (is_mod) VAL2CLASS(slots[1])->is_module = 1;
            if (block != NULL) {                            /* body block: def's land on the new class/module */
                RESULT br = korb_block_yield(c, slots + 2, block, def_env, NULL, 0, &slots[1]);
                if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
            }
            return RESULT_OK(slots[1]);
        }
        if (cname == vm->name_struct && VAL2CLASS(self)->members == KORB_NIL)
            return korb_struct_define(c, slots, VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc), block, def_env);   /* Struct.new(*members[, kw][ do…end]) → class */
        if (VAL2CLASS(self)->members != KORB_NIL) {        /* StructSubclass.new(*vals) / .new(member: v) → init */
            const bool is_data = VAL2CLASS(*recv_slot)->is_data;
            /* Data.new accepts positional OR keyword; detect keyword form as a single
             * Hash arg whose keys all name members (no kwargs flag exists to test). */
            if (is_data && !(argc == 1 && KORB_HASH_P(slots[-(intptr_t)argc]) &&
                             korb_data_all_keys_members(vm, VAL2CLASS(*recv_slot), VAL2HASH(slots[-(intptr_t)argc]))) &&
                argc != VAL2ARY(VAL2CLASS(*recv_slot)->members)->len)
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "wrong number of arguments (given %u, expected %u)",
                                  argc, VAL2ARY(VAL2CLASS(*recv_slot)->members)->len);
            VALUE obj = UNWRAP(korb_obj_new(c, slots, *recv_slot));
            slots[0] = obj;
            const bool kwinit = is_data
                ? (argc == 1 && KORB_HASH_P(slots[-(intptr_t)argc]) &&
                   korb_data_all_keys_members(vm, VAL2CLASS(*recv_slot), VAL2HASH(slots[-(intptr_t)argc])))
                : (VAL2CLASS(*recv_slot)->struct_kwinit && argc >= 1 && KORB_HASH_P(slots[-(intptr_t)argc]));
            for (uint32_t i = 0; ; i++) {
                const KorbArray *mem = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                if (i >= mem->len) break;
                VALUE iv = korb_member_ivar_sym(vm, mem->items->data[i]);
                if (kwinit) {                              /* keyword_init: pull member by name from the kwargs hash */
                    int32_t hi = korb_hash_find(VAL2HASH(slots[-(intptr_t)argc]), mem->items->data[i]);
                    slots[1] = hi >= 0 ? VAL2HASH(slots[-(intptr_t)argc])->items->data[2*hi+1] : KORB_NIL;
                } else {
                    slots[1] = (i < argc) ? slots[-(intptr_t)argc + (intptr_t)i] : KORB_NIL;
                }
                CHECK(korb_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[0]), iv, slots[1]));
            }
            return RESULT_OK(slots[0]);
        }
        if (cname == vm->class_name[KORB_C_ARRAY]) {       /* Array.new(n[,v]) / Array.new(n){|i|} */
            intptr_t n = 0;
            if (argc >= 1) {
                if (UNLIKELY(!korb_to_index(slots[-(intptr_t)argc], &n))) return korb_raise(c, slots, KORB_E_TYPE, line, "no implicit conversion into Integer");
                if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "negative array size");
            }
            slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)n));
            VALUE_REF dst = VALUE_REF_AT(&slots[0]);
            for (intptr_t i = 0; i < n; i++) {
                if (block != NULL) {
                    VALUE iv = LONG2FIX(i);
                    RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
                } else {
                    CHECK(korb_ary_push_val(c, slots + 1, dst, argc >= 2 ? slots[-(intptr_t)argc + 1] : KORB_NIL));
                }
            }
            return RESULT_OK(VALUE_REF_GET(dst));
        }
        if (cname == vm->class_name[KORB_C_PROC]) {         /* Proc.new { } → a real Proc (not a generic Object) */
            if (UNLIKELY(block == NULL))
                return korb_raise(c, slots, KORB_E_ARGUMENT, line, "tried to create Proc object without a block");
            /* block-arg def_env arrives tagged (base|1); korb_make_proc wants the raw base. */
            VALUE *const denv = (VALUE *)((uintptr_t)def_env & ~(uintptr_t)1u);
            return korb_make_proc(c, slots, block, denv, KORB_CSELF_VAL(captured_self), 0);
        }
        if (cname == vm->class_name[KORB_C_ENUMERATOR] && block != NULL) {   /* Enumerator.new { |y| ... } (eager) */
            if (vm->yielder_class == KORB_NIL) {                        /* lazily build Enumerator::Yielder (a GC root) */
                slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));
                korb_class_def_cfn(c, slots[0], "yield", korb_m_yielder_push, -1);
                korb_class_def_cfn(c, slots[0], "<<",    korb_m_yielder_push, -1);
                vm->yielder_class = slots[0];
            }
            slots[0] = UNWRAP(korb_ary_new(c, slots, 8));               /* collector */
            slots[1] = UNWRAP(korb_obj_new(c, slots + 1, vm->yielder_class));
            CHECK(korb_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[1]), korb_intern(vm, "@__c", 4), slots[0]));
            slots[2] = slots[1];                                        /* arg0 = yielder */
            RESULT br = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, captured_self);
            if (UNLIKELY(br.state != KORB_NORMAL && br.state != KORB_BREAK)) return br;
            return korb_enum_new(c, slots + 3, slots[0], KORB_NIL);     /* eager enum from collector */
        }
        if (cname == vm->class_name[KORB_C_HASH]) {         /* Hash.new([default]) / Hash.new { |h,k| } */
            slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
            if (block != NULL) {                            /* default_proc: called on [] miss with (hash, key) */
                slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
                ARO_STORE(c, VAL2HASH(slots[0]), (VALUE *)(uintptr_t)&VAL2HASH(slots[0])->default_proc, slots[1]);
            } else if (argc >= 1) {
                ARO_STORE(c, VAL2HASH(slots[0]), (VALUE *)(uintptr_t)&VAL2HASH(slots[0])->default_val, slots[-(intptr_t)argc]);
            }
            return RESULT_OK(slots[0]);
        }
        if (cname == vm->class_name[KORB_C_SET]) {          /* Set.new([enumerable]) */
            VALUE src = argc >= 1 ? korb_set_elems_of(slots[-(intptr_t)argc]) : KORB_NIL;
            if (argc >= 1 && src == KORB_NIL) return korb_raise(c, slots, KORB_E_ARGUMENT, line, "value must be enumerable");
            if (src == KORB_NIL) { slots[0] = UNWRAP(korb_ary_new(c, slots, 0)); return korb_set_new(c, slots + 1, slots[0]); }
            slots[0] = src;
            return korb_set_from_array(c, slots + 1, VALUE_REF_AT(&slots[0]));
        }
        if (cname == vm->class_name[KORB_C_STRING]) {       /* String.new([str]) */
            if (argc >= 1 && KORB_STRING_P(slots[-(intptr_t)argc])) {
                slots[0] = slots[-(intptr_t)argc];          /* root source across the alloc */
                uint32_t len = VAL2STR(slots[0])->len;
                KorbString *r = korb_str_alloc(c, slots + 1, len);
                memcpy(r->buf->data, VAL2STR(slots[0])->buf->data, len);   /* re-read src (moved) */
                return RESULT_OK((VALUE)r);
            }
            return korb_str_new(c, slots, "", 0);
        }
        /* subclass of a constructible builtin (String/Array/Hash/Set): build that
         * payload, tag it with the subclass via the override table, run the
         * subclass's initialize if it defines one (else builtin-construct args). */
        {
            enum korb_class base = korb_builtin_base_class(vm, self);
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
                    if (!uinit && argc >= 1 && KORB_STRING_P(slots[-(intptr_t)argc])) {
                        slots[1] = slots[-(intptr_t)argc];
                        uint32_t len = VAL2STR(slots[1])->len;
                        KorbString *r = korb_str_alloc(c, slots + 2, len);
                        memcpy(r->buf->data, VAL2STR(slots[1])->buf->data, len);
                        inst = (VALUE)r;
                    } else inst = UNWRAP(korb_str_new(c, slots + 1, "", 0));
                }
                slots[1] = inst;                               /* root instance */
                korb_klass_override_set(c, slots[1], slots[0]);   /* override class = the subclass */
                if (uinit) {
                    VALUE *ibase = slots - argc;
                    RESULT ir = korb_invoke_method(c, slots, uinit, argc, line, imid, slots[1], idef, block, def_env, KORB_CSELF_VAL(captured_self));
                    if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                    return RESULT_OK(ibase[uinit->locals_cnt - 1]);   /* the (possibly moved) instance */
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
                for (uint32_t i = 0; i < argc; i++) ibase[i] = slots[-(intptr_t)argc + (intptr_t)i];
                RESULT ir = korb_invoke_method(c, ibase + argc, euinit, argc, line, vm->mid_initialize, slots[1], eidef, block, def_env, KORB_CSELF_VAL(captured_self));
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_OK(slots[1]);                          /* exception identity (mutated in place) */
            }
            if (argc >= 1 && KORB_STRING_P(slots[-(intptr_t)argc])) /* default: arg0 is the message */
                ARO_STORE(c, VAL2EXC(slots[1]), &VAL2EXC(slots[1])->msg, slots[-(intptr_t)argc]);
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
            RESULT ir = korb_invoke_method(c, slots, init, argc, line, init_mid, obj, init_def, block, def_env, KORB_CSELF_VAL(captured_self));
            if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
            return RESULT_OK(base[init->locals_cnt - 1]);        /* the (possibly moved) obj */
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
        if (KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL) {
            struct korb_method *const gm = korb_method_lookup(vm, mid);
            if (gm) return korb_dispatch_method(c, slots, gm, mid, line, argc, KORB_NIL, block, def_env, captured_self);
        }
        /* user-defined method_missing(name, *args) catches the miss. */
        if (KORB_CLASS_P(start_cls)) {
            const uint32_t mm_mid = korb_intern(vm, "method_missing", 14);
            VALUE mm_def = KORB_NIL;
            struct korb_method *const mm = korb_mcache_find(vm, start_cls, mm_mid, &mm_def);
            if (mm) {                                          /* stage [self | :name | args...] */
                slots[0] = self;
                slots[1] = ID2SYM(mid);
                for (uint32_t j = 0; j < argc; j++) slots[2 + j] = slots[-(intptr_t)argc + (intptr_t)j];
                return korb_dispatch_method(c, slots + argc + 2, mm, mm_mid, line, argc + 1, mm_def, block, def_env, captured_self);
            }
        }
        return korb_raise(c, slots, KORB_E_NOMETHOD, line,
                          "undefined method '%s' for %s",
                          korb_sym_name(vm, mid), korb_a_type_name(self));
    }
    return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, block, def_env, captured_self);
}

RESULT
korb_send(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc)
{
    return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);
}

/* Classify a class for `.new` (cached in cls->new_kind; the inputs — name_sym,
 * is_module, members, builtin-base — are all fixed once the class exists, so a
 * one-shot classification stays valid).  1 = plain user class (generic alloc +
 * initialize), 2 = special (Fiber / Struct factory / Struct subclass / a builtin
 * class or subclass / module) that needs korb_send_impl's bespoke handling. */
static uint8_t korb_class_new_kind(struct korb_vm *const vm, const VALUE cls) {
    KorbClass *const k = VAL2CLASS(cls);
    if (LIKELY(k->new_kind != 0)) return k->new_kind;
    const uint32_t cname = k->name_sym;
    uint8_t kind = 1;
    if (k->is_module || k->members != KORB_NIL || cname == vm->name_fiber ||
        (cname == vm->name_struct) || cname == vm->name_module ||
        cname == vm->class_name[KORB_C_CLASS]  ||   /* Class.new / Module.new → real class, not a generic object */
        cname == vm->class_name[KORB_C_ARRAY]  || cname == vm->class_name[KORB_C_HASH] ||
        cname == vm->class_name[KORB_C_SET]    || cname == vm->class_name[KORB_C_STRING]) {
        kind = 2;
    } else if (korb_class_exc_etype(vm, cls) >= 0) {   /* exception class → KorbException, not a generic object */
        kind = 2;
    } else {
        const enum korb_class base = korb_builtin_base_class(vm, cls);
        if (base == KORB_C_STRING || base == KORB_C_ARRAY || base == KORB_C_HASH || base == KORB_C_SET) kind = 2;
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
__attribute__((no_stack_protector)) RESULT
korb_send_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                 struct korb_inlcache *ic)
{
    struct korb_vm *const vm = c->vm;
    const VALUE recv = slots[-(intptr_t)argc - 1];
    /* class receivers (Klass.new / Fiber.yield / Struct / class methods) and the
     * send/__send__/public_send family need korb_send_impl's special handling. */
    if (UNLIKELY(KORB_CLASS_P(recv) ||
                 mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send)) {
        /* hot path: Klass.new of a plain user class → alloc + cached initialize,
         * skipping korb_send_impl's long mid_new special-case cascade and the
         * uncached korb_class_find_method(initialize) it does on every call. */
        if (mid == vm->mid_new && KORB_CLASS_P(recv) && korb_class_new_kind(vm, recv) == 1) {
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
                const RESULT ir = korb_invoke_method(c, slots, init, argc, line, vm->mid_initialize,
                                                     obj, idef, NULL, NULL, KORB_NIL);
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_OK(base[init->locals_cnt - 1]);        /* the (possibly moved) obj */
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
            if (LIKELY(ic->kind == KORB_IC_SMETHOD && ic->serial == vm->method_serial && ic->klass == recv))
                return korb_dispatch_method(c, slots, ic->m, mid, line, argc, ic->def_class, NULL, NULL, NULL);
            const VALUE start_cls = korb_dispatch_class(c, recv);
            VALUE def_class = KORB_NIL;
            struct korb_method *const m =
                KORB_CLASS_P(start_cls) ? korb_mcache_find(vm, start_cls, mid, &def_class) : NULL;
            if (LIKELY(m != NULL)) {
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
    } else {
        klass = korb_dispatch_class(c, recv);
    }
    if (LIKELY(ic->kind == KORB_IC_INSTANCE && ic->serial == vm->method_serial && ic->klass == klass)) {
        struct korb_method *const m = ic->m;
        if (LIKELY(m->kind == KORB_METHOD_ISEQ && m->is_simple))   /* hot path: inlines invoke_simple, skips dispatch_method PLT */
            return korb_invoke_simple(c, slots, m, argc, line, mid, recv, ic->def_class);
        if (m->kind == KORB_METHOD_ATTR_R)                          /* attr/struct reader: inline ivar load, skip dispatch_method PLT */
            return RESULT_OK(korb_ivar_get(c, recv, ID2SYM(m->attr_ivar)));
        if (m->kind == KORB_METHOD_CFUNC && !m->uses_block &&       /* builtin (Array#<</[], String#..) — inline the CFUNC call, skip dispatch_method */
            LIKELY(m->params_cnt < 0 || (uint32_t)m->params_cnt == argc)) {
            RESULT r = m->rfn(c, slots, VALUE_REF_AT(&slots[-(intptr_t)argc - 1]),
                              VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc));
            if (UNLIKELY(r.state == KORB_RAISE)) {
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
    ic->kind = KORB_IC_INSTANCE;
    return korb_dispatch_method(c, slots, m, mid, line, argc, def_class, NULL, NULL, NULL);
}

RESULT
korb_send_blk(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
              uint32_t argc, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    RESULT r = korb_send_impl(c, slots, mid, line, argc, block, def_env, captured_self);
    if (r.state == KORB_BREAK) r.state = KORB_NORMAL;   /* `break [v]` in the block = call's value */
    return r;
}

/* ---- integer formatting (to_s / chr helpers) ----------------------------- */

static uint32_t
korb_fmt_int(intptr_t n, int base, char *buf)
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
 * below but used by Float#coerce in builtins/float.c. */
static RESULT korb_bi_float(CTX *c, VALUE *slots, VALUE_SLICE args);

#include "builtins/bignum.c"
#include "builtins/integer.c"
#include "builtins/float.c"
#include "builtins/string.c"
#include "builtins/symbol.c"
#include "builtins/enumerator.c"
#include "builtins/set.c"
#include "builtins/math.c"
#include "builtins/array.c"
#include "builtins/hash.c"
#include "builtins/array_enum.c"
#include "builtins/range.c"
#include "builtins/array_int_ext.c"
#include "builtins/array_ext.c"
#include "builtins/int_float_ext.c"
#include "builtins/fiber.c"
#include "builtins/arithseq.c"
#include "builtins/string_ext.c"
korb_register_core_methods(CTX *c)
{
    /* Integer */
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
    korb_def_cmethod(c, KORB_C_INTEGER, "rationalize", korb_m_int_to_r, -1);
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
    korb_def_cmethod(c, KORB_C_INTEGER, "chr", korb_m_int_chr, 0);
    korb_def_cmethod(c, KORB_C_INTEGER, "**", korb_m_int_pow, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "pow", korb_m_int_pow, -1);
    korb_def_cmethod(c, KORB_C_INTEGER, "divmod", korb_m_int_divmod, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "div", korb_m_int_div, 1);
    korb_def_cmethod(c, KORB_C_INTEGER, "modulo", korb_m_int_modulo, 1);
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
#ifdef KORB_HAVE_GMP
    korb_def_modfunc(c, c->slots, korb_builtin_class_obj(c->vm, KORB_C_INTEGER), "sqrt", korb_m_integer_sqrt, 1);   /* Integer.sqrt class method */
#endif
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
    korb_def_cmethod(c, KORB_C_INTEGER, "clone", korb_m_int_self2, 0);
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
    korb_def_cmethod(c, KORB_C_STRING, "length", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "size", korb_m_str_charlen, 0);
    korb_def_cmethod(c, KORB_C_STRING, "bytesize", korb_m_str_len, 0);
    korb_def_cmethod(c, KORB_C_STRING, "empty?", korb_m_str_empty, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_s", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_str", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "+@", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "-@", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_i", korb_m_str_to_i, -1);
    korb_def_cmethod(c, KORB_C_STRING, "hex", korb_m_str_hex, 0);
    korb_def_cmethod(c, KORB_C_STRING, "oct", korb_m_str_oct, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_r", korb_m_str_to_r, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_c", korb_m_str_to_c, 0);
    korb_def_cmethod(c, KORB_C_STRING, "to_sym", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "intern", korb_m_str_to_sym, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase", korb_m_str_upcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "downcase", korb_m_str_downcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize", korb_m_str_capitalize, 0);
    korb_def_cmethod(c, KORB_C_STRING, "reverse", korb_m_str_reverse, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<<", korb_m_str_ltlt, 1);
    korb_def_cmethod(c, KORB_C_STRING, "concat", korb_m_str_concat, -1);
    korb_def_cmethod(c, KORB_C_STRING, "replace", korb_m_str_replace, 1);
    korb_def_cmethod(c, KORB_C_STRING, "prepend", korb_m_str_prepend, -1);
    korb_def_cmethod(c, KORB_C_STRING, "clear", korb_m_str_clear, 0);
    korb_def_cmethod(c, KORB_C_STRING, "upcase!", korb_m_str_upcase_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "downcase!", korb_m_str_downcase_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "capitalize!", korb_m_str_capitalize_b, 0);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase!", korb_m_str_swapcase_b, 0);
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
    korb_def_cmethod(c, KORB_C_STRING, "unicode_normalize", korb_m_str_unicode_normalize, -1);
    korb_def_cmethod(c, KORB_C_STRING, "unicode_normalized?", korb_m_str_unicode_normalized_q, -1);
    korb_def_cmethod(c, KORB_C_STRING, "unicode_normalize!", korb_m_str_unicode_normalize_bang, -1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_prefix", korb_m_str_delete_prefix, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_suffix", korb_m_str_delete_suffix, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_prefix!", korb_m_str_delete_prefix_b, 1);
    korb_def_cmethod(c, KORB_C_STRING, "delete_suffix!", korb_m_str_delete_suffix_b, 1);
    korb_def_cmethod(c, KORB_C_STRING, "dump", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_STRING, "between?", korb_m_str_between, 2);
    korb_def_cmethod(c, KORB_C_STRING, "clamp", korb_m_str_clamp, 2);
    korb_def_cmethod(c, KORB_C_STRING, "delete", korb_m_str_delete, -1);
    korb_def_cmethod(c, KORB_C_STRING, "delete!", korb_m_str_delete_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "tr", korb_m_str_tr, 2);
    korb_def_cmethod(c, KORB_C_STRING, "tr_s", korb_m_str_tr_s, 2);
    korb_def_cmethod(c, KORB_C_STRING, "tr_s!", korb_m_str_tr_s_bang, 2);
    korb_def_cmethod(c, KORB_C_STRING, "gsub", korb_m_str_gsub, -1);
    korb_def_cmethod(c, KORB_C_STRING, "sub", korb_m_str_sub, -1);
    korb_def_cmethod(c, KORB_C_STRING, "gsub!", korb_m_str_gsub_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "sub!", korb_m_str_sub_b, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rpartition", korb_m_str_rpartition, 1);
    korb_def_cmethod(c, KORB_C_STRING, "partition", korb_m_str_partition, 1);
    korb_def_cmethod(c, KORB_C_STRING, "to_f", korb_m_str_to_f, 0);
    korb_def_cmethod(c, KORB_C_STRING, "scrub", korb_m_str_self, -1);
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
    korb_def_cmethod(c, KORB_C_STRING, "b", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "dedup", korb_m_str_self, 0);
    korb_def_cmethod(c, KORB_C_STRING, "encode", korb_m_obj_dup, -1);
    korb_def_cmethod(c, KORB_C_STRING, "encode!", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "force_encoding", korb_m_str_self, -1);
    korb_def_cmethod(c, KORB_C_STRING, "valid_encoding?", korb_m_true_lit, 0);
    korb_def_cmethod(c, KORB_C_STRING, "byteindex", korb_m_str_byteindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "byterindex", korb_m_str_byterindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "chr", korb_m_str_chr, 0);
    korb_def_cmethod(c, KORB_C_STRING, "ord", korb_m_str_ord, 0);
    korb_def_cmethod(c, KORB_C_STRING, "rindex", korb_m_str_rindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase", korb_m_str_swapcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "ljust", korb_m_str_ljust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rjust", korb_m_str_rjust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "center", korb_m_str_center, -1);
    korb_def_cmethod(c, KORB_C_STRING, "[]", korb_m_str_aref, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice", korb_m_str_aref, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_char", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_grapheme_cluster", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_line", korb_m_str_each_line, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "lines", korb_m_str_lines_b, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_byte", korb_m_str_each_byte, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "bytes", korb_m_str_bytes_b, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_codepoint", korb_m_str_each_codepoint, 0);

    /* Symbol */
    korb_def_cmethod(c, KORB_C_SYMBOL, "to_s", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "id2name", korb_m_sym_to_s, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "slice", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "[]", korb_m_sym_slice, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "succ", korb_m_sym_succ, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "next", korb_m_sym_succ, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "swapcase", korb_m_sym_swapcase, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "upcase", korb_m_sym_upcase, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "downcase", korb_m_sym_downcase, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "capitalize", korb_m_sym_capitalize, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<=>", korb_m_sym_cmp, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<", korb_m_sym_lt, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "<=", korb_m_sym_le, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, ">", korb_m_sym_gt, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, ">=", korb_m_sym_ge, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "casecmp", korb_m_sym_casecmp, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "casecmp?", korb_m_sym_casecmp_p, 1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "between?", korb_m_sym_between, 2);
    korb_def_cmethod(c, KORB_C_SYMBOL, "clamp", korb_m_sym_clamp, 2);
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
    korb_def_cmethod(c, KORB_C_NIL, "rationalize", korb_m_nil_to_r, -1);   /* ignores optional eps */
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
    korb_def_cmethod(c, KORB_C_ARRAY, "sample", korb_m_ary_sample, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shuffle", korb_m_ary_shuffle, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse", korb_m_ary_reverse, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "reverse!", korb_m_ary_reverse_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "rotate!", korb_m_ary_rotate_bang, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "product", korb_m_ary_product, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "fetch_values", korb_m_ary_fetch_values, -1);
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
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "min_by", korb_m_ary_min_by, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "max_by", korb_m_ary_max_by, 0);
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
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "index", korb_m_ary_index, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "assoc", korb_m_ary_assoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rassoc", korb_m_ary_rassoc, 1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "count", korb_m_ary_count, -1);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sum", korb_m_ary_sum_b, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "pack", korb_m_ary_pack, 1);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "uniq!", korb_m_ary_uniq_bang, 0);
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
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "find", korb_m_ary_find, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "rfind", korb_m_ary_rfind, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "detect", korb_m_ary_find, 0);
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
    korb_def_cmethod(c, KORB_C_HASH, "to_h", korb_m_hash_self, 0);
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
    korb_def_cmethod(c, KORB_C_HASH, "deconstruct_keys", korb_m_ary_self, -1);   /* pattern-match hook → self */
    korb_def_cmethod(c, KORB_C_HASH, "first", korb_m_hash_first, -1);
    korb_def_cmethod(c, KORB_C_HASH, "take", korb_m_hash_take, 1);
    korb_def_cmethod(c, KORB_C_HASH, "clear", korb_m_hash_clear, 0);
    korb_def_cmethod(c, KORB_C_HASH, "shift", korb_m_hash_shift, 0);
    korb_def_cmethod(c, KORB_C_HASH, "uniq", korb_m_hash_uniq, -1);
    korb_def_cmethod(c, KORB_C_HASH, "flatten", korb_m_hash_flatten, -1);
    korb_def_cmethod(c, KORB_C_HASH, "sort", korb_m_hash_sort, 0);
    korb_def_cmethod(c, KORB_C_HASH, "fetch_values", korb_m_hash_fetch_values, -1);
    korb_def_cmethod(c, KORB_C_HASH, "dig", korb_m_hash_dig, -1);
    korb_def_cmethod(c, KORB_C_HASH, "values_at", korb_m_hash_values_at, -1);
    korb_def_cmethod(c, KORB_C_HASH, "slice", korb_m_hash_slice, -1);
    korb_def_cmethod(c, KORB_C_HASH, "except", korb_m_hash_except, -1);
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
    korb_def_cmethod(c, KORB_C_HASH, "compare_by_identity", korb_m_hash_compare_by_id, 0);
    korb_def_cmethod(c, KORB_C_HASH, "compare_by_identity?", korb_m_lit_false, 0);
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
    korb_def_cmethod(c, KORB_C_RANGE, "===", korb_m_range_include, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "min", korb_m_range_min_cmp, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "max", korb_m_range_max_cmp, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "sum", korb_m_range_sum, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "frozen?", korb_m_range_frozen, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "to_a", korb_m_range_to_a, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "to_ary", korb_m_range_to_a, 0);
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
    korb_def_cmethod_blk(c, KORB_C_RANGE, "each_with_object", korb_m_range_each_with_object, -1);
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
    korb_def_cmethod(c, KORB_C_OBJECT, "==", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "===", korb_m_obj_eq, 1);   /* default: same as == (Class/Range/Regexp/Set override) */
    korb_def_cmethod(c, KORB_C_OBJECT, "!=", korb_m_obj_neq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "equal?", korb_m_obj_equal, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "eql?", korb_m_obj_eql, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "itself", korb_m_obj_itself, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "hash", korb_m_obj_hash, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_set", korb_m_obj_ivar_set, 2);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_get", korb_m_obj_ivar_get, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variables", korb_m_obj_ivars, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "method", korb_m_obj_method, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "freeze", korb_m_obj_freeze, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_frozen_q, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "call", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "[]", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "===", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "call", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "[]", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "()", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "yield", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "===", korb_m_proc_call, -1);
    korb_def_cmethod(c, KORB_C_PROC, "lambda?", korb_m_proc_lambda_q, 0);
    korb_def_cmethod(c, KORB_C_PROC, "arity", korb_m_proc_arity, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "receiver", korb_m_meth_recv, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "name", korb_m_meth_name, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "original_name", korb_m_meth_name, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "arity", korb_m_meth_arity, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "owner", korb_m_meth_owner, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "unbind", korb_m_meth_unbind, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "bind", korb_m_meth_bind, 1);
    korb_def_cmethod(c, KORB_C_METHOD, "bind_call", korb_m_meth_bind_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "parameters", korb_m_meth_parameters, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "instance_method", korb_m_class_instance_method, 1);
    korb_def_cmethod(c, KORB_C_FIBER, "resume", korb_m_fiber_resume, -1);
    korb_def_cmethod(c, KORB_C_FIBER, "alive?", korb_m_fiber_alive, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "<=>", korb_m_obj_cmp, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "class", korb_m_obj_class, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "is_a?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "kind_of?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "extend", korb_m_obj_extend, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "singleton_class", korb_m_obj_singleton_class, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "attached_object", korb_m_class_attached_object, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "respond_to?", korb_m_obj_respond_to, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "===", korb_m_class_case_eq, 1);
    korb_def_cmethod_blk(c, KORB_C_CLASS, "define_method", korb_m_define_method, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "superclass", korb_m_class_superclass, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "name", korb_m_class_name, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "to_s", korb_m_class_to_s, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "inspect", korb_m_class_to_s, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "ancestors", korb_m_class_ancestors, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "instance_methods", korb_m_class_instance_methods, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "include?", korb_m_class_include_q, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "<",  korb_m_class_lt, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "<=", korb_m_class_le, 1);
    korb_def_cmethod(c, KORB_C_CLASS, ">",  korb_m_class_gt, 1);
    korb_def_cmethod(c, KORB_C_CLASS, ">=", korb_m_class_ge, 1);
    korb_def_cmethod(c, KORB_C_STRING, "=~", korb_m_str_match_op, 1);
    korb_def_cmethod(c, KORB_C_STRING, "match?", korb_m_str_match_q, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "match", korb_m_str_match, -1);
    korb_def_cmethod_blk(c, KORB_C_SYMBOL, "match", korb_m_str_match, -1);
    korb_def_cmethod(c, KORB_C_SYMBOL, "match?", korb_m_str_match_q, -1);
    korb_def_cmethod(c, KORB_C_STRING, "scan", korb_m_str_scan, 1);
    korb_def_cmethod(c, KORB_C_STRING, "unpack", korb_m_str_unpack, 1);
    korb_def_cmethod(c, KORB_C_STRING, "unpack1", korb_m_str_unpack1, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "=~", korb_m_re_match_op, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "match?", korb_m_re_match_q, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "===", korb_m_re_match_q, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "source", korb_m_re_source, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "[]", korb_m_md_aref, -1);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "to_s", korb_m_md_to_s, 0);
    korb_def_cmethod(c, KORB_C_MATCHDATA, "to_a", korb_m_md_to_a, 0);
    korb_def_cmethod(c, KORB_C_CLASS, "private", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "public", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "protected", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "module_function", korb_m_visibility_noop, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_get", korb_m_class_const_get, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "const_defined?", korb_m_class_const_defined, 1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_reader", korb_m_class_attr_reader, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_writer", korb_m_class_attr_writer, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr_accessor", korb_m_class_attr_accessor, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "attr", korb_m_class_attr_reader, -1);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "then", korb_m_obj_then, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "yield_self", korb_m_obj_then, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "tap", korb_m_obj_tap, 0);
    korb_def_cmethod_blk(c, KORB_C_OBJECT, "loop", korb_m_loop, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_of?", korb_m_obj_instance_of, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_false, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "dup", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "clone", korb_m_obj_dup, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_NIL,    "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_TRUE,   "frozen?", korb_m_true_lit2, 0);
    korb_def_cmethod(c, KORB_C_FALSE,  "frozen?", korb_m_true_lit2, 0);

    /* Exception */
    korb_def_cmethod(c, KORB_C_EXCEPTION, "message", korb_m_exc_message, 0);
    korb_def_cmethod(c, KORB_C_EXCEPTION, "to_s", korb_m_exc_message, 0);
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
    korb_def_cmethod(c, KORB_C_FLOAT, "%", korb_m_num_mod, 1);
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
    korb_def_cmethod(c, KORB_C_RATIONAL, "truncate", korb_m_rat_to_i, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "floor", korb_m_rat_floor, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "ceil", korb_m_rat_ceil, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "round", korb_m_rat_round, -1);
    korb_def_cmethod(c, KORB_C_RATIONAL, "to_r", korb_m_rat_self, 0);
    korb_def_cmethod(c, KORB_C_RATIONAL, "rationalize", korb_m_rat_self, -1);
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
    korb_def_cmethod(c, KORB_C_COMPLEX, "magnitude", korb_m_cpx_abs, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "to_c", korb_m_cpx_self, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "+", korb_m_cpx_add, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "-", korb_m_cpx_sub, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "*", korb_m_cpx_mul, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "==", korb_m_cpx_eq, 1);
    korb_def_cmethod(c, KORB_C_COMPLEX, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_COMPLEX, "inspect", korb_m_obj_inspect, 0);

    /* Enumerator (eager) */
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "to_a", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "entries", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "force", korb_m_enum_to_a, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "size", korb_m_enum_size, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "to_s", korb_m_enum_inspect, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "inspect", korb_m_enum_inspect, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each", korb_m_enum_each, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "map", korb_m_enum_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "collect", korb_m_enum_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "with_index", korb_m_enum_with_index, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_with_index", korb_m_enum_with_index, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "with_object", korb_m_enum_with_object, -1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "each_with_object", korb_m_enum_with_object, -1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "next", korb_m_enum_next, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "peek", korb_m_enum_peek, 0);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "first", korb_m_enum_first, -1);
    korb_def_cmethod(c, KORB_C_ENUMERATOR, "take", korb_m_enum_take, 1);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "select", korb_m_enum_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "filter", korb_m_enum_select, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "reject", korb_m_enum_reject, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "filter_map", korb_m_enum_filter_map, 0);
    korb_def_cmethod_blk(c, KORB_C_ENUMERATOR, "take_while", korb_m_enum_take_while, 0);
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
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "last", korb_m_aseq_last, -1);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "begin", korb_m_aseq_begin, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "end", korb_m_aseq_end, 0);
    korb_def_cmethod(c, KORB_C_ARITHSEQ, "step", korb_m_aseq_step_acc, 0);

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
    korb_def_cmethod(c, KORB_C_SET, "merge", korb_m_set_merge, 1);
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
    korb_def_cmethod(c, KORB_C_SET, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_SET, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "to_set", korb_m_ary_to_set, 0);
    korb_def_cmethod(c, KORB_C_HASH, "to_set", korb_m_hash_to_set, 0);
    korb_def_cmethod(c, KORB_C_RANGE, "to_set", korb_m_range_to_set, 0);

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
static bool
korb_sym_label_bare(const char *nm)
{
    if (!(*nm == '_' || (*nm >= 'a' && *nm <= 'z') || (*nm >= 'A' && *nm <= 'Z')))
        return false;
    const char *p = nm + 1;
    while (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9')) p++;
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
    if (nm[0] == '_' || (nm[0] >= 'a' && nm[0] <= 'z') || (nm[0] >= 'A' && nm[0] <= 'Z')) {
        const char *p = nm + 1;                      /* identifier with trailing '=' (setter) */
        while (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) p++;
        if (*p == '=' && p[1] == '\0') return true;
    }
    static const char *const ops[] = {
        "+","-","*","/","%","**","==","!=","<","<=",">",">=","<=>","<<",">>",
        "&","|","^","~","!","[]","[]=","+@","-@","===","=~","!~", NULL };
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

/* Array renders identically for to_s and inspect: "[1, 2, \"x\"]" — elements
 * always use inspect form. */
static void
korb_fprint_ary(CTX *c, FILE *fp, VALUE v)
{
    const KorbArray *a = VAL2ARY(v);
    fputc('[', fp);
    for (uint32_t i = 0; i < a->len; i++) {
        if (i) fputs(", ", fp);
        korb_fprint_inspect(c, fp, a->items->data[i]);
    }
    fputc(']', fp);
}

/* Hash inspect (== to_s), CRuby 4.0 form: symbol keys as `name: v` (quoted if
 * not a bare label), other keys as `k => v`. */
static void
korb_fprint_hash(CTX *c, FILE *fp, VALUE v)
{
    const KorbHash *h = VAL2HASH(v);
    fputc('{', fp);
    for (uint32_t i = 0; i < h->len; i++) {
        if (i) fputs(", ", fp);
        VALUE k = h->items->data[2 * i];
        if (SYMBOL_P(k)) {
            const char *nm = korb_sym_name(c->vm, SYM2ID(k));
            if (korb_sym_label_bare(nm)) fputs(nm, fp);
            else korb_fprint_quoted(fp, nm, (uint32_t)strlen(nm));
            fputs(": ", fp);
        } else {
            korb_fprint_inspect(c, fp, k);
            fputs(" => ", fp);
        }
        korb_fprint_inspect(c, fp, h->items->data[2 * i + 1]);
    }
    fputc('}', fp);
}

void
korb_fprint_to_s(CTX *c, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))           { fprintf(fp, "%ld", (long)FIX2LONG(v)); return; }
    if (v == KORB_NIL)         { return; }                     /* "" */
    if (v == KORB_TRUE)        { fputs("true", fp); return; }
    if (v == KORB_FALSE)       { fputs("false", fp); return; }
    if (SYMBOL_P(v))           { fputs(korb_sym_name(c->vm, SYM2ID(v)), fp); return; }
    if (KORB_FLOAT_P(v))       { char b[40]; korb_float_to_s(korb_float_val(v), b); fputs(b, fp); return; }   /* flonum or heap */
    switch (KORB_OBJ_TYPE(v)) {
#ifdef KORB_HAVE_GMP
      case KORB_OBJ_BIGNUM: {
        char *s = mpz_get_str(NULL, 10, VAL2BIG(v)->z);   /* GMP-malloc'd */
        fputs(s, fp); free(s);
        return;
      }
#endif
      case KORB_OBJ_STRING: {
        const KorbString *s = VAL2STR(v);
        fwrite(s->buf->data, 1, s->len, fp);
        return;
      }
      case KORB_OBJ_ARRAY:
        korb_fprint_ary(c, fp, v);
        return;
      case KORB_OBJ_HASH:
        korb_fprint_hash(c, fp, v);
        return;
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, false);
        return;
      case KORB_OBJ_OBJECT: {
        const KorbObject *o = VAL2OBJ(v);
        if (o->klass == KORB_NIL) { fputs("main", fp); return; }       /* top-level self */
        fprintf(fp, "#<%s>", korb_sym_name(c->vm, VAL2CLASS(o->klass)->name_sym));
        return;
      }
      case KORB_OBJ_CLASS:
        fputs(korb_sym_name(c->vm, VAL2CLASS(v)->name_sym), fp);       /* class name */
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
        if (ib && ib[0] == '-') fprintf(fp, "-%si", ib + 1);
        else                    fprintf(fp, "+%si", ib ? ib : "0");
        free(ib);
        return;
      }
      case KORB_OBJ_EXCEPTION: {
        const KorbException *e = VAL2EXC(v);
        if (e->msg != KORB_NIL) fwrite(VAL2STR(e->msg)->buf->data, 1, VAL2STR(e->msg)->len, fp);
        else fputs(korb_etype_name(e->etype), fp);
        return;
      }
      case KORB_OBJ_ENUMERATOR: {
        const KorbEnumerator *e = VAL2ENUM(v);
        if (KORB_STRING_P(e->desc)) fwrite(VAL2STR(e->desc)->buf->data, 1, VAL2STR(e->desc)->len, fp);
        else fputs("#<Enumerator>", fp);
        return;
      }
      case KORB_OBJ_SET: {                             /* Set[a, b, c] (elements via inspect) */
        const KorbArray *el = VAL2ARY(VAL2SET(v)->elems);
        fputs("Set[", fp);
        for (uint32_t i = 0; i < el->len; i++) { if (i) fputs(", ", fp); korb_fprint_inspect(c, fp, el->items->data[i]); }
        fputc(']', fp);
        return;
      }
    }
    fputs("#<Object>", fp);
}

void
korb_fprint_inspect(CTX *c, FILE *fp, VALUE v)
{
    if (FIXNUM_P(v))     { fprintf(fp, "%ld", (long)FIX2LONG(v)); return; }
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
        bool binary = (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_BINARY) != 0;
        korb_fprint_quoted_enc(fp, s->buf->data, s->len, binary);
        return;
      }
      case KORB_OBJ_RANGE:
        korb_fprint_range(c, fp, v, true);   /* inspect endpoints */
        return;
      case KORB_OBJ_RATIONAL:                            /* inspect: (n/d) */
        fputc('(', fp); korb_fprint_to_s(c, fp, VAL2RAT(v)->num); fputc('/', fp); korb_fprint_to_s(c, fp, VAL2RAT(v)->den); fputc(')', fp);
        return;
      case KORB_OBJ_MATCHDATA: {                        /* inspect: #<MatchData "group0"> */
        const KorbString *m = VAL2STR(VAL2MD(v)->matched);
        fputs("#<MatchData ", fp);
        korb_fprint_quoted_enc(fp, m->buf->data, m->len, false);
        fputc('>', fp);
        return;
      }
      case KORB_OBJ_COMPLEX:                            /* inspect: (re±|im|i) */
        fputc('(', fp); korb_fprint_to_s(c, fp, v); fputc(')', fp);
        return;
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
    }
    korb_fprint_to_s(c, fp, v);
}

/* ---------------------------------------------------------------------------
 * Builtins.
 * ------------------------------------------------------------------------- */

/* puts one value, newline-terminated; arrays flatten recursively (each element
 * on its own line), matching CRuby.  An empty array prints nothing.  User
 * objects dispatch their to_s method (slots-threaded, may GC). */
static RESULT
korb_puts_one_to(CTX *c, VALUE *slots, VALUE v, FILE *fp)
{
    if (KORB_ARRAY_P(v)) {
        slots[0] = v;                                   /* root across to_s GC in recursion */
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++)
            CHECK(korb_puts_one_to(c, slots + 1, VAL2ARY(slots[0])->items->data[i], fp));
        return RESULT_OK(KORB_NIL);
    }
    if (KORB_OBJECT_P(v)) {                             /* user object → its to_s (user or default) */
        slots[0] = v;
        RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        v = r.value;                                    /* fall through to print the string */
    }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        fwrite(s->buf->data, 1, s->len, fp);
        if (s->len == 0 || s->buf->data[s->len - 1] != '\n') fputc('\n', fp);
        return RESULT_OK(KORB_NIL);
    }
    korb_fprint_to_s(c, fp, v);
    fputc('\n', fp);
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_puts_one(CTX *c, VALUE *slots, VALUE v) { return korb_puts_one_to(c, slots, v, stdout); }

/* require / require_relative / load: no-op returning true.  koruby has the
 * common stdlib (Set, etc.) built in and no real file loader, so a require of a
 * supported feature just succeeds; code needing an unsupported one fails later
 * on the missing feature, not here. */
static RESULT
korb_bi_require(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)c; (void)slots; (void)args;
    return RESULT_OK(KORB_TRUE);
}

/* Kernel#warn(*msgs) — write each message + newline to stderr (a trailing
 * keyword Hash, e.g. uplevel:/category:, is ignored). */
static RESULT
korb_bi_warn(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(args, n - 1))) n--;   /* drop uplevel:/category: kwargs */
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_puts_one_to(c, slots, VALUE_SLICE_GET(args, i), stderr));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_bi_puts(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) {
        fputc('\n', stdout);
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = 0; i < n; i++) CHECK(korb_puts_one(c, slots, VALUE_SLICE_GET(args, i)));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_bi_rational(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (KORB_RATIONAL_P(nv) && n < 2) return RESULT_OK(nv);
    if (UNLIKELY(!KORB_INTEGER_P(nv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Rational", korb_type_name(nv));
    VALUE den = LONG2FIX(1);
    if (n >= 2) {
        VALUE dv = VALUE_SLICE_GET(args, 1);
        if (UNLIKELY(!KORB_INTEGER_P(dv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Rational", korb_type_name(dv));
        den = dv;
    }
    return korb_rat_new_v(c, slots, nv, den);
}

static RESULT
korb_bi_complex(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE re = VALUE_SLICE_GET(args, 0);
    VALUE im = (n >= 2) ? VALUE_SLICE_GET(args, 1) : LONG2FIX(0);
    return korb_cpx_new(c, slots, re, im);
}

static RESULT
korb_bi_p(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    uint32_t n = VALUE_SLICE_LEN(args);
    for (uint32_t i = 0; i < n; i++) {
        korb_fprint_inspect(c, stdout, VALUE_SLICE_GET(args, i));
        fputc('\n', stdout);
    }
    /* M0: p(a) → a; p() → nil; p(a, b, ...) returns an Array in CRuby —
     * arrays land in M1, return the first arg until then. */
    return RESULT_OK(n > 0 ? VALUE_SLICE_GET(args, 0) : KORB_NIL);
}

/* Kernel#eval(string) — parse + run the string as a program in a fresh frame
 * (self = a throwaway `main`).  No caller-binding/lvar access (M0 minimal): the
 * eval'd code sees its own locals + a fresh self, which suffices for the common
 * literal/expression eval (e.g. eval("(1..)")). */
static RESULT
korb_bi_eval(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    const VALUE sv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    const KorbString *s = VAL2STR(sv);
    NODE *ast = koruby_parse_source(c, s->buf->data, s->len, "(eval)");   /* immortal AST; no GC */
    const uint32_t locals = koruby_toplevel_locals_cnt;
    VALUE *const cur = slots + locals;                  /* the eval program's body cursor */
    memset(slots, 0, (size_t)locals * sizeof(VALUE));   /* zero its locals */
    RESULT mr = korb_obj_new(c, cur, KORB_NIL);         /* fresh `main` self */
    if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
    slots[locals - 1] = mr.value;                       /* self cell (frame top) */
    return EVAL(c, ast, cur);
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
    intptr_t acc = 0; bool any = false, prev_us = false;
#ifdef KORB_HAVE_GMP
    bool big = false; mpz_t z;
#endif
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
#ifdef KORB_HAVE_GMP
        if (big) { mpz_mul_ui(z, z, (unsigned long)base); mpz_add_ui(z, z, (unsigned long)d); continue; }
        intptr_t nn;
        if (UNLIKELY(__builtin_mul_overflow(acc, (intptr_t)base, &nn) || __builtin_add_overflow(nn, (intptr_t)d, &nn))) {
            mpz_init_set_si(z, acc); mpz_mul_ui(z, z, (unsigned long)base); mpz_add_ui(z, z, (unsigned long)d);
            big = true; continue;
        }
        acc = nn;
#else
        if (__builtin_mul_overflow(acc, base, &acc) || __builtin_add_overflow(acc, d, &acc)) goto bad;
#endif
    }
    if (!any || prev_us) goto bad;
#ifdef KORB_HAVE_GMP
    if (big) {
        if (sign < 0) mpz_neg(z, z);
        RESULT r = korb_big_from_mpz(c, slots, z);
        mpz_clear(z);
        if (UNLIKELY(r.state != KORB_NORMAL)) return false;
        *out = r.value;
        return true;
    }
#else
    (void)c; (void)slots;
#endif
    acc *= sign;
    *out = LONG2FIX(acc);   /* always FIXABLE: overflow promoted to Bignum above */
    return true;
  bad:
#ifdef KORB_HAVE_GMP
    if (big) mpz_clear(z);
#endif
    return false;
}

/* Integer(arg[, base]) — Kernel conversion.  Integer→itself, Float→truncate
 * toward zero, String→strict parse (ArgumentError on garbage). */
static RESULT
korb_bi_integer(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (UNLIKELY(n < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (FIXNUM_P(a0)) return RESULT_OK(a0);
    if (KORB_FLOAT_P(a0)) {
        double d = korb_float_val(a0);
        if (UNLIKELY(!isfinite(d) || !FIXABLE((intptr_t)d)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "Integer(): value out of range");
        return RESULT_OK(LONG2FIX((intptr_t)d));           /* trunc toward zero */
    }
    if (KORB_STRING_P(a0)) {
        int base = 0;
        if (n >= 2) {
            VALUE b = VALUE_SLICE_GET(args, 1);
            if (UNLIKELY(!FIXNUM_P(b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(b));
            base = (int)FIX2LONG(b);
        }
        const KorbString *s = VAL2STR(a0);
        VALUE v;
        if (UNLIKELY(!korb_str_to_int(c, slots, s->buf->data, s->len, base, &v)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid value for Integer(): \"%.*s\"", (int)s->len, s->buf->data);
        return RESULT_OK(v);
    }
    if (a0 == KORB_NIL)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert nil into Integer");
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Integer", korb_type_name(a0));
}

/* Kernel#format / sprintf(fmt, *args) — delegate to String#% with the rest
 * args collected into an Array (the form korb_m_str_format expects). */
static RESULT
korb_bi_format(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no format string given");
    VALUE fmt = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(fmt)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(fmt));
    slots[0] = fmt;                                       /* root format string */
    const uint32_t n = VALUE_SLICE_LEN(args) - 1;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, n));     /* arr at slots[1], scratch from slots+2 */
    VALUE_REF arr = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_ary_push_val(c, slots + 2, arr, VALUE_SLICE_GET(args, i + 1)));
    return korb_m_str_format(c, slots + 2, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(&slots[1], 1));
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
    slots[0] = a0;                                        /* root before alloc */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, 1));     /* arr at slots[1] */
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[1]);
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
    return korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, KORB_NIL);
}

/* Float(arg) — Kernel conversion.  Float→itself, Integer→to f, String→strict. */
static RESULT
korb_bi_float(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE a0 = VALUE_SLICE_GET(args, 0);
    if (KORB_FLOAT_P(a0)) return RESULT_OK(a0);
    if (FIXNUM_P(a0)) return korb_float_new(c, slots, (double)FIX2LONG(a0));
    if (KORB_STRING_P(a0)) {
        const KorbString *s = VAL2STR(a0);
        char buf[64];
        if (s->len >= sizeof(buf)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid value for Float(): \"%.*s\"", (int)s->len, s->buf->data);
        memcpy(buf, s->buf->data, s->len); buf[s->len] = '\0';
        char *endp; errno = 0;
        double d = strtod(buf, &endp);
        while (*endp && isspace((unsigned char)*endp)) endp++;
        if (UNLIKELY(endp == buf || *endp != '\0'))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid value for Float(): \"%.*s\"", (int)s->len, s->buf->data);
        return korb_float_new(c, slots, d);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Float", korb_type_name(a0));
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
    memcpy(path, ps->buf->data, ps->len); path[ps->len] = '\0';
    FILE *const f = fopen(path, "rb");
    if (UNLIKELY(!f))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory - %s", path);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (UNLIKELY(sz < 0)) { fclose(f); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "ftell failed - %s", path); }
    char *const buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); abort(); }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)rd);
    free(buf);
    return r;
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
    (void)slots;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(args); i++) {
        korb_fprint_to_s(c, stdout, VALUE_SLICE_GET(args, i));
    }
    return RESULT_OK(KORB_NIL);
}

/* raise — `raise "msg"` / `raise` → RuntimeError.  (Class-form raise needs the
 * Exception hierarchy, not yet present.) */
static RESULT
korb_bi_raise(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n >= 1) {
        VALUE a0 = VALUE_SLICE_GET(args, 0);
        if (KORB_STRING_P(a0)) {
            const KorbString *s = VAL2STR(a0);
            return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%.*s", (int)s->len, s->buf->data);
        }
        if (KORB_EXC_P(a0)) return RESULT_RAISE_(a0);   /* re-raise an exception object */
        if (KORB_CLASS_P(a0)) {                          /* raise SomeError[, msg] */
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
                const KorbString *s = VAL2STR(VALUE_SLICE_GET(args, 1));
                r = korb_raise(c, slots + 1, (unsigned)et, 0, "%.*s", (int)s->len, s->buf->data);
            } else {
                r = korb_raise(c, slots + 1, (unsigned)et, 0, "%s", korb_sym_name(c->vm, VAL2CLASS(slots[0])->name_sym));
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
                const uint32_t iargc = (n >= 2) ? (n - 1) : 0;   /* msg args (skip the class) */
                for (uint32_t i = 0; i < iargc; i++) slots[2 + i] = VALUE_SLICE_GET(args, 1 + i);
                VALUE *const icur = slots + 2 + iargc;
                RESULT ir = korb_invoke_method(c, icur, uinit, iargc, 0, init_mid, slots[1], idef, NULL, NULL, KORB_NIL);
                if (UNLIKELY(ir.state == KORB_RAISE)) return ir;
                return RESULT_RAISE_((icur - iargc)[uinit->locals_cnt - 1]);   /* the (moved) exception */
            }
            return RESULT_RAISE_(slots[1]);
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
    }
    return korb_raise(c, slots, KORB_E_RUNTIME, 0, "unhandled exception");
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

    char *base = mmap(NULL, bytes + page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { perror("koruby_precise: mmap slots"); abort(); }
    if (mprotect(base + bytes, page, PROT_NONE) != 0) {
        perror("koruby_precise: mprotect guard");
        abort();
    }

    c->slots = (VALUE *)base;
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
        }
        else {
            char here;
            c->cstack_limit = &here - ((size_t)6 << 20);   /* fallback: ~6 MiB below */
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
    korb_builtin_define(c, "p",     korb_bi_p,     -1);
    korb_builtin_define(c, "print", korb_bi_print, -1);
    korb_builtin_define(c, "raise", korb_bi_raise, -1);
    korb_builtin_define(c, "warn", korb_bi_warn, -1);
    korb_builtin_define(c, "require", korb_bi_require, -1);
    korb_builtin_define(c, "require_relative", korb_bi_require, -1);
    korb_builtin_define(c, "load", korb_bi_require, -1);
    korb_builtin_define(c, "eval",  korb_bi_eval,  -1);
    korb_builtin_define(c, "__binread", korb_bi_binread, 1);
    korb_builtin_define(c, "__clock_gettime", korb_bi_clock_gettime, -1);
    korb_builtin_define(c, "Integer", korb_bi_integer, -1);
    korb_builtin_define(c, "Float", korb_bi_float, -1);
    korb_builtin_define(c, "Array", korb_bi_array, -1);
    korb_builtin_define(c, "String", korb_bi_string, -1);
    korb_builtin_define(c, "format", korb_bi_format, -1);
    korb_builtin_define(c, "sprintf", korb_bi_format, -1);
    korb_builtin_define(c, "Rational", korb_bi_rational, -1);
    korb_builtin_define(c, "Complex", korb_bi_complex, -1);

    /* Builtin class objects must exist before core methods are registered onto
     * them (korb_def_cmethod attaches CFUNC entries to the class objects). */
    korb_init_builtin_classes(c, c->slots);
    korb_init_exception_classes(c, c->slots);
    korb_init_math(c, c->slots);
    korb_register_core_methods(c);

    /* resolve dispatch-hot method names once (see struct korb_vm). */
    c->vm->mid_send        = korb_intern(c->vm, "send", 4);
    c->vm->mid___send__    = korb_intern(c->vm, "__send__", 8);
    c->vm->mid_public_send = korb_intern(c->vm, "public_send", 11);
    c->vm->mid_new         = korb_intern(c->vm, "new", 3);
    c->vm->mid_initialize  = korb_intern(c->vm, "initialize", 10);
    c->vm->mid_yield       = korb_intern(c->vm, "yield", 5);
    c->vm->name_fiber      = korb_intern(c->vm, "Fiber", 5);
    c->vm->name_struct     = korb_intern(c->vm, "Struct", 6);
    c->vm->mid_aref        = korb_intern(c->vm, "[]", 2);
    c->vm->mid_aset        = korb_intern(c->vm, "[]=", 3);
    c->vm->mid_eqq         = korb_intern(c->vm, "===", 3);
    c->vm->mid_band        = korb_intern(c->vm, "&", 1);
    c->vm->mid_bor         = korb_intern(c->vm, "|", 1);
    c->vm->mid_bxor        = korb_intern(c->vm, "^", 1);
    c->vm->mid_shl         = korb_intern(c->vm, "<<", 2);
    c->vm->mid_shr         = korb_intern(c->vm, ">>", 2);
    c->vm->mid_cmp         = korb_intern(c->vm, "<=>", 3);

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}
