/* koruby_precise — float.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Float methods ------------------------------------------------------- */
static RESULT korb_m_flt_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_flt_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_flt_to_rat(c, slots, SELF_FLT); }
/* unary minus `-@` (also node_neg's deopt target under basic-op redefinition). */
static RESULT korb_m_flt_uminus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, -SELF_FLT); }
/* Float#rationalize(eps=nil): simplest rational within eps (or within the
 * float's own rounding interval if no eps) of self. */
static RESULT korb_m_flt_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double f = SELF_FLT;
    if (UNLIKELY(!isfinite(f))) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "Infinity");
    int64_t n, d;
    if (VALUE_SLICE_LEN(a) >= 1) {
        double eps;
        if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &eps))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
        eps = fabs(eps);
        if (eps == 0.0) return korb_flt_to_rat(c, slots, f);          /* exact */
        if (!korb_rationalize_internal(f - eps, f + eps, &n, &d)) return korb_flt_to_rat(c, slots, f);
    } else {
        if (!korb_flt_simplest_roundtrip(f, &n, &d)) return korb_flt_to_rat(c, slots, f);
    }
    return korb_rat_new(c, slots, (intptr_t)n, (intptr_t)d);
}
static RESULT korb_m_flt_numerator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; RESULT r = korb_flt_to_rat(c, slots, SELF_FLT); if (r.state != KORB_NORMAL) return r; return RESULT_OK(LONG2FIX(VAL2RAT(r.value)->num));
}
static RESULT korb_m_flt_denominator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; RESULT r = korb_flt_to_rat(c, slots, SELF_FLT); if (r.state != KORB_NORMAL) return r; return RESULT_OK(LONG2FIX(VAL2RAT(r.value)->den));
}
static RESULT korb_m_flt_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_float_new(c, slots, fabs(SELF_FLT)); }
static RESULT korb_m_flt_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT == 0.0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_nan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(isnan(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_inf(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; double d = SELF_FLT; return RESULT_OK(isinf(d) ? LONG2FIX(d < 0 ? -1 : 1) : KORB_NIL); }
static RESULT korb_m_flt_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    double o; if (!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o)) return RESULT_OK(KORB_NIL);
    double s = SELF_FLT; return RESULT_OK(LONG2FIX((s > o) - (s < o)));
}
/* round/floor/ceil/truncate → Integer (kind 0=floor 1=ceil 2=round 3=trunc) */
static RESULT korb_flt_toint(CTX *c, VALUE *slots, double d, int kind) {
    double t = kind == 0 ? floor(d) : kind == 1 ? ceil(d) : kind == 2 ? round(d) : trunc(d);
    if (UNLIKELY(!isfinite(t) || t < (double)FIXNUM_MIN || t > (double)FIXNUM_MAX))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Float out of Fixnum range (Bignum not implemented)");
    return RESULT_OK(LONG2FIX((intptr_t)t));
}
/* round/floor/ceil/trunc with an optional digit count.
 * ndig>0 → Float to ndig decimals; ndig<=0 → Integer (rounded to 10^-ndig). */
static RESULT korb_flt_round_to(CTX *c, VALUE *slots, double d, int kind, VALUE_SLICE a) {
    intptr_t ndig = 0;
    if (VALUE_SLICE_LEN(a) >= 1) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &ndig))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (ndig == 0) return korb_flt_toint(c, slots, d, kind);
    double f = pow(10.0, (double)(ndig < 0 ? -ndig : ndig));
    double scaled = ndig > 0 ? d * f : d / f;
    double t = kind == 0 ? floor(scaled) : kind == 1 ? ceil(scaled) : kind == 2 ? round(scaled) : trunc(scaled);
    if (ndig > 0) return korb_float_new(c, slots, t / f);
    double res = t * f;
    if (UNLIKELY(!isfinite(res) || res < (double)FIXNUM_MIN || res > (double)FIXNUM_MAX))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Float out of Fixnum range (Bignum not implemented)");
    return RESULT_OK(LONG2FIX((intptr_t)res));
}
static RESULT korb_m_flt_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 3); }
static RESULT korb_m_flt_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 0, a); }
static RESULT korb_m_flt_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_flt_round_to(c, slots, SELF_FLT, 1, a); }
static RESULT korb_m_flt_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 2, a); }
static RESULT korb_m_flt_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 3, a); }
static RESULT korb_m_flt_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; char b[40]; uint32_t n = korb_float_to_s(SELF_FLT, b); return korb_str_new(c, slots, b, n);
}
static RESULT korb_m_flt_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, SELF_FLT / o);
}
static RESULT korb_m_flt_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return korb_flt_toint(c, slots, floor(SELF_FLT / o), 3);   /* floor → Integer */
}
static RESULT korb_m_flt_modulo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    double r = fmod(SELF_FLT, o);
    if (r != 0.0 && ((r < 0) != (o < 0))) r += o;             /* floored division remainder */
    return korb_float_new(c, slots, r);
}
static RESULT korb_m_flt_remainder(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, fmod(SELF_FLT, o));        /* C fmod: sign of dividend = remainder */
}
static RESULT korb_m_flt_finite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(isfinite(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)INFINITY)); }
static RESULT korb_m_flt_prev(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)-INFINITY)); }
static RESULT korb_m_flt_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    double s = SELF_FLT, q = floor(s / o);
    double r = fmod(s, o);                                /* fmod keeps -0.0 sign, matching CRuby */
    if (r != 0.0 && ((r < 0) != (o < 0))) r += o;        /* floored remainder (sign of divisor) */
    RESULT qr = korb_flt_toint(c, slots, q, 3);          /* quotient → Integer */
    if (UNLIKELY(qr.state != KORB_NORMAL)) return qr;
    slots[0] = qr.value;
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, r));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_flt_nonzero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT != 0.0 ? VALUE_REF_GET(self) : KORB_NIL); }
static RESULT korb_m_flt_neg_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT < 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_pos_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT > 0 ? KORB_TRUE : KORB_FALSE); }
/* coerce(other) → [Float(other), Float(self)] */
static RESULT korb_m_flt_coerce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    double s = SELF_FLT;
    slots[0] = UNWRAP(korb_float_new(c, slots, o));
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, s));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* Comparable#between?(min, max): short-circuits like CRuby — compare to min
 * first, return false if self < min before ever touching max (so a bad max with
 * self already below min is NOT an error). */
