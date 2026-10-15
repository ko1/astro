/* koruby_precise — array_int_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Array (query/mutate) + Integer (bit) methods ------------------- */

static RESULT korb_m_ary_unshift(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    uint32_t k = KORB_SLICE_LEN(a);
    if (k == 0) return RESULT_OK(KORB_REF_GET(self));
    CHECK(korb_ary_ensure(c, slots, self, k));
    KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
    void *const it = korb_ary_owner(ary); korb_value *const it_d = korb_ary_ptr(ary);
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)        /* shift right by k */
        ARO_STORE(c, it, &it_d[i + k], it_d[i]);
    for (uint32_t i = 0; i < k; i++)
        ARO_STORE(c, it, &it_d[i], KORB_SLICE_GET(a, i));
    ary->len += k;
    return RESULT_OK(KORB_REF_GET(self));
}

static RESULT korb_m_ary_shift(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", (unsigned)KORB_SLICE_LEN(a));
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    if (KORB_SLICE_LEN(a) >= 1) {                    /* shift(n): remove & return first n as array */
        korb_sword_t n;
        korb_value nv = KORB_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(nv, &n))) {       /* coerce the count via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n)) return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, 0));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        if (n > (korb_sword_t)VAL2ARY(KORB_REF_GET(self))->len) n = (korb_sword_t)VAL2ARY(KORB_REF_GET(self))->len;   /* clamp before narrowing */
        const uint32_t take = (uint32_t)n;
        korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = 0; i < take; i++)
            CHECK(korb_ary_push_val(c, slots + 1, dst, korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i]));
        KorbArray *ary2 = VAL2ARY(KORB_REF_GET(self));   /* now compact self in place */
        void *const it2 = korb_ary_owner(ary2); korb_value *const it2_d = korb_ary_ptr(ary2);
        for (uint32_t i = take; i < ary2->len; i++) ARO_STORE(c, it2, &it2_d[i - take], it2_d[i]);
        for (uint32_t i = ary2->len - take; i < ary2->len; i++) ARO_STORE(c, it2, &it2_d[i], KORB_NIL);
        ary2->len -= take;
        return RESULT_OK(KORB_REF_GET(dst));
    }
    KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    void *const it = korb_ary_owner(ary); korb_value *const it_d = korb_ary_ptr(ary);
    korb_value first = it_d[0];
    for (uint32_t i = 1; i < ary->len; i++) ARO_STORE(c, it, &it_d[i - 1], it_d[i]);
    ary->len--;
    ARO_STORE(c, it, &it_d[ary->len], KORB_NIL);
    return RESULT_OK(first);
}

/* assoc (idx 0) / rassoc (idx 1): find the sub-array whose [idx] == key */
static RESULT korb_ary_assoc(CTX *c, korb_value *slots, korb_ref self, korb_value key, uint32_t idx) {
    slots[0] = key;                                          /* root key across element == dispatches */
    const uint32_t to_ary = korb_intern_lit(c->vm, "to_ary", 6);
    const uint32_t n = VAL2ARY(KORB_REF_GET(self))->len;
    const bool eqr = KORB_EQ_REDEF(c);   /* read once: the loop calls out of line */
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(self)))[i];   /* element (rooted; returned on match) */
        if (!KORB_ARRAY_P(slots[1])) {                       /* coerce a non-Array element via #to_ary */
            if (!KORB_OBJECT_P(slots[1]) || !korb_responds_to_coerce(c, slots + 2, slots[1], to_ary)) continue;
            RESULT cr = korb_send_impl(c, slots + 2, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!KORB_ARRAY_P(cr.value)) continue;
            slots[1] = cr.value;
        }
        if (VAL2ARY(slots[1])->len <= idx) continue;
        const korb_value el = korb_ary_ptr(VAL2ARY(slots[1]))[idx];
        if (korb_eq_send_p_(eqr, el, slots[0])) {  /* user == → dispatch (el == key) */
            slots[2] = el; slots[3] = slots[0];              /* recv, arg */
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(slots[1]);
        } else if (korb_value_eq(el, slots[0])) {
            return RESULT_OK(slots[1]);
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_assoc(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_ary_assoc(c, slots, self, KORB_SLICE_GET(a, 0), 0); }
static RESULT korb_m_ary_rassoc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_ary_assoc(c, slots, self, KORB_SLICE_GET(a, 0), 1); }

static RESULT korb_m_ary_fetch(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const bool have_default = KORB_SLICE_LEN(a) >= 2;
    if (UNLIKELY(have_default && block != NULL)) korb_warn_caller(c, slots, a.p + KORB_FLINK_OFF, "block supersedes default value argument");
    slots[0] = KORB_SLICE_GET(a, 0);                     /* original index arg (rooted; yielded to the block as-is) */
    slots[1] = have_default ? KORB_SLICE_GET(a, 1) : KORB_NIL;
    korb_sword_t i;
    if (UNLIKELY(!korb_to_index(slots[0], &i))) {         /* coerce a non-Integer index via #to_int */
        korb_value iv2 = slots[0];
        RESULT cr = korb_coerce_to_int(c, slots + 2, &iv2);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv2, &i)) return korb_raise_no_int(c, slots, slots[0]);
    }
    const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));  /* re-read after possible dispatch */
    korb_sword_t orig = i;
    if (i < 0) i += ary->len;
    if (i >= 0 && (uint32_t)i < ary->len) return RESULT_OK(korb_ary_ptr(ary)[i]);
    if (block != NULL)                                    /* out of range: block yields the ORIGINAL arg (wins over a default) */
        return korb_block_yield(c, slots + 2, block, def_env, &slots[0], 1, cself);
    if (have_default) return RESULT_OK(slots[1]);
    return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld outside of array bounds: %ld...%u",
                      (long)orig, ary->len ? -(long)ary->len : 0L, ary->len);
}

/* Continue digging through a non-Hash/Array value: cur.dig(remaining keys) if it
 * defines #dig (e.g. a Struct or a user object), else TypeError. */
static RESULT korb_dig_dispatch(CTX *c, korb_value *slots, korb_value cur, korb_slice a, uint32_t k) {
    const uint32_t dig_id = korb_intern_lit(c->vm, "dig", 3);
    if (LIKELY(korb_responds_to(c, cur, dig_id))) {
        const uint32_t rest = KORB_SLICE_LEN(a) - k;
        slots[0] = cur;                                   /* receiver at base[-(rest+1)] */
        for (uint32_t j = 0; j < rest; j++) slots[1 + j] = KORB_SLICE_GET(a, k + j);   /* args at base[-rest..-1] */
        return korb_send_impl(c, slots + rest + 1, dig_id, 0, rest, NULL, NULL, NULL);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "%s does not have #dig method", korb_type_name(cur));
}
/* dig: recursive index into nested Array/Hash */
static RESULT korb_m_ary_dig(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    korb_value cur = KORB_REF_GET(self);
    for (uint32_t k = 0; k < KORB_SLICE_LEN(a); k++) {
        korb_value key = KORB_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        /* an intermediate value with its own class (singleton / builtin subclass)
         * may redefine #dig — CRuby dispatches instead of indexing directly */
        if (k > 0 && AROH_IS_GC_OBJECT(cur) && (((const AroObjectHeader *)(uintptr_t)cur)->flags & KORB_FL_HAS_KLASS))
            return korb_dig_dispatch(c, slots, cur, a, k);
        if (KORB_ARRAY_P(cur)) {
            korb_sword_t i;
            if (UNLIKELY(!korb_to_index(key, &i))) return korb_raise_no_int(c, slots, key);
            KorbArray *ar = VAL2ARY(cur);
            if (i < 0) i += ar->len;
            cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : korb_ary_ptr(ar)[i];
        } else if (KORB_HASH_P(cur)) {
            int32_t idx = korb_hash_find(VAL2HASH(cur), key);
            cur = idx < 0 ? KORB_NIL : korb_hash_pairs(VAL2HASH(cur))[2 * idx + 1];
        } else {
            return korb_dig_dispatch(c, slots, cur, a, k);   /* user object → cur.dig(rest...), else TypeError */
        }
    }
    return RESULT_OK(cur);
}

/* Coerce a shift amount to an Integer (Fixnum/Bignum) via #to_int, rooting self
 * in slots[0].  A Bignum amount that doesn't fit a word is an extreme shift:
 * right by a huge count → 0 (or -1 for negative self); left by a huge count of a
 * nonzero value → RangeError.  dir = +1 for `<<`, -1 for `>>`. */
