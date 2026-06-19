/* koruby_precise — integer.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Integer methods ----------------------------------------------------- */

static RESULT korb_m_int_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(selfv)) return mpz_sgn(VAL2BIG(selfv)->z) < 0 ? korb_big_neg(c, slots, selfv) : RESULT_OK(selfv);
#endif
    intptr_t n = FIX2LONG(selfv);
    if (n >= 0) return RESULT_OK(selfv);
#ifdef KORB_HAVE_GMP
    if (UNLIKELY(!FIXABLE(-n))) return korb_big_neg(c, slots, selfv);
#else
    if (UNLIKELY(!FIXABLE(-n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(-n));
}
/* unary minus `-@` — also the dispatch target node_neg deopts to when a basic
 * op has been redefined (so a redefined Integer#-@ is honored). */
static RESULT korb_m_int_uminus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(selfv)) return korb_big_neg(c, slots, selfv);
#endif
    intptr_t n = -FIX2LONG(selfv);
#ifdef KORB_HAVE_GMP
    if (UNLIKELY(!FIXABLE(n))) return korb_big_neg(c, slots, selfv);
#else
    if (UNLIKELY(!FIXABLE(n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(selfv)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 0, 0);
    intptr_t n; if (__builtin_add_overflow(FIX2LONG(selfv), (intptr_t)1, &n) || !FIXABLE(n)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 0, 0);
#else
    intptr_t n = SELF_INT + 1;
    if (UNLIKELY(!FIXABLE(n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_pred(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(selfv)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 1, 0);
    intptr_t n; if (__builtin_sub_overflow(FIX2LONG(selfv), (intptr_t)1, &n) || !FIXABLE(n)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 1, 0);
#else
    intptr_t n = SELF_INT - 1;
    if (UNLIKELY(!FIXABLE(n))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(FIXNUM_P(VALUE_REF_GET(self)) && FIX2LONG(VALUE_REF_GET(self)) == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_nonzero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((FIXNUM_P(VALUE_REF_GET(self)) && FIX2LONG(VALUE_REF_GET(self)) == 0) ? KORB_NIL : VALUE_REF_GET(self)); }
static RESULT korb_m_int_even(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) return RESULT_OK(mpz_even_p(VAL2BIG(v)->z) ? KORB_TRUE : KORB_FALSE);
#endif
    return RESULT_OK((FIX2LONG(v) & 1) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_odd (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) return RESULT_OK(mpz_odd_p(VAL2BIG(v)->z) ? KORB_TRUE : KORB_FALSE);
#endif
    return RESULT_OK((FIX2LONG(v) & 1) != 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_pos (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) return RESULT_OK(mpz_sgn(VAL2BIG(v)->z) > 0 ? KORB_TRUE : KORB_FALSE);
#endif
    return RESULT_OK(FIX2LONG(v) > 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_neg (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(v)) return RESULT_OK(mpz_sgn(VAL2BIG(v)->z) < 0 ? KORB_TRUE : KORB_FALSE);
#endif
    return RESULT_OK(FIX2LONG(v) < 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_true_lit (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
/* boolean logical ops (true & | ^ / false&nil share & | ^). */
static RESULT korb_m_bool_true_and(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self; VALUE o = VALUE_SLICE_GET(a, 0); return RESULT_OK((o != KORB_NIL && o != KORB_FALSE) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_bool_true_or(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_bool_true_xor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self; VALUE o = VALUE_SLICE_GET(a, 0); return RESULT_OK((o != KORB_NIL && o != KORB_FALSE) ? KORB_FALSE : KORB_TRUE); }
static RESULT korb_m_bool_false_and(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a){ (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static RESULT korb_m_bool_false_or(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self; VALUE o = VALUE_SLICE_GET(a, 0); return RESULT_OK((o != KORB_NIL && o != KORB_FALSE) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int base = 10;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE b = VALUE_SLICE_GET(a, 0);
        if (!FIXNUM_P(b)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(b));
        base = (int)FIX2LONG(b);
        if (base < 2 || base > 36) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid radix %d", base);
    }
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) {
        char *s = mpz_get_str(NULL, base, VAL2BIG(VALUE_REF_GET(self))->z);
        RESULT r = korb_str_new(c, slots, s, (uint32_t)strlen(s));
        free(s);
        return r;
    }
#endif
    char buf[80];
    uint32_t len = korb_fmt_int(SELF_INT, base, buf);
    return korb_str_new(c, slots, buf, len);
}
static RESULT korb_m_int_chr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = SELF_INT;
    if (n < 0 || n > 255) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of char range", (long)n);
    char ch = (char)n;
    RESULT r = korb_str_new(c, slots, &ch, 1);
    if (LIKELY(r.state == KORB_NORMAL)) ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_BINARY;   /* n.chr is ASCII-8BIT */
    return r;
}

/* floored integer division / modulo (Ruby semantics: quotient rounds toward
 * -inf, remainder takes the divisor's sign). */
static intptr_t korb_int_fdiv(intptr_t a, intptr_t b) {
    intptr_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static intptr_t korb_int_fmod(intptr_t a, intptr_t b) {
    intptr_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
static intptr_t korb_int_gcd(intptr_t a, intptr_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { intptr_t t = a % b; a = b; b = t; }
    return a;
}

static double korb_cospi(double x);
static double korb_sinpi(double x);
static RESULT korb_m_int_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE selfv = VALUE_REF_GET(self);
    VALUE ev = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(ev)) {                            /* Integer ** Float → Float (Complex if neg base) */
        double base; (void)korb_num_to_d(selfv, &base);
        double e = korb_float_val(ev);
        if (base < 0 && e != floor(e)) {
            double mag = pow(-base, e);
            slots[0] = UNWRAP(korb_float_new(c, slots, mag * korb_cospi(e)));
            slots[1] = UNWRAP(korb_float_new(c, slots + 1, mag * korb_sinpi(e)));
            return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
        }
        return korb_float_new(c, slots, pow(base, e));
    }
    if (UNLIKELY(!KORB_INTEGER_P(ev))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(ev));
#ifdef KORB_HAVE_GMP
    if (VALUE_SLICE_LEN(a) < 2 || !FIXNUM_P(selfv) || !FIXNUM_P(ev))   /* plain pow (incl. overflow/bignum) */
        return korb_int_pow(c, slots, selfv, ev, 0);
#endif
    intptr_t base = FIX2LONG(selfv);
    intptr_t exp = FIX2LONG(ev);
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* pow(exp, mod): modular exponentiation */
        VALUE mv = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!FIXNUM_P(mv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(mv));
        intptr_t mod = FIX2LONG(mv);
        if (UNLIKELY(mod == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        if (UNLIKELY(exp < 0)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "int.pow(n, m): n must be positive");
        intptr_t am = mod < 0 ? -mod : mod;
        __int128 bb = (((__int128)(base % am)) + am) % am, result = 1 % am;
        for (intptr_t e = exp; e > 0; e >>= 1) {
            if (e & 1) result = result * bb % am;
            bb = bb * bb % am;
        }
        intptr_t res = (intptr_t)result;
        if (mod < 0 && res != 0) res += mod;          /* floored result (sign of mod) */
        return RESULT_OK(LONG2FIX(res));
    }
    if (exp < 0) {                                    /* negative exponent → Rational(1, base^|exp|) */
        if (UNLIKELY(base == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        intptr_t p = 1;
        for (intptr_t i = 0; i < -exp; i++) {
            if (UNLIKELY(base != 0 && (p * base) / base != p)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
            p *= base;
            if (UNLIKELY(!FIXABLE(p))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
        }
        if (p == 1 || p == -1) return RESULT_OK(LONG2FIX(p));   /* base^|exp| == ±1 → Integer */
        return p < 0 ? korb_rat_new(c, slots, -1, -p) : korb_rat_new(c, slots, 1, p);
    }
    intptr_t r = 1;
    for (intptr_t i = 0; i < exp; i++) {
        if (UNLIKELY(base != 0 && (r * base) / base != r))   /* overflow */
            return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
        r *= base;
        if (UNLIKELY(!FIXABLE(r))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    }
    return RESULT_OK(LONG2FIX(r));
}

/* floored float modulo (sign follows divisor) — for Integer op Float. */
static RESULT korb_m_int_divmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(bv)) {                            /* Integer#divmod(Float) → [Integer floor div, Float mod] */
        double f = korb_float_val(bv), s; korb_num_to_d(VALUE_REF_GET(self), &s);
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        slots[0] = LONG2FIX((intptr_t)floor(s / f));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, korb_float_fmod(s, f)));
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        return RESULT_OK(slots[2]);
    }
    if (KORB_RATIONAL_P(bv)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), bv, 2);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (UNLIKELY(!FIXNUM_P(VALUE_REF_GET(self)) || !FIXNUM_P(bv)))   /* Bignum operand/self → GMP */
        return korb_int_intdiv(c, slots, VALUE_REF_GET(self), bv, 2);
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    intptr_t av = SELF_INT;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 2)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(korb_int_fdiv(av, b))));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(korb_int_fmod(av, b))));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_int_div(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(bv)) {                            /* Integer#div(Float) → floor(self/f) Integer */
        double f = korb_float_val(bv), s; korb_num_to_d(VALUE_REF_GET(self), &s);
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        return RESULT_OK(LONG2FIX((intptr_t)floor(s / f)));
    }
    if (KORB_RATIONAL_P(bv)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), bv, 0);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (UNLIKELY(!FIXNUM_P(VALUE_REF_GET(self)) || !FIXNUM_P(bv)))   /* Bignum operand/self → GMP */
        return korb_int_intdiv(c, slots, VALUE_REF_GET(self), bv, 0);
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(korb_int_fdiv(SELF_INT, b)));
}

static RESULT korb_m_int_modulo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(bv)) {                            /* Integer#modulo(Float) → Float (floored) */
        double f = korb_float_val(bv), s; korb_num_to_d(VALUE_REF_GET(self), &s);
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        return korb_float_new(c, slots, korb_float_fmod(s, f));
    }
    if (KORB_RATIONAL_P(bv)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), bv, 1);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (UNLIKELY(!FIXNUM_P(VALUE_REF_GET(self)) || !FIXNUM_P(bv)))   /* Bignum operand/self → GMP */
        return korb_int_intdiv(c, slots, VALUE_REF_GET(self), bv, 1);
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(korb_int_fmod(SELF_INT, b)));
}

static RESULT korb_m_int_gcd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    return RESULT_OK(LONG2FIX(korb_int_gcd(SELF_INT, FIX2LONG(bv))));
}

static RESULT korb_m_int_lcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t av = SELF_INT, b = FIX2LONG(bv);
    if (av == 0 || b == 0) return RESULT_OK(LONG2FIX(0));
    intptr_t g = korb_int_gcd(av, b);
    intptr_t l = (av / g) * b;
    if (l < 0) l = -l;
    if (UNLIKELY(!FIXABLE(l))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    return RESULT_OK(LONG2FIX(l));
}

static RESULT korb_m_int_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double o; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    return korb_float_new(c, slots, (double)SELF_INT / o);
}
static RESULT korb_m_int_ceildiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(bv)) {                            /* Integer#ceildiv(Float) → ceil(self/f) Integer */
        double f = korb_float_val(bv);
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        return RESULT_OK(LONG2FIX((intptr_t)ceil((double)SELF_INT / f)));
    }
    if (UNLIKELY(!FIXNUM_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(-korb_int_fdiv(-SELF_INT, b)));   /* ceil = -floor(-a/b) */
}
/* coerce(other) → [other, self] both as Integer, or both Float if other is Float */
static RESULT korb_m_int_coerce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    intptr_t s = SELF_INT;
    if (KORB_FLOAT_P(o)) {
        double od = korb_float_val(o);
        slots[0] = UNWRAP(korb_float_new(c, slots, od));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, (double)s));
    } else if (FIXNUM_P(o)) {
        slots[0] = o; slots[1] = LONG2FIX(s);
    } else {
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Integer", korb_type_name(o));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_int_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    VALUE selfv = VALUE_REF_GET(self), o = VALUE_SLICE_GET(a, 0);
#ifdef KORB_HAVE_GMP
    if (KORB_INTEGER_P(o)) return RESULT_OK(LONG2FIX(korb_int_cmp(selfv, o)));   /* exact */
#endif
    double y, x;
    if (!korb_num_to_d(o, &y)) return RESULT_OK(KORB_NIL);   /* incomparable → nil */
    (void)korb_num_to_d(selfv, &x);
    return RESULT_OK(LONG2FIX((x > y) - (x < y)));
}
static RESULT korb_m_int_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double x; (void)korb_num_to_d(VALUE_REF_GET(self), &x); return korb_float_new(c, slots, x);
}
static RESULT korb_m_int_quo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_rat_arith(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), 3);   /* exact division → Rational/Float */
}
static RESULT korb_m_int_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_rat_new(c, slots, SELF_INT, 1);
}
static RESULT korb_m_int_numerator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_int_denominator(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(1)); }
/* real-number helpers shared by Integer/Float/Rational: real=self, imaginary=0. */
static RESULT korb_m_num_real(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_num_imag(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(0)); }
/* angle/arg/phase of a real: 0 (Integer) if >= 0, Math::PI (Float) if < 0. */
static RESULT korb_m_num_angle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d;
    if (!korb_num_to_d(VALUE_REF_GET(self), &d)) return RESULT_OK(LONG2FIX(0));
    if (d < 0) return korb_float_new(c, slots, 3.141592653589793);
    return RESULT_OK(LONG2FIX(0));
}
static RESULT korb_m_num_real_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_lit_false(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static RESULT korb_m_lit_nil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_NIL); }
/* to_c on a real: Complex(self, 0). */
static RESULT korb_m_num_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_cpx_new(c, slots, VALUE_REF_GET(self), LONG2FIX(0)); }
/* polar of a real: [magnitude, angle]. magnitude=abs(self) (same class), angle per korb_m_num_angle. */
static RESULT korb_m_num_polar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d;
    VALUE sv = VALUE_REF_GET(self);
    if (!korb_num_to_d(sv, &d)) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    /* slots[0]=magnitude (abs, class-preserving), slots[1]=angle, slots[2]=result array */
    if (FIXNUM_P(sv)) { intptr_t n = FIX2LONG(sv); slots[0] = LONG2FIX(n < 0 ? -n : n); }
    else { slots[0] = UNWRAP(korb_float_new(c, slots, fabs(d))); }
    slots[1] = d < 0 ? UNWRAP(korb_float_new(c, slots + 1, 3.141592653589793)) : LONG2FIX(0);
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF arr = VALUE_REF_AT(slots + 2);
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}
/* rect/rectangular of a real: [self, 0]. */
static RESULT korb_m_num_rect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VALUE_REF_GET(self);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    VALUE_REF arr = VALUE_REF_AT(slots + 1);
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 2, arr, LONG2FIX(0)));
    return RESULT_OK(VALUE_REF_GET(arr));
}
/* decompose a finite double into exact num/den (reduced); den>0. */
static RESULT korb_flt_to_rat(CTX *c, VALUE *slots, double d) {
    if (UNLIKELY(!isfinite(d))) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "can't convert non-finite Float to Rational");
    if (d == 0.0) return korb_rat_new(c, slots, 0, 1);
    int e; double m = frexp(d, &e);                    /* d = m * 2^e, m in [0.5,1) */
    intptr_t mant = (intptr_t)ldexp(m, 53);            /* integer mantissa */
    e -= 53;
    if (e >= 0) {
        if (e >= 62) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Float magnitude too large for Rational (Bignum)");
        return korb_rat_new(c, slots, mant << e, 1);
    }
    if (-e >= 63) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Float too small for Rational (Bignum)");
    return korb_rat_new(c, slots, mant, (intptr_t)1 << (-e));   /* 1<<62 still fits int64 */
}

