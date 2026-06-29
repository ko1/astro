/* koruby_precise — array_int_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Array (query/mutate) + Integer (bit) methods ------------------- */

static RESULT korb_m_ary_unshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t k = VALUE_SLICE_LEN(a);
    if (k == 0) return RESULT_OK(VALUE_REF_GET(self));
    CHECK(korb_ary_ensure(c, slots, self, k));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)        /* shift right by k */
        ARO_STORE(c, it, &it->data[i + k], it->data[i]);
    for (uint32_t i = 0; i < k; i++)
        ARO_STORE(c, it, &it->data[i], VALUE_SLICE_GET(a, i));
    ary->len += k;
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_shift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* shift(n): remove & return first n as array */
        intptr_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t take = (uint32_t)n; if (take > VAL2ARY(VALUE_REF_GET(self))->len) take = VAL2ARY(VALUE_REF_GET(self))->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = 0; i < take; i++)
            CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        KorbArray *ary2 = VAL2ARY(VALUE_REF_GET(self));   /* now compact self in place */
        KorbArrayItems *it2 = ary2->items;
        for (uint32_t i = take; i < ary2->len; i++) ARO_STORE(c, it2, &it2->data[i - take], it2->data[i]);
        for (uint32_t i = ary2->len - take; i < ary2->len; i++) ARO_STORE(c, it2, &it2->data[i], KORB_NIL);
        ary2->len -= take;
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE first = it->data[0];
    for (uint32_t i = 1; i < ary->len; i++) ARO_STORE(c, it, &it->data[i - 1], it->data[i]);
    ary->len--;
    ARO_STORE(c, it, &it->data[ary->len], KORB_NIL);
    return RESULT_OK(first);
}

/* assoc (idx 0) / rassoc (idx 1): find the sub-array whose [idx] == key */
static RESULT korb_ary_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE key, uint32_t idx) {
    slots[0] = key;                                          /* root key across element == dispatches */
    const uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];   /* re-read each iter */
        if (!KORB_ARRAY_P(e) || VAL2ARY(e)->len <= idx) continue;
        const VALUE el = VAL2ARY(e)->items->data[idx];
        if (KORB_OBJECT_P(el) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (el == key) */
            slots[1] = e;                                    /* root e (returned on match) */
            slots[2] = el; slots[3] = slots[0];              /* recv, arg */
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[1]);
        } else if (korb_value_eq(el, slots[0])) {
            return RESULT_OK(e);
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_assoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_ary_assoc(c, slots, self, VALUE_SLICE_GET(a, 0), 0); }
static RESULT korb_m_ary_rassoc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_ary_assoc(c, slots, self, VALUE_SLICE_GET(a, 0), 1); }

static RESULT korb_m_ary_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const bool have_default = VALUE_SLICE_LEN(a) >= 2;
    slots[0] = VALUE_SLICE_GET(a, 0);                     /* original index arg (rooted; yielded to the block as-is) */
    slots[1] = have_default ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    intptr_t i;
    if (UNLIKELY(!korb_to_index(slots[0], &i))) {         /* coerce a non-Integer index via #to_int */
        VALUE iv2 = slots[0];
        RESULT cr = korb_coerce_to_int(c, slots + 2, &iv2);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv2, &i)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(slots[0]));
    }
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));  /* re-read after possible dispatch */
    intptr_t orig = i;
    if (i < 0) i += ary->len;
    if (i >= 0 && (uint32_t)i < ary->len) return RESULT_OK(ary->items->data[i]);
    if (block != NULL)                                    /* out of range: block yields the ORIGINAL arg (wins over a default) */
        return korb_block_yield(c, slots + 2, block, def_env, &slots[0], 1, cself);
    if (have_default) return RESULT_OK(slots[1]);
    return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld outside of array bounds: %ld...%u",
                      (long)orig, ary->len ? -(long)ary->len : 0L, ary->len);
}

/* dig: recursive index into nested Array/Hash */
static RESULT korb_m_ary_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    VALUE cur = VALUE_REF_GET(self);
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE key = VALUE_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        if (KORB_ARRAY_P(cur)) {
            intptr_t i;
            if (UNLIKELY(!korb_to_index(key, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(key));
            KorbArray *ar = VAL2ARY(cur);
            if (i < 0) i += ar->len;
            cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : ar->items->data[i];
        } else if (KORB_HASH_P(cur)) {
            int32_t idx = korb_hash_find(VAL2HASH(cur), key);
            cur = idx < 0 ? KORB_NIL : VAL2HASH(cur)->items->data[2 * idx + 1];
        } else {
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(cur));
        }
    }
    return RESULT_OK(cur);
}

/* Class name for coercion errors: nil/true/false render as "nil"/"true"/"false",
 * a user instance as its actual class name (not the generic "Object"). */
static const char *korb_coerce_name(CTX *c, VALUE v) {
    if (v == KORB_NIL)   return "nil";
    if (v == KORB_TRUE)  return "true";
    if (v == KORB_FALSE) return "false";
    if (KORB_OBJECT_P(v)) {
        const VALUE k = VAL2OBJ(v)->klass;
        if (KORB_CLASS_P(k) && VAL2CLASS(k)->name_sym)
            return korb_sym_name(c->vm, VAL2CLASS(k)->name_sym);
    }
    return korb_type_name(v);                       /* builtins: String / Integer / ... */
}
/* Coerce a shift argument to an intptr via #to_int (CRuby's rb_to_int).
 * to_int dispatch may GC, so the caller's self is held behind a VALUE_REF. */