static RESULT korb_int_shift_arg(CTX *c, korb_value *slots, korb_ref self, korb_value o, int dir, korb_sword_t *sh_out, bool *extreme, korb_value *ext_val) {
    slots[0] = KORB_REF_GET(self);
    *extreme = false;
    for (;;) {
        if (KORB_BIGNUM_P(o)) {                          /* amount doesn't fit a word → extreme shift */
            const int osign = korb_mp_sgn(VAL2BIG(o)->z);
            const bool right = (osign * dir) < 0;        /* left<<neg or right>>pos → shrink toward 0/-1 */
            const bool self_neg = KORB_BIGNUM_P(slots[0]) ? (korb_mp_sgn(VAL2BIG(slots[0])->z) < 0) : (FIX2LONG(slots[0]) < 0);
            const bool self_zero = !KORB_BIGNUM_P(slots[0]) && FIX2LONG(slots[0]) == 0;
            *extreme = true;
            if (right) { *ext_val = self_neg ? LONG2FIX(-1) : LONG2FIX(0); return RESULT_OK(KORB_NIL); }
            if (self_zero) { *ext_val = LONG2FIX(0); return RESULT_OK(KORB_NIL); }
            return korb_raise(c, slots, KORB_E_RANGE, 0, "shift width too big");
        }
        if (LIKELY(korb_to_index(o, sh_out))) return RESULT_OK(KORB_NIL);   /* Fixnum / Float / ... */
        /* otherwise coerce once via #to_int, then re-check (Bignum/Fixnum) */
        const uint32_t mid = korb_intern_lit(c->vm, "to_int", 6);
        if (!KORB_OBJECT_P(o) || !korb_responds_to(c, o, mid))   /* shift keeps the "of X into Integer" wording, nil included (CRuby) */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, o));
        slots[1] = o;
        RESULT r = korb_send_impl(c, slots + 2, mid, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (UNLIKELY(!KORB_INTEGER_P(r.value))) {
            const char *on = korb_coerce_name(c, slots[1]);
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Integer (%s#to_int gives %s)", on, on, korb_coerce_name(c, r.value));
        }
        o = r.value;                                     /* Fixnum or Bignum → loop once more */
    }
}
static RESULT korb_m_int_lshift(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t sh; bool extreme = false; korb_value ext = KORB_NIL;
    { RESULT cr = korb_int_shift_arg(c, slots, self, KORB_SLICE_GET(a, 0), +1, &sh, &extreme, &ext); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    if (extreme) return RESULT_OK(ext);
    if (KORB_BIGNUM_P(KORB_REF_GET(self))) return korb_int_shift(c, slots, KORB_REF_GET(self), sh);
    korb_sword_t n = FIX2LONG(KORB_REF_GET(self));
    korb_sword_t r = sh >= 0 ? (sh < 62 ? (n << sh) : 0) : (n >> (-sh < 63 ? -sh : 62));
    if (sh >= 0 && (sh >= 62 || (r >> sh) != n || !FIXABLE(r)))
        return korb_int_shift(c, slots, KORB_REF_GET(self), sh);
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_rshift(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t sh; bool extreme = false; korb_value ext = KORB_NIL;
    { RESULT cr = korb_int_shift_arg(c, slots, self, KORB_SLICE_GET(a, 0), -1, &sh, &extreme, &ext); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; }
    if (extreme) return RESULT_OK(ext);
    if (KORB_BIGNUM_P(KORB_REF_GET(self))) return korb_int_shift(c, slots, KORB_REF_GET(self), -sh);   /* x >> sh == shift by -sh */
    korb_sword_t n = FIX2LONG(KORB_REF_GET(self));
    if (sh >= 0)   /* arithmetic right shift — always fits in a Fixnum */
        return RESULT_OK(LONG2FIX(sh < 63 ? (n >> sh) : (n < 0 ? -1 : 0)));
    /* sh < 0 → left shift by -sh; may overflow into Bignum */
    const korb_sword_t ls = -sh;
    const korb_sword_t r = (ls < 62) ? (n << ls) : 0;
    if (ls >= 62 || (r >> ls) != n || !FIXABLE(r))
        return korb_int_shift(c, slots, KORB_REF_GET(self), ls);
    return RESULT_OK(LONG2FIX(r));
}
/* Integer#[] — bit reference: int[i] (single bit), int[i, len] (len-bit field),
 * int[range] (bits in range).  Two's-complement semantics for negatives. */
static RESULT korb_m_int_bitref(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value selfv = KORB_REF_GET(self);
    /* result = (self >> i), then: single_bit → &1 (n[i]); zero → 0; mask_len>0 →
     * low mask_len bits; mask_len==0 → all bits from the offset (endless/reversed
     * range, or n[i] with no length). */
    korb_sword_t i = 0, mask_len = 0; bool single_bit = false, zero = false;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_RANGE_P(KORB_SLICE_GET(a, 0))) {   /* int[i..j] */
        const KorbRange *rg = VAL2RANGE(KORB_SLICE_GET(a, 0));
        if (KORB_FLOAT_P(rg->rbegin) && isinf(korb_float_val(rg->rbegin)))   /* an Infinity boundary is out of domain */
            return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, korb_float_val(rg->rbegin) < 0 ? "-Infinity" : "Infinity");
        if (KORB_FLOAT_P(rg->rend) && isinf(korb_float_val(rg->rend)))
            return korb_raise(c, slots, KORB_E_FLOAT_DOMAIN, 0, korb_float_val(rg->rend) < 0 ? "-Infinity" : "Infinity");
        if (rg->rbegin == KORB_NIL) {                                       /* beginless (..j): result is 0 unless a bit in [0,j] is set */
            /* nz = self has any set bit in positions 0..j (two's-complement).
             * j<0 ⇒ empty range ⇒ 0; endless j ⇒ all bits ⇒ any nonzero self. */
            bool nz;
            const bool endless = (rg->rend == KORB_NIL);
            const bool small_j = !endless && FIXNUM_P(rg->rend) && FIX2LONG(rg->rend) < 62;
            if (small_j && FIXNUM_P(selfv)) {
                const korb_sword_t j = FIX2LONG(rg->rend);
                nz = (j < 0) ? false : (FIX2LONG(selfv) & ((((korb_sword_t)1) << (j + 1)) - 1)) != 0;
            } else if (small_j && FIX2LONG(rg->rend) < 0) {
                nz = false;                                                 /* Bignum self, empty range */
            } else {
                korb_mp_t zn; korb_to_mpz(selfv, zn);                            /* Fixnum or Bignum self, into two's-complement mpz */
                if (endless) {
                    nz = korb_mp_sgn(zn) != 0;
                } else {                                                    /* mask = (1<<(j+1))-1, nz = (self & mask) != 0 */
                    korb_mp_t mask, tmp; korb_mp_init(mask); korb_mp_init(tmp);
                    korb_mp_ui_pow_ui(mask, 2, (unsigned long)(FIX2LONG(rg->rend) + 1)); korb_mp_sub_ui(mask, mask, 1);
                    korb_mp_and(tmp, zn, mask); nz = korb_mp_sgn(tmp) != 0;
                    korb_mp_clear(mask); korb_mp_clear(tmp);
                }
                korb_mp_clear(zn);
            }
            if (nz) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "The beginless range for Integer#[] results in infinity");
            zero = true;                                                    /* not raised ⇒ bits [0,j] all clear ⇒ result is 0 */
        }
        i = (rg->rbegin == KORB_NIL) ? 0 : (FIXNUM_P(rg->rbegin) ? FIX2LONG(rg->rbegin) : 0);   /* beginless ⇒ 0 */
        if (rg->rend != KORB_NIL && FIXNUM_P(rg->rend)) {
            korb_sword_t len = FIX2LONG(rg->rend) - i + (rg->exclude_end ? 0 : 1);
            if (len > 0) mask_len = len;                                    /* reversed/empty ⇒ no upper mask */
        }                                                                  /* endless ⇒ no upper mask */
    } else {
        korb_value iv = KORB_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(iv, &i))) {                            /* coerce the bit index via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &iv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            selfv = KORB_REF_GET(self);                                    /* re-read: coercion may have moved a Bignum self */
            if (!korb_to_index(iv, &i)) {
                /* an index past `long` (Bignum, or a Float that big): every such
                 * bit is the sign bit; a negative one is below bit 0 → 0 */
                int sgn = 0;
                if (KORB_BIGNUM_P(iv)) sgn = korb_mp_sgn(VAL2BIG(iv)->z);
                else if (KORB_FLOAT_P(iv) && isfinite(korb_float_val(iv))) sgn = korb_float_val(iv) < 0 ? -1 : 1;
                else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
                if (sgn < 0 || KORB_SLICE_LEN(a) >= 2) return RESULT_OK(LONG2FIX(0));
                const bool neg = FIXNUM_P(selfv) ? FIX2LONG(selfv) < 0 : korb_mp_sgn(VAL2BIG(selfv)->z) < 0;
                return RESULT_OK(LONG2FIX(neg ? 1 : 0));
            }
        }
        if (KORB_SLICE_LEN(a) >= 2) {                                      /* n[i, len] */
            korb_sword_t len;
            korb_value lv = KORB_SLICE_GET(a, 1);
            if (UNLIKELY(!korb_to_index(lv, &len))) {
                RESULT cr = korb_coerce_to_int(c, slots, &lv);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(lv, &len)) return korb_raise_no_int(c, slots, lv);
                selfv = KORB_REF_GET(self);
            }
            if (len == 0) zero = true; else if (len > 0) mask_len = len;   /* len<0 ⇒ no upper mask (all bits) */
        } else single_bit = true;                                          /* n[i] */
    }
    if (LIKELY(FIXNUM_P(selfv) && single_bit)) {              /* n[i] on a Fixnum — bit test, no GMP alloc */
        const korb_sword_t n = FIX2LONG(selfv);
        if (i < 0)   return RESULT_OK(LONG2FIX(0));           /* bits below 0 don't exist */
        if (i >= 63) return RESULT_OK(LONG2FIX(n < 0 ? 1 : 0));   /* beyond Fixnum width = sign extension */
        return RESULT_OK(LONG2FIX((n >> i) & 1));
    }
    {
        korb_mp_t z, r; korb_to_mpz(selfv, z); korb_mp_init(r);
        if (i >= 0) korb_mp_fdiv_q_2exp(r, z, (korb_mp_bitcnt_t)i);     /* self >> i (arith) */
        else        korb_mp_mul_2exp(r, z, (korb_mp_bitcnt_t)(-i));     /* self << -i */
        if (zero) korb_mp_set_ui(r, 0);
        else if (single_bit) { korb_mp_t one; korb_mp_init_set_ui(one, 1); korb_mp_and(r, r, one); korb_mp_clear(one); }
        else if (mask_len > 0) { korb_mp_t m; korb_mp_init_set_ui(m, 1); korb_mp_mul_2exp(m, m, (korb_mp_bitcnt_t)mask_len); korb_mp_sub_ui(m, m, 1); korb_mp_and(r, r, m); korb_mp_clear(m); }
        /* else (mask_len==0, range no-mask): all bits from offset */
        korb_mp_clear(z);
        RESULT res = korb_big_from_mpz(c, slots, r); korb_mp_clear(r); return res;
    }
}
/* bitwise & | ^ (kind 0/1/2) */
static RESULT korb_int_bitop(CTX *c, korb_value *slots, korb_ref self, korb_slice a, int kind) {
    korb_value o = KORB_SLICE_GET(a, 0);
    korb_value s = KORB_REF_GET(self);
    if (LIKELY(FIXNUM_P(s) && FIXNUM_P(o))) {
        korb_sword_t x = FIX2LONG(s), y = FIX2LONG(o);
        return RESULT_OK(LONG2FIX(kind == 0 ? (x & y) : kind == 1 ? (x | y) : (x ^ y)));
    }
    if (UNLIKELY(!KORB_INTEGER_P(o))) {                /* user object → #coerce protocol; Float/etc. → TypeError */
        if (KORB_OBJECT_P(o)) {
            bool h; RESULT cr = korb_try_coerce(c, slots, s, &o, kind == 0 ? "&" : kind == 1 ? "|" : "^", 0, &h);
            if (h) return cr;
        }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_coerce_name(c, o));
    }
    return korb_int_bitwise(c, slots, s, o, kind);     /* Bignum operand → GMP two's-complement bitop */
}
static RESULT korb_m_int_and(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_int_bitop(c, slots, self, a, 0); }
static RESULT korb_m_int_or (CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_int_bitop(c, slots, self, a, 1); }
static RESULT korb_m_int_xor(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_int_bitop(c, slots, self, a, 2); }
static RESULT korb_m_int_inv(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a;
    const korb_value v = KORB_REF_GET(self);
    if (LIKELY(FIXNUM_P(v))) return RESULT_OK(LONG2FIX(~FIX2LONG(v)));
    korb_mp_t z; korb_to_mpz(v, z);          /* Bignum: ~z = -z-1 (two's complement) */
    korb_mp_com(z, z);
    const RESULT r = korb_big_from_mpz(c, slots, z);
    korb_mp_clear(z);
    return r;
}
static RESULT korb_m_int_remainder(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value o = KORB_SLICE_GET(a, 0);
    if (KORB_FLOAT_P(o)) {                             /* Integer#remainder(Float) → Float, truncated */
        double f = korb_float_val(o), s; korb_num_to_d(KORB_REF_GET(self), &s);
        if (UNLIKELY(f == 0.0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
        return korb_float_new(c, slots, fmod(s, f));   /* C fmod = truncated remainder (sign of dividend) */
    }
    if (KORB_RATIONAL_P(o)) return korb_int_rat_divmod(c, slots, KORB_REF_GET(self), o, 3);
    if (UNLIKELY(!KORB_INTEGER_P(o))) {                /* non-numeric arg → the coerce protocol */
        if (KORB_OBJECT_P(o)) { bool h; RESULT cr = korb_try_coerce(c, slots, KORB_REF_GET(self), &o, "remainder", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Integer", korb_coerce_name(c, o));
    }
    if (UNLIKELY(!FIXNUM_P(KORB_REF_GET(self)) || !FIXNUM_P(o)))   /* Bignum operand/self → GMP truncated */
        return korb_int_intdiv(c, slots, KORB_REF_GET(self), o, 3);
    korb_sword_t b = FIX2LONG(o);
    if (UNLIKELY(b == 0)) return korb_raise(c, slots, KORB_E_ZERODIV, 0, "divided by 0");
    return RESULT_OK(LONG2FIX(FIX2LONG(KORB_REF_GET(self)) % b));   /* truncated (sign of dividend) */
}
static RESULT korb_m_true_lit2(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_flt_abs2(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)a; double d = korb_float_val(KORB_REF_GET(self)); return korb_float_new(c, slots, d * d); }

static RESULT korb_m_ary_values_at(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    uint32_t k = KORB_SLICE_LEN(a);
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        korb_value iv = KORB_SLICE_GET(a, j);
        if (KORB_RANGE_P(iv)) {                       /* values_at(range): each index in the range */
            const KorbRange *r = VAL2RANGE(iv);
            slots[0] = r->rbegin; slots[1] = r->rend;   /* root the bounds across any #to_int dispatch */
            const bool excl = r->exclude_end != 0;
            const korb_sword_t len0 = VAL2ARY(KORB_REF_GET(self))->len;
            korb_sword_t b, e2, last;
            if (slots[0] == KORB_NIL) b = 0;          /* beginless → 0 */
            else { if (!korb_to_index(slots[0], &b)) { RESULT cr = korb_coerce_to_int(c, slots + 2, &slots[0]); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; if (!korb_to_index(slots[0], &b)) continue; } if (b < 0) b += len0; }
            if (slots[1] == KORB_NIL) last = len0 - 1; /* endless → to end */
            else { if (!korb_to_index(slots[1], &e2)) { RESULT cr = korb_coerce_to_int(c, slots + 2, &slots[1]); if (UNLIKELY(cr.state != KORB_NORMAL)) return cr; if (!korb_to_index(slots[1], &e2)) continue; } if (e2 < 0) e2 += len0; last = excl ? e2 - 1 : e2; }
            for (korb_sword_t i = b; i <= last; i++) {
                const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
                CHECK(korb_ary_push_val(c, slots + 2, dst, (i >= 0 && (uint32_t)i < ary->len) ? korb_ary_ptr(ary)[i] : KORB_NIL));
            }
            continue;
        }
        korb_sword_t i;
        if (!korb_to_index(iv, &i)) {                 /* coerce a non-Integer index via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots + 1, &iv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(iv, &i))
                return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, j));
        }
        const KorbArray *ary = VAL2ARY(KORB_REF_GET(self));   /* re-read after possible dispatch */
        korb_value e = KORB_NIL;
        if (i < 0) i += ary->len;
        if (i >= 0 && (uint32_t)i < ary->len) e = korb_ary_ptr(ary)[i];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_fill(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_sword_t n = VAL2ARY(KORB_REF_GET(self))->len;
    uint32_t base = block ? 0 : 1;                        /* position args start here */
    if (UNLIKELY(!block && KORB_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    if (UNLIKELY(!block && KORB_SLICE_LEN(a) > 3))        /* filler + start + length */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..3)", KORB_SLICE_LEN(a));
    if (UNLIKELY(block && KORB_SLICE_LEN(a) > 2))         /* start + length */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..2)", KORB_SLICE_LEN(a));
    korb_value v = block ? KORB_NIL : KORB_SLICE_GET(a, 0);
    /* compute fill region [beg, beg+len) following MRI rb_ary_fill order */
    korb_sword_t beg = 0, len = n; bool have_len = false;
    korb_value pos0 = KORB_SLICE_LEN(a) > base ? KORB_SLICE_GET(a, base) : KORB_NIL;
    if (UNLIKELY(KORB_RANGE_P(pos0) && KORB_SLICE_LEN(a) > base + 1))   /* fill(x, range, length) is invalid */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "length invalid with range");
    if (KORB_RANGE_P(pos0)) {
        korb_sword_t b = 0, e;
        slots[0] = v;                                    /* root the filler across the #to_int dispatch */
        slots[1] = VAL2RANGE(pos0)->rbegin;              /* stage bounds (rooted too) */
        slots[2] = VAL2RANGE(pos0)->rend;
        const bool excl = VAL2RANGE(pos0)->exclude_end != 0;
        const bool endless = (slots[2] == KORB_NIL);
        if (slots[1] != KORB_NIL && UNLIKELY(!korb_to_index(slots[1], &b))) {   /* beginless → 0; else coerce via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots + 3, &slots[1]);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(slots[1], &b)) return korb_raise_no_int(c, slots, slots[1]);
            n = VAL2ARY(KORB_REF_GET(self))->len;
        }
        const korb_sword_t braw = b;
        if (b < 0) b += n;
        if (b < 0) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld..%ld out of range", (long)braw, (long)(endless ? -1 : 0));   /* begin before array start */
        if (endless) {                                   /* endless range → to end of array */
            beg = b; len = n - b; have_len = true;
        } else {
            if (UNLIKELY(!korb_to_index(slots[2], &e))) {
                RESULT cr = korb_coerce_to_int(c, slots + 3, &slots[2]);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(slots[2], &e)) return korb_raise_no_int(c, slots, slots[2]);
                n = VAL2ARY(KORB_REF_GET(self))->len;
            }
            if (e < 0) e += n;
            beg = b; len = (excl ? e - 1 : e) - b + 1; have_len = true;
        }
        v = slots[0];                                    /* reload the (possibly GC-moved) filler */
    } else {
        if (pos0 != KORB_NIL) {
            if (UNLIKELY(!korb_to_index(pos0, &beg))) {   /* coerce start via #to_int (self is a korb_ref; n is a value) */
                RESULT cr = korb_coerce_to_int(c, slots, &pos0);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(pos0, &beg)) return korb_raise_no_int(c, slots, pos0);
                n = VAL2ARY(KORB_REF_GET(self))->len;
            }
        }
        korb_value pos1 = KORB_SLICE_LEN(a) > base + 1 ? KORB_SLICE_GET(a, base + 1) : KORB_NIL;
        if (pos1 != KORB_NIL) {
            if (UNLIKELY(KORB_BIGNUM_P(pos1)))           /* a Bignum length can't fit → RangeError (CRuby) */
                return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
            if (UNLIKELY(!korb_to_index(pos1, &len))) {
                RESULT cr = korb_coerce_to_int(c, slots, &pos1);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
                if (!korb_to_index(pos1, &len)) return korb_raise_no_int(c, slots, pos1);
                n = VAL2ARY(KORB_REF_GET(self))->len;
            }
            have_len = true;
        }
        if (beg < 0) { beg += n; if (beg < 0) beg = 0; }
        if (!have_len) len = n - beg;                    /* default: to end of array */
    }
    if (len < 0) len = 0;
    /* a fill region that would grow the array past what its uint32 index can hold
     * (koruby cap) is refused rather than looped to exhaustion. */
    if (UNLIKELY(beg > 0 && len > INTPTR_MAX - beg)) len = INTPTR_MAX;
    if (UNLIKELY(beg + len > (korb_sword_t)0x7fffffff))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "argument too big");
    slots[0] = v;                                        /* root value across any grow */
    /* No block and a region already inside the array: nothing in the loop can
     * allocate, so the array cannot move — hoist the per-element re-read and
     * the grow check out (they cost more than the store itself). */
    if (block == NULL && beg >= 0 && len > 0 &&
        beg + len <= (korb_sword_t)VAL2ARY(KORB_REF_GET(self))->len) {
        KorbArray *const ary = VAL2ARY(KORB_REF_GET(self));
        korb_value *const data = korb_ary_ptr(ary);
        const korb_value fv = slots[0];
        for (korb_sword_t i = beg; i < beg + len; i++) ARO_STORE(c, korb_ary_owner(ary), &data[i], fv);
        return RESULT_OK(KORB_REF_GET(self));
    }
    for (korb_sword_t i = beg; i < beg + len; i++) {
        if (i < 0) continue;
        if (block != NULL) {
            korb_value iv = LONG2FIX(i);
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;                          /* root before grow GC */
        }
        KorbArray *ary = VAL2ARY(KORB_REF_GET(self));
        if ((uint32_t)i >= ary->len) {
            CHECK(korb_ary_ensure(c, slots + 1, self, (uint32_t)i + 1 - ary->len));
            ary = VAL2ARY(KORB_REF_GET(self));
            for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ARO_STORE(c, korb_ary_owner(ary), &korb_ary_ptr(ary)[k], KORB_NIL);
            ary->len = (uint32_t)i + 1;
        }
        ary = VAL2ARY(KORB_REF_GET(self));
        ARO_STORE(c, korb_ary_owner(ary), &korb_ary_ptr(ary)[i], slots[0]);
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* Hash dig / values_at / slice / except */
static RESULT korb_m_hash_dig(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    korb_value cur = KORB_REF_GET(self);
    for (uint32_t k = 0; k < KORB_SLICE_LEN(a); k++) {
        korb_value key = KORB_SLICE_GET(a, k);
        if (cur == KORB_NIL) return RESULT_OK(KORB_NIL);
        if (k > 0 && AROH_IS_GC_OBJECT(cur) && (((const AroObjectHeader *)(uintptr_t)cur)->flags & KORB_FL_HAS_KLASS))
            return korb_dig_dispatch(c, slots, cur, a, k);   /* may redefine #dig (see Array#dig) */
        if (KORB_HASH_P(cur)) {
            int32_t idx = korb_hash_find(VAL2HASH(cur), key);
            if (idx >= 0) cur = korb_hash_pairs(VAL2HASH(cur))[2 * idx + 1];
            else {                                            /* miss → Hash's default (value or proc), as #[] would */
                KorbHash *const hc = VAL2HASH(cur);
                if (hc->default_proc != KORB_NIL) {
                    slots[0] = hc->default_proc; slots[1] = cur; slots[2] = key;   /* default_proc.call(hash, key) */
                    RESULT r = korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "call", 4), 0, 2, NULL, NULL, NULL);
                    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                    cur = r.value;
                }
                else cur = hc->default_val;
            }
        }
        else if (KORB_ARRAY_P(cur)) { korb_sword_t i; if (UNLIKELY(!korb_to_index(key, &i))) return korb_raise_no_int(c, slots, key); KorbArray *ar = VAL2ARY(cur); if (i < 0) i += ar->len; cur = (i < 0 || (uint32_t)i >= ar->len) ? KORB_NIL : korb_ary_ptr(ar)[i]; }
        else return korb_dig_dispatch(c, slots, cur, a, k);   /* user object → cur.dig(rest...), else TypeError */
    }
    return RESULT_OK(cur);
}
static RESULT korb_m_hash_aref(CTX *c, korb_value *slots, korb_ref self, korb_slice a);   /* fwd (hash.c) */
static RESULT korb_m_hash_values_at(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    uint32_t k = KORB_SLICE_LEN(a);
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, k)));
    for (uint32_t j = 0; j < k; j++) {
        RESULT ferr; int32_t idx = korb_hash_find_ctx(c, slots, self, KORB_SLICE_GET(a, j), &ferr);   /* CTX-aware key */
        if (UNLIKELY(ferr.state != KORB_NORMAL)) return ferr;
        if (idx < 0) {   /* a miss takes the same default path as #[] — a default_proc runs (CRuby) */
            slots[1] = KORB_SLICE_GET(a, j);
            const RESULT dr = korb_m_hash_aref(c, slots + 2, self, KORB_SLICE_MAKE(&slots[1], 1));
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            CHECK(korb_ary_push_val(c, slots + 1, dst, dr.value));
            continue;
        }
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));   /* re-read after possible eql? dispatch */
        CHECK(korb_ary_push_val(c, slots + 1, dst, korb_hash_pairs(h)[2 * idx + 1]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* slice (keep==true) / except (keep==false) the listed keys */
static RESULT korb_hash_pick(CTX *c, korb_value *slots, korb_ref self, korb_slice a, bool keep) {
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* except/slice keep compare_by_identity */
        ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(dst))->flags |= KORB_FL_CMP_BY_ID;
    if (keep) {                                           /* slice: walk the requested keys (preserve their order) */
        for (uint32_t j = 0; j < KORB_SLICE_LEN(a); j++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            const int32_t idx = korb_hash_find(h, KORB_SLICE_GET(a, j));
            if (idx < 0) continue;
            slots[0] = korb_hash_pairs(h)[2 * idx];           /* the hash's own key (root) */
            korb_value val = korb_hash_pairs(h)[2 * idx + 1];
            CHECK(korb_hash_set(c, slots + 1, dst, KORB_REF_AT(&slots[0]), val));
        }
    } else {                                              /* except: walk self, drop listed keys (self's order) */
        uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
        for (uint32_t i = 0; i < n; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            korb_value key = korb_hash_pairs(h)[2 * i];
            bool listed = false;
            for (uint32_t j = 0; j < KORB_SLICE_LEN(a); j++) if (korb_value_eq(KORB_SLICE_GET(a, j), key)) { listed = true; break; }
            if (!listed) {
                slots[0] = key;                           /* root key (scratch above dst) */
                korb_value val = korb_hash_pairs(VAL2HASH(KORB_REF_GET(self)))[2 * i + 1];
                CHECK(korb_hash_set(c, slots + 1, dst, KORB_REF_AT(&slots[0]), val));
            }
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* build an array of the first `limit` pairs ([k,v]) of the hash. */
static RESULT korb_hash_first_n(CTX *c, korb_value *slots, korb_ref self, uint32_t limit) {
    uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    if (n > limit) n = limit;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        slots[0] = korb_hash_pairs(h)[2 * i];
        slots[1] = korb_hash_pairs(h)[2 * i + 1];
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_take(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(KORB_SLICE_GET(a, 0), &n))) return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, 0));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    return korb_hash_first_n(c, slots, self, (uint32_t)n);
}
static RESULT korb_m_hash_first(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (KORB_SLICE_LEN(a) >= 1) return korb_m_hash_take(c, slots, self, a);
    const KorbHash *h = VAL2HASH(KORB_REF_GET(self));   /* no arg → first pair or nil */
    if (h->len == 0) return RESULT_OK(KORB_NIL);
    slots[0] = korb_hash_pairs(h)[0];
    slots[1] = korb_hash_pairs(h)[1];
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_clear(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    (void)c;(void)slots;(void)a;
    KORB_HASH_DROP_INDEX(VAL2HASH(KORB_REF_GET(self)));
    VAL2HASH(KORB_REF_GET(self))->len = 0;     /* drop all pairs (payload kept, becomes garbage) */
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_default_set(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_value v = KORB_SLICE_GET(a, 0);
    KorbHash *const h = VAL2HASH(KORB_REF_GET(self));
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_val, v);
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_proc, KORB_NIL);   /* default= clears any default_proc */
    return RESULT_OK(v);
}
/* Hash#initialize (private) — resets ONLY the default value / default_proc;
 * the storage is untouched, so a subclass may fill it before calling super. */
static RESULT korb_m_hash_initialize(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    if (UNLIKELY(block != NULL && KORB_SLICE_LEN(a) > 0))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0)", KORB_SLICE_LEN(a));
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", KORB_SLICE_LEN(a));
    korb_value dv = KORB_NIL, dp = KORB_NIL;
    if (block != NULL) {
        slots[0] = KORB_REF_GET(self);                  /* park across korb_make_proc's alloc */
        dp = UNWRAP(korb_block_to_proc(c, slots + 1, block, def_env, cself));
        KorbHash *const h0 = VAL2HASH(slots[0]);
        ARO_STORE(c, h0, (korb_value *)(uintptr_t)&h0->default_val, KORB_NIL);
        ARO_STORE(c, h0, (korb_value *)(uintptr_t)&h0->default_proc, dp);
        return RESULT_OK(slots[0]);
    }
    if (KORB_SLICE_LEN(a) == 1) dv = KORB_SLICE_GET(a, 0);
    KorbHash *const h = VAL2HASH(KORB_REF_GET(self));
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_val, dv);
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_proc, KORB_NIL);
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_compact_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    KorbHash *h = VAL2HASH(KORB_REF_GET(self));
    void *const it = korb_hash_owner(h); korb_value *const it_d = korb_hash_pairs(h);
    uint64_t *const hints = h->khints && h->khint_len == h->len ? (uint64_t *)korb_strbuf_data(h->khints) : NULL;
    uint32_t w = 0;
    for (uint32_t i = 0; i < h->len; i++) {
        if (it_d[2*i+1] == KORB_NIL) continue;
        if (w != i) {
            ARO_STORE(c, it, &it_d[2*w], it_d[2*i]); ARO_STORE(c, it, &it_d[2*w+1], it_d[2*i+1]);
            if (hints) hints[w] = hints[i];
        }
        w++;
    }
    if (w == h->len) return RESULT_OK(KORB_NIL);          /* unchanged → nil */
    for (uint32_t r = 2*w; r < 2*h->len; r++) ARO_STORE(c, it, &it_d[r], KORB_NIL);
    h->len = w;
    korb_hash_compacted(h, hints != NULL);
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_transform_values(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "transform_values", 16)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(KORB_REF_GET(self))->len));
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* keys unchanged → keep identity comparison */
        ((AroObjectHeader *)(uintptr_t)slots[0])->flags |= KORB_FL_CMP_BY_ID;
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[2], 1, cself);   /* yield value */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        CHECK(korb_hash_set(c, slots + 4, dst, KORB_REF_AT(&slots[1]), slots[3]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Build a key-transformed copy of self into a fresh hash (returned).  An optional
 * Hash arg renames listed keys; remaining keys go through the block; with neither
 * the key is kept.  Later collisions overwrite (last wins). */
static RESULT korb_hash_xform_keys(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    const bool has_arg = KORB_SLICE_LEN(a) >= 1;   /* an explicit nil is a TypeError, unlike no arg at all */
    const korb_value mapv = has_arg ? KORB_SLICE_GET(a, 0) : KORB_NIL;
    if (has_arg && !KORB_HASH_P(mapv))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, mapv));
    if (UNLIKELY(!has_arg && block == NULL))
        { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "transform_keys", 14)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    slots[0] = mapv;                                           /* root the rename map BEFORE any alloc moves it */
    slots[1] = UNWRAP(korb_hash_new(c, slots + 1, VAL2HASH(KORB_REF_GET(self))->len));
    korb_ref dst = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = korb_hash_pairs(h)[2 * i];                      /* old key */
        slots[3] = korb_hash_pairs(h)[2 * i + 1];                  /* value */
        slots[4] = slots[2];                                   /* new key (default = old) */
        bool mapped = false;
        if (slots[0] != KORB_NIL) {
            int32_t j = korb_hash_find(VAL2HASH(slots[0]), slots[2]);
            if (j >= 0) { slots[4] = korb_hash_pairs(VAL2HASH(slots[0]))[2 * j + 1]; mapped = true; }
        }
        if (!mapped && block != NULL) {
            RESULT r = korb_block_yield(c, slots + 5, block, def_env, &slots[2], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[4] = r.value;
        }
        CHECK(korb_hash_set(c, slots + 5, dst, KORB_REF_AT(&slots[4]), slots[3]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_transform_keys(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    return korb_hash_xform_keys(c, slots, self, a, block, def_env, cself);
}
/* transform_keys! — CRuby renames pair by pair over a snapshot of the entries,
 * so a `break` out of the block leaves the already-renamed keys in place and the
 * rest of the hash untouched (rebuilding into a temp would lose both). */
static RESULT korb_m_hash_delete(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself);   /* fwd (hash.c) */
static RESULT korb_m_hash_transform_keys_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(block == NULL && KORB_SLICE_LEN(a) == 0)) {   /* no block/arg → Enumerator over the bang form (its #each mutates self) */
        slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "transform_keys!", 15));
        return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1);
    }
    const bool has_arg = KORB_SLICE_LEN(a) >= 1;
    if (has_arg && !KORB_HASH_P(KORB_SLICE_GET(a, 0)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, KORB_SLICE_GET(a, 0)));
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    slots[0] = has_arg ? KORB_SLICE_GET(a, 0) : KORB_NIL;      /* rename map (rooted) */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, VAL2HASH(KORB_REF_GET(self))->len * 2));
    korb_ref pairs = KORB_REF_AT(&slots[1]);                  /* flat [k, v, …] snapshot */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = korb_hash_pairs(h)[2 * i];
        slots[3] = korb_hash_pairs(h)[2 * i + 1];
        CHECK(korb_ary_push_val(c, slots + 4, pairs, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, pairs, slots[3]));
    }
    slots[2] = UNWRAP(korb_hash_new(c, slots + 2, 8));          /* the keys produced so far */
    korb_ref seen = KORB_REF_AT(&slots[2]);
    const uint32_t np = VAL2ARY(KORB_REF_GET(pairs))->len;
    for (uint32_t i = 0; i + 1 < np; i += 2) {
        slots[3] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(pairs)))[i];       /* old key */
        slots[4] = korb_ary_ptr(VAL2ARY(KORB_REF_GET(pairs)))[i + 1];   /* value */
        slots[5] = slots[3];                                                       /* new key (default = old) */
        bool mapped = false;
        if (slots[0] != KORB_NIL) {
            const int32_t j = korb_hash_find(VAL2HASH(slots[0]), slots[3]);
            if (j >= 0) { slots[5] = korb_hash_pairs(VAL2HASH(slots[0]))[2 * j + 1]; mapped = true; }
        }
        if (!mapped && block != NULL) {
            RESULT r = korb_block_yield(c, slots + 6, block, def_env, &slots[3], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;     /* break/raise: keep the partial rename */
            slots[5] = r.value;
        }
        if (korb_hash_find(VAL2HASH(KORB_REF_GET(seen)), slots[3]) < 0) {   /* the old key was not re-created → drop it */
            slots[6] = slots[3];
            CHECK(korb_m_hash_delete(c, slots + 7, self, KORB_SLICE_MAKE(&slots[6], 1), NULL, NULL, NULL));
        }
        CHECK(korb_hash_set(c, slots + 6, self, KORB_REF_AT(&slots[5]), slots[4]));
        slots[6] = slots[5];
        CHECK(korb_hash_set(c, slots + 7, seen, KORB_REF_AT(&slots[6]), KORB_NIL));
    }
    return RESULT_OK(KORB_REF_GET(self));
}
/* transform_values! — replace each value in place with block(value); keys unchanged. */
static RESULT korb_m_hash_transform_values_b(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {   /* Enumerator over the bang form: its #each mutates self */
        slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "transform_values!", 17));
        return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1);
    }
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    for (uint32_t i = 0; ; i++) {
        KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2 * i + 1];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        h = VAL2HASH(KORB_REF_GET(self));                 /* re-read after yield */
        ARO_STORE(c, korb_hash_owner(h), &korb_hash_pairs(h)[2 * i + 1], r.value);
    }
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_ary_minmax(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself);
static RESULT korb_m_hash_minmax(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));   /* all pairs, then Array#minmax */
    return korb_m_ary_minmax(c, slots + 1, KORB_REF_AT(&slots[0]), a, NULL, NULL, NULL);
}
static RESULT korb_m_hash_default_proc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2HASH(KORB_REF_GET(self))->default_proc);
}
static RESULT korb_m_hash_default_proc_set(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_value p = KORB_SLICE_GET(a, 0);
    if (p != KORB_NIL && !KORB_PROC_P(p)) {                   /* coerce via #to_proc, else TypeError */
        const uint32_t to_proc = korb_intern_lit(c->vm, "to_proc", 7);
        if (!korb_responds_to_coerce_p(c, slots, &p, to_proc))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Proc", korb_coerce_name(c, p));
        slots[0] = p;
        RESULT pr = korb_send_impl(c, slots + 1, to_proc, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        if (UNLIKELY(!KORB_PROC_P(pr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Proc", korb_coerce_name(c, slots[0]));
        p = pr.value;
    }
    if (p != KORB_NIL && VAL2PROC(p)->is_lambda) {           /* a lambda default_proc must accept exactly 2 args */
        const KorbProc *const pp = VAL2PROC(p);
        korb_sword_t n;
        if (pp->iseq == NULL) n = -2;                        /* symbol/method proc */
        else {
            const NODE *const e = pp->iseq;
            const bool var = (e->u.node_entry.opt_defaults != NULL) || (e->u.node_entry.rest_slot >= 0 || e->u.node_entry.rest_slot == -2);
            const uint32_t reqc = var ? e->u.node_entry.req_cnt : e->u.node_entry.params_cnt;
            n = var ? -((korb_sword_t)reqc + 1) : (korb_sword_t)reqc;
        }
        if (n != 2 && (n >= 0 || n < -3))                    /* fixed arity != 2, or needs >2 before a splat */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "default_proc takes two arguments (2 for %ld)", (long)n);
    }
    KorbHash *const h = VAL2HASH(KORB_REF_GET(self));        /* re-read after the dispatch */
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_proc, p);
    ARO_STORE(c, h, (korb_value *)(uintptr_t)&h->default_val, KORB_NIL);   /* setting a proc clears the static default */
    return RESULT_OK(KORB_SLICE_GET(a, 0));   /* a setter call evaluates to its argument */
}
static RESULT korb_m_hash_compact(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(KORB_REF_GET(self))->len));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* compact retains compare_by_identity */
        ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(dst))->flags |= KORB_FL_CMP_BY_ID;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        if (korb_hash_pairs(h)[2*i+1] == KORB_NIL) continue;      /* drop nil values */
        slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
        CHECK(korb_hash_set(c, slots + 3, dst, KORB_REF_AT(&slots[1]), slots[2]));
    }
    KorbHash *const dh = VAL2HASH(KORB_REF_GET(dst));        /* retain default value / default_proc */
    const KorbHash *const sh = VAL2HASH(KORB_REF_GET(self));
    ARO_STORE(c, dh, (korb_value *)(uintptr_t)&dh->default_val, sh->default_val);
    ARO_STORE(c, dh, (korb_value *)(uintptr_t)&dh->default_proc, sh->default_proc);
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_flatten(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t depth = 1;                                   /* default flattens one level (pairs → k,v,...) */
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL) {
        korb_value dv = KORB_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(dv, &depth))) {       /* coerce depth via #to_int, else TypeError */
            RESULT cr = korb_coerce_to_int(c, slots, &dv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(dv, &depth)) return korb_raise_no_int(c, slots, KORB_SLICE_GET(a, 0));
        }
    }
    uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n * 2)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        slots[0] = korb_hash_pairs(h)[2 * i];
        slots[1] = korb_hash_pairs(h)[2 * i + 1];
        if (depth == 0) {                                /* depth 0 only: keep [k,v] pairs (negative = full) */
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[0]));
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    }
    if (depth > 1 || depth < 0) {                         /* flatten the k/v values further */
        slots[0] = LONG2FIX(depth < 0 ? -1 : depth - 1);
        return korb_m_ary_flatten(c, slots + 1, dst, KORB_SLICE_MAKE(&slots[0], 1));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Hash#invert — new hash with keys and values swapped (later dups win, like CRuby). */
static RESULT korb_m_hash_invert(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(KORB_REF_GET(self))->len));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_hash_pairs(h)[2*i];                /* old key → new value */
        slots[2] = korb_hash_pairs(h)[2*i+1];              /* old value → new key */
        CHECK(korb_hash_set(c, slots + 3, dst, KORB_REF_AT(&slots[2]), slots[1]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Hash#rehash — our hash has no cached digests, so this is a no-op returning self. */
static RESULT korb_m_hash_replace(CTX *c, korb_value *slots, korb_ref self, korb_slice a);   /* fwd */
static RESULT korb_m_hash_rehash(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    /* Recompute every key's hash (keys may have mutated) and drop duplicates:
     * re-insert each pair, in order, into a fresh hash (korb_hash_set recomputes
     * the hash and dedups by #eql?), then adopt those contents. */
    const uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    slots[0] = UNWRAP(korb_hash_new(c, slots, n ? n : 4));
    korb_ref tmp = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < VAL2HASH(KORB_REF_GET(self))->len; i++) {
        slots[1] = korb_hash_pairs(VAL2HASH(KORB_REF_GET(self)))[2 * i];       /* key (rooted across set) */
        const korb_value val = korb_hash_pairs(VAL2HASH(KORB_REF_GET(self)))[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, tmp, KORB_REF_AT(&slots[1]), val));
    }
    return korb_m_hash_replace(c, slots + 1, self, KORB_SLICE_MAKE(&slots[0], 1));   /* self ← recomputed copy */
}
/* Hash#replace(other) — replace self's contents with other's, in place; returns self. */
static RESULT korb_m_hash_replace(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    korb_value ov = KORB_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(ov))) {                            /* coerce via #to_hash (Hash subclasses are already KORB_HASH_P) */
        const uint32_t to_hash = korb_intern_lit(c->vm, "to_hash", 7);
        if (KORB_OBJECT_P(ov) && korb_responds_to_coerce_p(c, slots, &ov, to_hash)) {
            slots[0] = ov;
            RESULT hr = korb_send_impl(c, slots + 1, to_hash, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
            ov = hr.value;
        }
        if (UNLIKELY(!KORB_HASH_P(ov)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Hash", korb_coerce_name(c, KORB_SLICE_GET(a, 0)));
    }
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));            /* re-check (self could move under the dispatch) */
    if (ov == KORB_REF_GET(self)) return RESULT_OK(KORB_REF_GET(self));   /* h.replace(h) is a no-op (clear-then-copy would empty it) */
    KORB_HASH_DROP_INDEX(VAL2HASH(KORB_REF_GET(self)));
    VAL2HASH(KORB_REF_GET(self))->len = 0;                       /* clear, then copy other's pairs */
    slots[0] = ov;
    {   /* replace adopts the source's compare_by_identity setting (set or clear) */
        AroObjectHeader *const sh = (AroObjectHeader *)(uintptr_t)KORB_REF_GET(self);
        if (VAL2HASH(ov)->head.flags & KORB_FL_CMP_BY_ID) sh->flags |= KORB_FL_CMP_BY_ID;
        else                                              sh->flags &= (uint16_t)~KORB_FL_CMP_BY_ID;
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *oh = VAL2HASH(slots[0]);
        if (i >= oh->len) break;
        slots[1] = korb_hash_pairs(oh)[2 * i];
        korb_value val = korb_hash_pairs(VAL2HASH(slots[0]))[2 * i + 1];
        CHECK(korb_hash_set(c, slots + 2, self, KORB_REF_AT(&slots[1]), val));
    }
    /* replace also transfers the source's default value + default_proc. */
    KorbHash *const dst = VAL2HASH(KORB_REF_GET(self));
    const korb_value dv = VAL2HASH(slots[0])->default_val;
    const korb_value dp = VAL2HASH(slots[0])->default_proc;
    ARO_STORE(c, dst, (korb_value *)(uintptr_t)&dst->default_val, dv);
    ARO_STORE(c, dst, (korb_value *)(uintptr_t)&dst->default_proc, dp);
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_hash_yield(CTX *c, korb_value *slots, NODE *block, korb_value *def_env, korb_value *cself, uint32_t np, korb_value k, korb_value v);   /* fwd (defined below) */
/* Hash#drop_while { |k,v| } — drop leading pairs while the block is true, return
 * the remaining pairs as an Array of [k,v]. */
static RESULT korb_m_hash_drop_while(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block given");
    const uint32_t np = korb_entry_params_cnt(block);            /* gather the [k,v] pair for a 1-param block */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                 /* result pairs (rooted) */
    korb_ref out = KORB_REF_AT(&slots[0]);
    bool dropping = true;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_hash_pairs(h)[2 * i];                        /* k */
        slots[2] = korb_hash_pairs(h)[2 * i + 1];                    /* v */
        if (dropping) {
            RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[1], slots[2]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) continue;                  /* still dropping */
            dropping = false;
        }
        slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));        /* [k,v] pair */
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
        CHECK(korb_ary_push_val(c, slots + 4, out, slots[3]));
    }
    return RESULT_OK(KORB_REF_GET(out));
}
static RESULT korb_m_ary_tally(CTX *c, korb_value *slots, korb_ref self, korb_slice a);
static RESULT korb_m_hash_to_a(CTX *c, korb_value *slots, korb_ref self, korb_slice a);
/* Hash#tally — Enumerable#tally over [k,v] pairs (each unique → count 1). */
static RESULT korb_m_hash_tally(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = UNWRAP(korb_m_hash_to_a(c, slots, self, KORB_SLICE_MAKE(NULL, 0)));   /* pairs array */
    return korb_m_ary_tally(c, slots + 1, KORB_REF_AT(&slots[0]), a);
}
static RESULT korb_m_hash_to_a(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        slots[0] = korb_hash_pairs(h)[2 * i];      /* k */
        slots[1] = korb_hash_pairs(h)[2 * i + 1];  /* v */
        korb_value pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_ary_uniq(CTX *c, korb_value *slots, korb_ref self, korb_slice a);
static RESULT korb_m_hash_uniq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {   /* Enumerable#uniq over [k,v] pairs */
    slots[0] = UNWRAP(korb_m_hash_to_a(c, slots, self, KORB_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_uniq(c, slots + 1, KORB_REF_AT(&slots[0]), a);
}
static RESULT korb_m_hash_slice(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { return korb_hash_pick(c, slots, self, a, true); }
static RESULT korb_m_hash_except(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_hash_pick(c, slots, self, a, false); }

/* yield (k, v) to a Hash block: np>=2 → two args; else a single [k, v] pair. */
static RESULT korb_hash_yield(CTX *c, korb_value *slots, NODE *block, korb_value *def_env, korb_value *cself, uint32_t np, korb_value k, korb_value v) {
    if (np >= 2) { korb_value argv[2] = { k, v }; return korb_block_yield(c, slots, block, def_env, argv, 2, cself); }
    slots[0] = k; slots[1] = v;
    korb_value pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
    slots[2] = pair;
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    korb_value parg = slots[2];
    return korb_block_yield(c, slots + 3, block, def_env, &parg, 1, cself);
}
/* korb_hash_yield for a loop: |k, v| through the hoisted yield (ys set up on `slots` by the caller), a pair otherwise */
static inline RESULT korb_hash_yield_ys(CTX *c, const struct korb_yseq *ys, korb_value *slots, NODE *block, korb_value *def_env,
                                        korb_value *cself, uint32_t np, korb_value k, korb_value v) {
    if (np >= 2) { korb_value argv[2] = { k, v }; return korb_yseq_call(c, ys, argv, 2); }
    return korb_hash_yield(c, slots, block, def_env, cself, np, k, v);
}
/* no block → an Enumerator (with a size), built by the prelude's Hash#__enum_nb */
static RESULT korb_hash_enum_nb(CTX *c, korb_value *slots, korb_ref self, korb_slice a, const char *meth) {
    const uint32_t n = KORB_SLICE_LEN(a);
    slots[0] = KORB_REF_GET(self);
    slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
    for (uint32_t i = 0; i < n; i++) slots[2 + i] = KORB_SLICE_GET(a, i);
    return korb_send_impl(c, slots + 2 + n, korb_intern_lit(c->vm, "__enum_nb", 9), 0, 1 + n, NULL, NULL, NULL);
}
#define HASH_REQ_BLOCK(meth) do { if (UNLIKELY(block == NULL)) return korb_hash_enum_nb(c, slots, self, a, meth); } while (0)

/* Hash#reverse_each — yield (k,v) pairs in reverse insertion order; return self. */
static RESULT korb_m_hash_reverse_each(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    HASH_REQ_BLOCK("reverse_each");
    const uint32_t np = korb_entry_params_cnt(block);
    uint32_t i = VAL2HASH(KORB_REF_GET(self))->len;
    while (i > 0) {
        i--;
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) continue;                              /* shrunk during iteration */
        CHECK(korb_hash_yield(c, slots, block, def_env, cself, np, korb_hash_pairs(h)[2 * i], korb_hash_pairs(h)[2 * i + 1]));
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* Hash#each_with_index — yield ([k,v], index); return self. */
static RESULT korb_m_hash_each_with_index(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block == NULL) {                  /* → Enumerator of [[k,v], index]: build [k,v] pairs, delegate to Array#each_with_index */
        slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2HASH(KORB_REF_GET(self))->len));
        korb_ref pairs = KORB_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            if (i >= h->len) break;
            slots[1] = korb_hash_pairs(h)[2 * i];
            slots[2] = korb_hash_pairs(h)[2 * i + 1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
            CHECK(korb_ary_push_val(c, slots + 4, pairs, slots[3]));
        }
        return korb_m_ary_each_wi(c, slots + 1, pairs, a, NULL, def_env, cself);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2 * i];                      /* k (rooted) */
        slots[1] = korb_hash_pairs(h)[2 * i + 1];                  /* v (rooted) */
        korb_value pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
        korb_value argv[2] = { slots[2], LONG2FIX((korb_sword_t)i) };
        CHECK(korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself));
    }
    return RESULT_OK(KORB_REF_GET(self));
}

