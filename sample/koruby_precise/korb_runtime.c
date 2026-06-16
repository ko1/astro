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
korb_float_new(CTX *c, VALUE *slots, double d)
{
    KorbFloat *f = korb_alloc(c, slots, sizeof(KorbFloat), KORB_OBJ_FLOAT);
    f->val = d;
    return RESULT_OK((VALUE)f);
}

static intptr_t korb_gcd_pos(intptr_t a, intptr_t b) {   /* gcd of |a|,|b| */
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { intptr_t t = a % b; a = b; b = t; }
    return a;
}
/* Make a reduced Rational num/den (den != 0); den>0 normalized. den==0 → ZeroDiv. */
RESULT
korb_rat_new(CTX *c, VALUE *slots, intptr_t num, intptr_t den)
{
    if (UNLIKELY(den == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    if (den < 0) { num = -num; den = -den; }
    intptr_t g = korb_gcd_pos(num, den);
    if (g > 1) { num /= g; den /= g; }
    KorbRational *r = korb_alloc(c, slots, sizeof(KorbRational), KORB_OBJ_RATIONAL);
    r->num = num; r->den = den;
    return RESULT_OK((VALUE)r);
}
/* (num,den) of an Int-or-Rational; false if neither. */
static bool korb_as_rat(VALUE v, intptr_t *num, intptr_t *den) {
    if (FIXNUM_P(v))        { *num = FIX2LONG(v); *den = 1; return true; }
    if (KORB_RATIONAL_P(v)) { *num = VAL2RAT(v)->num; *den = VAL2RAT(v)->den; return true; }
    return false;
}
/* Rational arithmetic (op 0+ 1- 2* 3/); Float involved → Float, else exact Rational. */
RESULT korb_rat_arith(CTX *c, VALUE *slots, VALUE l, VALUE r, int op) {
    if (KORB_FLOAT_P(l) || KORB_FLOAT_P(r)) return korb_num_arith(c, slots, l, r, op, 0);
    intptr_t ln, ld, rn, rd;
    if (UNLIKELY(!korb_as_rat(l, &ln, &ld) || !korb_as_rat(r, &rn, &rd)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Rational", korb_type_name(KORB_RATIONAL_P(l) ? r : l));
    intptr_t num, den;
    switch (op) {
      case 0: num = ln * rd + rn * ld; den = ld * rd; break;
      case 1: num = ln * rd - rn * ld; den = ld * rd; break;
      case 2: num = ln * rn;           den = ld * rd; break;
      default: num = ln * rd;          den = ld * rn; break;   /* / */
    }
    return korb_rat_new(c, slots, num, den);
}
/* Rational compare vs Int/Rational/Float → -1/0/1, or 2 if incomparable. */
static int korb_rat_cmp(VALUE l, VALUE r) {
    intptr_t ln, ld, rn, rd;
    if (korb_as_rat(l, &ln, &ld) && korb_as_rat(r, &rn, &rd)) {
        intptr_t a = ln * rd, b = rn * ld;             /* dens > 0 → sign preserved */
        return (a > b) - (a < b);
    }
    double x, y;
    if (korb_num_to_d(l, &x) && korb_num_to_d(r, &y)) return (x > y) - (x < y);
    return 2;
}
static RESULT korb_m_rat_num(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_RAT->num)); }
static RESULT korb_m_rat_den(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_RAT->den)); }
static RESULT korb_m_rat_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, (double)SELF_RAT->num / (double)SELF_RAT->den); }
static RESULT korb_m_rat_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_RAT->num / SELF_RAT->den)); }
static RESULT korb_m_rat_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_rat_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; intptr_t n = SELF_RAT->num; return korb_rat_new(c, slots, n < 0 ? -n : n, SELF_RAT->den); }
static RESULT korb_m_rat_neg(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_rat_new(c, slots, -SELF_RAT->num, SELF_RAT->den); }
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
    if (KORB_FLOAT_P(v)) { *out = VAL2FLT(v)->val;     return true; }
    if (KORB_RATIONAL_P(v)) { *out = (double)VAL2RAT(v)->num / (double)VAL2RAT(v)->den; return true; }
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
    if (KORB_FLOAT_P(v)) { *out = (intptr_t)VAL2FLT(v)->val; return true; }
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
    }
    if (val == KORB_NIL) return RESULT_OK(VALUE_REF_GET(aref));
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

