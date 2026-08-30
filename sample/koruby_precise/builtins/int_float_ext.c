/* koruby_precise — int_float_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Integer / Float methods ---------------------------------------- */
static RESULT korb_m_int_self2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* #clone for an always-frozen immediate (Integer/Float/Symbol/Rational/Complex,
 * nil/true/false): returns self, but honours the freeze: keyword — freeze:false
 * can't unfreeze an immutable value → ArgumentError (CRuby). */
static RESULT korb_m_immed_clone(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n >= 1) {
        const VALUE last = VALUE_SLICE_GET(a, n - 1);
        if (UNLIKELY(!KORB_HASH_P(last)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0)", n);
        const int32_t fi = korb_hash_find(VAL2HASH(last), ID2SYM(korb_intern(c->vm, "freeze", 6)));
        if (fi >= 0) {
            const VALUE fv = korb_items_data(VAL2HASH(last)->items)[2 * fi + 1];
            if (UNLIKELY(fv != KORB_NIL && fv != KORB_TRUE && fv != KORB_FALSE))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unexpected value for freeze: %s", korb_type_name(fv));
            if (fv == KORB_FALSE)
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "can't unfreeze %s", korb_type_name(VALUE_REF_GET(self)));
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_int_abs2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE sv = VALUE_REF_GET(self);
    if (!FIXNUM_P(sv)) { korb_mp_t z; korb_to_mpz(sv, z); korb_mp_mul(z, z, z); RESULT out = korb_big_from_mpz(c, slots, z); korb_mp_clear(z); return out; }
    korb_sword_t n = FIX2LONG(sv), r;
    if (__builtin_mul_overflow(n, n, &r) || !FIXABLE(r)) {
        korb_mp_t z; korb_to_mpz(sv, z); korb_mp_mul(z, z, z); RESULT out = korb_big_from_mpz(c, slots, z); korb_mp_clear(z); return out;
    }
    return RESULT_OK(LONG2FIX(r));
}
static RESULT korb_m_int_bits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int mode) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_INTEGER_P(o))) {                   /* coerce a Float / #to_int object to an Integer (CRuby rb_to_int) */
        if (KORB_OBJECT_P(o) || KORB_FLOAT_P(o)) {
            slots[0] = o;
            RESULT tr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
            if (KORB_INTEGER_P(tr.value)) o = tr.value;
        }
        if (UNLIKELY(!KORB_INTEGER_P(o)))
            return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
    }
    const VALUE sv = VALUE_REF_GET(self);                 /* re-read: #to_int may have GC'd (self stays rooted) */
    if (LIKELY(FIXNUM_P(sv) && FIXNUM_P(o))) {
        const korb_sword_t m = FIX2LONG(o), n = FIX2LONG(sv) & m;
        const bool r = mode == 0 ? (n == 0) : mode == 1 ? (n != 0) : (n == m);   /* nobits/anybits/allbits */
        return RESULT_OK(r ? KORB_TRUE : KORB_FALSE);
    }
    slots[0] = o;                                         /* root o across the AND alloc + compare */
    RESULT ar = korb_int_bitwise(c, slots + 1, sv, o, 0);   /* self & o */
    if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
    bool r;
    if (mode == 2) r = (korb_int_cmp(ar.value, slots[0]) == 0);       /* allbits: (self & o) == o */
    else { const bool z = (korb_int_cmp(ar.value, LONG2FIX(0)) == 0); r = (mode == 0) ? z : !z; }
    return RESULT_OK(r ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_int_nobits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_m_int_bits(c, slots, self, a, 0); }