static RESULT korb_num_between(CTX *c, VALUE *slots, VALUE self, VALUE lo, VALUE hi) {
    int c1 = korb_cmp_full(c, self, lo);
    if (UNLIKELY(c1 == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(self), korb_type_name(lo));
    if (c1 < 0) return RESULT_OK(KORB_FALSE);
    int c2 = korb_cmp_full(c, self, hi);
    if (UNLIKELY(c2 == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed", korb_type_name(self), korb_type_name(hi));
    return RESULT_OK(c2 <= 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_num_between(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1));
}

/* Integer round/floor/ceil/truncate(ndigits). ndig>=0 → self; ndig<0 → snap to 10^-ndig.
 * kind: 0=floor 1=ceil 2=round 3=trunc */
static RESULT korb_int_round_to(CTX *c, VALUE *slots, intptr_t v, int kind, VALUE_SLICE a) {
    intptr_t ndig = 0;
    if (VALUE_SLICE_LEN(a) >= 1) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &ndig))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (ndig >= 0) return RESULT_OK(LONG2FIX(v));      /* no fractional digits in an Integer */
    intptr_t f = 1;
    for (intptr_t k = 0; k < -ndig; k++) {
        if (UNLIKELY(f > FIXNUM_MAX / 10)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of Fixnum range (Bignum not implemented)");
        f *= 10;
    }
    intptr_t q = v / f, r = v % f, res;
    switch (kind) {
      case 0:  res = (r != 0 && v < 0) ? (q - 1) * f : q * f; break;   /* floor */
      case 1:  res = (r != 0 && v > 0) ? (q + 1) * f : q * f; break;   /* ceil */
      case 3:  res = q * f; break;                                     /* truncate */
      default: {                                                       /* round half up */
        intptr_t ar = r < 0 ? -r : r;
        res = q * f;
        if (ar * 2 >= f) res += (v < 0 ? -f : f);
        break;
      }
    }
    return RESULT_OK(LONG2FIX(res));
}
static RESULT korb_m_int_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_int_round_to(c, slots, SELF_INT, 0, a); }
static RESULT korb_m_int_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { return korb_int_round_to(c, slots, SELF_INT, 1, a); }
static RESULT korb_m_int_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_int_round_to(c, slots, SELF_INT, 2, a); }
static RESULT korb_m_int_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_round_to(c, slots, SELF_INT, 3, a); }
static RESULT korb_m_int_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo, hi;
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* clamp(lo..hi) */
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        lo = r->rbegin; hi = r->rend;
    } else { lo = VALUE_SLICE_GET(a, 0); hi = VALUE_SLICE_GET(a, 1); }
    intptr_t n = SELF_INT;
    double lod, hid;
    if (lo != KORB_NIL) {                              /* nil bound = unbounded on that side */
        if (FIXNUM_P(lo)) { if (n < FIX2LONG(lo)) return RESULT_OK(lo); }
        else { if (UNLIKELY(!korb_num_to_d(lo, &lod))) return korb_raise(c, slots, KORB_E_TYPE, 0, "comparison failed"); if ((double)n < lod) return RESULT_OK(lo); }
    }
    if (hi != KORB_NIL) {
        if (FIXNUM_P(hi)) { if (n > FIX2LONG(hi)) return RESULT_OK(hi); }
        else { if (UNLIKELY(!korb_num_to_d(hi, &hid))) return korb_raise(c, slots, KORB_E_TYPE, 0, "comparison failed"); if ((double)n > hid) return RESULT_OK(hi); }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_int_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(8));   /* bytes in a machine word (Fixnum) */
}
static RESULT korb_m_int_bit_length(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    intptr_t n = FIX2LONG(VALUE_REF_GET(self));
    if (n < 0) n = ~n;                                 /* -n-1: bits of the two's-complement magnitude */
    intptr_t len = 0;
    while (n > 0) { len++; n >>= 1; }
    return RESULT_OK(LONG2FIX(len));
}
static RESULT korb_m_int_digits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t n = SELF_INT;
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of domain");
    intptr_t base = 10;
    if (VALUE_SLICE_LEN(a) >= 1) {
        if (!FIXNUM_P(VALUE_SLICE_GET(a, 0))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        base = FIX2LONG(VALUE_SLICE_GET(a, 0));
        if (base < 2) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %ld", (long)base);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    do {
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(n % base)));
        n /= base;
    } while (n > 0);
    return RESULT_OK(VALUE_REF_GET(dst));
}

