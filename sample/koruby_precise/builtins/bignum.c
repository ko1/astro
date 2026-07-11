/* koruby_precise — bignum.c: arbitrary-precision Integer.
 *
 * Abstraction layer over GMP (the only backend today); a future hand-rolled
 * limb-in-GC implementation can replace the body behind the same korb_int_ /
 * korb_big_ API.  Compiled only when KORB_HAVE_GMP is defined — otherwise the
 * overflow sites keep raising NotImplementedError (build-time "give up").
 *
 * #included into korb_runtime.c's TU. */
#ifdef KORB_HAVE_GMP

/* Initialise a local temp mpz from a Fixnum / Bignum VALUE.  Caller mpz_clear's
 * it.  The copy is independent of the GC heap, so it survives later allocs. */
static void korb_to_mpz(VALUE v, mpz_t out) {
    if (FIXNUM_P(v)) mpz_init_set_si(out, (long)FIX2LONG(v));
    else             mpz_init_set(out, VAL2BIG(v)->z);
}
/* Normalise an mpz result → Fixnum when it fits, else a fresh Bignum object. */
static RESULT korb_big_from_mpz(CTX *c, VALUE *slots, const mpz_t src) {
    if (mpz_fits_slong_p(src)) {
        long v = mpz_get_si(src);
        if (FIXABLE((intptr_t)v)) return RESULT_OK(LONG2FIX((intptr_t)v));
    }
    KorbBignum *b = korb_alloc(c, slots, sizeof(KorbBignum), KORB_OBJ_BIGNUM);   /* may GC; src is local */
    mpz_init_set(b->z, src);
    aro_gc_finalize_register(c, b);   /* free the mpz limbs (AROH_FINALIZE → mpz_clear) when b is collected */
    aro_gc_account_external(c, (ssize_t)KORB_BIG_LIMB_BYTES(b->z));   /* limbs are external malloc → GC pressure (immutable, so the matching -delta is in AROH_FINALIZE) */
    return RESULT_OK((VALUE)b);
}
/* mpz_get_d truncates toward zero; CRuby rounds an Integer to the nearest double
 * (ties to even), so (10**308).to_f == 1.0e308, not 9.999...e307.  Take the
 * truncated value and its neighbour toward ±Inf, then pick whichever is closer
 * by exact mpz distance (tie → the candidate with an even significand bit). */
double korb_big_to_d(VALUE v) {
    mpz_srcptr z = VAL2BIG(v)->z;
    const double lo = mpz_get_d(z);                       /* |lo| <= |z| (toward zero) */
    if (isinf(lo)) return lo;                             /* magnitude overflows double */
    const double hi = nextafter(lo, lo < 0 ? -HUGE_VAL : HUGE_VAL);
    if (isinf(hi)) return lo;                             /* lo already the max finite */
    mpz_t zl, zh, dl, dh;
    mpz_init(zl); mpz_init(zh); mpz_init(dl); mpz_init(dh);
    mpz_set_d(zl, lo); mpz_set_d(zh, hi);
    mpz_sub(dl, z, zl); mpz_abs(dl, dl);                  /* |z - lo| */
    mpz_sub(dh, zh, z); mpz_abs(dh, dh);                  /* |hi - z| */
    const int cmp = mpz_cmp(dh, dl);                      /* <0: hi nearer, >0: lo nearer */
    double r = lo;
    if (cmp < 0) r = hi;
    else if (cmp == 0) {                                  /* exact tie → round to even significand */
        uint64_t hbits; __builtin_memcpy(&hbits, &hi, sizeof hbits);
        r = (hbits & 1u) == 0 ? hi : lo;
    }
    mpz_clear(zl); mpz_clear(zh); mpz_clear(dl); mpz_clear(dh);
    return r;
}

/* Integer +,-,*,/,% (op 0..4) over Fixnum/Bignum; result normalised. */
RESULT korb_int_arith(CTX *c, VALUE *slots, VALUE a, VALUE b, int op, uint32_t line) {
    mpz_t za, zb, zr;
    korb_to_mpz(a, za); korb_to_mpz(b, zb); mpz_init(zr);
    bool ok = true;
    switch (op) {
      case 0: mpz_add(zr, za, zb); break;
      case 1: mpz_sub(zr, za, zb); break;
      case 2: mpz_mul(zr, za, zb); break;
      case 3: case 4:
        if (mpz_sgn(zb) == 0) { ok = false; break; }
        if (op == 3) mpz_fdiv_q(zr, za, zb);    /* Ruby floor division */
        else         mpz_fdiv_r(zr, za, zb);    /* floored modulo (sign of divisor) */
        break;
      default: ok = false; break;
    }
    mpz_clear(za); mpz_clear(zb);
    if (UNLIKELY(!ok)) { mpz_clear(zr); return korb_raise(c, slots, KORB_E_ZERODIV, line, "divided by 0"); }
    RESULT r = korb_big_from_mpz(c, slots, zr);
    mpz_clear(zr);
    return r;
}
/* Integer &,|,^ (op 0..2) over Fixnum/Bignum; two's-complement semantics for
 * negatives (GMP matches Ruby).  Result normalised back to Fixnum when it fits. */