static RESULT korb_m_int_anybits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 1); }
static RESULT korb_m_int_allbits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_int_bits(c, slots, self, a, 2); }
/* korb_m_int_gcd / korb_m_int_lcm are defined earlier in the TU (integer.c). */
static RESULT korb_m_int_gcdlcm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(!KORB_INTEGER_P(VALUE_SLICE_GET(a, 0)))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion");
    RESULT gr = korb_m_int_gcd(c, slots, self, a); if (UNLIKELY(gr.state != KORB_NORMAL)) return gr; slots[0] = gr.value;
    RESULT lr = korb_m_int_lcm(c, slots + 1, self, a); if (UNLIKELY(lr.state != KORB_NORMAL)) return lr; slots[1] = lr.value;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, dst, slots[1]));
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
    double e;
    if (UNLIKELY(!korb_num_to_d(VALUE_SLICE_GET(a, 0), &e))) {
        const VALUE ev = VALUE_SLICE_GET(a, 0);
        if (KORB_OBJECT_P(ev)) { bool h; RESULT cr = korb_try_coerce(c, slots, VALUE_REF_GET(self), ev, "**", 0, &h); if (h) return cr; }
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s can't be coerced into Float", korb_coerce_name(c, ev));
    }
    double base = korb_float_val(VALUE_REF_GET(self));
    if (base < 0 && e != floor(e)) {                  /* negative base ^ non-integer → Complex */
        double mag = pow(-base, e);
        slots[0] = UNWRAP(korb_float_new(c, slots, mag * korb_cospi(e)));
        slots[1] = UNWRAP(korb_float_new(c, slots + 1, mag * korb_sinpi(e)));
        return korb_cpx_new(c, slots + 2, slots[0], slots[1]);
    }
    return korb_float_new(c, slots, pow(base, e));
}
static RESULT korb_m_flt_angle(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; double d = korb_float_val(VALUE_REF_GET(self));
    if (isnan(d)) return RESULT_OK(VALUE_REF_GET(self));                      /* arg(NaN) = NaN */
    return signbit(d) ? korb_float_new(c, slots, M_PI) : RESULT_OK(LONG2FIX(0));  /* arg: 0 or PI; -0.0 → PI (signbit) */
}
static RESULT korb_num_between(CTX *c, VALUE *slots, VALUE self, VALUE lo, VALUE hi);
static RESULT korb_m_flt_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_num_between(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_flt_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    double s = korb_float_val(VALUE_REF_GET(self)), lo, hi;
    VALUE vlo, vhi;
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {   /* clamp(lo..hi) */
        const KorbRange *r = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        if (UNLIKELY(r->exclude_end)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot clamp with an exclusive range");
        vlo = r->rbegin; vhi = r->rend;
    } else { vlo = VALUE_SLICE_GET(a, 0); vhi = VALUE_SLICE_GET(a, 1); }
    if (vlo != KORB_NIL && vhi != KORB_NIL) {          /* CRuby: min must be <= max */
        double al, ah;
        if (korb_num_to_d(vlo, &al) && korb_num_to_d(vhi, &ah) && al > ah)
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "min argument must be less than or equal to max argument");
    }
    if (UNLIKELY(isnan(s)) && (vlo != KORB_NIL || vhi != KORB_NIL))   /* NaN can't be compared with the bounds */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of Float with %s failed", korb_type_name(vlo != KORB_NIL ? vlo : vhi));
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
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));    /* insert checks frozen upfront (even with no values) */
    VALUE iv = VALUE_SLICE_GET(a, 0);
    korb_sword_t idx;
    if (UNLIKELY(!korb_to_index(iv, &idx))) {            /* coerce position via #to_int */
        RESULT cr = korb_coerce_to_int(c, slots, &iv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(iv, &idx)) return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 0));
    }
    uint32_t k = VALUE_SLICE_LEN(a) - 1;
    if (k == 0) return RESULT_OK(VALUE_REF_GET(self));
    korb_sword_t orig = idx;
    uint32_t oldlen = VAL2ARY(VALUE_REF_GET(self))->len;
    if (idx < 0) idx += (korb_sword_t)oldlen + 1;
    if (UNLIKELY(idx < 0)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld too small for array", (long)orig);
    uint32_t at = (uint32_t)idx;
    uint32_t pad = at > oldlen ? at - oldlen : 0;
    CHECK(korb_ary_ensure(c, slots, self, pad + k));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    if (at <= oldlen) {
        for (int32_t r = (int32_t)oldlen - 1; r >= (int32_t)at; r--) ARO_STORE(c, it, &korb_items_data(it)[r + k], korb_items_data(it)[r]);
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &korb_items_data(it)[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = oldlen + k;
    } else {
        for (uint32_t r = oldlen; r < at; r++) ARO_STORE(c, it, &korb_items_data(it)[r], KORB_NIL);
        for (uint32_t j = 0; j < k; j++) ARO_STORE(c, it, &korb_items_data(it)[at + j], VALUE_SLICE_GET(a, 1 + j));
        ary->len = at + k;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Hash#sort → array of [k,v] pairs sorted by key; fetch_values(*keys) */
static RESULT korb_m_hash_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    RESULT ar = korb_m_hash_to_a(c, slots, self, a);     /* pairs array (rooted via return) */
    if (ar.state != KORB_NORMAL) return ar;
    VALUE_REF dst = SLOTS_PUSH(slots, ar.value);
    const uint32_t len = VAL2ARY(VALUE_REF_GET(dst))->len;   /* insertion sort by pair (block comparator or <=>) */
    for (uint32_t i = 1; i < len; i++) {
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(dst))->items)[i];   /* key pair, rooted across yields */
        uint32_t j = i;
        while (j > 0) {
            KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
            const VALUE pa = korb_items_data(d->items)[j-1], pb = slots[0];
            int cmp;
            if (block != NULL) {                          /* compare whole pairs (CRuby yields the [k,v] arrays) */
                RESULT cr = korb_cmp_block(c, slots + 1, pa, pb, block, def_env, cself, &cmp);
                if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            } else {
                cmp = korb_cmp_full(c, korb_items_data(VAL2ARY(pa)->items)[0], korb_items_data(VAL2ARY(pb)->items)[0]);
            }
            if (cmp != 1) break;
            d = VAL2ARY(VALUE_REF_GET(dst));              /* re-fetch: a block yield may have moved the array */
            ARO_STORE(c, d->items, &korb_items_data(d->items)[j], korb_items_data(d->items)[j-1]); j--;
        }
        KorbArray *d = VAL2ARY(VALUE_REF_GET(dst));
        ARO_STORE(c, d->items, &korb_items_data(d->items)[j], slots[0]);
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Hash#to_h [{ |k,v| [nk,nv] }] — without a block returns self; with a block
 * builds a new Hash from the [new_key, new_value] pairs the block returns. */
static RESULT korb_m_hash_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) {
        /* a plain Hash is answered as is, but a subclass instance must become a
         * real Hash (CRuby) */
        const VALUE sv = VALUE_REF_GET(self);
        if (LIKELY(!(AROH_IS_GC_OBJECT(sv) && (((const AroObjectHeader *)(uintptr_t)sv)->flags & KORB_FL_HAS_KLASS))))
            return RESULT_OK(sv);
        slots[0] = UNWRAP(korb_hash_new(c, slots, VAL2HASH(sv)->len));
        VALUE_REF plain = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
            if (i >= h->len) break;
            slots[1] = korb_items_data(h->items)[2*i];
            slots[2] = korb_items_data(h->items)[2*i+1];
            CHECK(korb_hash_set(c, slots + 3, plain, VALUE_REF_AT(&slots[1]), slots[2]));
        }
        {   /* the plain copy keeps the default / default_proc / compare_by_identity (CRuby) */
            KorbHash *const d = VAL2HASH(VALUE_REF_GET(plain));
            const KorbHash *const s0 = VAL2HASH(VALUE_REF_GET(self));
            ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->default_val,  s0->default_val);
            ARO_STORE(c, d, (VALUE *)(uintptr_t)&d->default_proc, s0->default_proc);
            if (s0->head.flags & KORB_FL_CMP_BY_ID) d->head.flags |= KORB_FL_CMP_BY_ID;
        }
        return RESULT_OK(VALUE_REF_GET(plain));
    }
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));
    VALUE_REF nh = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[1] = korb_items_data(h->items)[2*i]; slots[2] = korb_items_data(h->items)[2*i+1];
        VALUE argv[2] = { slots[1], slots[2] };
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[3] = r.value;
        if (UNLIKELY(!KORB_ARRAY_P(slots[3]))) {          /* a pair may be any #to_ary object */
            const char *const cls = korb_coerce_name(c, slots[3]);
            const RESULT cr = korb_coerce_to_ary(c, slots + 4, &slots[3]);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (UNLIKELY(cr.value != KORB_TRUE) || !KORB_ARRAY_P(slots[3]))
                return korb_raise(c, slots + 4, KORB_E_TYPE, 0,
                                  "wrong element type %s (expected array)", cls);
        }
        /* the pair must be exactly [key, value] — a different length is an
         * ArgumentError, not a type error */
        if (UNLIKELY(VAL2ARY(slots[3])->len != 2))
            return korb_raise(c, slots + 4, KORB_E_ARGUMENT, 0,
                              "element has wrong array length (expected 2, was %u)", VAL2ARY(slots[3])->len);
        slots[4] = korb_items_data(VAL2ARY(slots[3])->items)[0];     /* new key */
        slots[5] = korb_items_data(VAL2ARY(slots[3])->items)[1];     /* new value */
        CHECK(korb_hash_set(c, slots + 6, nh, VALUE_REF_AT(&slots[4]), slots[5]));
    }
    return RESULT_OK(VALUE_REF_GET(nh));
}
static RESULT korb_m_hash_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        int32_t idx = korb_hash_find(h, VALUE_SLICE_GET(a, j));
        if (idx < 0) {
            if (block != NULL) {                          /* block form: yield the missing key, use its result */
                slots[1] = VALUE_SLICE_GET(a, j);         /* root the key across the yield */
                RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
                continue;
            }
            {   char *kb = NULL; size_t ksz = 0; FILE *km = open_memstream(&kb, &ksz);
                if (km) { korb_fprint_inspect(c, km, VALUE_SLICE_GET(a, j)); fclose(km); }
                char msg[512]; snprintf(msg, sizeof msg, "key not found: %s", kb ? kb : "");
                free(kb);
                return korb_raise_key(c, slots + 1, VALUE_REF_GET(self), VALUE_SLICE_GET(a, j), msg);   /* KeyError w/ #receiver + #key */
            }
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(h->items)[2 * idx + 1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* delete_if/reject!(keep_when_false) and keep_if/select!(keep_when_true), in-place */
static RESULT korb_ary_to_enum(CTX *c, VALUE *slots, VALUE_REF self, const char *meth);   /* fwd (array_enum.c) */
static RESULT korb_ary_filter_bang(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, bool keep_truthy, bool ret_nil_if_unchanged, const char *meth) {
    if (UNLIKELY(block == NULL)) return korb_ary_to_enum(c, slots, self, meth);   /* no block → an Enumerator */
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* CRuby raises FrozenError upfront, even on an empty array */
    uint32_t w = 0; bool changed = false;
    for (uint32_t r = 0; ; r++) {
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (r >= ary->len) break;
        slots[0] = korb_items_data(ary->items)[r];                    /* root elem across the yield */
        RESULT res = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(res.state != KORB_NORMAL)) {          /* block raised/broke: finalize like CRuby */
            /* keep the survivors decided so far ([0,w)) + the unprocessed tail
             * ([r,len), incl. the current element), dropping the already-deleted
             * ones ([w,r)); then propagate the non-normal state. */
            KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
            if (w < r) {                                   /* left-shift the tail down over the deleted gap */
                const uint32_t tail = a2->len - r;
                for (uint32_t k = 0; k < tail; k++) ARO_STORE(c, a2->items, &korb_items_data(a2->items)[w + k], korb_items_data(a2->items)[r + k]);
                const uint32_t newlen = w + tail;
                for (uint32_t k = newlen; k < a2->len; k++) ARO_STORE(c, a2->items, &korb_items_data(a2->items)[k], KORB_NIL);
                a2->len = newlen;
            }
            return res;
        }
        bool kept = (KORB_TRUTHY(res.value) == keep_truthy);
        if (kept) {
            KorbArray *a2 = VAL2ARY(VALUE_REF_GET(self));
            if (w != r) ARO_STORE(c, a2->items, &korb_items_data(a2->items)[w], slots[0]);
            w++;
        } else changed = true;
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, ary->items, &korb_items_data(ary->items)[r], KORB_NIL);
    ary->len = w;
    if (ret_nil_if_unchanged && !changed) return RESULT_OK(KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_delete_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, false, "delete_if"); }
static RESULT korb_m_ary_reject_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, false, true, "reject!"); }
static RESULT korb_m_ary_keep_if(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, false, "keep_if"); }
static RESULT korb_m_ary_select_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_ary_filter_bang(c, slots, self, block, def_env, cself, true, true, "select!"); }