static RESULT korb_arg_to_index(CTX *c, VALUE *slots, VALUE o, intptr_t *out) {
    if (LIKELY(korb_to_index(o, out))) return RESULT_OK(KORB_NIL);
    const uint32_t mid = korb_intern(c->vm, "to_int", 6);
    if (!KORB_OBJECT_P(o) || !korb_responds_to(c, o, mid))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, o));
    slots[0] = o;
    RESULT r = korb_send_impl(c, slots + 1, mid, 0, 0, NULL, NULL, KORB_NIL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    o = slots[0];                                 /* re-read: to_int dispatch may have moved o */
    if (UNLIKELY(!KORB_INTEGER_P(r.value))) {
        const char *on = korb_coerce_name(c, o);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Integer (%s#to_int gives %s)",
                          on, on, korb_type_name(r.value));
    }
    if (UNLIKELY(!korb_to_index(r.value, out)))   /* huge Bignum amount (rare) */
        return korb_raise(c, slots, KORB_E_RANGE, 0, "shift width too big");
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_int_lshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    intptr_t sh;
    { RESULT cr = korb_arg_to_index(c, slots, o, &sh); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) return korb_int_shift(c, slots, VALUE_REF_GET(self), sh);
#endif
    intptr_t n = FIX2LONG(VALUE_REF_GET(self));
    intptr_t r = sh >= 0 ? (sh < 62 ? (n << sh) : 0) : (n >> (-sh < 63 ? -sh : 62));
    if (sh >= 0 && (sh >= 62 || (r >> sh) != n || !FIXABLE(r)))
#ifdef KORB_HAVE_GMP
        return korb_int_shift(c, slots, VALUE_REF_GET(self), sh);
#else
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_rshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    intptr_t sh;
    { RESULT cr = korb_arg_to_index(c, slots, o, &sh); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
#ifdef KORB_HAVE_GMP
    if (KORB_BIGNUM_P(VALUE_REF_GET(self))) return korb_int_shift(c, slots, VALUE_REF_GET(self), -sh);   /* x >> sh == shift by -sh */
#endif
    intptr_t n = FIX2LONG(VALUE_REF_GET(self));
    if (sh >= 0)   /* arithmetic right shift — always fits in a Fixnum */
        return RESULT_OK(LONG2FIX(sh < 63 ? (n >> sh) : (n < 0 ? -1 : 0)));
    /* sh < 0 → left shift by -sh; may overflow into Bignum */
    const intptr_t ls = -sh;
    const intptr_t r = (ls < 62) ? (n << ls) : 0;
    if (ls >= 62 || (r >> ls) != n || !FIXABLE(r))
#ifdef KORB_HAVE_GMP
        return korb_int_shift(c, slots, VALUE_REF_GET(self), ls);
#else
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
#endif
    return RESULT_OK(LONG2FIX(r));
}
/* Integer#[] — bit reference: int[i] (single bit), int[i, len] (len-bit field),
 * int[range] (bits in range).  Two's-complement semantics for negatives. */
static RESULT korb_m_int_bitref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE selfv = VALUE_REF_GET(self);
    /* result = (self >> i), then: single_bit → &1 (n[i]); zero → 0; mask_len>0 →
     * low mask_len bits; mask_len==0 → all bits from the offset (endless/reversed
     * range, or n[i] with no length). */
    intptr_t i = 0, mask_len = 0; bool single_bit = false, zero = false;
    if (VALUE_SLICE_LEN(a) >= 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* int[i..j] */
        const KorbRange *rg = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        i = (rg->rbegin == KORB_NIL) ? 0 : (FIXNUM_P(rg->rbegin) ? FIX2LONG(rg->rbegin) : 0);   /* beginless ⇒ 0 */
        if (rg->rend != KORB_NIL && FIXNUM_P(rg->rend)) {
            intptr_t len = FIX2LONG(rg->rend) - i + (rg->exclude_end ? 0 : 1);
            if (len > 0) mask_len = len;                                    /* reversed/empty ⇒ no upper mask */
        }                                                                  /* endless ⇒ no upper mask */
    } else {
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (VALUE_SLICE_LEN(a) >= 2) {                                      /* n[i, len] */
            intptr_t len;
            if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
            if (len == 0) zero = true; else if (len > 0) mask_len = len;   /* len<0 ⇒ no upper mask (all bits) */
        } else single_bit = true;                                          /* n[i] */
    }
#ifdef KORB_HAVE_GMP
    if (LIKELY(FIXNUM_P(selfv) && single_bit)) {              /* n[i] on a Fixnum — bit test, no GMP alloc */
        const intptr_t n = FIX2LONG(selfv);
        if (i < 0)   return RESULT_OK(LONG2FIX(0));           /* bits below 0 don't exist */
        if (i >= 63) return RESULT_OK(LONG2FIX(n < 0 ? 1 : 0));   /* beyond Fixnum width = sign extension */
        return RESULT_OK(LONG2FIX((n >> i) & 1));
    }
    {
        mpz_t z, r; korb_to_mpz(selfv, z); mpz_init(r);
        if (i >= 0) mpz_fdiv_q_2exp(r, z, (mp_bitcnt_t)i);     /* self >> i (arith) */
        else        mpz_mul_2exp(r, z, (mp_bitcnt_t)(-i));     /* self << -i */
        if (zero) mpz_set_ui(r, 0);
        else if (single_bit) { mpz_t one; mpz_init_set_ui(one, 1); mpz_and(r, r, one); mpz_clear(one); }
        else if (mask_len > 0) { mpz_t m; mpz_init_set_ui(m, 1); mpz_mul_2exp(m, m, (mp_bitcnt_t)mask_len); mpz_sub_ui(m, m, 1); mpz_and(r, r, m); mpz_clear(m); }
        /* else (mask_len==0, range no-mask): all bits from offset */
        mpz_clear(z);
        RESULT res = korb_big_from_mpz(c, slots, r); mpz_clear(r); return res;
    }
#else
    intptr_t n = FIX2LONG(selfv);
    if (zero) return RESULT_OK(LONG2FIX(0));
    intptr_t shifted = i >= 0 ? (i < 63 ? (n >> i) : (n < 0 ? -1 : 0)) : (n << (-i));
    if (single_bit) {
        if (i < 0 || i >= 63) return RESULT_OK(LONG2FIX(n < 0 && i >= 63 ? 1 : 0));
        return RESULT_OK(LONG2FIX((n >> i) & 1));
    }
    if (mask_len <= 0) return RESULT_OK(LONG2FIX(shifted));                /* no upper mask */
    intptr_t mask = mask_len >= 63 ? -1 : ((intptr_t)1 << mask_len) - 1;
    return RESULT_OK(LONG2FIX(shifted & mask));
#endif
}
/* bitwise & | ^ (kind 0/1/2) */
static RESULT korb_int_bitop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int kind) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    VALUE s = VALUE_REF_GET(self);
    if (LIKELY(FIXNUM_P(s) && FIXNUM_P(o))) {
        intptr_t x = FIX2LONG(s), y = FIX2LONG(o);
        return RESULT_OK(LONG2FIX(kind == 0 ? (x & y) : kind == 1 ? (x | y) : (x ^ y)));
    }
    if (UNLIKELY(!KORB_INTEGER_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(o));
    return korb_int_bitwise(c, slots, s, o, kind);     /* Bignum operand → GMP two's-complement bitop */
}
static RESULT korb_m_int_and(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 0); }
static RESULT korb_m_int_or (CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 1); }
static RESULT korb_m_int_xor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_int_bitop(c, slots, self, a, 2); }
static RESULT korb_m_int_inv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(~FIX2LONG(VALUE_REF_GET(self)))); }
static RESULT korb_m_int_remainder(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(o)) {                             /* Integer#remainder(Float) → Float, truncated */
        double f = korb_float_val(o), s; korb_num_to_d(VALUE_REF_GET(self), &s);
        return korb_float_new(c, slots, fmod(s, f));   /* C fmod = truncated remainder (sign of dividend) */
    }
    if (KORB_RATIONAL_P(o)) return korb_int_rat_divmod(c, slots, VALUE_REF_GET(self), o, 3);
    if (UNLIKELY(!KORB_INTEGER_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_type_name(o));
    if (UNLIKELY(!FIXNUM_P(VALUE_REF_GET(self)) || !FIXNUM_P(o)))   /* Bignum operand/self → GMP truncated */
        return korb_int_intdiv(c, slots, VALUE_REF_GET(self), o, 3);
    intptr_t b = FIX2LONG(o);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(FIX2LONG(VALUE_REF_GET(self)) % b));   /* truncated (sign of dividend) */
}
static RESULT korb_m_true_lit2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_flt_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)a; double d = korb_float_val(VALUE_REF_GET(self)); return korb_float_new(c, slots, d * d); }

