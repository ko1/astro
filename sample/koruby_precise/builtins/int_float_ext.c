/* koruby_precise — int_float_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Integer / Float methods ---------------------------------------- */
static RESULT korb_m_int_self2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_int_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; intptr_t n = FIX2LONG(VALUE_REF_GET(self)); intptr_t r;
    if (__builtin_mul_overflow(n, n, &r) || !FIXABLE(r)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Integer overflow (Bignum is not implemented)");
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_bits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    intptr_t m;
    if (UNLIKELY(!korb_to_index(o, &m))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(o));
    intptr_t n = FIX2LONG(VALUE_REF_GET(self)) & m;
    bool r = mode == 0 ? (n == 0) : mode == 1 ? (n != 0) : (n == m);   /* nobits/anybits/allbits */
    return RESULT_OK(r ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_nobits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_m_int_bits(c, slots, self, a, 0); }
static RESULT korb_m_int_anybits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 1); }
static RESULT korb_m_int_allbits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 2); }
static RESULT korb_m_int_gcdlcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    intptr_t x = FIX2LONG(VALUE_REF_GET(self)), y = FIX2LONG(o);
    intptr_t g = korb_int_gcd(x, y), l = (x == 0 || y == 0) ? 0 : (x / g) * y;
    if (l < 0) l = -l;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 2)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(g)));
    CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(l)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* cos(pi*x) / sin(pi*x) — exact 0/±1 at half-integers (matches CRuby's accurate
 * polar form so `(-1.0)**0.5` yields a clean (0.0+1.0i)). */