static RESULT korb_hash_pair_at(CTX *c, VALUE *slots, VALUE_REF self, uint32_t i, VALUE *out);
static RESULT korb_m_hash_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) { slots[0] = VALUE_REF_GET(self); slots[1] = ID2SYM(korb_intern(c->vm, "take_while", 10)); return korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1); }
    uint32_t np = korb_entry_params_cnt(block);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots + 1, block, def_env, cself, np, korb_items_data(h->items)[2*i], korb_items_data(h->items)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        VALUE pair; CHECK(korb_hash_pair_at(c, slots + 1, self, i, &pair));   /* pair at slots[3] */
        CHECK(korb_ary_push_val(c, slots + 4, dst, slots[3]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_hash_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) {                              /* sum(init): fold init + [k,v] over pairs via + */
        uint32_t plus_mid = korb_intern(c->vm, "+", 1);
        slots[0] = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : LONG2FIX(0);   /* acc = init (default 0), rooted */
        for (uint32_t i = 0; i < VAL2HASH(VALUE_REF_GET(self))->len; i++) {
            VALUE pair; CHECK(korb_hash_pair_at(c, slots + 1, self, i, &pair));   /* pair at slots[3] */
            slots[4] = slots[0]; slots[5] = slots[3];                            /* recv=acc, arg=pair */
            RESULT r = korb_send_impl(c, slots + 6, plus_mid, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    uint32_t np = korb_entry_params_cnt(block);
    korb_sword_t acc = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    for (uint32_t i = 0; ; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        RESULT r = korb_hash_yield(c, slots, block, def_env, cself, np, korb_items_data(h->items)[2*i], korb_items_data(h->items)[2*i+1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (UNLIKELY(!FIXNUM_P(r.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "Hash#sum block must return Integer here");
        acc += FIX2LONG(r.value);
    }
    return RESULT_OK(LONG2FIX(acc));
}

static RESULT korb_m_obj_false(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }

static RESULT korb_m_ary_difference(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* difference(*arrays): coerce each via #to_ary */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) {
            RESULT cr = korb_coerce_to_ary(c, slots, &ov);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
            VALUE_REF_SET(VALUE_SLICE_REF(a, k), ov);
        }
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* e, rooted across #eql? dispatch + push */
        bool removed = false;                            /* removed when present in any arg array (by #hash + #eql?) */
        for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {
            bool has; CHECK(korb_arr_member_eql(c, slots + 2, VALUE_SLICE_REF(a, k), slots[1], &has));
            if (has) { removed = true; break; }
        }
        if (!removed) CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
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
    { bool h; RESULT ur = korb_user_binop(c, slots, l, rhs, "-", &h); if (h) return ur; }
    return korb_raise(c, slots, KORB_E_NOMETHOD, line, "undefined method '-' for %s", korb_a_type_name(l));
}
static RESULT korb_m_ary_replace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));    /* modify-check before coercing the argument */
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* other (rooted; possibly coerced) */
    if (UNLIKELY(!KORB_ARRAY_P(slots[0]))) {            /* coerce via #to_ary before mutating self */
        const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
        if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], to_ary)) {
            RESULT cr = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            slots[0] = cr.value;
        }
        if (!KORB_ARRAY_P(slots[0])) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    if (slots[0] == VALUE_REF_GET(self)) return RESULT_OK(VALUE_REF_GET(self));   /* a.replace(a) is a no-op (clear-then-copy would empty it) */
    VAL2ARY(VALUE_REF_GET(self))->len = 0;               /* clear, then copy other */
    const uint32_t on = VAL2ARY(slots[0])->len;
    for (uint32_t i = 0; i < on; i++)
        CHECK(korb_ary_push_val(c, slots + 1, self, korb_items_data(VAL2ARY(slots[0])->items)[i]));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_hash_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise_no_int(c, slots, nv);
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2HASH(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (uint32_t i = (uint32_t)n; i < len; i++) {
        const KorbHash *h = VAL2HASH(VALUE_REF_GET(self));
        if (i >= h->len) break;
        slots[0] = korb_items_data(h->items)[2*i]; slots[1] = korb_items_data(h->items)[2*i+1];
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
/* Run the copy hook on the new object at slots[1] with the original as its one
 * positional argument (plus clone's freeze: Hash when there is one). */


static RESULT korb_m_obj_dup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (below) */
/* Run the copy hook on the new object at slots[1] with the original as its one
 * positional argument (plus clone's freeze: Hash when there is one). */
static RESULT korb_copy_hook(CTX *c, VALUE *slots, VALUE_REF self, uint32_t hook_mid, const VALUE *hook_kw) {
    slots[2] = VALUE_REF_GET(self);                      /* orig (arg); recv = the new object at slots[1] */
    if (hook_kw == NULL || *hook_kw == KORB_NIL) return korb_send_impl(c, slots + 3, hook_mid, 0, 1, NULL, NULL, NULL);
    slots[3] = *hook_kw;                                 /* clone's freeze: keywords — read from the caller's rooted slot */
    return korb_send_impl(c, slots + 4, hook_mid, 0, 2, NULL, NULL, NULL);
}
/* Copy the source's generic (side-table) ivars onto the fresh copy at *pdst.
 * CRuby copies ivars before the copy hook, so a user #initialize_copy can still
 * overwrite them.  No-op unless the source has any. */
static RESULT korb_copy_gen_ivars(CTX *c, VALUE *slots, VALUE_REF self, VALUE *pdst) {
    const VALUE src = VALUE_REF_GET(self);
    if (!AROH_IS_GC_OBJECT(src) || !(((const AroObjectHeader *)(uintptr_t)src)->flags & KORB_FL_HAS_IVARS)) return RESULT_OK(*pdst);
    if (!AROH_IS_GC_OBJECT(*pdst)) return RESULT_OK(*pdst);
    for (uint32_t i = 0; ; i++) {
        const VALUE h = korb_objivar_hash_of(c->vm, VALUE_REF_GET(self));   /* re-read: ivar_set GCs */
        if (h == KORB_NIL || i >= VAL2HASH(h)->len) break;
        slots[0] = korb_items_data(VAL2HASH(h)->items)[2 * i];
        slots[1] = korb_items_data(VAL2HASH(h)->items)[2 * i + 1];
        CHECK(korb_ivar_set(c, slots + 2, VALUE_REF_AT(pdst), slots[0], slots[1]));
    }
    return RESULT_OK(*pdst);
}
/* Shared body of #dup and #clone.  `hook_mid` is the copy hook CRuby runs on the
 * new object (initialize_dup / initialize_clone); `hook_kw` is the freeze: Hash
 * clone passes along, or nil. */
static RESULT korb_obj_copy_impl(CTX *c, VALUE *slots, VALUE_REF self, uint32_t hook_mid, const VALUE *hook_kw) {
    const VALUE v = VALUE_REF_GET(self);
    /* preserve the subclass: a builtin-subclass instance dups to the same class */
    const bool sub = AROH_IS_GC_OBJECT(v) && (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_HAS_KLASS);
    slots[0] = sub ? korb_klass_override_get(c->vm, v) : KORB_NIL;   /* override class (rooted across the copy) */
    /* String/Array/Hash/Set build the copy without dispatching #initialize_copy
     * (the default is a no-op — a plain builtin can't override it).  A subclass or
     * singleton CAN, so dispatch it in the tail below only when `sub` (zero cost
     * for a plain String#dup). */
    bool need_initcopy = false, gen_done = false;   /* gen_done: generic (side-table) ivars already copied */
    if (KORB_STRING_P(v)) {
        uint32_t len = VAL2STR(v)->len;
        KorbString *r = korb_str_alloc(c, slots + 1, len);
        memcpy(korb_strbuf_data(r->buf), korb_strbuf_data(VAL2STR(VALUE_REF_GET(self))->buf), len);   /* re-read after alloc */
        KORB_STR_ENC_SET((VALUE)r, KORB_STR_ENC(VALUE_REF_GET(self)));   /* preserve encoding */
        slots[1] = (VALUE)r;
        need_initcopy = true;
    } else if (KORB_ARRAY_P(v)) {
        uint32_t n = VAL2ARY(v)->len;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n));
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < n; i++)
            CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        need_initcopy = true;
    } else if (KORB_HASH_P(v)) {
        uint32_t n = VAL2HASH(v)->len;
        slots[1] = UNWRAP(korb_hash_new(c, slots + 1, n));
        /* Preserve compare_by_identity: set the flag BEFORE inserting so
         * identity-distinct keys (e.g. two equal-but-distinct Strings) don't
         * collapse under the copy's default value-equality. */
        if (VAL2HASH(VALUE_REF_GET(self))->head.flags & KORB_FL_CMP_BY_ID)   /* re-fetch: korb_hash_new above GC-moved the receiver; `v` is stale */
            ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_CMP_BY_ID;
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < n; i++) {
            slots[2] = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[2 * i];
            VALUE val = korb_items_data(VAL2HASH(VALUE_REF_GET(self))->items)[2 * i + 1];
            CHECK(korb_hash_set(c, slots + 3, dst, VALUE_REF_AT(&slots[2]), val));
        }
        KorbHash *const dh = VAL2HASH(slots[1]);            /* preserve default value / default_proc */
        const KorbHash *const sh = VAL2HASH(VALUE_REF_GET(self));
        ARO_STORE(c, dh, (VALUE *)(uintptr_t)&dh->default_val, sh->default_val);
        ARO_STORE(c, dh, (VALUE *)(uintptr_t)&dh->default_proc, sh->default_proc);
        need_initcopy = true;
    } else if (KORB_SET_P(v)) {                            /* copy a Set (dup returned self before) */
        const uint32_t sn = VAL2ARY(korb_set_elems_of(v))->len;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, sn));   /* fresh element copy */
        VALUE_REF cp = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < sn; i++)
            CHECK(korb_ary_push_val(c, slots + 2, cp, korb_items_data(VAL2ARY(korb_set_elems_of(VALUE_REF_GET(self)))->items)[i]));
        RESULT sr = korb_set_from_array(c, slots + 2, cp);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[1] = sr.value;
        need_initcopy = true;
    } else if (KORB_METHOD_P(v)) {                         /* Method/UnboundMethod → fresh (unfrozen) shallow copy */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbMethod *m = korb_alloc(c, slots + 2, sizeof(KorbMethod), KORB_OBJ_METHOD);
        const KorbMethod *src = VAL2METH(slots[1]);        /* re-read: alloc may have GC-moved the source */
        m->mid = src->mid; m->unbound = src->unbound;
        ARO_STORE(c, m, (VALUE *)(uintptr_t)&m->recv,  src->recv);
        ARO_STORE(c, m, (VALUE *)(uintptr_t)&m->owner, src->owner);
        slots[1] = (VALUE)m;
        CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
        gen_done = true;
    } else if (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_REGEXP) {   /* Regexp → fresh copy (was aliasing self) */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbRegexp *nre = korb_alloc(c, slots + 2, sizeof(KorbRegexp), KORB_OBJ_REGEXP);
        const KorbRegexp *sre = VAL2RE(slots[1]);          /* re-read: alloc may have GC-moved the source */
        nre->ci = sre->ci; nre->flags = sre->flags;
        ARO_STORE(c, nre, (VALUE *)(uintptr_t)&nre->source, sre->source);
        slots[1] = (VALUE)nre;
        CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
        gen_done = true;
        slots[2] = VALUE_REF_GET(self);                    /* CRuby calls #initialize_copy(orig) after copying */
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_RANGE) {   /* Range → fresh (unfrozen) copy (literals/Range.new are frozen) */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbRange *nr = korb_alloc(c, slots + 2, sizeof(KorbRange), KORB_OBJ_RANGE);
        const KorbRange *sr = VAL2RANGE(slots[1]);         /* re-read: alloc may have GC-moved the source */
        nr->exclude_end = sr->exclude_end;
        ARO_STORE(c, nr, (VALUE *)(uintptr_t)&nr->rbegin, sr->rbegin);
        ARO_STORE(c, nr, (VALUE *)(uintptr_t)&nr->rend,   sr->rend);
        slots[1] = (VALUE)nr;
        CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
        gen_done = true;
        slots[2] = VALUE_REF_GET(self);                    /* CRuby calls #initialize_copy(orig) after copying */
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_BINDING) {   /* Binding → fresh (unfrozen) shallow copy */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbBinding *nb = korb_alloc(c, slots + 2, sizeof(KorbBinding), KORB_OBJ_BINDING);
        const KorbBinding *sb = VAL2BIND(slots[1]);     /* re-read: alloc may have GC-moved the source */
        nb->name_syms = sb->name_syms; nb->name_cnt = sb->name_cnt; nb->src_node = sb->src_node;
        ARO_STORE(c, nb, (VALUE *)(uintptr_t)&nb->env,   sb->env);
        ARO_STORE(c, nb, (VALUE *)(uintptr_t)&nb->self,  sb->self);
        ARO_STORE(c, nb, (VALUE *)(uintptr_t)&nb->extra, sb->extra);
        slots[1] = (VALUE)nb;
        CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
        gen_done = true;
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_PROC) {   /* Proc → fresh (unfrozen) shallow copy */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbProc *np = korb_alloc(c, slots + 2, sizeof(KorbProc), KORB_OBJ_PROC);
        const KorbProc *sp = VAL2PROC(slots[1]);           /* re-read: alloc may have GC-moved the source */
        np->iseq = sp->iseq; np->sym_mid = sp->sym_mid; np->is_lambda = sp->is_lambda;
        ARO_STORE(c, np, (VALUE *)(uintptr_t)&np->env,  sp->env);
        ARO_STORE(c, np, (VALUE *)(uintptr_t)&np->self, sp->self);
        slots[1] = (VALUE)np;
        CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
        gen_done = true;
        /* CRuby calls #initialize_copy(orig) after copying (default no-op; a user override runs). */
        slots[2] = VALUE_REF_GET(self);
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (AROH_IS_GC_OBJECT(v) && KORB_OBJ_TYPE(v) == KORB_OBJ_EXCEPTION) {   /* Exception → fresh copy (was aliasing self) */
        slots[1] = v;                                      /* root the source across the alloc */
        KorbException *ne = korb_alloc(c, slots + 2, sizeof(KorbException), KORB_OBJ_EXCEPTION);
        const KorbException *se = VAL2EXC(slots[1]);        /* re-read: alloc may have GC-moved the source */
        ne->etype = se->etype; ne->line = se->line;
        ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->msg,       se->msg);
        ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->exc_class, se->exc_class);
        ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->cause,     se->cause);
        ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->backtrace, se->backtrace);
        ARO_STORE(c, ne, (VALUE *)(uintptr_t)&ne->ivars,     KORB_NIL);
        slots[1] = (VALUE)ne;                              /* result (rooted) */
        if (VAL2EXC(VALUE_REF_GET(self))->ivars != KORB_NIL) {   /* dup the ivar side-hash so the copy is independent */
            slots[2] = VAL2EXC(VALUE_REF_GET(self))->ivars;
            RESULT hd = korb_m_obj_dup(c, slots + 3, VALUE_REF_AT(&slots[2]), VALUE_SLICE_MAKE(NULL, 0));
            if (UNLIKELY(hd.state != KORB_NORMAL)) return hd;
            ARO_STORE(c, VAL2EXC(slots[1]), (VALUE *)(uintptr_t)&VAL2EXC(slots[1])->ivars, hd.value);
        }
        /* CRuby calls #initialize_copy(orig) after copying (default no-op; a user override runs) */
        slots[2] = VALUE_REF_GET(self);
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (KORB_OBJECT_P(v)) {                         /* user object → fresh instance, shallow-copy ivars */
        slots[1] = UNWRAP(korb_obj_new(c, slots + 1, VAL2OBJ(v)->klass));
        VALUE_REF dst = VALUE_REF_AT(&slots[1]);
        const uint32_t sid0 = VAL2OBJ(VALUE_REF_GET(self))->shape_id;   /* re-read self (obj_new GC'd; `v` is stale) */
        const uint32_t nv = c->vm->shapes[sid0].ivar_count;
        if (nv) {
            uint32_t *const syms = (uint32_t *)malloc((size_t)nv * sizeof(uint32_t));   /* libc: stable across GC */
            if (UNLIKELY(!syms)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of memory");
            for (uint32_t sid = sid0; sid; ) { const struct korb_shape *s = &c->vm->shapes[sid]; if (s->ivar_count >= 1 && s->ivar_count <= nv) syms[s->ivar_count - 1] = s->edge_sym; sid = s->parent; }
            for (uint32_t i = 0; i < nv; i++) {
                slots[2] = korb_items_data(VAL2OBJ(VALUE_REF_GET(self))->ivars)[i];   /* re-read self (ivar_set GCs) */
                RESULT ir = korb_ivar_set(c, slots + 3, dst, ID2SYM(syms[i]), slots[2]);
                if (UNLIKELY(ir.state != KORB_NORMAL)) { free(syms); return ir; }
            }
            free(syms);
        }
        /* CRuby calls #initialize_copy(orig) on the new object after copying the
         * ivars — the default is a no-op here, but a user override runs its logic. */
        slots[2] = VALUE_REF_GET(self);                   /* orig (arg); recv = the new object at slots[1] */
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    } else if (KORB_CLASS_P(v)) {                          /* Module/Class → anonymous copy with its own method table */
        RESULT cr = korb_class_dup(c, slots + 1, v);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        slots[1] = cr.value;
    } else {
        return RESULT_OK(v);   /* immediate / no special copy */
    }
    if (!gen_done) CHECK(korb_copy_gen_ivars(c, slots + 2, self, &slots[1]));
    if (sub && !(KORB_CLASS_P(slots[0]) && VAL2CLASS(slots[0])->is_singleton))
        korb_klass_override_set(c, slots[1], slots[0]);   /* dup keeps a builtin-subclass class but NOT a singleton class */
    if (need_initcopy && sub) {                           /* String/Array/Hash/Set subclass: run the (possibly overridden) hook now that the class is set */
        RESULT icr = korb_copy_hook(c, slots, self, hook_mid, hook_kw);
        if (UNLIKELY(icr.state != KORB_NORMAL)) return icr;
    }
    return RESULT_OK(slots[1]);
}
static RESULT korb_m_obj_dup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    return korb_obj_copy_impl(c, slots, self, korb_intern(c->vm, "initialize_dup", 14), NULL);
}
/* Object#clone(freeze: nil) — like dup, but copies the frozen state (unless
 * freeze: false), and freeze: true forces it.  (Singleton-class copy not done.) */