static RESULT korb_m_ary_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        VALUE iv = VALUE_SLICE_GET(a, j);
        if (KORB_RANGE_P(iv)) {                       /* values_at(range): each index in the range */
            const KorbRange *r = VAL2RANGE(iv);
            const intptr_t len0 = VAL2ARY(VALUE_REF_GET(self))->len;
            intptr_t b, e2, last;
            if (r->rbegin == KORB_NIL) b = 0;         /* beginless → 0 */
            else { if (!korb_to_index(r->rbegin, &b)) continue; if (b < 0) b += len0; }
            if (r->rend == KORB_NIL) last = len0 - 1; /* endless → to end */
            else { if (!korb_to_index(r->rend, &e2)) continue; if (e2 < 0) e2 += len0; last = r->exclude_end ? e2 - 1 : e2; }
            for (intptr_t i = b; i <= last; i++) {
                const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
                CHECK(korb_ary_push_val(c, slots + 1, dst, (i >= 0 && (uint32_t)i < ary->len) ? ary->items->data[i] : KORB_NIL));
            }
            continue;
        }
        intptr_t i;
        if (!korb_to_index(iv, &i)) {                 /* coerce a non-Integer index via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots + 1, &iv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(iv, &i))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, j)));
        }
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));   /* re-read after possible dispatch */
        VALUE e = KORB_NIL;
        if (i < 0) i += ary->len;
        if (i >= 0 && (uint32_t)i < ary->len) e = ary->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_fill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    intptr_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    uint32_t base = block ? 0 : 1;                        /* position args start here */
    if (UNLIKELY(!block && VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    VALUE v = block ? KORB_NIL : VALUE_SLICE_GET(a, 0);
    /* compute fill region [beg, beg+len) following MRI rb_ary_fill order */
    intptr_t beg = 0, len = n; bool have_len = false;
    VALUE pos0 = VALUE_SLICE_LEN(a) > base ? VALUE_SLICE_GET(a, base) : KORB_NIL;
    if (KORB_RANGE_P(pos0)) {
        const KorbRange *r = VAL2RANGE(pos0);
        intptr_t b = 0, e;
        if (r->rbegin != KORB_NIL && UNLIKELY(!korb_to_index(r->rbegin, &b)))   /* beginless → 0 */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += n;
        if (b < 0) b = 0;
        if (r->rend == KORB_NIL) {                       /* endless range → to end of array */
            beg = b; len = n - b; have_len = true;
        } else {
            if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
            if (e < 0) e += n;
            beg = b; len = (r->exclude_end ? e - 1 : e) - b + 1; have_len = true;
        }
    } else {
        if (pos0 != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(pos0, &beg))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(pos0));
        }
        VALUE pos1 = VALUE_SLICE_LEN(a) > base + 1 ? VALUE_SLICE_GET(a, base + 1) : KORB_NIL;
        if (pos1 != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(pos1, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(pos1));
            have_len = true;
        }
        if (beg < 0) { beg += n; if (beg < 0) beg = 0; }
        if (!have_len) len = n - beg;                    /* default: to end of array */
    }
    if (len < 0) len = 0;
    slots[0] = v;                                        /* root value across any grow */
    for (intptr_t i = beg; i < beg + len; i++) {
        if (i < 0) continue;
        if (block != NULL) {
            VALUE iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;                          /* root before grow GC */
        }
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if ((uint32_t)i >= ary->len) {
            CHECK(korb_ary_ensure(c, slots + 1, self, (uint32_t)i + 1 - ary->len));
            ary = VAL2ARY(VALUE_REF_GET(self));
            for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ARO_STORE(c, ary->items, &ary->items->data[k], KORB_NIL);
            ary->len = (uint32_t)i + 1;
        }
        ary = VAL2ARY(VALUE_REF_GET(self));
        ARO_STORE(c, ary->items, &ary->items->data[i], slots[0]);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash dig / values_at / slice / except */
static RESULT korb_m_hash_dig(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    VALUE cur = VALUE_REF_GET(self);
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
        VALUE key = VALUE_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        if (KORB_HASH_P(cur)) { int32_t idx = korb_hash_find(VAL2HASH(cur), key); cur = idx < 0 ? KORB_NIL : VAL2HASH(cur)->items->data[2 * idx + 1]; }
        else if (KORB_ARRAY_P(cur)) { intptr_t i; if (UNLIKELY(!korb_to_index(key, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(key)); KorbArray *ar = VAL2ARY(cur); if (i < 0) i += ar->len; cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : ar->items->data[i]; }
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(cur));
    }
    return RESULT_OK(cur);
}
static RESULT korb_m_hash_values_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t k = VALUE_SLICE_LEN(a);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, j));
        CHECK(korb_ary_push_val(c, slots + 1, dst, idx < 0 ? h->default_val : h->items->data[2 * idx + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* slice (keep==true) / except (keep==false) the listed keys */
static RESULT korb_hash_pick(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool keep) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        VALUE key = h->items->data[2 * i];
        bool listed = false;
        for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) if (korb_value_eq(VALUE_SLICE_GET(a, j), key)) { listed = true; break; }
        if (listed == keep) {
            slots[0] = key;                               /* root key (scratch above dst) */
            VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* build an array of the first `limit` pairs ([k,v]) of the hash. */
static RESULT korb_hash_first_n(CTX *c, VALUE *slots, VALUE_REF self, uint32_t limit) {
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    if (n > limit) n = limit;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[0] = h->items->data[2 * i];
        slots[1] = h->items->data[2 * i + 1];
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    return korb_hash_first_n(c, slots, self, (uint32_t)n);
}
static RESULT korb_m_hash_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) return korb_m_hash_take(c, slots, self, a);
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));   /* no arg → first pair or nil */
    if (h->len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = h->items->data[0];
    slots[1] = h->items->data[1];
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_clear(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KORB_HASH_DROP_INDEX(VAL2HASH(VALUE_REF_GET(self)));
    VAL2HASH(VALUE_REF_GET(self))->len = 0;     /* drop all pairs (payload kept, becomes garbage) */
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_default_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE v = VALUE_SLICE_GET(a, 0);
    ARO_STORE(c, VAL2HASH(VALUE_REF_GET(self)), (VALUE *)(uintptr_t)&VAL2HASH(VALUE_REF_GET(self))->default_val, v);
    return RESULT_OK(v);
}
static RESULT korb_m_hash_compact_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    KorbArrayItems *it = h->items;
    uint32_t w = 0;
    for (uint32_t i = 0; i < h->len; i++) {
        if (it->data[2*i+1] == KORB_NIL) continue;
        if (w != i) { ARO_STORE(c, it, &it->data[2*w], it->data[2*i]); ARO_STORE(c, it, &it->data[2*w+1], it->data[2*i+1]); }
        w++;
    }
    if (w == h->len) return RESULT_OK(KORB_NIL);          /* unchanged → nil */
    h->len = w;
    KORB_HASH_DROP_INDEX(h);                              /* pairs compacted → index stale */
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_transform_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#transform_values without a block is not supported");
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(VALUE_REF_GET(self))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);   /* yield value */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        CHECK(korb_hash_set(c, slots + 4, dst, VALUE_REF_AT(&slots[1]), slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Build a key-transformed copy of self into a fresh hash (returned).  An optional
 * Hash arg renames listed keys; remaining keys go through the block; with neither
 * the key is kept.  Later collisions overwrite (last wins). */
static RESULT korb_hash_xform_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const VALUE mapv = (VALUE_SLICE_LEN(a) >= 1) ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    if (mapv != KORB_NIL && !KORB_HASH_P(mapv))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Hash)", korb_type_name(mapv));
    if (UNLIKELY(mapv == KORB_NIL && block == NULL))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#transform_keys without a block is not supported");
    slots[0] = mapv;                                           /* root the rename map BEFORE any alloc moves it */
    slots[1] = UNWRAP(korb_hash_new(c, slots + 1, VAL2HASH(VALUE_REF_GET(self))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2 * i];                      /* old key */
        slots[3] = h->items->data[2 * i + 1];                  /* value */
        slots[4] = slots[2];                                   /* new key (default = old) */
        bool mapped = false;
        if (slots[0] != KORB_NIL) {
            int32_t j = korb_hash_find(VAL2HASH(slots[0]), slots[2]);
            if (j >= 0) { slots[4] = VAL2HASH(slots[0])->items->data[2 * j + 1]; mapped = true; }
        }
        if (!mapped && block != NULL) {
            RESULT r = korb_block_yield(c, slots + 5, block, def_env, &slots[2], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[4] = r.value;
        }
        CHECK(korb_hash_set(c, slots + 5, dst, VALUE_REF_AT(&slots[4]), slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_transform_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_hash_xform_keys(c, slots, self, a, block, def_env, cself);
}
/* transform_keys! — rebuild into a temp, then replace self's contents. */
static RESULT korb_m_hash_transform_keys_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_hash_xform_keys(c, slots + 1, self, a, block, def_env, cself));   /* result rooted at slots[0] */
    VALUE_REF res = VALUE_REF_AT(&slots[0]);
    KORB_HASH_DROP_INDEX(VAL2HASH(VALUE_REF_GET(self)));
    VAL2HASH(VALUE_REF_GET(self))->len = 0;                     /* clear self (payload kept) */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *r = VAL2HASH(VALUE_REF_GET(res));
        if (i >= r->len) break;
        slots[1] = r->items->data[2 * i];                      /* key */
        VALUE val = VAL2HASH(VALUE_REF_GET(res))->items->data[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, self, VALUE_REF_AT(&slots[1]), val));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* transform_values! — replace each value in place with block(value); keys unchanged. */
static RESULT korb_m_hash_transform_values_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#transform_values! without a block is not supported");
    for (uint32_t i = 0; ; i++) {
        KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2 * i + 1];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        h = VAL2HASH(VALUE_REF_GET(self));                 /* re-read after yield */
        ARO_STORE(c, h->items, &h->items->data[2 * i + 1], r.value);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_hash_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));   /* all pairs, then Array#minmax */
    return korb_m_ary_minmax(c, slots + 1, VALUE_REF_AT(&slots[0]), a, NULL, NULL, KORB_NIL);
}
static RESULT korb_m_hash_default_proc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2HASH(VALUE_REF_GET(self))->default_proc);
}
static RESULT korb_m_hash_default_proc_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE p = VALUE_SLICE_GET(a, 0);
    if (p != KORB_NIL && !KORB_PROC_P(p)) {                   /* coerce via #to_proc, else TypeError */
        const uint32_t to_proc = korb_intern(c->vm, "to_proc", 7);
        if (!korb_responds_to(c, p, to_proc))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Proc", korb_type_name(p));
        slots[0] = p;
        RESULT pr = korb_send_impl(c, slots + 1, to_proc, 0, 0, NULL, NULL, KORB_NIL);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        if (UNLIKELY(!KORB_PROC_P(pr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Proc", korb_type_name(slots[0]));
        p = pr.value;
    }
    KorbHash *const h = VAL2HASH(VALUE_REF_GET(self));        /* re-read after the dispatch */
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->default_proc, p);
    ARO_STORE(c, h, (VALUE *)(uintptr_t)&h->default_val, KORB_NIL);   /* setting a proc clears the static default */
    return RESULT_OK(VALUE_SLICE_GET(a, 0));   /* a setter call evaluates to its argument */
}
static RESULT korb_m_hash_compact(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(VALUE_REF_GET(self))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        if (h->items->data[2*i+1] == KORB_NIL) continue;      /* drop nil values */
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[1]), slots[2]));
    }
    KorbHash *const dh = VAL2HASH(VALUE_REF_GET(dst));        /* retain default value / default_proc */
    const KorbHash *const sh = VAL2HASH(VALUE_REF_GET(self));
    ARO_STORE(c, dh, (VALUE *)(uintptr_t)&dh->default_val, sh->default_val);
    ARO_STORE(c, dh, (VALUE *)(uintptr_t)&dh->default_proc, sh->default_proc);
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_flatten(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t depth = 1;                                   /* default flattens one level (pairs → k,v,...) */
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) (void)korb_to_index(VALUE_SLICE_GET(a, 0), &depth);
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n * 2)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[0] = h->items->data[2 * i];
        slots[1] = h->items->data[2 * i + 1];
        if (depth == 0) {                                /* depth 0 only: keep [k,v] pairs (negative = full) */
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[0]));
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    }
    if (depth > 1 || depth < 0) {                         /* flatten the k/v values further */
        slots[0] = LONG2FIX(depth < 0 ? -1 : depth - 1);
        return korb_m_ary_flatten(c, slots + 1, dst, VALUE_SLICE_MAKE(&slots[0], 1));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Hash#invert — new hash with keys and values swapped (later dups win, like CRuby). */
static RESULT korb_m_hash_invert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(VALUE_REF_GET(self))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2*i];                /* old key → new value */
        slots[2] = h->items->data[2*i+1];              /* old value → new key */
        CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[2]), slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Hash#rehash — our hash has no cached digests, so this is a no-op returning self. */