/* Hash#cycle([n]) — yield (k,v) pairs n times (forever if n omitted); return nil. */
static RESULT korb_m_hash_cycle(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    HASH_REQ_BLOCK("cycle");
    const uint32_t np = korb_entry_params_cnt(block);
    bool bounded = KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL;
    korb_sword_t n = 0;
    if (bounded && UNLIKELY(!korb_to_index(KORB_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (bounded && n <= 0) return RESULT_OK(KORB_NIL);
    if (VAL2HASH(KORB_REF_GET(self))->len == 0) return RESULT_OK(KORB_NIL);
    for (korb_sword_t pass = 0; !bounded || pass < n; pass++) {
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            if (i >= h->len) break;
            CHECK(korb_hash_yield(c, slots, block, def_env, cself, np, korb_hash_pairs(h)[2 * i], korb_hash_pairs(h)[2 * i + 1]));
        }
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_hash_flat_map(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    HASH_REQ_BLOCK("flat_map");
    uint32_t np = korb_entry_params_cnt(block);
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, captured_self, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) {                 /* flatten one level */
            slots[1] = r.value;
            uint32_t sublen = VAL2ARY(slots[1])->len;
            for (uint32_t j = 0; j < sublen; j++)
                CHECK(korb_ary_push_val(c, slots + 2, dst, korb_ary_ptr(VAL2ARY(slots[1]))[j]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_map(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    HASH_REQ_BLOCK("map");
    uint32_t np = korb_entry_params_cnt(block);
    if (block == KORB_BLK_CPROC && KORB_PROC_P(KORB_CSELF_VAL(captured_self))) {   /* &method(:m): m's arity decides (CRuby) */
        slots[0] = KORB_CSELF_VAL(captured_self);
        const RESULT ar = korb_send(c, slots + 1, korb_intern_lit(c->vm, "arity", 5), 0, 0);
        if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        if (FIXNUM_P(ar.value) && (FIX2LONG(ar.value) >= 2 || FIX2LONG(ar.value) <= -3)) np = 2;
    }
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    struct korb_yseq ys = { 0 };                       /* set up only for |k, v| */
    if (np >= 2) CHECK(korb_yseq_init(c, slots + 1, &ys, block, def_env, captured_self));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield_ys(c, &ys, slots + 1, block, def_env, captured_self, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* select(keep=1)/reject(keep=0) → new Hash */
static RESULT korb_hash_filter(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "select", 6)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, 4)));
    if (VAL2HASH(KORB_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* select/reject retain compare_by_identity */
        ((AroObjectHeader *)(uintptr_t)KORB_REF_GET(dst))->flags |= KORB_FL_CMP_BY_ID;
    struct korb_yseq ys;
    CHECK(korb_yseq_init(c, slots + 1, &ys, block, def_env, captured_self));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        korb_value k = korb_hash_pairs(h)[2*i], v = korb_hash_pairs(h)[2*i+1];
        /* CRuby Hash#select/reject yield |key, value| as TWO values (a 1-param
         * block gets the key), unlike each/map which gather the [k,v] pair. */
        RESULT r = korb_hash_yield_ys(c, &ys, slots + 1, block, def_env, captured_self, 2, k, v);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) {
            /* re-read BOTH key and value fresh from self: the block ran (and may
             * have GC'd, moving the heap key/value) — the pre-yield `k`/`v` locals
             * are stale.  (v was already re-read; k was not → STRESS+PURGE SEGV.) */
            const KorbHash *const hh = VAL2HASH(KORB_REF_GET(self));
            slots[0] = korb_hash_pairs(hh)[2*i];
            korb_value vv = korb_hash_pairs(hh)[2*i+1];
            CHECK(korb_hash_set(c, slots + 1, dst, KORB_REF_AT(&slots[0]), vv));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_select(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_hash_reject(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { (void)a; return korb_hash_filter(c, slots, self, block, def_env, captured_self, false); }
/* any?(0)/all?(1)/none?(2) */
static RESULT korb_hash_quant(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self, int mode) {
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1))                 /* all?/any?/none?/one? take at most one pattern arg */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", KORB_SLICE_LEN(a));
    if (KORB_SLICE_LEN(a) >= 1) {                        /* pattern === [k,v] pair */
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            if (i >= h->len) break;
            slots[0] = korb_hash_pairs(h)[2*i]; slots[1] = korb_hash_pairs(h)[2*i+1];
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
            CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
            bool t; CHECK(korb_pat_eq(c, slots + 3, KORB_SLICE_GET(a, 0), slots[2], &t));
            if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
            if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
            if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
        }
        return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
    }
    if (block == NULL) {                                  /* pairs are always truthy */
        uint32_t len = VAL2HASH(KORB_REF_GET(self))->len;
        if (mode == 0) return RESULT_OK(len > 0 ? KORB_TRUE : KORB_FALSE);   /* any? */
        if (mode == 2) return RESULT_OK(len == 0 ? KORB_TRUE : KORB_FALSE);  /* none? */
        return RESULT_OK(KORB_TRUE);                                          /* all? */
    }
    uint32_t np = korb_entry_params_cnt(block);
    struct korb_yseq ys = { 0 };                       /* set up only for |k, v| */
    if (np >= 2) CHECK(korb_yseq_init(c, slots, &ys, block, def_env, captured_self));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield_ys(c, &ys, slots, block, def_env, captured_self, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        bool t = KORB_TRUTHY(r.value);
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_hash_any(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self)  { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_hash_all(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self)  { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_hash_none(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) { return korb_hash_quant(c, slots, self, a, block, def_env, captured_self, 2); }
/* in-place select!/filter!(keep_truthy=1) and reject!/delete_if(keep_truthy=0) */
static RESULT korb_hash_filter_bang(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *cself, bool keep_truthy, bool ret_nil_if_unchanged, const char *meth) {
    if (UNLIKELY(block == NULL)) {                       /* no block → an Enumerator (to_enum(:meth)) — even when frozen */
        slots[0] = KORB_REF_GET(self);
        slots[1] = ID2SYM(korb_intern(c->vm, meth, (uint32_t)strlen(meth)));
        return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1);
    }
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));    /* a block-bearing modify on a frozen Hash → FrozenError */
    uint32_t w = 0; bool changed = false;
    const uint32_t len0 = VAL2HASH(KORB_REF_GET(self))->len;
    bool hints_moved = VAL2HASH(KORB_REF_GET(self))->khints && VAL2HASH(KORB_REF_GET(self))->khint_len == len0;
    for (uint32_t r = 0; ; r++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (r >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2*r];                    /* root k,v across the yield */
        slots[1] = korb_hash_pairs(h)[2*r+1];
        /* select!/reject!/keep_if/delete_if yield |key, value| as TWO values */
        RESULT res = korb_hash_yield(c, slots + 2, block, def_env, cself, 2, slots[0], slots[1]);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        if (KORB_TRUTHY(res.value) == keep_truthy) {
            KorbHash *h2 = VAL2HASH(KORB_REF_GET(self));
            hints_moved = hints_moved && h2->len == len0 && h2->khints && h2->khint_len == len0;   /* the block may reshape the Hash */
            if (w != r) {
                ARO_STORE(c, korb_hash_owner(h2), &korb_hash_pairs(h2)[2*w],   slots[0]);
                ARO_STORE(c, korb_hash_owner(h2), &korb_hash_pairs(h2)[2*w+1], slots[1]);
                if (hints_moved) { uint64_t *const hints = (uint64_t *)korb_strbuf_data(h2->khints); hints[w] = hints[r]; }
            }
            w++;
        } else changed = true;
    }
    KorbHash *h = VAL2HASH(KORB_REF_GET(self));
    hints_moved = hints_moved && h->len == len0 && h->khints && h->khint_len == len0;
    for (uint32_t r = 2*w; r < 2*h->len; r++) ARO_STORE(c, korb_hash_owner(h), &korb_hash_pairs(h)[r], KORB_NIL);
    h->len = w;
    korb_hash_compacted(h, hints_moved);
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(KORB_REF_GET(self));
}
static RESULT korb_m_hash_select_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, true, "select!"); }
static RESULT korb_m_hash_keep_if(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, true, false, "keep_if"); }
static RESULT korb_m_hash_reject_bang(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, true, "reject!"); }
static RESULT korb_m_hash_delete_if(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { (void)a; return korb_hash_filter_bang(c, slots, self, block, def_env, cself, false, false, "delete_if"); }
static RESULT korb_hash_pair_at(CTX *c, korb_value *slots, korb_ref self, uint32_t i, korb_value *out);
static RESULT korb_m_hash_one(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", KORB_SLICE_LEN(a));
    bool has_pat = KORB_SLICE_LEN(a) >= 1;
    if (!has_pat && UNLIKELY(block == NULL)) {        /* no block, no pattern → exactly one pair */
        return RESULT_OK(VAL2HASH(KORB_REF_GET(self))->len == 1 ? KORB_TRUE : KORB_FALSE);
    }
    uint32_t np = block ? korb_entry_params_cnt(block) : 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        bool t;
        if (has_pat) {
            CHECK(korb_hash_pair_at(c, slots, self, i, &slots[2]));
            CHECK(korb_pat_eq(c, slots + 3, KORB_SLICE_GET(a, 0), slots[2], &t));
        } else {
            RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        }
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
/* Build a fresh [k,v] pair array at *out (a rooted slot). cursor = scratch above it. */
static RESULT korb_hash_make_pair(CTX *c, korb_value *cursor, korb_value *kslot, korb_value *vslot, korb_value *out) {
    *out = UNWRAP(korb_ary_new(c, cursor, 2));
    CHECK(korb_ary_push_val(c, cursor + 1, KORB_REF_AT(out), *kslot));
    CHECK(korb_ary_push_val(c, cursor + 1, KORB_REF_AT(out), *vslot));
    return RESULT_OK(*out);
}
/* sort_by → array of [k,v] pairs sorted by block key */
static RESULT korb_m_hash_sort_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "sort_by", 7)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        korb_ref vals = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    korb_ref keys = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = korb_hash_pairs(h)[2*i]; slots[3] = korb_hash_pairs(h)[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));  /* pair at slots[4] */
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[5] = r.value;
        CHECK(korb_ary_push_val(c, slots + 6, vals, slots[4]));
        CHECK(korb_ary_push_val(c, slots + 6, keys, slots[5]));
    }
    RESULT err = RESULT_OK(KORB_NIL);                    /* sort the keys' indices, reorder both columns */
    uint32_t *const perm = korb_ary_msort_perm(c, slots + 2, keys, korb_msort_cmp_spaceship, NULL, &err);
    if (!perm) return err;
    const bool ok = korb_ary_apply_perm(c, vals, perm) && korb_ary_apply_perm(c, keys, perm);
    free(perm);
    if (!ok) return korb_raise(c, slots + 2, KORB_E_RUNTIME, 0, "out of memory");
    return RESULT_OK(KORB_REF_GET(vals));
}
/* min_by/max_by → the [k,v] pair with the extreme block key */
static RESULT korb_hash_minmax_by(CTX *c, korb_value *slots, korb_ref self, NODE *block, korb_value *def_env, korb_value *cself, int want) {
    if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "min_by", 6)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = KORB_NIL; slots[1] = KORB_NIL; bool have = false;   /* best pair / best key */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = korb_hash_pairs(h)[2*i]; slots[3] = korb_hash_pairs(h)[2*i+1];
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
static RESULT korb_m_hash_min_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_hash_max_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) { (void)a; return korb_hash_minmax_by(c, slots, self, block, def_env, cself,  1); }
static RESULT korb_m_hash_minmax_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_minmax_by(c, slots, self, block, def_env, cself, -1));      /* min pair */
    slots[1] = UNWRAP(korb_hash_minmax_by(c, slots + 1, self, block, def_env, cself, 1));   /* max pair */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(slots[2]);
}
/* filter_map → collect truthy block results */
static RESULT korb_m_hash_filter_map(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "filter_map", 10)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, cself, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* partition → [yes_pairs, no_pairs] */
static RESULT korb_m_hash_partition(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "partition", 9)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));        korb_ref yes = KORB_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));    korb_ref no  = KORB_REF_AT(&slots[1]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[2] = korb_hash_pairs(h)[2*i]; slots[3] = korb_hash_pairs(h)[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[2], &slots[3], &slots[4]));
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[2], slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 5, KORB_TRUTHY(r.value) ? yes : no, slots[4]));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));    korb_ref out = KORB_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
    return RESULT_OK(KORB_REF_GET(out));
}
/* find/detect → the first [k,v] pair whose block is truthy; find(ifnone) calls
 * that proc when nothing matched (rspec's include matcher passes one). */
