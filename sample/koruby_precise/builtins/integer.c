/* koruby_precise — integer.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Integer methods ----------------------------------------------------- */
static RESULT korb_flt_toint(CTX *c, VALUE *slots, double d, int kind);   /* fwd (float.c) — Fixnum-or-Bignum from an integer-valued double */

static RESULT korb_m_int_abs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(selfv)) return korb_mp_sgn(VAL2BIG(selfv)->z) < 0 ? korb_big_neg(c, slots, selfv) : RESULT_OK(selfv);
    intptr_t n = FIX2LONG(selfv);
    if (n >= 0) return RESULT_OK(selfv);
    if (UNLIKELY(!FIXABLE(-n))) return korb_big_neg(c, slots, selfv);
    return RESULT_OK(LONG2FIX(-n));
}
/* unary minus `-@` — also the dispatch target node_neg deopts to when a basic
 * op has been redefined (so a redefined Integer#-@ is honored). */
static RESULT korb_m_int_uminus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(selfv)) return korb_big_neg(c, slots, selfv);
    intptr_t n = -FIX2LONG(selfv);
    if (UNLIKELY(!FIXABLE(n))) return korb_big_neg(c, slots, selfv);
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_succ(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(selfv)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 0, 0);
    intptr_t n; if (__builtin_add_overflow(FIX2LONG(selfv), (intptr_t)1, &n) || !FIXABLE(n)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 0, 0);
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_pred(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE selfv = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(selfv)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 1, 0);
    intptr_t n; if (__builtin_sub_overflow(FIX2LONG(selfv), (intptr_t)1, &n) || !FIXABLE(n)) return korb_int_arith(c, slots, selfv, LONG2FIX(1), 1, 0);
    return RESULT_OK(LONG2FIX(n));
}
static RESULT korb_m_int_zero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(FIXNUM_P(VALUE_REF_GET(self)) && FIX2LONG(VALUE_REF_GET(self)) == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_int_nonzero(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK((FIXNUM_P(VALUE_REF_GET(self)) && FIX2LONG(VALUE_REF_GET(self)) == 0) ? KORB_NIL : VALUE_REF_GET(self)); }
static RESULT korb_m_int_even(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(v)) return RESULT_OK(korb_mp_even_p(VAL2BIG(v)->z) ? KORB_TRUE : KORB_FALSE);
    return RESULT_OK((FIX2LONG(v) & 1) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_odd (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(v)) return RESULT_OK(korb_mp_odd_p(VAL2BIG(v)->z) ? KORB_TRUE : KORB_FALSE);
    return RESULT_OK((FIX2LONG(v) & 1) != 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_pos (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(v)) return RESULT_OK(korb_mp_sgn(VAL2BIG(v)->z) > 0 ? KORB_TRUE : KORB_FALSE);
    return RESULT_OK(FIX2LONG(v) > 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_neg (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (KORB_BIGNUM_P(v)) return RESULT_OK(korb_mp_sgn(VAL2BIG(v)->z) < 0 ? KORB_TRUE : KORB_FALSE);
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
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) {
        char *s = korb_mp_get_str(NULL, base, VAL2BIG(VALUE_REF_GET(self))->z);
        RESULT r = korb_str_new(c, slots, s, (uint32_t)strlen(s));
        free(s);
        return r;
    }
    char buf[80];
    uint32_t len = korb_fmt_int(SELF_INT, base, buf);
    return korb_str_new(c, slots, buf, len);
}
static RESULT korb_m_int_chr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const intptr_t n = SELF_INT;
    /* optional Encoding arg: US-ASCII (0..127 byte), ASCII-8BIT/BINARY (0..255
     * byte), UTF-8 (codepoint → 1..4 UTF-8 bytes).  No arg → ASCII-8BIT byte. */
    int kind = 0;   /* 0 = ascii-8bit byte, 1 = us-ascii byte, 2 = utf-8 */
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);
        RESULT nr;
        if (KORB_STRING_P(slots[0])) {                /* encoding name given as a String ("utf-8") */
            nr = RESULT_OK(slots[0]);
        } else {                                       /* Encoding object → query its #name */
            nr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "name", 4), 0, 0, NULL, NULL, NULL);
        }
        if (nr.state == KORB_NORMAL && KORB_STRING_P(nr.value)) {
            const KorbString *nm = VAL2STR(nr.value);
            #define ENC_IS(lit) (nm->len == sizeof(lit) - 1 && memcmp(korb_strbuf_data(nm->buf), lit, nm->len) == 0)
            if (ENC_IS("UTF-8")) kind = 2;
            else if (ENC_IS("US-ASCII") || ENC_IS("ASCII")) kind = 1;
            else if (ENC_IS("ASCII-8BIT") || ENC_IS("BINARY")) kind = 0;
            else kind = 2;   /* other ascii-compatible → treat like UTF-8 for the codepoint */
            #undef ENC_IS
        }
    }
    if (kind == 2) {                                  /* UTF-8 encode the codepoint */
        if (n < 0 || n > 0x10FFFF) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of char range", (long)n);
        char b[4]; int len;
        const uint32_t cp = (uint32_t)n;
        if (cp < 0x80)        { b[0] = (char)cp; len = 1; }
        else if (cp < 0x800)  { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
        else if (cp < 0x10000){ b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
        else                  { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
        return korb_str_new(c, slots, b, (uint32_t)len);   /* UTF-8 string (not binary) */
    }
    const intptr_t hi = (kind == 1) ? 127 : 255;
    if (n < 0 || n > hi) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of char range", (long)n);
    char ch = (char)n;
    RESULT r = korb_str_new(c, slots, &ch, 1);
    if (LIKELY(r.state == KORB_NORMAL)) {
        /* With an explicit Encoding the result carries it (US-ASCII / ASCII-8BIT);
         * with no argument CRuby picks US-ASCII for 0..127, else ASCII-8BIT. */
        const uint32_t enc = (VALUE_SLICE_LEN(a) >= 1)
            ? ((kind == 1) ? KORB_ENC_USASCII : KORB_ENC_BINARY)
            : ((n < 0x80) ? KORB_ENC_USASCII : KORB_ENC_BINARY);
        KORB_STR_ENC_SET(r.value, enc);
    }
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
    if (KORB_RATIONAL_P(ev)) {                         /* Integer ** Rational */
        if (VAL2RAT(ev)->den == LONG2FIX(1)) {         /* integer-valued exponent → Rational result (CRuby) */
            const RESULT ip = korb_int_pow(c, slots, selfv, VAL2RAT(ev)->num, 0);
            if (UNLIKELY(ip.state != KORB_NORMAL)) return ip;
            if (FIXNUM_P(ip.value)) return korb_rat_new(c, slots, FIX2LONG(ip.value), 1);   /* n → (n/1) */
            return ip;                                 /* already Rational (neg exp) or Bignum */
        }
        double base; (void)korb_num_to_d(selfv, &base); /* fractional → Float (Complex if neg base) */
        double e; (void)korb_num_to_d(ev, &e);
        if (base < 0) {
            double mag = pow(-base, e);
            slots[0] = UNWRAP(korb_float_new(c, slots, mag * korb_cospi(e)));
            slots[1] = UNWRAP(korb_float_new(c, slots + 1, mag * korb_sinpi(e)));
            return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
        }
        return korb_float_new(c, slots, pow(base, e));
    }
    if (UNLIKELY(!KORB_INTEGER_P(ev))) {
        if (KORB_OBJECT_P(ev)) { bool h; RESULT cr = korb_try_coerce(c, slots, selfv, ev, "**", 0, &h); if (h) return cr; }   /* obj#coerce → a ** b */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(ev));
    }
    if (VALUE_SLICE_LEN(a) >= 2 && !(FIXNUM_P(selfv) && FIXNUM_P(ev) && FIXNUM_P(VALUE_SLICE_GET(a, 1)))) {
        VALUE mv = VALUE_SLICE_GET(a, 1);              /* pow(exp, mod) with a Bignum operand → GMP modular exponentiation */
        if (UNLIKELY(!KORB_INTEGER_P(mv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(mv));
        korb_mp_t zm; korb_to_mpz(mv, zm);
        if (UNLIKELY(korb_mp_sgn(zm) == 0)) { korb_mp_clear(zm); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
        korb_mp_t ze; korb_to_mpz(ev, ze);
        if (UNLIKELY(korb_mp_sgn(ze) < 0)) { korb_mp_clear(zm); korb_mp_clear(ze); return korb_raise(c, slots, KORB_E_RANGE, 0, "Integer#pow() 1st argument cannot be negative when 2nd argument specified"); }
        korb_mp_t zb, zr; korb_to_mpz(selfv, zb); korb_mp_init(zr);
        korb_mp_powm(zr, zb, ze, zm);
        if (korb_mp_sgn(zm) < 0 && korb_mp_sgn(zr) != 0) korb_mp_add(zr, zr, zm);   /* floored result (sign of mod) */
        RESULT out = korb_big_from_mpz(c, slots, zr);
        korb_mp_clear(zb); korb_mp_clear(ze); korb_mp_clear(zm); korb_mp_clear(zr);
        return out;
    }
    if (VALUE_SLICE_LEN(a) < 2 || !FIXNUM_P(selfv) || !FIXNUM_P(ev))   /* plain pow (incl. overflow/bignum) */
        return korb_int_pow(c, slots, selfv, ev, 0);
    intptr_t base = FIX2LONG(selfv);
    intptr_t exp = FIX2LONG(ev);
    if (VALUE_SLICE_LEN(a) >= 2) {                    /* pow(exp, mod): modular exponentiation */
        VALUE mv = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!FIXNUM_P(mv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(mv));
        intptr_t mod = FIX2LONG(mv);
        if (UNLIKELY(mod == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        if (UNLIKELY(exp < 0)) return korb_raise(c, slots, KORB_E_RANGE, 0, "Integer#pow() 1st argument cannot be negative when 2nd argument specified");
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
        if (UNLIKELY(isnan(f))) return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, "NaN");
        const double q = floor(s / f);
        if (q >= -4611686018427387904.0 && q < 4611686018427387904.0)   /* within [-2^62, 2^62): a Fixnum */
            slots[0] = LONG2FIX((intptr_t)q);
        else { korb_mp_t zq; korb_mp_init(zq); korb_mp_set_d(zq, q); slots[0] = UNWRAP(korb_big_from_mpz(c, slots, zq)); korb_mp_clear(zq); }   /* quotient exceeds Fixnum range */
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, korb_float_fmod(s, f)));
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        return RESULT_OK(slots[2]);
    }
    if (KORB_RATIONAL_P(bv)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), bv, 2);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) {                  /* a, b = bv.coerce(self); a.divmod(b) */
        if (KORB_OBJECT_P(bv)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), bv, "divmod", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    }
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
        return korb_flt_toint(c, slots, s / f, 0);    /* floor → Fixnum or Bignum (a huge quotient overflows intptr_t) */
    }
    if (KORB_RATIONAL_P(bv)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), bv, 0);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) {                  /* a, b = bv.coerce(self); a.div(b) */
        if (KORB_OBJECT_P(bv)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), bv, "div", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    }
    if (UNLIKELY(!FIXNUM_P(VALUE_REF_GET(self)) || !FIXNUM_P(bv)))   /* Bignum operand/self → GMP */
        return korb_int_intdiv(c, slots, VALUE_REF_GET(self), bv, 0);
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(korb_int_fdiv(SELF_INT, b)));
}