static RESULT korb_m_hash_rehash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self));
}
/* Hash#replace(other) — replace self's contents with other's, in place; returns self. */
static RESULT korb_m_hash_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) {                            /* coerce via #to_hash (Hash subclasses are already KORB_HASH_P) */
        const uint32_t to_hash = korb_intern(c->vm, "to_hash", 7);
        if (KORB_OBJECT_P(ov) && korb_responds_to(c, ov, to_hash)) {
            slots[0] = ov;
            RESULT hr = korb_send_impl(c, slots + 1, to_hash, 0, 0, NULL, NULL, KORB_NIL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            ov = hr.value;
        }
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));            /* re-check (self could move under the dispatch) */
    KORB_HASH_DROP_INDEX(VAL2HASH(VALUE_REF_GET(self)));
    VAL2HASH(VALUE_REF_GET(self))->len = 0;                       /* clear, then copy other's pairs */
    slots[0] = ov;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *oh = VAL2HASH(slots[0]);
        if (i >= oh->len) break;
        slots[1] = oh->items->data[2 * i];
        VALUE val = VAL2HASH(slots[0])->items->data[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, self, VALUE_REF_AT(&slots[1]), val));
    }
    /* replace also transfers the source's default value + default_proc. */
    KorbHash *const dst = VAL2HASH(VALUE_REF_GET(self));
    const VALUE dv = VAL2HASH(slots[0])->default_val;
    const VALUE dp = VAL2HASH(slots[0])->default_proc;
    ARO_STORE(c, dst, (VALUE *)(uintptr_t)&dst->default_val, dv);
    ARO_STORE(c, dst, (VALUE *)(uintptr_t)&dst->default_proc, dp);
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Hash#drop_while { |k,v| } — drop leading pairs while the block is true, return
 * the remaining pairs as an Array of [k,v]. */