static double korb_cospi(double x) {
    double r = fmod(fabs(x), 2.0);
    if (r == 0.5 || r == 1.5) return 0.0;
    return cos(M_PI * x);
}
static double korb_sinpi(double x) {
    double sgn = x < 0 ? -1.0 : 1.0, r = fmod(fabs(x), 2.0);
    if (r == 0.0 || r == 1.0) return 0.0;
    if (r == 0.5) return sgn;
    if (r == 1.5) return -sgn;
    return sin(M_PI * x);
}
static RESULT korb_m_flt_pow(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double e; if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_type_name(VALUE_SLICE_GET(a, 0)));
    double base = VAL2FLT(VALUE_REF_GET(self))->val;
    if (base < 0 && e != floor(e)) {                  /* negative base ^ non-integer → Complex */
        double mag = pow(-base, e);
        slots[0] = UNWRAP(korb_float_new(c, slots, mag * korb_cospi(e)));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, mag * korb_sinpi(e)));
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    return korb_float_new(c, slots, pow(base, e));
}
static RESULT korb_m_flt_angle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d = VAL2FLT(VALUE_REF_GET(self))->val;
    return d < 0 ? korb_float_new(c, slots, M_PI) : RESULT_OK(LONG2FIX(0));   /* arg: 0 (Integer) or PI */
}
static RESULT korb_num_between(CTX *c, VALUE *slots, VALUE self, VALUE lo, VALUE hi);
static RESULT korb_m_flt_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_num_between(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_flt_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double s = VAL2FLT(VALUE_REF_GET(self))->val, lo, hi;
    VALUE vlo, vhi;
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* clamp(lo..hi) */
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        vlo = r->rbegin; vhi = r->rend;
    } else { vlo = VALUE_SLICE_GET(a, 0); vhi = VALUE_SLICE_GET(a, 1); }
    if (vlo != KORB_NIL) {
        if (UNLIKELY(!korb_num_to_d(vlo, &lo))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if (s < lo) return RESULT_OK(vlo);
    }
    if (vhi != KORB_NIL) {
        if (UNLIKELY(!korb_num_to_d(vhi, &hi))) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison failed");
        if (s > hi) return RESULT_OK(vhi);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_insert(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t idx;
    if (UNLIKELY(!korb_to_index(iv, &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    uint32_t k = VALUE_SLICE_LEN(a) - 1;
    if (k == 0) return RESULT_OK(VALUE_REF_GET(self));
    intptr_t orig = idx;
    uint32_t oldlen = VAL2ARY(VALUE_REF_GET(self))->len;
    if (idx < 0) idx += (intptr_t)oldlen + 1;
    if (UNLIKELY(idx < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array", (long)orig);
    uint32_t at = (uint32_t)idx;
    uint32_t pad = at > oldlen ? at - oldlen : 0;
    CHECK(korb_ary_ensure(c, slots, self, pad + k));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    if (at <= oldlen) {
        for (int32_t r = (int32_t)oldlen - 1; r >= (int32_t)at; r--) ARO_STORE(c, it, &it->data[r + k], it->data[r]);
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &it->data[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = oldlen + k;
    } else {
        for (uint32_t r = oldlen; r < at; r++) it->data[r] = KORB_NIL;
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &it->data[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = at + k;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#sort → array of [k,v] pairs sorted by key; fetch_values(*keys) */
static RESULT korb_m_hash_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    RESULT ar = korb_m_hash_to_a(c, slots, self, a);     /* pairs array (rooted via return) */
    if (ar.state != KORB_NORMAL) return ar;
    VALUE_REF dst = SLOTS_PUSH(slots, ar.value);
    KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));           /* in-place insertion sort by pair[0] */
    VALUE *data = d->items->data;
    for (uint32_t i = 1; i < d->len; i++) {
        VALUE key = data[i]; uint32_t j = i;
        while (j > 0) {
            VALUE pa = VAL2ARY(data[j-1])->items->data[0], pb = VAL2ARY(key)->items->data[0];
            int cmp = korb_cmp_full(c, pa, pb);
            if (cmp != 1) break;
            data[j] = data[j-1]; j--;
        }
        data[j] = key;
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, j));
        if (idx < 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "key not found");
        CHECK(korb_ary_push_val(c, slots + 1, dst, h->items->data[2 * idx + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* delete_if/reject!(keep_when_false) and keep_if/select!(keep_when_true), in-place */
static RESULT korb_ary_filter_bang(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE cself, bool keep_truthy, bool ret_nil_if_unchanged) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "in-place filter without a block is not supported");
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; ; r++) {
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (r >= ary->len) break;
        slots[0] = ary->items->data[r];                    /* root elem across the yield */
        RESULT res = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        bool kept = (KORB_TRUTHY(res.value) == keep_truthy);
        if (kept) {
            KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
            if (w != r) ARO_STORE(c, a2->items, &a2->items->data[w], slots[0]);
            w++;
        } else changed = true;
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t r = w; r < ary->len; r++) ary->items->data[r] = KORB_NIL;
    ary->len = w;
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, false); }
static RESULT korb_m_ary_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, true); }
static RESULT korb_m_ary_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, false); }
static RESULT korb_m_ary_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, true); }

static RESULT korb_hash_pair_at(CTX *c, VALUE *slots, VALUE_REF self, uint32_t i, VALUE *out);
static RESULT korb_m_hash_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Hash#take_while without a block is not supported");
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        VALUE pair; CHECK(korb_hash_pair_at(c, slots + 1, self, i, &pair));   /* pair at slots[3] */
        CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    if (block == NULL) {                              /* sum(init): fold init + [k,v] over pairs via + */
        if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no init given");
        uint32_t plus_mid = korb_intern(c->vm, "+", 1);
        slots[0] = VALUE_SLICE_GET(a, 0);             /* acc = init (rooted) */
        for (uint32_t i = 0; i < VAL2HASH(VALUE_REF_GET(self))->len; i++) {
            VALUE pair; CHECK(korb_hash_pair_at(c, slots + 1, self, i, &pair));   /* pair at slots[3] */
            slots[4] = slots[0]; slots[5] = slots[3];                            /* recv=acc, arg=pair */
            RESULT r = korb_send_impl(c, slots + 6, plus_mid, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    uint32_t np = korb_entry_params_cnt(block);
    intptr_t acc = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, h->items->data[2*i], h->items->data[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (UNLIKELY(!FIXNUM_P(r.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "Hash#sum block must return Integer here");
        acc += FIX2LONG(r.value);
    }
    return RESULT_OK(LONG2FIX(acc));
}

static RESULT korb_m_obj_false(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }

static RESULT korb_m_ary_difference(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++)    /* difference(*arrays) */
        if (UNLIKELY(!KORB_ARRAY_P(VALUE_SLICE_GET(a, k)))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        bool removed = false;
        for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) if (korb_ary_has(VAL2ARY(VALUE_SLICE_GET(a, k)), e)) { removed = true; break; }
        if (!removed) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
RESULT korb_sub_slow(CTX *c, VALUE *slots, VALUE_REF lhs, VALUE rhs, uint32_t line) {
    VALUE l = VALUE_REF_GET(lhs);
    if (KORB_ARRAY_P(l)) {                            /* Array - Array → set difference */
        slots[0] = rhs;
        return korb_m_ary_difference(c, slots + 1, lhs, VALUE_SLICE_MAKE(slots, 1));
    }
    if (KORB_SET_P(l)) { slots[0] = rhs; return korb_m_set_diff(c, slots + 1, lhs, VALUE_SLICE_MAKE(&slots[0], 1)); }   /* Set - → difference */
    return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '-' for %s", korb_a_type_name(l));
}
static RESULT korb_m_ary_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
    VAL2ARY(VALUE_REF_GET(self))->len = 0;               /* clear, then copy other */
    uint32_t on = VAL2ARY(VALUE_SLICE_GET(a, 0))->len;
    for (uint32_t i = 0; i < on; i++)
        CHECK(korb_ary_push_val(c, slots, self, VAL2ARY(VALUE_SLICE_GET(a, 0))->items->data[i]));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = (uint32_t)n; i < len; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = h->items->data[2*i]; slots[1] = h->items->data[2*i+1];
        VALUE pair = UNWRAP(korb_ary_new(c, slots + 2, 2));
        slots[2] = pair;
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* dup: shallow copy. Immutables (fixnum/symbol/nil/true/false/float) return self;
 * String/Array/Hash get a fresh shallow copy. */
static RESULT korb_m_obj_dup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE v = VALUE_REF_GET(self);
    if (KORB_STRING_P(v)) {
        uint32_t len = VAL2STR(v)->len;
        KorbString *r = korb_str_alloc(c, slots, len);
        const KorbString *s = VAL2STR(VALUE_REF_GET(self));   /* re-read after alloc */
        memcpy(r->buf->data, s->buf->data, len);
        return RESULT_OK((VALUE)r);
    }
    if (KORB_ARRAY_P(v)) {
        uint32_t n = VAL2ARY(v)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
        for (uint32_t i = 0; i < n; i++) {
            VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
            CHECK(korb_ary_push_val(c, slots, dst, e));
        }
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_HASH_P(v)) {
        uint32_t n = VAL2HASH(v)->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_hash_new(c, slots, n)));
        for (uint32_t i = 0; i < n; i++) {
            slots[0] = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i];
            VALUE val = VAL2HASH(VALUE_REF_GET(self))->items->data[2 * i + 1];
            CHECK(korb_hash_set(c, slots + 1, dst, VALUE_REF_AT(&slots[0]), val));
        }
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    return RESULT_OK(v);   /* immutable / no special copy */
}

/* in-place reverse of items[lo, hi) — no alloc, so pointers are stable. */
static void korb_ary_rev_range(CTX *c, KorbArrayItems *it, uint32_t lo, uint32_t hi) {
    while (lo + 1 < hi + 1 && lo < hi) {       /* lo < hi (guard wrap) */
        hi--;
        VALUE t = it->data[lo];
        ARO_STORE(c, it, &it->data[lo], it->data[hi]);
        ARO_STORE(c, it, &it->data[hi], t);
        lo++;
    }
}
static RESULT korb_m_ary_reverse_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    korb_ary_rev_range(c, ary->items, 0, ary->len);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_rotate_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t cnt = 1;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE cv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(cv, &cnt))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(cv));
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    uint32_t n = ary->len;
    if (n > 1) {
        intptr_t k = ((cnt % (intptr_t)n) + (intptr_t)n) % (intptr_t)n;   /* normalize */
        KorbArrayItems *it = ary->items;
        korb_ary_rev_range(c, it, 0, (uint32_t)k);
        korb_ary_rev_range(c, it, (uint32_t)k, n);
        korb_ary_rev_range(c, it, 0, n);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* product(other, ...) → cartesian product as an array of rows. */
static RESULT korb_m_ary_product(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    uint32_t na = VALUE_SLICE_LEN(a);
    if (na > 15) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#product with >16 arrays is not supported");
    for (uint32_t j = 0; j < na; j++)
        if (UNLIKELY(!KORB_ARRAY_P(VALUE_SLICE_GET(a, j))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, j)));
    uint32_t k = na + 1;                                  /* self + args */
    #define ARR_J(j) ((j) == 0 ? VAL2ARY(VALUE_REF_GET(self)) : VAL2ARY(VALUE_SLICE_GET(a, (j) - 1)))
    uint32_t lens[16];
    uint64_t total = 1;
    for (uint32_t j = 0; j < k; j++) { lens[j] = ARR_J(j)->len; total *= lens[j]; }
    /* with a block: yield each combination, return self (no result array) */
    if (block != NULL) {
        if (total == 0) return RESULT_OK(VALUE_REF_GET(self));
        uint32_t bidx[16] = {0};
        for (uint64_t t = 0; t < total; t++) {
            slots[0] = UNWRAP(korb_ary_new(c, slots + 1, k));   /* row at slots[0] */
            VALUE_REF row = VALUE_REF_AT(&slots[0]);
            for (uint32_t j = 0; j < k; j++) CHECK(korb_ary_push_val(c, slots + 1, row, ARR_J(j)->items->data[bidx[j]]));
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            for (int j = (int)k - 1; j >= 0; j--) { if (++bidx[j] < lens[j]) break; bidx[j] = 0; }
        }
        return RESULT_OK(VALUE_REF_GET(self));
    }
    uint32_t capa = total > 0 && total < 0x40000000ull ? (uint32_t)total : 4;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, capa)));
    if (total == 0) return RESULT_OK(VALUE_REF_GET(dst));
    uint32_t idx[16] = {0};
    for (uint64_t t = 0; t < total; t++) {
        slots[0] = UNWRAP(korb_ary_new(c, slots, k));    /* row, rooted at slots[0] */
        VALUE_REF row = VALUE_REF_AT(&slots[0]);
        for (uint32_t j = 0; j < k; j++) {
            VALUE e = ARR_J(j)->items->data[idx[j]];
            CHECK(korb_ary_push_val(c, slots + 1, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        for (int j = (int)k - 1; j >= 0; j--) { if (++idx[j] < lens[j]) break; idx[j] = 0; }
    }
    #undef ARR_J
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        VALUE iv = VALUE_SLICE_GET(a, j);
        intptr_t idx;
        if (UNLIKELY(!korb_to_index(iv, &idx))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        intptr_t n = ary->len, orig = idx;
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld outside of array bounds: -%ld...%ld", (long)orig, (long)n, (long)n);
        CHECK(korb_ary_push_val(c, slots + 1, dst, ary->items->data[idx]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* one?: exactly one truthy element (or exactly one block-truthy element). */
static RESULT korb_m_ary_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE captured_self) {
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[0] = ary->items->data[i];
        bool t;
        if (has_pat) {
            t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]);
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(slots[0]);
        }
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}

