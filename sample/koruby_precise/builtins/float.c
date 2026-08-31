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
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", VALUE_SLICE_LEN(a));
    double f = SELF_FLT;
    if (UNLIKELY(!isfinite(f)))
        return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, "%s", isnan(f) ? "NaN" : (f < 0 ? "-Infinity" : "Infinity"));
    int64_t n, d;
    if (VALUE_SLICE_LEN(a) >= 1) {
        double eps;
        if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &eps))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
        eps = fabs(eps);
        if (eps == 0.0) return korb_flt_to_rat(c, slots, f);          /* exact */
        /* the ceil-based CF needs a positive interval → rationalize |f| and
         * negate the numerator (matches CRuby float_rationalize). */
        const bool neg = f < 0.0;
        const double af = neg ? -f : f;
        if (!korb_rationalize_internal(af - eps, af + eps, &n, &d)) return korb_flt_to_rat(c, slots, f);
        if (neg) n = -n;
    } else {
        if (!korb_flt_simplest_roundtrip(f, &n, &d)) return korb_flt_to_rat(c, slots, f);
    }
    return korb_rat_new(c, slots, (korb_sword_t)n, (korb_sword_t)d);
}
/* Rational#rationalize(eps=nil): the simplest rational within eps of self (no
 * eps → self unchanged); >1 arg → ArgumentError. */