static RESULT korb_m_hash_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block given");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                 /* result pairs (rooted) */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    bool dropping = true;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2 * i];                        /* k */
        slots[2] = h->items->data[2 * i + 1];                    /* v */
        if (dropping) {
            VALUE argv[2] = { slots[1], slots[2] };
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) continue;                  /* still dropping */
            dropping = false;
        }
        slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));        /* [k,v] pair */
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, out, slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(out));
}
static RESULT korb_m_ary_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_hash_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
/* Hash#tally — Enumerable#tally over [k,v] pairs (each unique → count 1). */
static RESULT korb_m_hash_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_m_hash_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));   /* pairs array */
    return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_hash_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[0] = h->items->data[2 * i];      /* k */
        slots[1] = h->items->data[2 * i + 1];  /* v */
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_hash_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* Enumerable#uniq over [k,v] pairs */
    slots[0] = UNWRAP(korb_m_hash_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_uniq(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_hash_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_hash_pick(c, slots, self, a, true); }
static RESULT korb_m_hash_except(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_hash_pick(c, slots, self, a, false); }

/* yield (k, v) to a Hash block: np>=2 → two args; else a single [k, v] pair. */
static RESULT korb_hash_yield(CTX *c, VALUE *slots, NODE *block, VALUE *def_env, VALUE *cself, uint32_t np, VALUE k, VALUE v) {
    if (np >= 2) { VALUE argv[2] = { k, v }; return korb_block_yield(c, slots, block, def_env, argv, 2, cself); }
    slots[0] = k; slots[1] = v;
    VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
    slots[2] = pair;
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    VALUE parg = slots[2];
    return korb_block_yield(c, slots + 3, block, def_env, &parg, 1, cself);
}
#define HASH_REQ_BLOCK(what) do { if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, what " without a block is not supported"); } while (0)

/* Hash#reverse_each — yield (k,v) pairs in reverse insertion order; return self. */
static RESULT korb_m_hash_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; HASH_REQ_BLOCK("Hash#reverse_each");
    const uint32_t np = korb_entry_params_cnt(block);
    uint32_t i = VAL2HASH(VALUE_REF_GET(self))->len;
    while (i > 0) {
        i--;
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) continue;                              /* shrunk during iteration */
        CHECK(korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2 * i], h->items->data[2 * i + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#each_with_index — yield ([k,v], index); return self. */
static RESULT korb_m_hash_each_with_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) {                  /* → Enumerator of [[k,v], index]: build [k,v] pairs, delegate to Array#each_with_index */
        slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2HASH(VALUE_REF_GET(self))->len));
        VALUE_REF pairs = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            slots[1] = h->items->data[2 * i];
            slots[2] = h->items->data[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, pairs, slots[3]));
        }
        return korb_m_ary_each_wi(c, slots + 1, pairs, a, NULL, def_env, cself);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2 * i];                      /* k (rooted) */
        slots[1] = h->items->data[2 * i + 1];                  /* v (rooted) */
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        VALUE argv[2] = { slots[2], LONG2FIX((intptr_t)i) };
        CHECK(korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#cycle([n]) — yield (k,v) pairs n times (forever if n omitted); return nil. */
static RESULT korb_m_hash_cycle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    HASH_REQ_BLOCK("Hash#cycle");
    const uint32_t np = korb_entry_params_cnt(block);
    bool bounded = VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL;
    intptr_t n = 0;
    if (bounded && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (bounded && n <= 0) return RESULT_OK(KORB_NIL);
    if (VAL2HASH(VALUE_REF_GET(self))->len == 0) return RESULT_OK(KORB_NIL);
    for (intptr_t pass = 0; !bounded || pass < n; pass++) {
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            CHECK(korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2 * i], h->items->data[2 * i + 1]));
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_hash_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; HASH_REQ_BLOCK("Hash#flat_map");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) {                 /* flatten one level */
            slots[1] = r.value;
            uint32_t sublen = VAL2ARY(slots[1])->len;
            for (uint32_t j = 0; j < sublen; j++)
                CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(slots[1])->items->data[j]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; HASH_REQ_BLOCK("Hash#map");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* select(keep=1)/reject(keep=0) → new Hash */
static RESULT korb_hash_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#select/reject without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        VALUE k = h->items->data[2*i], v = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, k, v);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) {
            /* re-read BOTH key and value fresh from self: the block ran (and may
             * have GC'd, moving the heap key/value) — the pre-yield `k`/`v` locals
             * are stale.  (v was already re-read; k was not → STRESS+PURGE SEGV.) */
            const KorbHash *const hh = VAL2HASH(VALUE_REF_GET(self));
            slots[0] = hh->items->data[2*i];
            VALUE vv = hh->items->data[2*i+1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), vv));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_hash_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, false); }