static RESULT korb_m_obj_clone(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int fmode = -1;                                       /* -1 preserve, 0 unfreeze, 1 freeze */
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {
        const VALUE h = VALUE_SLICE_GET(a, n - 1);
        const int32_t fi = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern(c->vm, "freeze", 6)));
        if (fi >= 0) {
            const VALUE fv = korb_items_data(VAL2HASH(h)->items)[2 * fi + 1];
            if (UNLIKELY(fv != KORB_NIL && fv != KORB_TRUE && fv != KORB_FALSE))   /* clone(freeze:) accepts only true/false/nil */
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unexpected value for freeze: %s", korb_type_name(fv));
            fmode = (fv == KORB_NIL) ? -1 : (KORB_TRUTHY(fv) ? 1 : 0);
        }
    }
    /* CRuby hands #initialize_clone the freeze: keywords it was given, and only
     * those — a plain clone passes none. */
    slots[0] = (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) ? VALUE_SLICE_GET(a, n - 1) : KORB_NIL;
    const VALUE sv = VALUE_REF_GET(self);
    const bool self_frozen = AROH_IS_GC_OBJECT(sv) && (((const AroObjectHeader *)(uintptr_t)sv)->flags & KORB_FL_FROZEN);
    RESULT r = korb_obj_copy_impl(c, slots + 1, self, korb_intern(c->vm, "initialize_clone", 16), &slots[0]);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    const VALUE sv2 = VALUE_REF_GET(self);   /* re-fetch: korb_m_obj_dup GC-moved the receiver; `sv` is stale */
    /* clone (unlike dup) carries the singleton class, so singleton methods survive. */
    const bool self_sub = AROH_IS_GC_OBJECT(sv2) && (((const AroObjectHeader *)(uintptr_t)sv2)->flags & KORB_FL_HAS_KLASS);
    if (self_sub && AROH_IS_GC_OBJECT(r.value)) {
        const VALUE ov = korb_klass_override_get(c->vm, sv2);
        if (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) {
            slots[0] = r.value;
            korb_klass_override_set(c, slots[0], ov);
            r.value = slots[0];
        }
    }
    if (((fmode == 1) || (fmode == -1 && self_frozen)) && AROH_IS_GC_OBJECT(r.value))
        ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;
    return r;
}