/* index of key in the pair array, or -1 */
static int32_t
korb_hash_find(const KorbHash *h, VALUE key)
{
    const VALUE *const d = h->items->data;
    if (h->head.flags & KORB_FL_CMP_BY_ID) {        /* compare_by_identity */
        for (uint32_t i = 0; i < h->len; i++)
            if (d[2 * i] == key) return (int32_t)i;
        return -1;
    }
    for (uint32_t i = 0; i < h->len; i++)
        if (korb_value_eq(d[2 * i], key)) return (int32_t)i;
    return -1;
}

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

/* ivar index of `sym` in `shape` (walk to root via edge_sym), or -1 if absent. */
int32_t
korb_shape_index(struct korb_vm *vm, uint32_t shape, uint32_t sym)
{
    while (shape) {
        const struct korb_shape *s = &vm->shapes[shape];
        if (s->edge_sym == sym) return (int32_t)s->ivar_count - 1;
        shape = s->parent;
    }
    return -1;
}

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

static void korb_bt_append(struct korb_vm *vm, uint32_t line, const char *name);

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

static RESULT korb_struct_define(CTX *c, VALUE *slots, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    slots[0] = UNWRAP(korb_class_new(c, slots, 0, korb_builtin_class_obj(vm, KORB_C_OBJECT)));   /* anon class, super Object */
    VALUE_REF cls = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VALUE_SLICE_LEN(a)));
    VALUE_REF mem = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, VAL2STR(sym)->buf->data, VAL2STR(sym)->len));
        if (!SYMBOL_P(sym)) continue;                          /* skip keyword_init: hash etc. */
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256];
        snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, nm, strlen(nm)), ivar, 0);   /* reader */
        snprintf(buf, sizeof buf, "%s=", nm); korb_class_def_attr(c, VALUE_REF_GET(cls), korb_intern(vm, buf, strlen(buf)), ivar, 1);  /* writer */
        CHECK(korb_ary_push_val(c, slots + 2, mem, sym));
    }
    ARO_STORE(c, VAL2CLASS(VALUE_REF_GET(cls)), (VALUE *)(uintptr_t)&VAL2CLASS(VALUE_REF_GET(cls))->members, VALUE_REF_GET(mem));
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
    m->mid = mid;
    ARO_STORE(c, m, (VALUE *)(uintptr_t)&m->recv, VALUE_REF_GET(rref));
    return RESULT_OK((VALUE)m);
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
static RESULT korb_m_str_match_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    VALUE re;
    if (KORB_REGEXP_P(pv)) re = pv;
    else if (KORB_STRING_P(pv)) { slots[0] = pv; re = UNWRAP(korb_regexp_new(c, slots + 1, slots[0], 0)); }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_type_name(pv));
    RESULT r = korb_re_match_index(c, slots + 1, re, VALUE_REF_GET(self));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_NIL ? KORB_FALSE : KORB_TRUE);
}
/* String#scan(pat) — array of all (whole) matches.  Group captures unsupported
 * (engine returns whole-match only); no-group patterns are exact. */
static RESULT korb_m_str_scan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    uint8_t ci = 0; VALUE patstr;
    if (KORB_REGEXP_P(pv)) { patstr = VAL2RE(pv)->source; ci = VAL2RE(pv)->ci; }
    else if (KORB_STRING_P(pv)) patstr = pv;
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Regexp)", korb_type_name(pv));
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
    m->rfn = NULL; m->rbfn = NULL; m->bfn = NULL; m->is_simple = 0;
    return m;
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
    };
    const char *const nm = korb_sym_name(vm, mid);
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (strcmp(nm, ops[i]) == 0) { vm->basic_op_redefined = true; return; }
}

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
static RESULT
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
        cur[0] = UNWRAP(korb_ary_new(c, cur, surplus ? surplus : 4));
        VALUE_REF arr = VALUE_REF_AT(&cur[0]);
        for (uint32_t i = 0; i < surplus; i++)
            CHECK(korb_ary_push_val(c, cur + 1, arr, base[(uint32_t)m->params_cnt + i]));
        rest_arr = VALUE_REF_GET(arr); have_rest = true;   /* C-local; no alloc until stored below */
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
        base[locals_cnt - 5] = (VALUE)((uintptr_t)block   | 1u);
        base[locals_cnt - 4] = (VALUE)((uintptr_t)def_env | 1u);
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
    if (r.state == KORB_RETURN) r.state = KORB_NORMAL;
    else if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(vm, e->line, korb_sym_name(vm, mid));
        e->line = line;
    }
    return r;
}

/* Streamlined ISEQ invoke for is_simple methods (fixed positional arity, no
 * rest/opt/post/kw/block) — none of the generic argument machinery, fewer
 * params.  Caller guarantees m->is_simple.  always_inline so it folds into the
 * dispatch site (removes a call layer on the hot path). */