static RESULT korb_m_int_gcd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE bv = VALUE_SLICE_GET(a, 0), sv = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (FIXNUM_P(sv) && FIXNUM_P(bv)) {
        intptr_t g = korb_int_gcd(FIX2LONG(sv), FIX2LONG(bv));
        if (LIKELY(FIXABLE(g))) return RESULT_OK(LONG2FIX(g));   /* non-fixable only when both are the min Fixnum (g == 2^62) */
    }
    {   /* Bignum operand or non-fixable Fixnum gcd → GMP (always non-negative) */
        korb_mp_t za, zb, zr; korb_to_mpz(sv, za); korb_to_mpz(bv, zb); korb_mp_init(zr);
        korb_mp_gcd(zr, za, zb);
        RESULT r = korb_big_from_mpz(c, slots, zr);
        korb_mp_clear(za); korb_mp_clear(zb); korb_mp_clear(zr);
        return r;
    }
}

static RESULT korb_m_int_lcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE bv = VALUE_SLICE_GET(a, 0), sv = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (FIXNUM_P(sv) && FIXNUM_P(bv)) {
        intptr_t av = FIX2LONG(sv), b = FIX2LONG(bv);
        if (av == 0 || b == 0) return RESULT_OK(LONG2FIX(0));
        intptr_t g = korb_int_gcd(av, b), l;
        if (LIKELY(!__builtin_mul_overflow(av / g, b, &l))) {
            if (l < 0) l = -l;
            if (LIKELY(FIXABLE(l))) return RESULT_OK(LONG2FIX(l));
        }
    }
    {   /* Bignum operand or Fixnum overflow → GMP lcm (non-negative; 0 if either is 0) */
        korb_mp_t za, zb, zr; korb_to_mpz(sv, za); korb_to_mpz(bv, zb); korb_mp_init(zr);
        korb_mp_lcm(zr, za, zb);
        RESULT r = korb_big_from_mpz(c, slots, zr);
        korb_mp_clear(za); korb_mp_clear(zb); korb_mp_clear(zr);
        return r;
    }
}