/* any?(0)/all?(1)/none?(2) */
static RESULT korb_hash_quant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, int mode) {
    if (VALUE_SLICE_LEN(a) >= 1) {                        /* pattern === [k,v] pair */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            bool t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[2]);
            if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
            if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
            if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
    }
    if (block == NULL) {                                  /* pairs are always truthy */
        uint32_t len = VAL2HASH(VALUE_REF_GET(self))->len;
        if (mode == 0) return RESULT_OK(len > 0 ? KORB_TRUE : KORB_FALSE);   /* any? */
        if (mode == 2) return RESULT_OK(len == 0 ? KORB_TRUE : KORB_FALSE);  /* none? */
        return RESULT_OK(KORB_TRUE);                                          /* all? */
    }
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, captured_self, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        bool t = KORB_TRUTHY(r.value);
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_hash_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_hash_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_hash_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 2); }
/* in-place select!/filter!(keep_truthy=1) and reject!/delete_if(keep_truthy=0) */
static RESULT korb_hash_filter_bang(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, bool keep_truthy, bool ret_nil_if_unchanged) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "in-place Hash filter without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; ; r++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (r >= h->len) break;
        slots[0] = h->items->data[2*r];                    /* root k,v across the yield */
        slots[1] = h->items->data[2*r+1];
        RESULT res = korb_hash_yield(c, slots + 2, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        if (KORB_TRUTHY(res.value) == keep_truthy) {
            KorbHash *h2 = VAL2HASH(VALUE_REF_GET(self));
            if (w != r) {
                ARO_STORE(c, h2->items, &h2->items->data[2*w],   slots[0]);
                ARO_STORE(c, h2->items, &h2->items->data[2*w+1], slots[1]);
            }
            w++;
        } else changed = true;
    }
    KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    for (uint32_t r = 2*w; r < 2*h->len; r++) ARO_STORE(c, h->items, &h->items->data[r], KORB_NIL);
    h->len = w;
    KORB_HASH_DROP_INDEX(h);                              /* pairs compacted → index stale */
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self)); (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, true); }
static RESULT korb_m_hash_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self)); (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, false); }
static RESULT korb_m_hash_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self)); (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, true); }
static RESULT korb_m_hash_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self)); (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, false); }
static RESULT korb_hash_pair_at(CTX *c, VALUE *slots, VALUE_REF self, uint32_t i, VALUE *out);
static RESULT korb_m_hash_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    if (!has_pat && UNLIKELY(block == NULL)) {        /* no block, no pattern → exactly one pair */
        return RESULT_OK(VAL2HASH(VALUE_REF_GET(self))->len == 1 ? KORB_TRUE : KORB_FALSE);
    }
    uint32_t np = block ? korb_entry_params_cnt(block) : 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        bool t;
        if (has_pat) {
            VALUE pair; CHECK(korb_hash_pair_at(c, slots, self, i, &pair));
            t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[2]);
        } else {
            RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        }
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
/* Build a fresh [k,v] pair array at *out (a rooted slot). cursor = scratch above it. */
static RESULT korb_hash_make_pair(CTX *c, VALUE *cursor, VALUE *kslot, VALUE *vslot, VALUE *out) {
    *out = UNWRAP(korb_ary_new(c, cursor, 2));
    CHECK(korb_ary_push_val(c, cursor + 1, VALUE_REF_AT(out), *kslot));
    CHECK(korb_ary_push_val(c, cursor + 1, VALUE_REF_AT(out), *vslot));
    return RESULT_OK(*out);
}
/* sort_by → array of [k,v] pairs sorted by block key */
static RESULT korb_m_hash_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#sort_by without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF vals = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF keys = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));  /* pair at slots[4] */
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[5] = r.value;
        CHECK(korb_ary_push_val(c, slots + 6, vals, slots[4]));
        CHECK(korb_ary_push_val(c, slots + 6, keys, slots[5]));
    }
    KorbArray *vd = VAL2ARY(VALUE_REF_GET(vals)), *kd = VAL2ARY(VALUE_REF_GET(keys));
    KorbArrayItems *const vit = vd->items, *const kit = kd->items;
    const VALUE *vdat = vit->data, *kdat = kit->data;
    for (uint32_t i = 1; i < vd->len; i++) {
        VALUE vk = vdat[i], kk = kdat[i]; uint32_t j = i;
        while (j > 0) {
            int cmp = korb_cmp_full(c, kdat[j-1], kk);
            if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
            if (cmp <= 0) break;
            ARO_STORE(c, vit, &vdat[j], vdat[j-1]); ARO_STORE(c, kit, &kdat[j], kdat[j-1]); j--;
        }
        ARO_STORE(c, vit, &vdat[j], vk); ARO_STORE(c, kit, &kdat[j], kk);
    }
    return RESULT_OK(VALUE_REF_GET(vals));
}
/* min_by/max_by → the [k,v] pair with the extreme block key */
static RESULT korb_hash_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#min_by/max_by without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = KORB_NIL; slots[1] = KORB_NIL; bool have = false;   /* best pair / best key */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[5] = r.value;
        if (!have) { slots[0] = slots[4]; slots[1] = slots[5]; have = true; continue; }
        int cmp = korb_cmp_full(c, slots[5], slots[1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want < 0 && cmp < 0) || (want > 0 && cmp > 0)) { slots[0] = slots[4]; slots[1] = slots[5]; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_hash_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_hash_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself,  1); }