static inline __attribute__((always_inline)) RESULT
korb_invoke_simple(CTX *c, VALUE *slots, struct korb_method *m, uint32_t argc,
                   uint32_t line, uint32_t mid, VALUE self, VALUE def_class)
{
    if (UNLIKELY(argc != (uint32_t)m->params_cnt))
        return korb_raise(c, slots, KORB_E_ARGUMENT, line,
                          "wrong number of arguments (given %u, expected %d)", argc, m->params_cnt);
    VALUE *const base = slots - argc;
    const uint32_t locals_cnt = m->locals_cnt;
    char cstack_probe;
    if (UNLIKELY(base + locals_cnt + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit))
        return korb_raise(c, slots, KORB_E_SYSSTACK, line, "stack level too deep");
    if (locals_cnt > argc) memset(base + argc, 0, (locals_cnt - argc) * sizeof(VALUE));
    base[locals_cnt - 1] = self;
    base[locals_cnt - 2] = (VALUE)((uintptr_t)m | 1u);   /* method entry (tagged); super/__method__ source */
    (void)def_class;
    NODE *const body = m->body;
    RESULT r = (*body->head.dispatcher)(c, body, base + locals_cnt);
    if (r.state == KORB_RETURN) r.state = KORB_NORMAL;
    else if (UNLIKELY(r.state == KORB_RAISE)) {
        KorbException *e = VAL2EXC(r.value);
        korb_bt_append(c->vm, e->line, korb_sym_name(c->vm, mid));
        e->line = line;
    }
    return r;
}

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
        const VALUE k = VAL2OBJ(self)->klass;
        if (k != KORB_NIL) return k;                 /* user instance */
        /* `main` (klass==nil) falls through to Object */
    } else if (AROH_IS_GC_OBJECT(self) &&
               (((const AroObjectHeader *)(uintptr_t)self)->flags & KORB_FL_HAS_KLASS)) {
        const VALUE ov = korb_klass_override_get(vm, self);   /* raw: singleton kept */
        if (ov != KORB_NIL) return ov;
    }
    if (KORB_EXC_P(self)) {
        const uint32_t et = VAL2EXC(self)->etype;
        if (et < 16 && vm->exc_name[et]) {
            const VALUE k = korb_const_get(vm, vm->exc_name[et]);
            if (KORB_CLASS_P(k)) return k;
        }
    }
    return korb_builtin_class_obj(vm, korb_class_of(self));
}