/* in-place reverse of items[lo, hi) — no alloc, so pointers are stable. */
static void korb_ary_rev_range(CTX *c, KorbArrayItems *it, uint32_t lo, uint32_t hi) {
    while (lo + 1 < hi + 1 && lo < hi) {       /* lo < hi (guard wrap) */
        hi--;
        VALUE t = korb_items_data(it)[lo];
        ARO_STORE(c, it, &korb_items_data(it)[lo], korb_items_data(it)[hi]);
        ARO_STORE(c, it, &korb_items_data(it)[hi], t);
        lo++;
    }
}
static RESULT korb_m_ary_reverse_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    korb_ary_rev_range(c, ary->items, 0, ary->len);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_rotate_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    korb_sword_t cnt = 1;
    if (VALUE_SLICE_LEN(a) >= 1) {
        VALUE cv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(cv, &cnt))) {        /* coerce via #to_int */
            const VALUE orig = cv;
            RESULT cr = korb_coerce_to_int(c, slots, &cv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(cv, &cnt)) return korb_raise_no_int(c, slots, orig);
        }
    }
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    uint32_t n = ary->len;
    if (n > 1) {
        korb_sword_t k = ((cnt % (korb_sword_t)n) + (korb_sword_t)n) % (korb_sword_t)n;   /* normalize */
        KorbArrayItems *it = ary->items;
        korb_ary_rev_range(c, it, 0, (uint32_t)k);
        korb_ary_rev_range(c, it, (uint32_t)k, n);
        korb_ary_rev_range(c, it, 0, n);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* product(other, ...) → cartesian product as an array of rows. */
static RESULT korb_m_ary_product(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t na = VALUE_SLICE_LEN(a);
    if (na > 15) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Array#product with >16 arrays is not supported");
    /* collect [self, *args] into a rooted array, coercing each arg via #to_ary. */
    VALUE_REF cargs = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, na + 1)));
    CHECK(korb_ary_push_val(c, slots, cargs, VALUE_REF_GET(self)));
    for (uint32_t j = 0; j < na; j++) {
        slots[0] = VALUE_SLICE_GET(a, j);
        if (!KORB_ARRAY_P(slots[0])) {                   /* coerce a #to_ary object to an Array */
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce_p(c, slots + 1, &slots[0], to_ary)) {
                RESULT r = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                slots[0] = r.value;
            }
            if (UNLIKELY(!KORB_ARRAY_P(slots[0])))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, j)));
        }
        CHECK(korb_ary_push_val(c, slots + 1, cargs, slots[0]));
    }
    uint32_t k = na + 1;                                  /* self + args */
    #define ARR_J(j) (VAL2ARY(korb_items_data(VAL2ARY(VALUE_REF_GET(cargs))->items)[(j)]))
    uint32_t lens[16];
    uint64_t total = 1;
    for (uint32_t j = 0; j < k; j++) {
        lens[j] = ARR_J(j)->len;
        /* CRuby raises RangeError when the number of products can't fit; koruby
         * array sizes are uint32, so cap at INT32_MAX (also avoids uint64 overflow
         * silently wrapping and then looping to exhaustion). */
        if (lens[j] != 0 && total > (uint64_t)0x7fffffff / lens[j])
            return korb_raise(c, slots, KORB_E_RANGE, 0, "too big to product");
        total *= lens[j];
    }
    /* with a block: yield each combination, return self (no result array) */
    if (block != NULL) {
        if (total == 0) return RESULT_OK(VALUE_REF_GET(self));
        uint32_t bidx[16] = {0};
        for (uint64_t t = 0; t < total; t++) {
            slots[0] = UNWRAP(korb_ary_new(c, slots + 1, k));   /* row at slots[0] */
            VALUE_REF row = VALUE_REF_AT(&slots[0]);
            for (uint32_t j = 0; j < k; j++) CHECK(korb_ary_push_val(c, slots + 1, row, korb_items_data(ARR_J(j)->items)[bidx[j]]));
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
            VALUE e = korb_items_data(ARR_J(j)->items)[idx[j]];
            CHECK(korb_ary_push_val(c, slots + 1, row, e));
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        for (int j = (int)k - 1; j >= 0; j--) { if (++idx[j] < lens[j]) break; idx[j] = 0; }
    }
    #undef ARR_J
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_fetch_values(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, VALUE_SLICE_LEN(a))));
    for (uint32_t j = 0; j < VALUE_SLICE_LEN(a); j++) {
        slots[1] = VALUE_SLICE_GET(a, j);                 /* original index arg (rooted; yielded as-is) */
        korb_sword_t idx;
        if (UNLIKELY(!korb_to_index(slots[1], &idx))) {   /* coerce a non-Integer index via #to_int */
            VALUE iv2 = slots[1];
            RESULT cr = korb_coerce_to_int(c, slots + 2, &iv2);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(iv2, &idx)) return korb_raise_no_int(c, slots, slots[1]);
        }
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        korb_sword_t n = ary->len, orig = idx;
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) {
            if (block != NULL) {                          /* block form: yield the original index, use its result */
                RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
                continue;
            }
            return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld outside of array bounds: -%ld...%ld", (long)orig, (long)n, (long)n);
        }
        CHECK(korb_ary_push_val(c, slots + 2, dst, korb_items_data(ary->items)[idx]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* one?: exactly one truthy element (or exactly one block-truthy element). */
static RESULT korb_m_ary_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", (unsigned)VALUE_SLICE_LEN(a));
    const bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    slots[0] = has_pat ? VALUE_SLICE_GET(a, 0) : KORB_NIL;           /* pattern (rooted) */
    const bool pat_obj = has_pat && KORB_OBJECT_P(slots[0]);         /* user object → dispatch #=== */
    const uint32_t ceq = pat_obj ? korb_intern(c->vm, "===", 3) : 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; ; i++) {
        if (i >= VAL2ARY(VALUE_REF_GET(self))->len) break;
        slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        bool t;
        if (has_pat) {
            if (pat_obj) {
                slots[2] = slots[0]; slots[3] = slots[1];
                RESULT r = korb_send_impl(c, slots + 4, ceq, 0, 1, NULL, NULL, NULL);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                t = KORB_TRUTHY(r.value);
            } else {
                t = korb_case_eq(c, slots[0], slots[1]);
            }
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(slots[1]);
        }
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}