static RESULT korb_m_hash_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_minmax_by(c, slots, self, block, def_env, cself, -1));      /* min pair */
    slots[1] = UNWRAP(korb_hash_minmax_by(c, slots + 1, self, block, def_env, cself, 1));   /* max pair */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
/* filter_map → collect truthy block results */
static RESULT korb_m_hash_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#filter_map without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* partition → [yes_pairs, no_pairs] */
static RESULT korb_m_hash_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#partition without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        VALUE_REF yes = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    VALUE_REF no  = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = h->items->data[2*i]; slots[3] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 5, KORB_TRUTHY(r.value) ? yes : no, slots[4]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    VALUE_REF out = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(VALUE_REF_GET(out));
}
/* find/detect → the first [k,v] pair whose block is truthy, else nil */
static RESULT korb_m_hash_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* find_all/select(Enumerable) → array of [k,v] pairs where block truthy */
static RESULT korb_m_hash_find_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find_all without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* grep/grep_v(pattern): pairs where (pattern === [k,v]) == keep, as [k,v] array. */
static RESULT korb_hash_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool keep) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));   /* pair at slots[2] */
        if (korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[2]) == keep)
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_hash_grep(c, slots, self, a, true); }
static RESULT korb_m_hash_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_hash_grep(c, slots, self, a, false); }
/* zip → array of rows: [ [k,v], other0[i], other1[i], ... ] per pair i */
/* i-th element of a zip source arg: Array → data[i], Range → lo+i (in bounds), else nil. */
static VALUE korb_zip_elem(VALUE arg, uint32_t i) {
    if (KORB_ARRAY_P(arg)) return i < VAL2ARY(arg)->len ? VAL2ARY(arg)->items->data[i] : KORB_NIL;
    if (KORB_RANGE_P(arg)) { intptr_t lo, hi; if (korb_range_int_bounds(VAL2RANGE(arg), &lo, &hi)) { intptr_t v = lo + (intptr_t)i; if (v < hi) return LONG2FIX(v); } }
    if (KORB_ARITHSEQ_P(arg)) {   /* e.g. n.upto(∞): begin + i*step, lazily by index */
        VALUE beginv, limv, stepv; bool excl;
        korb_aseq_params(VAL2ASEQ(arg), &beginv, &limv, &stepv, &excl);
        if (FIXNUM_P(beginv) && FIXNUM_P(stepv)) {
            const intptr_t v = FIX2LONG(beginv) + (intptr_t)i * FIX2LONG(stepv);
            if (limv == KORB_NIL) return LONG2FIX(v);                       /* endless */
            double dl;
            if (korb_num_to_d(limv, &dl) && isinf(dl) && dl > 0) return LONG2FIX(v);   /* +∞ limit */
            if (FIXNUM_P(limv) && v < FIX2LONG(limv)) return LONG2FIX(v);   /* finite (exclusive-ish) */
        }
    }
    return KORB_NIL;
}
static RESULT korb_m_hash_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t k = VALUE_SLICE_LEN(a);
    uint32_t n = VAL2HASH(VALUE_REF_GET(self))->len;
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, n)) : KORB_NIL;   /* dst (unused w/ block) */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[1], &slots[2], &slots[3]));  /* pair at slots[3] */
        slots[4] = UNWRAP(korb_ary_new(c, slots + 5, k + 1));                /* row at slots[4] */
        VALUE_REF row = VALUE_REF_AT(&slots[4]);
        CHECK(korb_ary_push_val(c, slots + 5, row, slots[3]));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 5, row, korb_zip_elem(VALUE_SLICE_GET(a, j), i)));
        if (block != NULL) {
            slots[5] = (k == 0) ? slots[3] : slots[4];   /* no other args → yield the bare [k,v] pair */
            RESULT r = korb_block_yield(c, slots + 6, block, def_env, &slots[5], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 5, dst, slots[4]));
        }
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}
/* max/min over [k,v] pairs (Array#<=>: key then value). want: 1 max, -1 min. */
static RESULT korb_hash_minmax(CTX *c, VALUE *slots, VALUE_REF self, int want) {
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    if (h->len == 0) return RESULT_OK(KORB_NIL);
    uint32_t best = 0;
    for (uint32_t i = 1; i < h->len; i++) {
        h = VAL2HASH(VALUE_REF_GET(self));
        int kc = korb_cmp_full(c, h->items->data[2*i], h->items->data[2*best]);
        int cmp = (kc != 0) ? kc : korb_cmp_full(c, h->items->data[2*i+1], h->items->data[2*best+1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want > 0 && cmp > 0) || (want < 0 && cmp < 0)) best = i;
    }
    h = VAL2HASH(VALUE_REF_GET(self));
    slots[0] = h->items->data[2*best]; slots[1] = h->items->data[2*best+1];
    CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) {  /* max(n) → n largest pairs */
        slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));
        return korb_ary_minmax_n(c, slots + 1, VALUE_REF_AT(&slots[0]), 1, n);
    }
    return korb_hash_minmax(c, slots, self,  1);
}
static RESULT korb_m_hash_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t n;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(VALUE_SLICE_GET(a, 0), &n)) {  /* min(n) → n smallest pairs */
        slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));
        return korb_ary_minmax_n(c, slots + 1, VALUE_REF_AT(&slots[0]), -1, n);
    }
    return korb_hash_minmax(c, slots, self, -1);
}
/* group_by → Hash{ block_key => [[k,v], ...] } over pairs */
static RESULT korb_m_hash_group_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a; if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#group_by without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));     VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *hh = VAL2HASH(VALUE_REF_GET(self));
        if (i >= hh->len) break;
        slots[1] = hh->items->data[2*i]; slots[2] = hh->items->data[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[1], &slots[2], &slots[3]));   /* pair at slots[3] */
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[1], slots[2]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[4] = r.value;                            /* key */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[4]);
        if (idx < 0) {
            slots[5] = UNWRAP(korb_ary_new(c, slots + 6, 4));
            CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[3]));
            CHECK(korb_hash_set(c, slots + 6, h, VALUE_REF_AT(&slots[4]), slots[5]));
        } else {
            slots[5] = VAL2HASH(VALUE_REF_GET(h))->items->data[2*idx+1];
            CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[3]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
/* find_index → integer index of first pair where block truthy, else nil */
/* build [k,v] for pair i into slots[base..]; returns the pair VALUE rooted at slots[base+2]. */
static RESULT korb_hash_pair_at(CTX *c, VALUE *slots, VALUE_REF self, uint32_t i, VALUE *out) {
    const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
    slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    *out = slots[2];
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* find_index(pair): first index of matching [k,v] */
        for (uint32_t i = 0; i < VAL2HASH(VALUE_REF_GET(self))->len; i++) {
            VALUE pair; CHECK(korb_hash_pair_at(c, slots, self, i, &pair));
            if (korb_value_eq(slots[2], VALUE_SLICE_GET(a, 0))) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#find_index without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block != NULL && VALUE_SLICE_LEN(a) == 0) {   /* count { |k,v| ... } */
        uint32_t np = korb_entry_params_cnt(block);
        intptr_t n = 0;
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* count(pair): pairs == arg */
        intptr_t n = 0;
        for (uint32_t i = 0; i < VAL2HASH(VALUE_REF_GET(self))->len; i++) {
            VALUE pair; CHECK(korb_hash_pair_at(c, slots, self, i, &pair));
            if (korb_value_eq(slots[2], VALUE_SLICE_GET(a, 0))) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    return RESULT_OK(LONG2FIX(VAL2HASH(VALUE_REF_GET(self))->len));
}
static RESULT korb_m_hash_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    uint32_t op_mid;
    /* A trailing operator Symbol/String selects the symbol form, overriding any block. */
    const uint32_t na0 = VALUE_SLICE_LEN(a);
    const bool sym_form = na0 >= 1 && korb_reduce_op(c, VALUE_SLICE_GET(a, na0 - 1), &op_mid);
    if (block == NULL && !sym_form)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
    if (sym_form) {                               /* reduce(:+) / reduce(init, :+) on [k,v] pairs [block ignored] */
        uint32_t na = na0;
        bool have_acc = (na >= 2);
        if (have_acc) slots[0] = VALUE_SLICE_GET(a, 0);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));   /* [k,v] pair */
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
            if (!have_acc) { slots[0] = slots[3]; have_acc = true; continue; }
            slots[4] = slots[0]; slots[5] = slots[3];           /* recv=acc, arg=pair */
            RESULT r = korb_send_impl(c, slots + 6, op_mid, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(have_acc ? slots[0] : KORB_NIL);
    }
    uint32_t np = korb_entry_params_cnt(block);   /* acc + pair: block takes |acc, (k,v)| */
    bool have_acc = (VALUE_SLICE_LEN(a) >= 1);     /* no init → first [k,v] pair seeds the accumulator */
    if (have_acc) slots[0] = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        /* build [k,v] pair and yield (acc, pair) */
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
        if (!have_acc) { slots[0] = slots[3]; have_acc = true; continue; }
        VALUE argv[2] = { slots[0], slots[3] };
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, np >= 2 ? 2 : 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(have_acc ? slots[0] : KORB_NIL);
}
static RESULT korb_m_hash_each_wo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    HASH_REQ_BLOCK("Hash#each_with_object");
    if (VALUE_SLICE_LEN(a) < 1) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    slots[0] = VALUE_SLICE_GET(a, 0);             /* memo */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = h->items->data[2*i]; slots[2] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[2]));
        VALUE argv[2] = { slots[3], slots[0] };   /* (pair, memo) */
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
#undef HASH_REQ_BLOCK