/* Simplest rational p/q in [a, b] (a <= b), CF-convergent search matching
 * CRuby's nurat_rationalize_internal.  Returns false on non-convergence. */
static bool korb_rationalize_internal(double a, double b, int64_t *restrict pn, int64_t *restrict pd) {
    int64_t p0 = 0, p1 = 1, q0 = 1, q1 = 0;
    for (int i = 0; i < 64; i++) {
        double c = ceil(a);
        if (!isfinite(c) || fabs(c) > 9.0e18) return false;
        if (c < b) { *pn = (int64_t)c * p1 + p0; *pd = (int64_t)c * q1 + q0; return *pd != 0; }
        int64_t k = (int64_t)c - 1;
        int64_t p2 = k * p1 + p0, q2 = k * q1 + q0;
        double bk = b - (double)k, ak = a - (double)k;
        if (ak == 0.0 || bk == 0.0) return false;
        double t = 1.0 / bk;
        b = 1.0 / ak;
        a = t;
        p0 = p1; p1 = p2; q0 = q1; q1 = q2;
    }
    return false;
}
/* Simplest fraction p/q that round-trips to f as a double (CF convergents of f,
 * first convergent reproducing f exactly).  Used by no-arg Float#rationalize. */
static bool korb_flt_simplest_roundtrip(double f, int64_t *restrict pn, int64_t *restrict pd) {
    double af = fabs(f), x = af;
    int64_t hm1 = 1, hm2 = 0, km1 = 0, km2 = 1;
    for (int i = 0; i < 64; i++) {
        double fl = floor(x);
        if (fabs(fl) > 9.0e18) return false;
        int64_t a = (int64_t)fl;
        if (hm1 != 0 && fabs((double)a * (double)hm1) > 9.0e18) return false;
        int64_t h = a * hm1 + hm2, k = a * km1 + km2;
        if (k != 0 && (double)h / (double)k == af) { *pn = f < 0 ? -h : h; *pd = k; return true; }
        double frac = x - fl;
        if (frac == 0.0) { *pn = f < 0 ? -h : h; *pd = (k != 0 ? k : 1); return true; }
        x = 1.0 / frac;
        hm2 = hm1; hm1 = h; km2 = km1; km1 = k;
    }
    return false;
}