static RESULT korb_m_int_fdiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE sv = VALUE_REF_GET(self), bv = VALUE_SLICE_GET(a, 0);
    /* Integer#fdiv(Integer) when a plain double/double would lose precision (a
     * Bignum operand, or a Fixnum past 2^53): divide as an exact rational so the
     * result is correctly rounded — handles subnormals (1.fdiv(10**323)) and
     * magnitudes a double can't represent.  CRuby does the same. */
    if (KORB_INTEGER_P(bv)) {
        bool exact = KORB_BIGNUM_P(bv) || KORB_BIGNUM_P(sv);
        if (!exact) {
            const intptr_t si = FIX2LONG(sv), bi = FIX2LONG(bv);
            const intptr_t lim = (intptr_t)1 << 53;
            exact = si > lim || si < -lim || bi > lim || bi < -lim;
        }
        if (exact) {
            korb_mp_t zn, zd; korb_to_mpz(sv, zn); korb_to_mpz(bv, zd);
            if (korb_mp_sgn(zd) != 0) {                       /* zero divisor → Float ±Inf/NaN below */
                korb_mq_t q; korb_mq_init(q);
                korb_mq_set_num(q, zn); korb_mq_set_den(q, zd); korb_mq_canonicalize(q);
                double r = korb_mq_get_d(q);                  /* GMP truncates toward zero — round to nearest below */
                if (isfinite(r)) {
                    const double r2 = nextafter(r, r >= 0 ? HUGE_VAL : -HUGE_VAL);   /* next double away from zero */
                    if (isfinite(r2)) {
                        korb_mq_t qr, qr2, mid; korb_mq_init(qr); korb_mq_init(qr2); korb_mq_init(mid);
                        korb_mq_set_d(qr, r); korb_mq_set_d(qr2, r2);
                        korb_mq_add(mid, qr, qr2); korb_mq_div_2exp(mid, mid, 1);            /* (r + r2) / 2 */
                        const int cmp = korb_mq_cmp(q, mid);
                        if (cmp == 0) {                                            /* exact tie → round half to even */
                            uint64_t bits; memcpy(&bits, &r, sizeof bits);
                            if (bits & 1u) r = r2;
                        } else if ((cmp > 0) == (r >= 0)) {                        /* exact beyond the midpoint → round away from zero */
                            r = r2;
                        }
                        korb_mq_clear(qr); korb_mq_clear(qr2); korb_mq_clear(mid);
                    }
                }
                korb_mq_clear(q); korb_mp_clear(zn); korb_mp_clear(zd);
                return korb_float_new(c, slots, r);
            }
            korb_mp_clear(zn); korb_mp_clear(zd);
        }
    }
    double s; korb_num_to_d(sv, &s);                      /* self may be a Bignum */
    double o;
    if (UNLIKELY(!korb_num_to_d(bv, &o))) {               /* non-numeric arg → the coerce protocol (a,b = arg.coerce(self); a.fdiv(b)) */
        if (KORB_OBJECT_P(bv)) {
            bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), bv, "fdiv", 0, &h);
            if (h) return cr;
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    }
    return korb_float_new(c, slots, s / o);
}
static RESULT korb_m_int_ceildiv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE bv = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(bv)) {                            /* Integer#ceildiv(Float) → ceil(self/f) Integer */
        double f = korb_float_val(bv), s;
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        (void)korb_num_to_d(VALUE_REF_GET(self), &s);  /* works for Bignum self too */
        return RESULT_OK(LONG2FIX((intptr_t)ceil(s / f)));
    }
    if (KORB_RATIONAL_P(bv)) {                         /* Integer#ceildiv(Rational p/q) → ceil(self*q / p), exact */
        korb_mp_t za, zp, zq, prod, res;
        korb_to_mpz(VALUE_REF_GET(self), za); korb_to_mpz(VAL2RAT(bv)->num, zp); korb_to_mpz(VAL2RAT(bv)->den, zq);
        if (UNLIKELY(korb_mp_sgn(zp) == 0)) { korb_mp_clear(za); korb_mp_clear(zp); korb_mp_clear(zq); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
        korb_mp_init(prod); korb_mp_init(res); korb_mp_mul(prod, za, zq);   /* den q > 0 (normalized), so cdiv toward +inf is ceil */
        korb_mp_cdiv_q(res, prod, zp);
        RESULT out = korb_big_from_mpz(c, slots, res);
        korb_mp_clear(za); korb_mp_clear(zp); korb_mp_clear(zq); korb_mp_clear(prod); korb_mp_clear(res);
        return out;
    }
    if (UNLIKELY(!KORB_INTEGER_P(bv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(bv));
    if (KORB_BIGNUM_P(VALUE_REF_GET(self)) || KORB_BIGNUM_P(bv)) {   /* ceiling division in GMP (no intptr_t overflow) */
        korb_mp_t za, zb, zq;
        korb_to_mpz(VALUE_REF_GET(self), za); korb_to_mpz(bv, zb);
        if (UNLIKELY(korb_mp_sgn(zb) == 0)) { korb_mp_clear(za); korb_mp_clear(zb); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
        korb_mp_init(zq); korb_mp_cdiv_q(zq, za, zb);
        RESULT out = korb_big_from_mpz(c, slots, zq);
        korb_mp_clear(za); korb_mp_clear(zb); korb_mp_clear(zq);
        return out;
    }
    intptr_t b = FIX2LONG(bv);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(-korb_int_fdiv(-SELF_INT, b)));   /* ceil = -floor(-a/b) */
}
/* coerce(other) → [other, self] both as Integer, or both Float if other is Float */
static RESULT korb_m_int_coerce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE o = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(o) || KORB_BIGNUM_P(o)) {                /* Integer → [other, self] */
        slots[0] = o; slots[1] = VALUE_REF_GET(self);
    } else {                                              /* else (incl. Rational) → [Float(other), Float(self)] */
        double od;
        if (korb_num_to_d(o, &od)) {                     /* Float/Rational → its double */
            slots[0] = UNWRAP(korb_float_new(c, slots, od));
        } else if (KORB_STRING_P(o)) {                   /* "2.5" → Float() parse (ArgumentError if invalid) */
            RESULT fr = korb_bi_float(c, slots, a);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            slots[0] = fr.value;
        } else if (KORB_OBJECT_P(o) && korb_responds_to(c, o, korb_intern(c->vm, "to_f", 4))) {
            slots[0] = o;
            RESULT fr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_f", 4), 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            if (UNLIKELY(!KORB_FLOAT_P(fr.value)))           /* #to_f must return a Float (slots[0]=o, re-read after the dispatch's GC) */
                return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Float", korb_type_name(slots[0]));
            slots[0] = fr.value;
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't coerce %s into Integer", korb_type_name(o));
        }
        double sd = 0; (void)korb_num_to_d(VALUE_REF_GET(self), &sd);
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, sd));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_int_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE selfv = VALUE_REF_GET(self), o = VALUE_SLICE_GET(a, 0);
    if (KORB_INTEGER_P(o)) return RESULT_OK(LONG2FIX(korb_int_cmp(selfv, o)));   /* exact */
    double y, x;
    if (!korb_num_to_d(o, &y)) {                            /* coercible object → a, b = o.coerce(self); a <=> b */
        if (KORB_OBJECT_P(o)) { bool h; RESULT cr = korb_try_coerce(c, slots, selfv, o, "<=>", 0, &h); if (h) return cr; }
        return RESULT_OK(KORB_NIL);                          /* incomparable → nil */
    }
    if (KORB_FLOAT_P(o)) {                                   /* Integer <=> Float: exact (no lossy cast) */
        const int cmp = korb_big_flo_cmp(selfv, y);
        return RESULT_OK(cmp == 2 ? KORB_NIL : LONG2FIX(cmp));
    }
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
    (void)a; return korb_rat_new_v(c, slots, VALUE_REF_GET(self), LONG2FIX(1));   /* VALUE-based: Bignum-safe */
}
/* Integer#rationalize([eps]) — exact for an integer; eps is ignored, but a 2nd arg is an error. */
static RESULT korb_m_int_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", (unsigned)VALUE_SLICE_LEN(a));
    return korb_rat_new_v(c, slots, VALUE_REF_GET(self), LONG2FIX(1));
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
/* `caller` / `caller_locations`: koruby keeps no walkable file:line call stack, so
 * return an empty Array (as at top level) — unblocks call sites; the backtrace
 * CONTENT specs still fail.  Real backtraces need per-frame line tracking. */
static RESULT korb_m_empty_ary(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_ary_new(c, slots, 0); }
/* to_c on a real: Complex(self, 0). */
static RESULT korb_m_num_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; return korb_cpx_new(c, slots, VALUE_REF_GET(self), LONG2FIX(0)); }
/* polar of a real: [magnitude, angle]. magnitude=abs(self) (same class), angle per korb_m_num_angle. */
static RESULT korb_m_num_polar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d;
    VALUE sv = VALUE_REF_GET(self);
    if (!korb_num_to_d(sv, &d)) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a real");
    /* slots[0]=magnitude (abs, class-preserving), slots[1]=angle, slots[2]=result array */
    if (FIXNUM_P(sv)) { intptr_t n = FIX2LONG(sv); slots[0] = LONG2FIX(n < 0 ? -n : n); }
    else if (KORB_BIGNUM_P(sv)) { slots[0] = (d < 0) ? UNWRAP(korb_big_neg(c, slots, sv)) : sv; }   /* exact |self| */
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
    if (UNLIKELY(!isfinite(d))) return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, "%s", isnan(d) ? "NaN" : (d < 0 ? "-Infinity" : "Infinity"));
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