static RESULT korb_m_rat_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", VALUE_SLICE_LEN(a));
    if (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_GET(a, 0) == KORB_NIL) return RESULT_OK(VALUE_REF_GET(self));
    double f = 0, eps = 0;
    korb_num_to_d(VALUE_REF_GET(self), &f);
    if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &eps))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    eps = fabs(eps);
    if (eps == 0.0) return RESULT_OK(VALUE_REF_GET(self));
    const bool neg = f < 0.0; const double af = neg ? -f : f;
    int64_t n, d;
    if (!korb_rationalize_internal(af - eps, af + eps, &n, &d)) return RESULT_OK(VALUE_REF_GET(self));
    if (neg) n = -n;
    return korb_rat_new(c, slots, (korb_sword_t)n, (korb_sword_t)d);
}
static RESULT korb_m_flt_numerator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; if (UNLIKELY(!isfinite(SELF_FLT))) return RESULT_OK(VALUE_REF_GET(self));   /* NaN/±Infinity → self (CRuby) */
    RESULT r = korb_flt_to_rat(c, slots, SELF_FLT); if (r.state != KORB_NORMAL) return r; return RESULT_OK(VAL2RAT(r.value)->num);
}
static RESULT korb_m_flt_denominator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; if (UNLIKELY(!isfinite(SELF_FLT))) return RESULT_OK(LONG2FIX(1));           /* NaN/±Infinity → 1 (CRuby) */
    RESULT r = korb_flt_to_rat(c, slots, SELF_FLT); if (r.state != KORB_NORMAL) return r; return RESULT_OK(VAL2RAT(r.value)->den);
}
static RESULT korb_m_flt_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_float_new(c, slots, fabs(SELF_FLT)); }
static RESULT korb_m_flt_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_FLT == 0.0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_nan(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(isnan(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_inf(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; double d = SELF_FLT; return RESULT_OK(isinf(d) ? LONG2FIX(d < 0 ? -1 : 1) : KORB_NIL); }
static RESULT korb_m_flt_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE ov = VALUE_SLICE_GET(a, 0);
    double o;
    if (!korb_num_to_d(ov, &o)) {                            /* coercible object → a, b = o.coerce(self); a <=> b */
        const double si = SELF_FLT;
        if (isinf(si) && KORB_OBJECT_P(ov)) {                /* ±Inf <=> obj#infinite? (1=+Inf, -1=-Inf, nil=finite) */
            const uint32_t inf_mid = korb_intern(c->vm, "infinite?", 9);
            if (korb_responds_to(c, ov, inf_mid)) {
                slots[0] = ov;
                RESULT ir = korb_send_impl(c, slots + 1, inf_mid, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
                const int self_level = si > 0 ? 1 : -1;
                const int other_level = (ir.value == LONG2FIX(1)) ? 1 : (ir.value == LONG2FIX(-1)) ? -1 : 0;
                return RESULT_OK(LONG2FIX((self_level > other_level) - (self_level < other_level)));
            }
        }
        if (KORB_OBJECT_P(ov)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ov, "<=>", 0, &h); if (h) return cr; }
        return RESULT_OK(KORB_NIL);
    }
    double s = SELF_FLT;
    if (UNLIKELY(isnan(s) || isnan(o))) return RESULT_OK(KORB_NIL);   /* NaN is unordered */
    if (KORB_BIGNUM_P(ov)) {                                 /* Float <=> Bignum: exact (invert Integer<=>Float) */
        const int cmp = korb_big_flo_cmp(ov, s);
        return RESULT_OK(cmp == 2 ? KORB_NIL : LONG2FIX(-cmp));
    }
    return RESULT_OK(LONG2FIX((s > o) - (s < o)));
}
/* An integer-valued double → Fixnum, or Bignum when it exceeds the Fixnum range. */
static RESULT korb_flt_int_result(CTX *c, VALUE *slots, double t) {
    if (LIKELY(t >= (double)FIXNUM_MIN && t <= (double)FIXNUM_MAX)) return RESULT_OK(LONG2FIX((korb_sword_t)t));
    korb_mp_t z; korb_mp_init(z); korb_mp_set_d(z, t);            /* t is already integer-valued (floor/ceil/round/trunc) */
    RESULT r = korb_big_from_mpz(c, slots, z);
    korb_mp_clear(z);
    return r;
}
/* round/floor/ceil/truncate → Integer (kind 0=floor 1=ceil 2=round 3=trunc) */
static RESULT korb_flt_toint(CTX *c, VALUE *slots, double d, int kind) {
    if (UNLIKELY(!isfinite(d)))                          /* Infinity / NaN → Integer is out of domain */
        return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, isnan(d) ? "NaN" : (d < 0 ? "-Infinity" : "Infinity"));
    double t = kind == 0 ? floor(d) : kind == 1 ? ceil(d) : kind == 2 ? round(d) : trunc(d);
    return korb_flt_int_result(c, slots, t);
}
/* round/floor/ceil/trunc with an optional digit count.
 * ndig>0 → Float to ndig decimals; ndig<=0 → Integer (rounded to 10^-ndig). */
/* round()'s `half:` keyword → 0=up (default, ties away from zero), 1=even
 * (banker's), 2=down (ties toward zero).  *npos = positional arg count with any
 * trailing keyword Hash excluded. */
/* npos = positional count (excl. trailing kwargs Hash).  *bad ← the invalid
 * `half:` value if any (else KORB_NIL), for the caller to raise ArgumentError. */
static int korb_round_half_v(CTX *c, VALUE_SLICE a, uint32_t *npos, VALUE *bad) {
    *bad = KORB_NIL;
    const uint32_t n = VALUE_SLICE_LEN(a);
    *npos = n;
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {     /* trailing kwargs Hash */
        *npos = n - 1;
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t idx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "half", 4)));
        if (idx >= 0) {
            const VALUE hv = korb_items_data(h->items)[2 * idx + 1];
            const char *nm = SYMBOL_P(hv) ? korb_sym_name(c->vm, SYM2ID(hv))
                           : (KORB_STRING_P(hv) ? korb_strbuf_data(VAL2STR(hv)->buf) : NULL);
            if (nm) {
                if (!strcmp(nm, "even")) return 1;
                if (!strcmp(nm, "down")) return 2;
                if (!strcmp(nm, "up"))   return 0;
                *bad = hv;                                     /* unknown mode name → ArgumentError */
            } else if (hv != KORB_NIL) *bad = hv;              /* non-Symbol/String → ArgumentError */
        }
    }
    return 0;
}
static int korb_round_half(CTX *c, VALUE_SLICE a, uint32_t *npos) { VALUE bad; return korb_round_half_v(c, a, npos, &bad); }
#define KORB_ROUND_CHECK_HALF(c, slots, a, npos) do { \
    VALUE _bad; (void)korb_round_half_v((c), (a), (npos), &_bad); \
    if (UNLIKELY(_bad != KORB_NIL)) return korb_raise((c), (slots), KORB_E_ARGUMENT, 0, "invalid rounding mode: %s", \
        SYMBOL_P(_bad) ? korb_sym_name((c)->vm, SYM2ID(_bad))                                  \
        : (KORB_STRING_P(_bad) ? korb_strbuf_data(VAL2STR(_bad)->buf) : korb_type_name(_bad))); \
} while (0)
/* round v to an integer-valued double under the given half mode. */
static double korb_round_half_apply(double v, int half) {
    if (half == 1) return nearbyint(v);                        /* :even (FP default mode = round-half-even) */
    if (half == 2) { double tr = trunc(v), fr = v - tr; return (fabs(fr) > 0.5) ? tr + (v < 0 ? -1.0 : 1.0) : tr; }
    return round(v);                                           /* :up — ties away from zero */
}
static RESULT korb_flt_round_to(CTX *c, VALUE *slots, double d, int kind, VALUE_SLICE a) {
    uint32_t npos; const int half = korb_round_half(c, a, &npos); KORB_ROUND_CHECK_HALF(c, slots, a, &npos);
    korb_sword_t ndig = 0;
    if (npos >= 1) {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &ndig))) return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
    }
    if (ndig <= 0 && UNLIKELY(!isfinite(d)))                    /* Inf/NaN → Integer-returning form is out of domain */
        return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, isnan(d) ? "NaN" : (d < 0 ? "-Infinity" : "Infinity"));
    if (ndig == 0) {
        if (kind == 2 && half != 0) {                          /* round to integer with explicit half mode */
            const double t = korb_round_half_apply(d, half);
            return korb_flt_int_result(c, slots, t);
        }
        return korb_flt_toint(c, slots, d, kind);
    }
    double f = pow(10.0, (double)(ndig < 0 ? -ndig : ndig));
    if (ndig > 0 && (isinf(f) || !isfinite(d * f))) return korb_float_new(c, slots, d);   /* beyond float precision → self */
    double scaled = ndig > 0 ? d * f : d / f;
    double t = kind == 0 ? floor(scaled) : kind == 1 ? ceil(scaled) : kind == 2 ? korb_round_half_apply(scaled, half) : trunc(scaled);
    if (ndig > 0) return korb_float_new(c, slots, t / f);
    double res = t * f;
    return korb_flt_int_result(c, slots, res);
}
static RESULT korb_m_flt_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)a; return korb_flt_toint(c, slots, SELF_FLT, 3); }
static RESULT korb_m_flt_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 0, a); }
static RESULT korb_m_flt_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_flt_round_to(c, slots, SELF_FLT, 1, a); }
static RESULT korb_m_flt_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 2, a); }
static RESULT korb_m_flt_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_flt_round_to(c, slots, SELF_FLT, 3, a); }
static RESULT korb_m_flt_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; char b[40]; uint32_t n = korb_float_to_s(SELF_FLT, b);
    VALUE s = UNWRAP(korb_str_new(c, slots, b, n));   /* CRuby: always US-ASCII */
    KORB_STR_ENC_SET(s, KORB_ENC_USASCII);
    return RESULT_OK(s);
}
static RESULT korb_m_flt_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE arg = VALUE_SLICE_GET(a, 0);
    if (KORB_COMPLEX_P(arg)) {                          /* self / (cr+ci·i) → Complex (Float components) */
        const double s = SELF_FLT;
        double cr, ci;
        if (UNLIKELY(!korb_num_to_d(VAL2CPX(arg)->re, &cr) || !korb_num_to_d(VAL2CPX(arg)->im, &ci)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "Complex can't be coerced into Float");
        const double den = cr * cr + ci * ci;            /* (s+0i)/(cr+ci·i); full formula keeps signed-zero parity with CRuby */
        slots[0] = UNWRAP(korb_float_new(c, slots, (s * cr + 0.0 * ci) / den));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, (0.0 * cr - s * ci) / den));
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    double o; if (UNLIKELY(!korb_num_to_d(arg, &o))) {
        if (KORB_OBJECT_P(arg)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), arg, "fdiv", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, arg));
    }
    return korb_float_new(c, slots, SELF_FLT / o);
}
static RESULT korb_m_flt_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) {
        const VALUE ov = VALUE_SLICE_GET(a, 0);
        if (KORB_OBJECT_P(ov)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ov, "div", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, ov));
    }
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return korb_flt_toint(c, slots, floor(SELF_FLT / o), 3);   /* floor → Integer */
}
static RESULT korb_m_flt_modulo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) {
        const VALUE ov = VALUE_SLICE_GET(a, 0);
        if (KORB_OBJECT_P(ov)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ov, "%", 0, &h); if (h) return cr; }   /* obj#coerce → a % b */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, ov));
    }
    if (UNLIKELY(o == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    double r = fmod(SELF_FLT, o);
    if (r != 0.0 && ((r < 0) != (o < 0))) r += o;             /* floored division remainder */
    return korb_float_new(c, slots, r);
}
static RESULT korb_m_flt_remainder(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) {
        const VALUE ov = VALUE_SLICE_GET(a, 0);
        if (KORB_OBJECT_P(ov)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ov, "remainder", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, ov));
    }
    /* CRuby Numeric#remainder: z = self % o (floored), then z - o when the signs
     * of self and o differ.  This reconstruction (not a plain fmod) reproduces
     * CRuby's exact rounding, e.g. 0.333.remainder(-1) == 0.33299999999999996. */
    const double x = SELF_FLT;
    double z = fmod(x, o);
    if (z != 0.0 && ((z < 0) != (o < 0))) z += o;             /* → floored mod */
    if (z != 0.0 && ((x < 0) != (o < 0))) z -= o;             /* differ-sign adjust back to dividend-signed */
    return korb_float_new(c, slots, z);
}
static RESULT korb_m_flt_finite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(isfinite(SELF_FLT) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_flt_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)INFINITY)); }
static RESULT korb_m_flt_prev(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_float_new(c, slots, nextafter(SELF_FLT, (double)-INFINITY)); }
static RESULT korb_m_flt_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) {
        const VALUE ov = VALUE_SLICE_GET(a, 0);
        if (KORB_OBJECT_P(ov)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ov, "divmod", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, ov));
    }
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
    double o;
    if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) {
        /* CRuby coerces via Float(): a numeric String parses, else TypeError. */
        if (KORB_STRING_P(VALUE_SLICE_GET(a, 0))) {
            RESULT fr = korb_bi_float(c, slots, a);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            o = korb_float_val(fr.value);
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
    }
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
static RESULT korb_int_round_to(CTX *c, VALUE *slots, korb_sword_t v, int kind, VALUE_SLICE a) {
    uint32_t npos; const int half = korb_round_half(c, a, &npos); KORB_ROUND_CHECK_HALF(c, slots, a, &npos);
    korb_sword_t ndig = 0;
    if (npos >= 1) {
        VALUE dv = VALUE_SLICE_GET(a, 0);
        if (KORB_FLOAT_P(dv)) {                        /* a non-finite Float digits count is out of range */
            const double d = korb_float_val(dv);
            if (UNLIKELY(isinf(d) || isnan(d))) return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", isnan(d) ? "NaN" : "Infinity");
            ndig = (korb_sword_t)d;
        } else if (UNLIKELY(KORB_BIGNUM_P(dv))) {      /* a Bignum digits count can't fit */
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        } else if (UNLIKELY(!korb_to_index(dv, &ndig))) {
            RESULT cr = korb_coerce_to_int(c, slots, &dv);   /* coerce via #to_int */
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(dv, &ndig)) return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
        }
    }
    if (UNLIKELY(ndig > INT32_MAX || ndig < INT32_MIN)) /* ndigits must fit in a C int (CRuby) */
        return korb_raise(c, slots, KORB_E_RANGE, 0, "integer %ld too big to convert to `int'", (long)ndig);
    if (ndig >= 0) return RESULT_OK(LONG2FIX(v));      /* no fractional digits in an Integer */
    korb_sword_t f = 1;
    for (korb_sword_t k = 0; k < -ndig; k++) {
        if (UNLIKELY(f > FIXNUM_MAX / 10)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of Fixnum range (Bignum not implemented)");
        f *= 10;
    }
    korb_sword_t q = v / f, r = v % f, res;
    switch (kind) {
      case 0:  res = (r != 0 && v < 0) ? (q - 1) * f : q * f; break;   /* floor */
      case 1:  res = (r != 0 && v > 0) ? (q + 1) * f : q * f; break;   /* ceil */
      case 3:  res = q * f; break;                                     /* truncate */
      default: {                                                       /* round (half: mode) */
        korb_sword_t ar = r < 0 ? -r : r;
        res = q * f;
        const korb_sword_t twice = ar * 2;
        if (twice > f) res += (v < 0 ? -f : f);                        /* clear majority → away from zero */
        else if (twice == f) {                                         /* exact tie */
            if (half == 1) { if (q & 1) res += (v < 0 ? -f : f); }     /* :even → nearest even multiple */
            else if (half != 2) res += (v < 0 ? -f : f);              /* :up (default); :down keeps q (toward zero) */
        }
        break;
      }
    }
    return RESULT_OK(LONG2FIX(res));
}
/* round/floor/ceil/truncate for a Bignum receiver with a negative digit count —
 * done in GMP so it doesn't overflow the korb_sword_t fixnum path (kind: 0 floor,
 * 1 ceil, 2 round, 3 truncate). */
static RESULT korb_bigint_round_to(CTX *c, VALUE *slots, VALUE bigself, int kind, VALUE_SLICE a) {
    slots[0] = bigself;                                  /* root across a possible #to_int digit coercion */
    uint32_t npos; const int half = korb_round_half(c, a, &npos); KORB_ROUND_CHECK_HALF(c, slots, a, &npos);
    korb_sword_t ndig = 0;
    if (npos >= 1) {
        VALUE dv = VALUE_SLICE_GET(a, 0);
        if (KORB_FLOAT_P(dv)) {
            const double d = korb_float_val(dv);
            if (UNLIKELY(isinf(d) || isnan(d))) return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", isnan(d) ? "NaN" : "Infinity");
            ndig = (korb_sword_t)d;
        } else if (UNLIKELY(KORB_BIGNUM_P(dv))) {
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        } else if (UNLIKELY(!korb_to_index(dv, &ndig))) {
            RESULT cr = korb_coerce_to_int(c, slots + 1, &dv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(dv, &ndig)) return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
        }
    }
    if (ndig >= 0) return RESULT_OK(slots[0]);            /* an Integer has no fractional digits */
    korb_mp_t z, f, q, r, res;
    korb_to_mpz(slots[0], z);
    korb_mp_init(f); korb_mp_ui_pow_ui(f, 10, (unsigned long)(-ndig));
    korb_mp_init(q); korb_mp_init(r); korb_mp_init(res);
    korb_mp_tdiv_qr(q, r, z, f);                              /* truncated: q toward 0, r has sign of z */
    const int rsgn = korb_mp_sgn(r), zsgn = korb_mp_sgn(z);
    switch (kind) {
      case 0:  if (rsgn != 0 && zsgn < 0) korb_mp_sub_ui(q, q, 1); korb_mp_mul(res, q, f); break;   /* floor */
      case 1:  if (rsgn != 0 && zsgn > 0) korb_mp_add_ui(q, q, 1); korb_mp_mul(res, q, f); break;   /* ceil  */
      case 3:  korb_mp_mul(res, q, f); break;                                                   /* truncate */
      default: {                                                                            /* round */
        korb_mp_mul(res, q, f);
        korb_mp_t ar; korb_mp_init(ar); korb_mp_abs(ar, r); korb_mp_mul_ui(ar, ar, 2);   /* twice = 2|r| */
        const int cmp = korb_mp_cmp(ar, f);
        bool away = false;
        if (cmp > 0) away = true;                                        /* clear majority */
        else if (cmp == 0) away = (half == 1) ? (korb_mp_odd_p(q) != 0) : (half != 2);   /* tie: even/up/down */
        if (away) { if (zsgn < 0) korb_mp_sub(res, res, f); else korb_mp_add(res, res, f); }
        korb_mp_clear(ar);
        break;
      }
    }
    RESULT out = korb_big_from_mpz(c, slots + 1, res);
    korb_mp_clear(z); korb_mp_clear(f); korb_mp_clear(q); korb_mp_clear(r); korb_mp_clear(res);
    return out;
}
static RESULT korb_m_int_floor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return KORB_BIGNUM_P(VALUE_REF_GET(self)) ? korb_bigint_round_to(c, slots, VALUE_REF_GET(self), 0, a) : korb_int_round_to(c, slots, SELF_INT, 0, a); }
static RESULT korb_m_int_ceil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { return KORB_BIGNUM_P(VALUE_REF_GET(self)) ? korb_bigint_round_to(c, slots, VALUE_REF_GET(self), 1, a) : korb_int_round_to(c, slots, SELF_INT, 1, a); }
static RESULT korb_m_int_round(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return KORB_BIGNUM_P(VALUE_REF_GET(self)) ? korb_bigint_round_to(c, slots, VALUE_REF_GET(self), 2, a) : korb_int_round_to(c, slots, SELF_INT, 2, a); }
static RESULT korb_m_int_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return KORB_BIGNUM_P(VALUE_REF_GET(self)) ? korb_bigint_round_to(c, slots, VALUE_REF_GET(self), 3, a) : korb_int_round_to(c, slots, SELF_INT, 3, a); }
static RESULT korb_m_int_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE lo, hi;
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* clamp(lo..hi) */
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        if (UNLIKELY(r->exclude_end)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot clamp with an exclusive range");
        lo = r->rbegin; hi = r->rend;
    } else { lo = VALUE_SLICE_GET(a, 0); hi = VALUE_SLICE_GET(a, 1); }
    if (lo != KORB_NIL && hi != KORB_NIL) {            /* CRuby: min must be <= max */
        bool bad;
        if (FIXNUM_P(lo) && FIXNUM_P(hi)) bad = FIX2LONG(lo) > FIX2LONG(hi);
        else { double al, ah; bad = korb_num_to_d(lo, &al) && korb_num_to_d(hi, &ah) && al > ah; }
        if (UNLIKELY(bad)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "min argument must be less than or equal to max argument");
    }
    korb_sword_t n = SELF_INT;
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
    (void)c;(void)slots;(void)a;
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) {              /* Bignum: bytes needed to hold |self| */
        korb_mp_t z; korb_to_mpz(VALUE_REF_GET(self), z);
        const size_t bytes = (korb_mp_sizeinbase(z, 2) + 7) / 8;
        korb_mp_clear(z);
        return RESULT_OK(LONG2FIX((korb_sword_t)bytes));
    }
    return RESULT_OK(LONG2FIX(8));   /* a Fixnum occupies a machine word */
}
static RESULT korb_m_int_bit_length(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) {
        korb_mp_t z; korb_to_mpz(VALUE_REF_GET(self), z);
        if (korb_mp_sgn(z) < 0) korb_mp_com(z, z);             /* ~z = -z-1: two's-complement magnitude */
        const size_t len = (korb_mp_sgn(z) == 0) ? 0 : korb_mp_sizeinbase(z, 2);
        korb_mp_clear(z);
        return RESULT_OK(LONG2FIX((korb_sword_t)len));
    }
    korb_sword_t n = FIX2LONG(VALUE_REF_GET(self));
    if (n < 0) n = ~n;                                 /* -n-1: bits of the two's-complement magnitude */
    korb_sword_t len = 0;
    while (n > 0) { len++; n >>= 1; }
    return RESULT_OK(LONG2FIX(len));
}
static RESULT korb_m_int_digits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    korb_sword_t base = 10;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE bv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(bv, &base))) {       /* coerce the radix via #to_int (before self is read) */
            RESULT cr = korb_coerce_to_int(c, slots, &bv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(bv, &base)) return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
        }
        if (base < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative radix");
        if (base < 2) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %ld", (long)base);
    }
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) {
        korb_mp_t z; korb_to_mpz(VALUE_REF_GET(self), z);   /* GMP copy: independent of the GC heap, survives the allocs below */
        if (korb_mp_sgn(z) < 0) { korb_mp_clear(z); return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "out of domain"); }
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 8)));
        if (korb_mp_sgn(z) == 0) {
            CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(0)));
        } else {
            while (korb_mp_sgn(z) > 0) {
                const unsigned long d = korb_mp_fdiv_q_ui(z, z, (unsigned long)base);   /* z = z/base, returns z%base */
                CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX((korb_sword_t)d)));
            }
        }
        korb_mp_clear(z);
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    korb_sword_t n = SELF_INT;
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "out of domain");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    do {
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(n % base)));
        n /= base;
    } while (n > 0);
    return RESULT_OK(VALUE_REF_GET(dst));
}