/* True if `self` responds to `mid` (own MRO incl. inherited builtins). */
static bool
korb_responds_to(CTX *c, VALUE self, uint32_t mid)
{
    const VALUE start = korb_dispatch_class(c, self);
    return KORB_CLASS_P(start) && korb_class_find_method(start, mid, NULL) != NULL;
}

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
        { "ArithmeticSequence", KORB_C_ARITHSEQ },
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

    /* Struct factory class — `Struct.new(*members)` builds anonymous subclasses. */
    { uint32_t s = korb_intern(vm, "Struct", 6); korb_const_define(c, s, korb_class_new(c, slots, s, korb_const_get(vm, object_sym)).value); }

    /* Comparable / Enumerable as builtin modules, mixed into the relevant types
     * so is_a?/kind_of? report membership (the comparison/iteration methods
     * already exist natively on those types). */
    uint32_t comp_sym = korb_intern(vm, "Comparable", 10);
    korb_const_define(c, comp_sym, KORB_NIL);                     /* reserve a const slot (rooted) */
    { VALUE comp = korb_class_new(c, slots, comp_sym, KORB_NIL).value; VAL2CLASS(comp)->is_module = 1; korb_const_define(c, comp_sym, comp); }
    uint32_t enum_sym = korb_intern(vm, "Enumerable", 10);
    korb_const_define(c, enum_sym, KORB_NIL);
    { VALUE enm = korb_class_new(c, slots, enum_sym, KORB_NIL).value; VAL2CLASS(enm)->is_module = 1; korb_const_define(c, enum_sym, enm); }
    static const int comp_in[] = { KORB_C_INTEGER, KORB_C_FLOAT, KORB_C_STRING, KORB_C_SYMBOL, KORB_C_RATIONAL };
    for (size_t i = 0; i < sizeof(comp_in)/sizeof(comp_in[0]); i++) {
        slots[0] = korb_const_get(vm, comp_sym);
        VALUE k = korb_const_get(vm, vm->class_name[comp_in[i]]);
        (void)korb_do_include(c, slots + 1, k, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    static const int enum_in[] = { KORB_C_ARRAY, KORB_C_HASH, KORB_C_RANGE, KORB_C_SET, KORB_C_ENUMERATOR };
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
        { "IndexError",          -1,                "StandardError" },
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
    return korb_value_eq(pat, val);
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
        intptr_t cnt = FIXNUM_P(rhs) ? FIX2LONG(rhs) : (intptr_t)VAL2FLT(rhs)->val;
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

static void
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
      default:              return "RuntimeError";
    }
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
static RESULT
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
    if (AROH_IS_GC_OBJECT(self) && !(KORB_OBJECT_P(self) && VAL2OBJ(self)->klass == KORB_NIL)) {
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
RESULT
korb_call_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line,
                 struct korb_callcache *cc, struct korb_inlcache *ic,
                 uint32_t argc, VALUE self)
{
    struct korb_vm *const vm = c->vm;
    if (LIKELY(KORB_OBJECT_P(self) && VAL2OBJ(self)->klass != KORB_NIL)) {
        const VALUE klass = VAL2OBJ(self)->klass;
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
    }
    return korb_call_impl(c, slots, mid, line, cc, argc, self, NULL, NULL, NULL);
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
NODE    *korb_entry_body(NODE *entry)       { return entry->u.node_entry.body; }

/* Core block invocation: lay out the block frame at cursor `slots` and
 * dispatch the entry.  Args come from `argv` (argv[i] copied into block
 * params; extra dropped, missing → nil — CRuby semantics).  argv may alias the
 * cursor region (node_yield passes &slots[-argc]); copies happen before any
 * GC, so raw VALUEs in argv are safe.  A stack-overflow check returns RAISE. */
RESULT
korb_block_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env,
                 const VALUE *argv, uint32_t argc, VALUE *captured_self)
{
    const uint32_t blocals = korb_entry_locals_cnt(block);   /* incl. self cell */
    /* block frame: bf[0]=PREV(def_env, tagged), bf[1..1+blocals)=block locals,
     * with the block's self cell at base[fs-1] = bf[blocals]. */
    VALUE *const bf = slots;
    char cstack_probe;
    if (UNLIKELY(bf + 1 + blocals + KORB_FRAME_SLACK > c->slots_limit ||
                 &cstack_probe < c->cstack_limit)) {
        return korb_raise(c, slots, KORB_E_SYSSTACK, 0, "stack level too deep");
    }
    bf[0] = (VALUE)((uintptr_t)def_env | 1u);
    uint32_t dn = korb_entry_destructure_n(block);
    if (dn > 0) {                                       /* |(a, b, ...)| — splat the array arg */
        VALUE arr = (argc >= 1) ? argv[0] : KORB_NIL;
        if (KORB_ARRAY_P(arr)) {
            const KorbArray *ar = VAL2ARY(arr);
            for (uint32_t i = 0; i < dn; i++) bf[1 + i] = i < ar->len ? ar->items->data[i] : KORB_NIL;
        } else {
            bf[1] = arr;                               /* non-array → first target, rest nil */
            for (uint32_t i = 1; i < dn; i++) bf[1 + i] = KORB_NIL;
        }
        for (uint32_t i = dn; i < blocals; i++) bf[1 + i] = KORB_NIL;
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
    bf[blocals] = *captured_self;                       /* block's lexical self (re-read fresh) */

    RESULT r = (*block->head.dispatcher)(c, block, bf + 1 + blocals);
    if (r.state == KORB_NEXT) r.state = KORB_NORMAL;   /* `next [v]` = block value */
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
    NODE  *entry   = (NODE  *)(uintptr_t)((uintptr_t)block_cell   & ~(uintptr_t)1u);
    VALUE *def_env = (VALUE *)(uintptr_t)((uintptr_t)def_env_cell & ~(uintptr_t)1u);
    return korb_block_yield(c, slots, entry, def_env, slots - argc, argc, captured_self);
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
      case KORB_C_ARITHSEQ: return "Enumerator::ArithmeticSequence";
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
    else if (KORB_CLASS_P(self) && mid == vm->mid_new) {
        uint32_t cname = VAL2CLASS(self)->name_sym;
        if (cname == korb_intern(vm, "Fiber", 5))
            return korb_fiber_new(c, slots, block, def_env, captured_self);
        if (cname == korb_intern(vm, "Struct", 6) && VAL2CLASS(self)->members == KORB_NIL)
            return korb_struct_define(c, slots, VALUE_SLICE_MAKE(&slots[-(intptr_t)argc], argc));   /* Struct.new(*members) → class */
        if (VAL2CLASS(self)->members != KORB_NIL) {        /* StructSubclass.new(*vals) → positional init */
            VALUE obj = UNWRAP(korb_obj_new(c, slots, *recv_slot));
            slots[0] = obj;
            for (uint32_t i = 0; ; i++) {
                const KorbArray *mem = VAL2ARY(VAL2CLASS(*recv_slot)->members);
                if (i >= mem->len) break;
                slots[1] = (i < argc) ? slots[-(intptr_t)argc + (intptr_t)i] : KORB_NIL;
                VALUE iv = korb_member_ivar_sym(vm, mem->items->data[i]);
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
        if (cname == vm->class_name[KORB_C_HASH]) {         /* Hash.new([default]) */
            slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
            if (argc >= 1) ARO_STORE(c, VAL2HASH(slots[0]), (VALUE *)(uintptr_t)&VAL2HASH(slots[0])->default_val, slots[-(intptr_t)argc]);
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
                uint32_t imid = korb_intern(vm, "initialize", 10);
                VALUE idef = KORB_NIL;
                struct korb_method *uinit = korb_class_find_method(*recv_slot, imid, &idef);
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
        uint32_t init_mid = korb_intern(vm, "initialize", 10);
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

/* Per-call-site cached plain send (no block).  A monomorphic site resolves the
 * receiver's dispatch class, and on a serial+class match invokes the cached
 * method directly — skipping korb_send_impl's prologue, special-case probes and
 * the mcache hash.  Special receivers (a class via new/yield/..., the send
 * family) and lookup misses fall through to korb_send_impl for full handling;
 * those never fill the cache, so such sites simply stay on the slow path.  Only
 * normal-receiver, normal-method sites cache. */
RESULT
korb_send_cached(CTX *c, VALUE *slots, uint32_t mid, uint32_t line, uint32_t argc,
                 struct korb_inlcache *ic)
{
    struct korb_vm *const vm = c->vm;
    const VALUE recv = slots[-(intptr_t)argc - 1];
    /* class receivers (Klass.new / Fiber.yield / Struct / class methods) and the
     * send/__send__/public_send family need korb_send_impl's special handling. */
    if (UNLIKELY(KORB_CLASS_P(recv) ||
                 mid == vm->mid_send || mid == vm->mid___send__ || mid == vm->mid_public_send))
        return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);

    const VALUE klass = korb_dispatch_class(c, recv);
    if (LIKELY(ic->serial == vm->method_serial && ic->klass == klass))
        return korb_dispatch_method(c, slots, ic->m, mid, line, argc, ic->def_class, NULL, NULL, NULL);

    VALUE def_class = KORB_NIL;
    struct korb_method *const m =
        KORB_CLASS_P(klass) ? korb_mcache_find(vm, klass, mid, &def_class) : NULL;
    if (UNLIKELY(m == NULL))   /* NoMethodError (rare) — let korb_send_impl format/raise */
        return korb_send_impl(c, slots, mid, line, argc, NULL, NULL, NULL);
    ic->serial = vm->method_serial; ic->klass = klass; ic->m = m; ic->def_class = def_class;
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
    korb_def_cmethod(c, KORB_C_STRING, "to_i", korb_m_str_to_i, 0);
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
    korb_def_cmethod(c, KORB_C_STRING, "chars", korb_m_str_chars, 0);
    korb_def_cmethod(c, KORB_C_STRING, "codepoints", korb_m_str_codepoints, 0);
    korb_def_cmethod(c, KORB_C_STRING, "succ", korb_m_str_succ, 0);
    korb_def_cmethod(c, KORB_C_STRING, "next", korb_m_str_succ, 0);
    korb_def_cmethod(c, KORB_C_STRING, "succ!", korb_m_str_succ_bang, 0);
    korb_def_cmethod(c, KORB_C_STRING, "next!", korb_m_str_succ_bang, 0);
    korb_def_cmethod(c, KORB_C_STRING, "tr!", korb_m_str_tr_bang, 2);
    korb_def_cmethod(c, KORB_C_STRING, "grapheme_clusters", korb_m_str_chars, 0);
    korb_def_cmethod(c, KORB_C_STRING, "<=>", korb_m_str_cmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "%", korb_m_str_format, 1);
    korb_def_cmethod(c, KORB_C_STRING, "*", korb_m_str_mul, 1);
    korb_def_cmethod(c, KORB_C_STRING, "+", korb_m_str_plus, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp", korb_m_str_casecmp, 1);
    korb_def_cmethod(c, KORB_C_STRING, "casecmp?", korb_m_str_casecmp_p, 1);
    korb_def_cmethod(c, KORB_C_STRING, "byteslice", korb_m_str_byteslice, -1);
    korb_def_cmethod(c, KORB_C_STRING, "getbyte", korb_m_str_getbyte, 1);
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
    korb_def_cmethod(c, KORB_C_STRING, "rindex", korb_m_str_rindex, -1);
    korb_def_cmethod(c, KORB_C_STRING, "swapcase", korb_m_str_swapcase, 0);
    korb_def_cmethod(c, KORB_C_STRING, "ljust", korb_m_str_ljust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "rjust", korb_m_str_rjust, -1);
    korb_def_cmethod(c, KORB_C_STRING, "center", korb_m_str_center, -1);
    korb_def_cmethod(c, KORB_C_STRING, "[]", korb_m_str_aref, -1);
    korb_def_cmethod(c, KORB_C_STRING, "slice", korb_m_str_aref, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_char", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_grapheme_cluster", korb_m_str_each_char, 0);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_line", korb_m_str_each_line, 0);
    korb_def_cmethod(c, KORB_C_STRING, "lines", korb_m_str_lines, -1);
    korb_def_cmethod_blk(c, KORB_C_STRING, "each_byte", korb_m_str_each_byte, 0);
    korb_def_cmethod(c, KORB_C_STRING, "bytes", korb_m_str_bytes, 0);
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
    korb_def_cmethod(c, KORB_C_SYMBOL, "intern", korb_m_sym_to_sym, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "length", korb_m_sym_len, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "empty?", korb_m_sym_empty, 0);
    korb_def_cmethod(c, KORB_C_SYMBOL, "size", korb_m_sym_len, 0);

    /* nil */
    korb_def_cmethod(c, KORB_C_NIL, "to_s", korb_m_nil_to_s, 0);
    korb_def_cmethod(c, KORB_C_NIL, "to_i", korb_m_nil_to_i, 0);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "to_ary", korb_m_ary_self, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "entries", korb_m_obj_dup, 0);
    korb_def_cmethod_blk(c, KORB_C_ARRAY, "sort!", korb_m_ary_sort_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "tally", korb_m_ary_tally, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "first", korb_m_ary_first, -1);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "pop", korb_m_ary_pop, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "include?", korb_m_ary_include, 1);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "sum", korb_m_ary_sum, -1);
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
    korb_def_cmethod(c, KORB_C_ARRAY, "uniq", korb_m_ary_uniq, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "uniq!", korb_m_ary_uniq_bang, 0);
    korb_def_cmethod(c, KORB_C_ARRAY, "flatten", korb_m_ary_flatten, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "flatten!", korb_m_ary_flatten_b, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "concat", korb_m_ary_concat, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "unshift", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "prepend", korb_m_ary_unshift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "shift", korb_m_ary_shift, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "assoc", korb_m_ary_assoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "rassoc", korb_m_ary_rassoc, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "fetch", korb_m_ary_fetch, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "dig", korb_m_ary_dig, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "take", korb_m_ary_take, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "drop", korb_m_ary_drop, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "difference", korb_m_ary_difference, -1);
    korb_def_cmethod(c, KORB_C_ARRAY, "-", korb_m_ary_difference, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "replace", korb_m_ary_replace, 1);
    korb_def_cmethod(c, KORB_C_ARRAY, "delete", korb_m_ary_delete, 1);
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
    korb_def_cmethod(c, KORB_C_HASH, "fetch", korb_m_hash_fetch, -1);
    korb_def_cmethod(c, KORB_C_HASH, "assoc", korb_m_hash_assoc, 1);
    korb_def_cmethod(c, KORB_C_HASH, "keys", korb_m_hash_keys, 0);
    korb_def_cmethod(c, KORB_C_HASH, "values", korb_m_hash_values, 0);
    korb_def_cmethod(c, KORB_C_HASH, "delete", korb_m_hash_delete, 1);
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
    korb_def_cmethod(c, KORB_C_HASH, "to_a", korb_m_hash_to_a, 0);
    korb_def_cmethod(c, KORB_C_HASH, "entries", korb_m_hash_to_a, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_entry", korb_m_hash_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "reverse_each", korb_m_hash_reverse_each, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "each_with_index", korb_m_hash_each_with_index, 0);
    korb_def_cmethod_blk(c, KORB_C_HASH, "cycle", korb_m_hash_cycle, -1);
    korb_def_cmethod(c, KORB_C_HASH, "tally", korb_m_hash_tally, 0);
    korb_def_cmethod(c, KORB_C_HASH, "invert", korb_m_hash_invert, 0);
    korb_def_cmethod(c, KORB_C_HASH, "rehash", korb_m_hash_rehash, 0);
    korb_def_cmethod(c, KORB_C_HASH, "replace", korb_m_hash_replace, 1);
    korb_def_cmethod_blk(c, KORB_C_HASH, "drop_while", korb_m_hash_drop_while, 0);
    korb_def_cmethod(c, KORB_C_HASH, "deconstruct_keys", korb_m_ary_self, -1);   /* pattern-match hook → self */
    korb_def_cmethod(c, KORB_C_HASH, "first", korb_m_hash_first, -1);
    korb_def_cmethod(c, KORB_C_HASH, "take", korb_m_hash_take, 1);
    korb_def_cmethod(c, KORB_C_HASH, "clear", korb_m_hash_clear, 0);
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
    korb_def_cmethod(c, KORB_C_RANGE, "count", korb_m_range_count, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "include?", korb_m_range_include, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "member?", korb_m_range_include, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "cover?", korb_m_range_cover, 1);
    korb_def_cmethod(c, KORB_C_RANGE, "===", korb_m_range_include, 1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "min", korb_m_range_min_cmp, -1);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "max", korb_m_range_max_cmp, -1);
    korb_def_cmethod(c, KORB_C_RANGE, "sum", korb_m_range_sum, -1);
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
    korb_def_cmethod(c, KORB_C_RANGE, "tally", korb_m_range_tally, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "collect", korb_m_range_map, 0);
    korb_def_cmethod_blk(c, KORB_C_RANGE, "step", korb_m_range_step, 1);
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
    korb_def_cmethod(c, KORB_C_RANGE, "zip", korb_m_range_zip, -1);
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
    korb_def_cmethod(c, KORB_C_OBJECT, "eql?", korb_m_obj_eq, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "itself", korb_m_obj_itself, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_set", korb_m_obj_ivar_set, 2);
    korb_def_cmethod(c, KORB_C_OBJECT, "instance_variable_get", korb_m_obj_ivar_get, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "method", korb_m_obj_method, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "freeze", korb_m_obj_freeze, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "frozen?", korb_m_obj_frozen_q, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "call", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "[]", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "===", korb_m_meth_call, -1);
    korb_def_cmethod(c, KORB_C_METHOD, "receiver", korb_m_meth_recv, 0);
    korb_def_cmethod(c, KORB_C_METHOD, "name", korb_m_meth_name, 0);
    korb_def_cmethod(c, KORB_C_FIBER, "resume", korb_m_fiber_resume, -1);
    korb_def_cmethod(c, KORB_C_FIBER, "alive?", korb_m_fiber_alive, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "<=>", korb_m_obj_cmp, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "to_s", korb_m_obj_to_s, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "inspect", korb_m_obj_inspect, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "class", korb_m_obj_class, 0);
    korb_def_cmethod(c, KORB_C_OBJECT, "is_a?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "kind_of?", korb_m_obj_is_a, 1);
    korb_def_cmethod(c, KORB_C_OBJECT, "extend", korb_m_obj_extend, -1);
    korb_def_cmethod(c, KORB_C_OBJECT, "respond_to?", korb_m_obj_respond_to, -1);
    korb_def_cmethod(c, KORB_C_CLASS, "===", korb_m_class_case_eq, 1);
    korb_def_cmethod(c, KORB_C_STRING, "=~", korb_m_str_match_op, 1);
    korb_def_cmethod(c, KORB_C_STRING, "match?", korb_m_str_match_q, -1);
    korb_def_cmethod(c, KORB_C_STRING, "scan", korb_m_str_scan, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "=~", korb_m_re_match_op, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "match?", korb_m_re_match_q, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "===", korb_m_re_match_q, 1);
    korb_def_cmethod(c, KORB_C_REGEXP, "source", korb_m_re_source, 0);
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
    korb_def_cmethod(c, KORB_C_RATIONAL, "truncate", korb_m_rat_to_i, 0);
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

/* Range: "1..5" / "1...5"; endpoints to_s for to_s, inspect for inspect. */
static void
korb_fprint_range(CTX *c, FILE *fp, VALUE v, bool insp)
{
    const KorbRange *r = VAL2RANGE(v);
    if (insp) korb_fprint_inspect(c, fp, r->rbegin); else korb_fprint_to_s(c, fp, r->rbegin);
    fputs(r->exclude_end ? "..." : "..", fp);
    if (insp) korb_fprint_inspect(c, fp, r->rend); else korb_fprint_to_s(c, fp, r->rend);
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
        korb_float_to_s(VAL2FLT(v)->val, fb);
        fputs(fb, fp);
        return;
      }
      case KORB_OBJ_RATIONAL:
        fprintf(fp, "%ld/%ld", (long)VAL2RAT(v)->num, (long)VAL2RAT(v)->den);   /* to_s: n/d */
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
      case KORB_OBJ_RATIONAL:
        fprintf(fp, "(%ld/%ld)", (long)VAL2RAT(v)->num, (long)VAL2RAT(v)->den);   /* inspect: (n/d) */
        return;
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
 * on its own line), matching CRuby.  An empty array prints nothing. */
static void
korb_puts_one(CTX *c, VALUE v)
{
    if (KORB_ARRAY_P(v)) {
        const KorbArray *a = VAL2ARY(v);
        for (uint32_t i = 0; i < a->len; i++) korb_puts_one(c, a->items->data[i]);
        return;
    }
    if (KORB_STRING_P(v)) {
        const KorbString *s = VAL2STR(v);
        fwrite(s->buf->data, 1, s->len, stdout);
        if (s->len == 0 || s->buf->data[s->len - 1] != '\n') fputc('\n', stdout);
        return;
    }
    korb_fprint_to_s(c, stdout, v);
    fputc('\n', stdout);
}

static RESULT
korb_bi_puts(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    uint32_t n = VALUE_SLICE_LEN(args);
    if (n == 0) {
        fputc('\n', stdout);
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = 0; i < n; i++) korb_puts_one(c, VALUE_SLICE_GET(args, i));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_bi_rational(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    uint32_t n = VALUE_SLICE_LEN(args);
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE nv = VALUE_SLICE_GET(args, 0);
    if (KORB_RATIONAL_P(nv) && n < 2) return RESULT_OK(nv);
    if (UNLIKELY(!FIXNUM_P(nv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Rational", korb_type_name(nv));
    intptr_t num = FIX2LONG(nv), den = 1;
    if (n >= 2) {
        VALUE dv = VALUE_SLICE_GET(args, 1);
        if (UNLIKELY(!FIXNUM_P(dv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Rational", korb_type_name(dv));
        den = FIX2LONG(dv);
    }
    return korb_rat_new(c, slots, num, den);
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

/* strict string→integer parse for Integer():  optional surrounding whitespace,
 * sign, base prefix (0x/0b/0o/0d, leading-0 octal when base auto), and `_`
 * digit separators (single, between digits).  base==0 = auto-detect.  Returns
 * false on any malformation or fixnum overflow. */
static bool
korb_str_to_int(const char *s, uint32_t len, int base, intptr_t *out)
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
    for (; i < end; i++) {
        char ch = s[i];
        if (ch == '_') { if (!any || prev_us) return false; prev_us = true; continue; }
        prev_us = false;
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') d = (ch | 0x20) - 'a' + 10;
        else return false;
        if (d >= base) return false;
        if (__builtin_mul_overflow(acc, base, &acc) ||
            __builtin_add_overflow(acc, d, &acc)) return false;
        any = true;
    }
    if (!any || prev_us) return false;
    acc *= sign;
    if (!FIXABLE(acc)) return false;
    *out = acc;
    return true;
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
        double d = VAL2FLT(a0)->val;
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
        intptr_t v;
        if (UNLIKELY(!korb_str_to_int(s->buf->data, s->len, base, &v)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid value for Integer(): \"%.*s\"", (int)s->len, s->buf->data);
        return RESULT_OK(LONG2FIX(v));
    }
    if (a0 == KORB_NIL)
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert nil into Integer");
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Integer", korb_type_name(a0));
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
            const KorbClass *k = VAL2CLASS(a0);
            if (k->exc_etype < 0)
                return korb_raise(c, slots, KORB_E_TYPE, 0, "exception class/object expected");
            if (n >= 2 && KORB_STRING_P(VALUE_SLICE_GET(args, 1))) {
                const KorbString *s = VAL2STR(VALUE_SLICE_GET(args, 1));
                return korb_raise(c, slots, (unsigned)k->exc_etype, 0, "%.*s", (int)s->len, s->buf->data);
            }
            return korb_raise(c, slots, (unsigned)k->exc_etype, 0, "%s", korb_sym_name(c->vm, k->name_sym));
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
    korb_builtin_define(c, "__binread", korb_bi_binread, 1);
    korb_builtin_define(c, "__clock_gettime", korb_bi_clock_gettime, -1);
    korb_builtin_define(c, "Integer", korb_bi_integer, -1);
    korb_builtin_define(c, "Float", korb_bi_float, -1);
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
    c->vm->mid_yield       = korb_intern(c->vm, "yield", 5);
    c->vm->name_fiber      = korb_intern(c->vm, "Fiber", 5);
    c->vm->mid_aref        = korb_intern(c->vm, "[]", 2);
    c->vm->mid_aset        = korb_intern(c->vm, "[]=", 3);

    return c;
}

void
korb_ctx_free(CTX *c)
{
    aro_gc_fini(c);
    /* slots mmap + VM tables are process-lifetime; OS reclaims. */
}