static RESULT korb_find_ifnone(CTX *c, korb_value *slots, korb_slice a);   /* fwd (builtins/array_enum.c) */
static RESULT korb_m_hash_find(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (UNLIKELY(block == NULL)) {
        slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "find", 4));
        if (KORB_SLICE_LEN(a) >= 1) { slots[2] = KORB_SLICE_GET(a, 0); return korb_send(c, slots + 3, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 2); }
        return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1);
    }
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2*i]; slots[1] = korb_hash_pairs(h)[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            return RESULT_OK(slots[2]);
        }
    }
    return korb_find_ifnone(c, slots, a);
}
/* find_all/select(Enumerable) → array of [k,v] pairs where block truthy */
static RESULT korb_m_hash_find_all(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "find_all", 8)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2*i]; slots[1] = korb_hash_pairs(h)[2*i+1];
        RESULT r = korb_hash_yield(c, slots + 3, block, def_env, cself, np, slots[0], slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) {
            CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        }
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* grep/grep_v(pattern): pairs where (pattern === [k,v]) == keep, as [k,v] array. */
static RESULT korb_hash_grep(CTX *c, korb_value *slots, korb_ref self, korb_slice a, bool keep) {
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    korb_ref dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_hash_pairs(h)[2*i]; slots[1] = korb_hash_pairs(h)[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));   /* pair at slots[2] */
        bool gm; CHECK(korb_pat_eq(c, slots + 3, KORB_SLICE_GET(a, 0), slots[2], &gm));
        if (gm == keep) CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_hash_grep(CTX *c, korb_value *slots, korb_ref self, korb_slice a)   { return korb_hash_grep(c, slots, self, a, true); }