RESULT korb_int_bitwise(CTX *c, VALUE *slots, VALUE a, VALUE b, int op) {
    mpz_t za, zb, zr;
    korb_to_mpz(a, za); korb_to_mpz(b, zb); mpz_init(zr);
    switch (op) {
      case 0: mpz_and(zr, za, zb); break;
      case 1: mpz_ior(zr, za, zb); break;
      default: mpz_xor(zr, za, zb); break;
    }
    mpz_clear(za); mpz_clear(zb);
    RESULT r = korb_big_from_mpz(c, slots, zr);
    mpz_clear(zr);
    return r;
}
/* Integer divmod family over Fixnum/Bignum operands (op 0 div=floor quotient,
 * 1 modulo=floored remainder, 2 divmod=[q,r], 3 remainder=truncated remainder).
 * Results normalised back to Fixnum when they fit. */
RESULT korb_int_intdiv(CTX *c, VALUE *slots, VALUE a, VALUE b, int op) {
    mpz_t za, zb, zq, zr;
    korb_to_mpz(a, za); korb_to_mpz(b, zb);
    if (UNLIKELY(mpz_sgn(zb) == 0)) { mpz_clear(za); mpz_clear(zb); return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0"); }
    mpz_init(zq); mpz_init(zr);
    if (op == 3) mpz_tdiv_qr(zq, zr, za, zb);          /* truncated (remainder) */
    else         mpz_fdiv_qr(zq, zr, za, zb);          /* floored (div / modulo / divmod) */
    mpz_clear(za); mpz_clear(zb);
    RESULT r;
    if (op == 0)                  r = korb_big_from_mpz(c, slots, zq);
    else if (op == 1 || op == 3)  r = korb_big_from_mpz(c, slots, zr);
    else {                                              /* divmod → [q, r] */
        slots[0] = korb_big_from_mpz(c, slots, zq).value;        /* park (rooted across next alloc) */
        slots[1] = korb_big_from_mpz(c, slots + 1, zr).value;
        slots[2] = korb_ary_new(c, slots + 2, 2).value;
        VALUE_REF dst = VALUE_REF_AT(&slots[2]);
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
        r = RESULT_OK(slots[2]);
    }
    mpz_clear(zq); mpz_clear(zr);
    return r;
}
/* base ** exp (both Integer).  Negative exp → Rational (delegated by caller for
 * the Fixnum case; here exp>=0 expected — negative falls back to a float-free
 * Rational via korb_rat_new). */
RESULT korb_int_pow(CTX *c, VALUE *slots, VALUE base, VALUE expv, uint32_t line) {
    (void)line;
    if (FIXNUM_P(expv) && FIX2LONG(expv) < 0) {           /* a ** -n → Rational(1, a**n) */
        mpz_t zb, zr; korb_to_mpz(base, zb); mpz_init(zr);
        mpz_pow_ui(zr, zb, (unsigned long)(-FIX2LONG(expv)));
        mpz_clear(zb);
        RESULT denr = korb_big_from_mpz(c, slots, zr);     /* a**n (normalised) */
        mpz_clear(zr);
        if (FIXNUM_P(denr.value)) {
            intptr_t d = FIX2LONG(denr.value);
            if (d == 1 || d == -1) return RESULT_OK(LONG2FIX(d));   /* 1/±1 → Integer (CRuby) */
            return korb_rat_new(c, slots, 1, d);
        }
        return denr;                                       /* huge denom: best-effort (rare) */
    }
    mpz_t zb; korb_to_mpz(base, zb);
    const int base_mag = mpz_cmpabs_ui(zb, 1);            /* <0: |base|=0, ==0: |base|=1, >0: |base|>=2 */
    if (base_mag <= 0) {                                  /* 0/1/-1 ** e is trivial even for a huge e */
        int res;
        if (mpz_sgn(zb) == 0)      res = (expv == LONG2FIX(0)) ? 1 : 0;   /* 0**0 = 1, else 0 ** (e>0) = 0 */
        else if (base_mag == 0 && mpz_sgn(zb) > 0) res = 1;   /* 1 ** e = 1 */
        else { bool even = FIXNUM_P(expv) ? (FIX2LONG(expv) % 2 == 0)   /* -1 ** e */
                                          : ({ mpz_t ze; korb_to_mpz(expv, ze); bool ev = mpz_even_p(ze); mpz_clear(ze); ev; });
               res = even ? 1 : -1; }
        mpz_clear(zb);
        return RESULT_OK(LONG2FIX(res));
    }
    unsigned long e;
    if (FIXNUM_P(expv)) e = (unsigned long)FIX2LONG(expv);
    else {
        mpz_t ze; korb_to_mpz(expv, ze);
        const bool fits = mpz_fits_ulong_p(ze) != 0;
        if (fits) e = mpz_get_ui(ze);
        mpz_clear(ze);
        if (!fits) { mpz_clear(zb); return korb_raise(c, slots, KORB_E_ARGUMENT, line, "exponent is too large"); }
    }
    /* |base|>=2: guard against an astronomically large result (CRuby raises rather
     * than trying to allocate it) — estimated result is e * bit_length(base) bits. */
    if ((double)e * (double)mpz_sizeinbase(zb, 2) > 1073741824.0) {   /* > 2^30 bits (~128 MB) */
        mpz_clear(zb);
        return korb_raise(c, slots, KORB_E_ARGUMENT, line, "exponent is too large");
    }
    mpz_t zr; mpz_init(zr);
    mpz_pow_ui(zr, zb, e);
    mpz_clear(zb);
    RESULT r = korb_big_from_mpz(c, slots, zr);
    mpz_clear(zr);
    return r;
}
/* a << n (n>0) / a >> -n.  `amount` may be negative to shift right. */
RESULT korb_int_shift(CTX *c, VALUE *slots, VALUE a, intptr_t amount) {
    mpz_t za, zr; korb_to_mpz(a, za); mpz_init(zr);
    if (amount >= 0) mpz_mul_2exp(zr, za, (mp_bitcnt_t)amount);
    else             mpz_fdiv_q_2exp(zr, za, (mp_bitcnt_t)(-amount));
    mpz_clear(za);
    RESULT r = korb_big_from_mpz(c, slots, zr);
    mpz_clear(zr);
    return r;
}
/* compare two Integers (Fixnum/Bignum) → -1 / 0 / 1. */
int korb_int_cmp(VALUE a, VALUE b) {
    if (FIXNUM_P(a) && FIXNUM_P(b)) { intptr_t x = FIX2LONG(a), y = FIX2LONG(b); return x < y ? -1 : x > y ? 1 : 0; }
    mpz_t za, zb; korb_to_mpz(a, za); korb_to_mpz(b, zb);
    int r = mpz_cmp(za, zb);
    mpz_clear(za); mpz_clear(zb);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
/* Exact <=> of an Integer `bi` against a double `d` (no precision loss from
 * casting a Bignum to double).  Returns -1/0/1, or 2 when d is NaN. */
int korb_big_flo_cmp(VALUE bi, double d) {
    if (d != d) return 2;                            /* NaN: incomparable */
    if (isinf(d)) return d > 0 ? -1 : 1;
    mpz_t z; korb_to_mpz(bi, z);
    const double fl = floor(d);
    mpz_t zd; mpz_init(zd); mpz_set_d(zd, fl);       /* fl integral → exact */
    const int c = mpz_cmp(z, zd);
    mpz_clear(zd); mpz_clear(z);
    if (c != 0) return c > 0 ? 1 : -1;
    return (d > fl) ? -1 : 0;                        /* bi == floor(d); a fractional d is larger */
}
/* unary minus of a Bignum (normalised). */
RESULT korb_big_neg(CTX *c, VALUE *slots, VALUE v) {
    mpz_t z; korb_to_mpz(v, z); mpz_neg(z, z);
    RESULT r = korb_big_from_mpz(c, slots, z);
    mpz_clear(z);
    return r;
}

/* Integer.sqrt(n) — exact integer square root (floor of √n) for any non-negative
 * Integer; via mpz_sqrt so Bignums are exact (no Float rounding).  Class method. */
static RESULT korb_m_integer_sqrt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    VALUE n = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(n)) n = LONG2FIX((intptr_t)korb_float_val(n));   /* Integer.sqrt(8.5) → isqrt(8) (CRuby truncates) */
    if (UNLIKELY(!KORB_INTEGER_P(n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    mpz_t z, r;
    korb_to_mpz(n, z);
    if (UNLIKELY(mpz_sgn(z) < 0)) { mpz_clear(z); return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "Numerical argument is out of domain - \"isqrt\""); }
    mpz_init(r);
    mpz_sqrt(r, z);
    RESULT res = korb_big_from_mpz(c, slots, r);
    mpz_clear(z); mpz_clear(r);
    return res;
}

#endif /* KORB_HAVE_GMP */