static RESULT korb_m_hash_grep_v(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { return korb_hash_grep(c, slots, self, a, false); }
/* zip → array of rows: [ [k,v], other0[i], other1[i], ... ] per pair i */
/* i-th element of a zip source arg: Array → data[i], Range → lo+i (in bounds), else nil. */
static korb_value korb_zip_elem(korb_value arg, uint32_t i) {
    if (KORB_ARRAY_P(arg)) return i < VAL2ARY(arg)->len ? korb_ary_ptr(VAL2ARY(arg))[i] : KORB_NIL;
    if (KORB_RANGE_P(arg)) {
        korb_sword_t lo, hi;
        if (korb_range_int_bounds(VAL2RANGE(arg), &lo, &hi)) { korb_sword_t v = lo + (korb_sword_t)i; if (v < hi) return LONG2FIX(v); }
        else {   /* an Integer start with an endless / Float end (0..Float::INFINITY): by index too */
            const KorbRange *const rg = VAL2RANGE(arg);
            if (FIXNUM_P(rg->rbegin)) {
                const korb_sword_t v = FIX2LONG(rg->rbegin) + (korb_sword_t)i;
                double e;
                if (rg->rend == KORB_NIL) return LONG2FIX(v);
                if (KORB_FLOAT_P(rg->rend) && korb_num_to_d(rg->rend, &e) && (rg->exclude_end ? (double)v < e : (double)v <= e)) return LONG2FIX(v);
            }
        }
    }
    if (KORB_ARITHSEQ_P(arg)) {   /* e.g. n.upto(∞): begin + i*step, lazily by index */
        korb_value beginv, limv, stepv; bool excl;
        korb_aseq_params(VAL2ASEQ(arg), &beginv, &limv, &stepv, &excl);
        if (FIXNUM_P(beginv) && FIXNUM_P(stepv)) {
            const korb_sword_t v = FIX2LONG(beginv) + (korb_sword_t)i * FIX2LONG(stepv);
            if (limv == KORB_NIL) return LONG2FIX(v);                       /* endless */
            double dl;
            if (korb_num_to_d(limv, &dl) && isinf(dl) && dl > 0) return LONG2FIX(v);   /* +∞ limit */
            if (FIXNUM_P(limv) && v < FIX2LONG(limv)) return LONG2FIX(v);   /* finite (exclusive-ish) */
        }
    }
    return KORB_NIL;
}
static RESULT korb_m_hash_zip(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    uint32_t k = KORB_SLICE_LEN(a);
    uint32_t n = VAL2HASH(KORB_REF_GET(self))->len;
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, n)) : KORB_NIL;   /* dst (unused w/ block) */
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[1], &slots[2], &slots[3]));  /* pair at slots[3] */
        slots[4] = UNWRAP(korb_ary_new(c, slots + 5, k + 1));                /* row at slots[4] */
        korb_ref row = KORB_REF_AT(&slots[4]);
        CHECK(korb_ary_push_val(c, slots + 5, row, slots[3]));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 5, row, korb_zip_elem(KORB_SLICE_GET(a, j), i)));
        if (block != NULL) {
            slots[5] = (k == 0) ? slots[3] : slots[4];   /* no other args → yield the bare [k,v] pair */
            RESULT r = korb_block_yield(c, slots + 6, block, def_env, &slots[5], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 5, dst, slots[4]));
        }
    }
    return RESULT_OK(block != NULL ? KORB_NIL : KORB_REF_GET(dst));
}
/* max/min over [k,v] pairs (Array#<=>: key then value). want: 1 max, -1 min. */
static RESULT korb_hash_minmax(CTX *c, korb_value *slots, korb_ref self, int want) {
    const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
    if (h->len == 0) return RESULT_OK(KORB_NIL);
    uint32_t best = 0;
    for (uint32_t i = 1; i < h->len; i++) {
        h = VAL2HASH(KORB_REF_GET(self));
        int kc = korb_cmp_full(c, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*best]);
        int cmp = (kc != 0) ? kc : korb_cmp_full(c, korb_hash_pairs(h)[2*i+1], korb_hash_pairs(h)[2*best+1]);
        if (UNLIKELY(cmp == 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if ((want > 0 && cmp > 0) || (want < 0 && cmp < 0)) best = i;
    }
    h = VAL2HASH(KORB_REF_GET(self));
    slots[0] = korb_hash_pairs(h)[2*best]; slots[1] = korb_hash_pairs(h)[2*best+1];
    CHECK(korb_hash_make_pair(c, slots + 3, &slots[0], &slots[1], &slots[2]));
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_max(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t n;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(KORB_SLICE_GET(a, 0), &n)) {  /* max(n) → n largest pairs */
        slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));
        return korb_ary_minmax_n(c, slots + 1, KORB_REF_AT(&slots[0]), 1, n, NULL, NULL, NULL);
    }
    return korb_hash_minmax(c, slots, self,  1);
}
static RESULT korb_m_hash_min(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_sword_t n;
    if (KORB_SLICE_LEN(a) >= 1 && KORB_SLICE_GET(a, 0) != KORB_NIL && korb_to_index(KORB_SLICE_GET(a, 0), &n)) {  /* min(n) → n smallest pairs */
        slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));
        return korb_ary_minmax_n(c, slots + 1, KORB_REF_AT(&slots[0]), -1, n, NULL, NULL, NULL);
    }
    return korb_hash_minmax(c, slots, self, -1);
}
/* group_by → Hash{ block_key => [[k,v], ...] } over pairs */
static RESULT korb_m_hash_group_by(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    (void)a; if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "group_by", 8)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));     korb_ref h = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *hh = VAL2HASH(KORB_REF_GET(self));
        if (i >= hh->len) break;
        slots[1] = korb_hash_pairs(hh)[2*i]; slots[2] = korb_hash_pairs(hh)[2*i+1];
        CHECK(korb_hash_make_pair(c, slots + 5, &slots[1], &slots[2], &slots[3]));   /* pair at slots[3] */
        RESULT r = korb_hash_yield(c, slots + 5, block, def_env, cself, np, slots[1], slots[2]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[4] = r.value;                            /* key */
        int32_t idx = korb_hash_find(VAL2HASH(KORB_REF_GET(h)), slots[4]);
        if (idx < 0) {
            slots[5] = UNWRAP(korb_ary_new(c, slots + 6, 4));
            CHECK(korb_ary_push_val(c, slots + 6, KORB_REF_AT(&slots[5]), slots[3]));
            CHECK(korb_hash_set(c, slots + 6, h, KORB_REF_AT(&slots[4]), slots[5]));
        } else {
            slots[5] = korb_hash_pairs(VAL2HASH(KORB_REF_GET(h)))[2*idx+1];
            CHECK(korb_ary_push_val(c, slots + 6, KORB_REF_AT(&slots[5]), slots[3]));
        }
    }
    return RESULT_OK(KORB_REF_GET(h));
}
/* find_index → integer index of first pair where block truthy, else nil */
/* build [k,v] for pair i into slots[base..]; returns the pair korb_value rooted at slots[base+2]. */
static RESULT korb_hash_pair_at(CTX *c, korb_value *slots, korb_ref self, uint32_t i, korb_value *out) {
    const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
    slots[0] = korb_hash_pairs(h)[2*i]; slots[1] = korb_hash_pairs(h)[2*i+1];
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), slots[1]));
    *out = slots[2];
    return RESULT_OK(slots[2]);
}
static RESULT korb_m_hash_find_index(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (KORB_SLICE_LEN(a) >= 1) {                    /* find_index(pair): first index of matching [k,v] */
        for (uint32_t i = 0; i < VAL2HASH(KORB_REF_GET(self))->len; i++) {
            korb_value pair; CHECK(korb_hash_pair_at(c, slots, self, i, &pair));
            if (korb_value_eq(slots[2], KORB_SLICE_GET(a, 0))) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    if (UNLIKELY(block == NULL)) { slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(korb_intern_lit(c->vm, "find_index", 10)); return korb_send(c, slots + 2, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_hash_count(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    if (block != NULL && KORB_SLICE_LEN(a) == 0) {   /* count { |k,v| ... } */
        uint32_t np = korb_entry_params_cnt(block);
        korb_sword_t n = 0;
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            if (i >= h->len) break;
            RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, korb_hash_pairs(h)[2*i], korb_hash_pairs(h)[2*i+1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    if (KORB_SLICE_LEN(a) >= 1) {                    /* count(pair): pairs == arg */
        korb_sword_t n = 0;
        for (uint32_t i = 0; i < VAL2HASH(KORB_REF_GET(self))->len; i++) {
            korb_value pair; CHECK(korb_hash_pair_at(c, slots, self, i, &pair));
            if (korb_value_eq(slots[2], KORB_SLICE_GET(a, 0))) n++;
        }
        return RESULT_OK(LONG2FIX(n));
    }
    return RESULT_OK(LONG2FIX(VAL2HASH(KORB_REF_GET(self))->len));
}
static RESULT korb_m_hash_reduce(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    uint32_t op_mid;
    /* argc 2 → sym form (op=a[1], init=a[0]), block IGNORED; argc 1 + no block →
     * sym form (op=a[0]); argc 1 + block → block form with init (the arg is the
     * seed even when it is a String — `h.inject("") { … }`); argc 0 → block. */
    const uint32_t na0 = KORB_SLICE_LEN(a);
    bool sym_form = false;
    if (UNLIKELY(na0 >= 2 && block != NULL)) korb_warn_caller(c, slots, a.p + KORB_FLINK_OFF, "given block not used");
    if (na0 >= 2 || (na0 == 1 && block == NULL)) {
        bool ok; RESULT rr = korb_reduce_resolve_op(c, slots, KORB_SLICE_GET(a, na0 - 1), &op_mid, &ok);
        if (UNLIKELY(!ok)) return rr;
        sym_form = true;
    }
    if (block == NULL && !sym_form)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
    if (sym_form) {                               /* reduce(:+) / reduce(init, :+) on [k,v] pairs [block ignored] */
        uint32_t na = na0;
        bool have_acc = (na >= 2);
        if (have_acc) slots[0] = KORB_SLICE_GET(a, 0);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
            if (i >= h->len) break;
            slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
            slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));   /* [k,v] pair */
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
            if (!have_acc) { slots[0] = slots[3]; have_acc = true; continue; }
            slots[4] = slots[0]; slots[5] = slots[3];           /* recv=acc, arg=pair */
            RESULT r = korb_send_impl(c, slots + 6, op_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(have_acc ? slots[0] : KORB_NIL);
    }
    uint32_t np = korb_entry_params_cnt(block);   /* acc + pair: block takes |acc, (k,v)| */
    bool have_acc = (KORB_SLICE_LEN(a) >= 1);     /* no init → first [k,v] pair seeds the accumulator */
    if (have_acc) slots[0] = KORB_SLICE_GET(a, 0);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        /* build [k,v] pair and yield (acc, pair) */
        slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
        korb_value pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
        if (!have_acc) { slots[0] = slots[3]; have_acc = true; continue; }
        korb_value argv[2] = { slots[0], slots[3] };
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, np >= 2 ? 2 : 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(have_acc ? slots[0] : KORB_NIL);
}
static RESULT korb_m_hash_each_wo(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *captured_self) {
    if (KORB_SLICE_LEN(a) < 1) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    if (UNLIKELY(block == NULL)) {                /* no block → self.to_enum(:each_with_object, memo) */
        slots[0] = KORB_REF_GET(self);
        slots[1] = ID2SYM(korb_intern_lit(c->vm, "each_with_object", 16));
        slots[2] = KORB_SLICE_GET(a, 0);
        return korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "__to_enum_sized", 15), 0, 2, NULL, NULL, NULL);
    }
    slots[0] = KORB_SLICE_GET(a, 0);             /* memo */
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(KORB_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_hash_pairs(h)[2*i]; slots[2] = korb_hash_pairs(h)[2*i+1];
        korb_value pair = UNWRAP(korb_ary_new(c, slots + 3, 2));
        slots[3] = pair;
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 4, KORB_REF_AT(&slots[3]), slots[2]));
        korb_value argv[2] = { slots[3], slots[0] };   /* (pair, memo) */
        RESULT r = korb_block_yield(c, slots + 4, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
#undef HASH_REQ_BLOCK

