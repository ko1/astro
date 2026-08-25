/* koruby_precise — array.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Array methods ------------------------------------------------------- */
static RESULT korb_lazy_new(CTX *c, VALUE *slots, VALUE source, uint8_t mode);   /* enumerator.c */

/* Array#initialize — callable via send/super; Array.new uses its own fast path.
 * (), (size[, default]), (size){|i| ...}, (other_array). */
static RESULT korb_m_ary_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const uint32_t argc = VALUE_SLICE_LEN(a);
    if (UNLIKELY(argc > 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..2)", argc);
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VAL2ARY(VALUE_REF_GET(self))->len = 0;               /* reset */
    if (argc == 0) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* a0 (rooted across #to_ary/#to_int dispatch) */
    if (argc == 1 && !FIXNUM_P(slots[0])) {              /* 1-arg form may be an Array copy (Array or #to_ary) */
        if (!KORB_ARRAY_P(slots[0]) && KORB_OBJECT_P(slots[0])) {
            const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
            if (korb_responds_to_coerce_p(c, slots + 1, &slots[0], to_ary)) {
                RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);   /* receiver at slots[0] */
                if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
                if (UNLIKELY(!KORB_ARRAY_P(ar.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to Array", korb_type_name(slots[0]));
                slots[0] = ar.value;
            }
        }
        if (KORB_ARRAY_P(slots[0])) {                    /* copy another array */
            const uint32_t n = VAL2ARY(slots[0])->len;
            for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, self, korb_items_data(VAL2ARY(slots[0])->items)[i]));
            return RESULT_OK(VALUE_REF_GET(self));
        }
    }
    korb_sword_t n;
    if (UNLIKELY(!korb_to_index(slots[0], &n))) {        /* size form */
        if (KORB_BIGNUM_P(slots[0])) {                   /* a real Integer, just too large for an array size */
            if (korb_mp_sgn(VAL2BIG(slots[0])->z) < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "array size too big");
        }
        VALUE sz = slots[0];                             /* else coerce via #to_int */
        RESULT ci = korb_coerce_to_int(c, slots + 1, &sz);
        if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
        if (ci.value != KORB_TRUE || !korb_to_index(sz, &n))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(slots[0]));
    }
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    if (block != NULL && argc >= 2) korb_warn(c, slots, "block supersedes default value argument");
    slots[0] = (!block && argc >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;   /* default (rooted) */
    for (korb_sword_t i = 0; i < n; i++) {
        VALUE el = slots[0];
        if (block != NULL) {
            VALUE iv = LONG2FIX(i);
            RESULT y = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (y.state == KORB_BREAK && korb_break_owned(c, block, def_env)) return RESULT_OK(y.value);   /* break v → the value (only ours) */
            if (UNLIKELY(y.state != KORB_NORMAL)) return y;
            el = y.value;
        }
        slots[1] = el;
        CHECK(korb_ary_push_val(c, slots + 2, self, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_ary_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_ARY->len)); }
static RESULT korb_m_ary_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_ARY->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_ary_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_ARRAY_P(o)) {                            /* coerce via #to_ary, else nil */
        const uint32_t to_ary = korb_intern(c->vm, "to_ary", 6);
        if (!KORB_OBJECT_P(o) || !korb_responds_to_coerce_p(c, slots, &o, to_ary)) return RESULT_OK(KORB_NIL);
        slots[0] = o;
        RESULT ar = korb_send_impl(c, slots + 1, to_ary, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(ar.state != KORB_NORMAL)) return ar;
        if (!KORB_ARRAY_P(ar.value)) return RESULT_OK(KORB_NIL);
        o = ar.value;
    }
    slots[0] = VALUE_REF_GET(self);                    /* root both across any <=> dispatch GC */
    slots[1] = o;
    const uint32_t xl0 = VAL2ARY(slots[0])->len, yl0 = VAL2ARY(slots[1])->len;
    const uint32_t m = xl0 < yl0 ? xl0 : yl0;
    for (uint32_t i = 0; i < m; i++) {
        const VALUE xi = korb_items_data(VAL2ARY(slots[0])->items)[i], yi = korb_items_data(VAL2ARY(slots[1])->items)[i];
        if (xi == yi && !KORB_FLOAT_P(xi)) continue;   /* identical (also breaks self-referential) */
        if (UNLIKELY(KORB_OBJECT_P(xi) || KORB_OBJECT_P(yi))) {   /* user/Comparable element → dispatch <=> */
            slots[2] = xi; slots[3] = yi;
            RESULT cr = korb_send(c, slots + 4, c->vm->mid_cmp, 0, 1);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (cr.value == KORB_NIL) return RESULT_OK(KORB_NIL);      /* element uncomparable → nil */
            if (cr.value == LONG2FIX(0)) continue;                     /* equal → next element */
            return RESULT_OK(cr.value);                                /* first non-zero → the element's raw result */
        }
        const int cmp = korb_cmp_full(c, xi, yi);
        if (UNLIKELY(cmp == 2)) return RESULT_OK(KORB_NIL);
        if (cmp != 0) return RESULT_OK(LONG2FIX(cmp));
    }
    const uint32_t xl = VAL2ARY(slots[0])->len, yl = VAL2ARY(slots[1])->len;   /* re-read post-dispatch */
    return RESULT_OK(LONG2FIX((xl > yl) - (xl < yl)));
}
static RESULT korb_m_ary_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_ary_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* first(n) → first n as array */
        korb_sword_t n;
        if (UNLIKELY(KORB_BIGNUM_P(VALUE_SLICE_GET(a, 0)))) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* coerce count via #to_int */
            VALUE nv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (UNLIKELY(KORB_BIGNUM_P(nv))) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
            if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t take = (uint32_t)n; if (take > SELF_ARY->len) take = SELF_ARY->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(SELF_ARY->items)[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? korb_items_data(ary->items)[0] : KORB_NIL);
}
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* last(n) → last n as array */
        korb_sword_t n;
        if (UNLIKELY(KORB_BIGNUM_P(VALUE_SLICE_GET(a, 0)))) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) {   /* coerce count via #to_int */
            VALUE nv = VALUE_SLICE_GET(a, 0);
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t len = SELF_ARY->len;
        uint32_t take = (uint32_t)n; if (take > len) take = len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = len - take; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(SELF_ARY->items)[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? korb_items_data(ary->items)[ary->len - 1] : KORB_NIL);
}

/* fresh array = self[start, len) */
static RESULT korb_ary_subseq(CTX *c, VALUE *slots, VALUE_REF self, uint32_t start, uint32_t len) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++)
        CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[start + i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_index_coerce(CTX *c, VALUE *slots, VALUE v, korb_sword_t *out);   /* fwd (defined below) */
static RESULT korb_m_ary_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    uint32_t n = SELF_ARY->len;
    VALUE i0 = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(i0)) {                                 /* a[b..e] → subarray (incl. beginless/endless) */
        if (UNLIKELY(VALUE_SLICE_LEN(a) >= 2))             /* a[range, len] is invalid */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of Range into Integer");
        const KorbRange *r = VAL2RANGE(i0);
        const bool beginless = (r->rbegin == KORB_NIL);    /* a[..e] → from 0 */
        const bool endless   = (r->rend   == KORB_NIL);    /* a[b..] → to the end */
        korb_sword_t b = 0, e;                                 /* coerce endpoints via #to_int */
        if (!beginless) CHECK(korb_index_coerce(c, slots, r->rbegin, &b));
        if (!endless)   CHECK(korb_index_coerce(c, slots, r->rend, &e));
        n = SELF_ARY->len;                                 /* re-read after any #to_int GC */
        if (endless) e = (korb_sword_t)n;
        if (b < 0) b += n;
        if (!endless && e < 0) e += n;
        if (b < 0 || b > (korb_sword_t)n) return RESULT_OK(KORB_NIL);
        korb_sword_t last = (endless || r->exclude_end) ? e - 1 : e, cnt = last - b + 1;   /* endless end is exclusive of n */
        if (cnt < 0) cnt = 0;
        if (b + cnt > (korb_sword_t)n) cnt = (korb_sword_t)n - b;
        return korb_ary_subseq(c, slots, self, (uint32_t)b, (uint32_t)cnt);
    }
    if (KORB_ARITHSEQ_P(i0)) {                             /* a[(b..e).step(s)] → strided subarray */
        if (UNLIKELY(VALUE_SLICE_LEN(a) >= 2))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of Enumerator::ArithmeticSequence into Integer");
        VALUE bv, ev, sv; bool excl;
        korb_aseq_params(VAL2ASEQ(i0), &bv, &ev, &sv, &excl);
        if (UNLIKELY(!FIXNUM_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        const korb_sword_t s = FIX2LONG(sv);
        if (UNLIKELY(s == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0");
        if (UNLIKELY((bv != KORB_NIL && !FIXNUM_P(bv)) || (ev != KORB_NIL && !FIXNUM_P(ev))))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        korb_sword_t b = (bv == KORB_NIL) ? 0 : FIX2LONG(bv);
        if (b < 0) b += n;
        if (UNLIKELY(b < 0 || b > (korb_sword_t)n))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld out of range", (long)(bv == KORB_NIL ? 0 : FIX2LONG(bv)));
        korb_sword_t e; bool e_incl;
        if (ev == KORB_NIL) { if (s > 0) { e = (korb_sword_t)n; e_incl = false; } else { e = 0; e_incl = true; } }
        else {
            e = FIX2LONG(ev); if (e < 0) e += n; e_incl = !excl;
            if (UNLIKELY(e > (korb_sword_t)n)) return korb_raise(c, slots, KORB_E_RANGE, 0, "%lld out of range", (long long)FIX2LONG(ev));
        }
        slots[0] = UNWRAP(korb_ary_new(c, slots, 8));       /* result (rooted) */
        VALUE_REF out = VALUE_REF_AT(&slots[0]);
        for (korb_sword_t idx = b; (s > 0) ? (e_incl ? idx <= e : idx < e) : (e_incl ? idx >= e : idx > e); idx += s) {
            if (idx < 0 || idx >= (korb_sword_t)n) continue;
            slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[idx];   /* re-read self: push GCs */
            CHECK(korb_ary_push_val(c, slots + 2, out, slots[1]));
        }
        return RESULT_OK(VALUE_REF_GET(out));
    }
    korb_sword_t i;
    if (UNLIKELY(!korb_to_index(i0, &i))) {
        if (KORB_INTEGER_P(i0)) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        if (KORB_FLOAT_P(i0)) { char fb[40]; korb_float_to_s(korb_float_val(i0), fb);
                                return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", fb); }
        RESULT cr = korb_coerce_to_int(c, slots, &i0);     /* coerce via #to_int */
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(i0, &i)) {
            if (KORB_INTEGER_P(i0)) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
        if (KORB_FLOAT_P(i0)) { char fb[40]; korb_float_to_s(korb_float_val(i0), fb);
                                return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", fb); }
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        }
        n = SELF_ARY->len;                                 /* re-read after dispatch */
    }
    if (i < 0) i += n;
    if (VALUE_SLICE_LEN(a) >= 2) {                          /* a[start, len] → subarray */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        korb_sword_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) {
            if (KORB_INTEGER_P(lv)) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
            if (KORB_FLOAT_P(lv)) { char fb[40]; korb_float_to_s(korb_float_val(lv), fb);
                                    return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", fb); }
            RESULT cr = korb_coerce_to_int(c, slots, &lv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(lv, &len)) {
            if (KORB_INTEGER_P(lv)) return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'long'");
            if (KORB_FLOAT_P(lv)) { char fb[40]; korb_float_to_s(korb_float_val(lv), fb);
                                    return korb_raise(c, slots, KORB_E_RANGE, 0, "float %s out of range of integer", fb); }
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        }
            n = SELF_ARY->len;
        }
        if (len < 0 || i < 0 || i > (korb_sword_t)n) return RESULT_OK(KORB_NIL);
        if (i + len > (korb_sword_t)n) len = (korb_sword_t)n - i;
        return korb_ary_subseq(c, slots, self, (uint32_t)i, (uint32_t)len);
    }
    if (i < 0 || (uint32_t)i >= n) return RESULT_OK(KORB_NIL);
    return RESULT_OK(korb_items_data(SELF_ARY->items)[i]);
}

/* Replace self[start, dellen) with valref (spliced if Array, else single element).
 * `valref` must be a rooted slot. Returns the replacement value. */
static RESULT korb_ary_splice(CTX *c, VALUE *slots, VALUE_REF self, korb_sword_t start, korb_sword_t dellen, VALUE_REF valref) {
    korb_sword_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (start < 0) start += len;
    if (UNLIKELY(start < 0)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld too small for array; minimum: -%ld", (long)(start - len + len), (long)len);
    if (dellen < 0) dellen = 0;
    bool splat = KORB_ARRAY_P(VALUE_REF_GET(valref));
    /* fast path: same-length in-range replacement (a[i, n] = n-elem array, or a[i]=v).
     * No length change → overwrite in place: O(n), no temp array, no full rebuild.
     * Hot in optcarrot (`@bg_pixels[x, 8] = lut_entry` per tile). */
    const korb_sword_t repl = splat ? (korb_sword_t)VAL2ARY(VALUE_REF_GET(valref))->len : 1;
    if (start <= len && start + dellen <= len && repl == dellen &&
        VALUE_REF_GET(self) != VALUE_REF_GET(valref)) {   /* in-place; aliasing → slow path */
        if (splat) {
            for (korb_sword_t j = 0; j < repl; j++)
                korb_ary_store_at(c, VALUE_REF_GET(self), (uint32_t)(start + j),
                                  korb_items_data(VAL2ARY(VALUE_REF_GET(valref))->items)[j]);
        } else {
            korb_ary_store_at(c, VALUE_REF_GET(self), (uint32_t)start, VALUE_REF_GET(valref));
        }
        return RESULT_OK(VALUE_REF_GET(valref));
    }
    /* build the new sequence in a temp array (rooted), then copy back into self */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF tmp = VALUE_REF_AT(&slots[0]);
    for (korb_sword_t i = 0; i < start; i++) {
        VALUE e = (i < len) ? korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i] : KORB_NIL;   /* pad gap with nil */
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    if (splat) {
        uint32_t vn = VAL2ARY(VALUE_REF_GET(valref))->len;
        for (uint32_t j = 0; j < vn; j++) {
            VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(valref))->items)[j];
            CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
        }
    } else {
        CHECK(korb_ary_push_val(c, slots + 1, tmp, VALUE_REF_GET(valref)));
    }
    for (korb_sword_t i = start + dellen; i < len; i++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    /* overwrite self with tmp */
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t tn = VAL2ARY(VALUE_REF_GET(tmp))->len;
    for (uint32_t j = 0; j < tn; j++) {
        VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(tmp))->items)[j];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(valref));
}
/* Coerce an index arg to an korb_sword_t, dispatching #to_int (may GC); TypeError otherwise. */
static RESULT korb_index_coerce(CTX *c, VALUE *slots, VALUE v, korb_sword_t *out) {
    if (korb_to_index(v, out)) return RESULT_OK(KORB_TRUE);
    VALUE cv = v;
    RESULT ci = korb_coerce_to_int(c, slots, &cv);
    if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
    if (ci.value == KORB_TRUE && korb_to_index(cv, out)) return RESULT_OK(KORB_TRUE);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(v));
}
static RESULT korb_m_ary_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2..3)", VALUE_SLICE_LEN(a));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (VALUE_SLICE_LEN(a) >= 3) {                        /* a[start, len] = val */
        korb_sword_t start, dellen;
        CHECK(korb_index_coerce(c, slots, iv, &start));
        CHECK(korb_index_coerce(c, slots, VALUE_SLICE_GET(a, 1), &dellen));
        return korb_ary_splice(c, slots, self, start, dellen, VALUE_SLICE_REF(a, 2));
    }
    if (KORB_RANGE_P(iv)) {                               /* a[b..e] = val (incl. beginless/endless) */
        const KorbRange *r = VAL2RANGE(iv);
        const bool beginless = (r->rbegin == KORB_NIL);
        const bool endless   = (r->rend   == KORB_NIL);
        const bool excl      = (r->exclude_end != 0);      /* capture before any #to_int GC (r may move) */
        const korb_sword_t len = VAL2ARY(VALUE_REF_GET(self))->len;
        korb_sword_t b, e;
        if (beginless) b = 0;
        else CHECK(korb_index_coerce(c, slots, r->rbegin, &b));
        const korb_sword_t braw = b;                           /* original begin (for the out-of-range message) */
        if (endless) e = len;
        else CHECK(korb_index_coerce(c, slots, r->rend, &e));
        const korb_sword_t eraw = e;
        if (b < 0) b += len;
        if (UNLIKELY(b < 0)) {                              /* begin below the array start → RangeError */
            if (endless) return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld.. out of range", (long)braw);
            return korb_raise(c, slots, KORB_E_RANGE, 0, "%ld%s%ld out of range", (long)braw, excl ? "..." : "..", (long)eraw);
        }
        if (!endless && e < 0) e += len;
        korb_sword_t last = (endless || excl) ? e - 1 : e, dellen = last - b + 1;
        if (dellen < 0) dellen = 0;
        return korb_ary_splice(c, slots, self, b, dellen, VALUE_SLICE_REF(a, 1));
    }
    korb_sword_t i;
    CHECK(korb_index_coerce(c, slots, iv, &i));          /* may GC (via #to_int) → read ary after */
    KorbArray *ary = SELF_ARY;
    if (i < 0) i += ary->len;
    if (UNLIKELY(i < 0)) return korb_raise(c, slots, KORB_E_INDEX, 0, "index %ld too small for array; minimum: -%u", (long)i, ary->len);
    if ((uint32_t)i >= ary->len) {
        CHECK(korb_ary_ensure(c, slots, self, (uint32_t)i + 1 - ary->len));
        ary = SELF_ARY;                                  /* re-read after grow GC */
        for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ARO_STORE(c, ary->items, &korb_items_data(ary->items)[k], KORB_NIL);
        ary->len = (uint32_t)i + 1;
    }
    VALUE val = VALUE_SLICE_GET(a, 1);                    /* re-read (rooted) after GC */
    KorbArrayItems *it = ary->items;
    ARO_STORE(c, it, &korb_items_data(it)[i], val);
    return RESULT_OK(val);
}

/* slice!: remove and return element (single index) or subarray (range/start,len). */
static RESULT korb_m_ary_slice_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));   /* CRuby raises FrozenError upfront */
    VALUE iv = VALUE_SLICE_GET(a, 0);
    korb_sword_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    korb_sword_t start, dellen; bool subseq_form = false;
    if (KORB_RANGE_P(iv)) {
        const KorbRange *r = VAL2RANGE(iv);
        const bool beginless = (r->rbegin == KORB_NIL);
        const bool endless   = (r->rend   == KORB_NIL);
        korb_sword_t b, e;
        if (beginless) b = 0;
        else if (UNLIKELY(!korb_to_index(r->rbegin, &b))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (endless) e = n;
        else if (UNLIKELY(!korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += n;
        if (!endless && e < 0) e += n;
        korb_sword_t last = (endless || r->exclude_end) ? e - 1 : e;
        start = b; dellen = last - b + 1; if (dellen < 0) dellen = 0;
        subseq_form = true;
    } else if (VALUE_SLICE_LEN(a) >= 2) {
        VALUE lv = VALUE_SLICE_GET(a, 1);
        if (UNLIKELY(!korb_to_index(iv, &start))) {        /* coerce start via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &iv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(iv, &start)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(!korb_to_index(lv, &dellen))) {       /* coerce length via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &lv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(lv, &dellen)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        }
        n = VAL2ARY(VALUE_REF_GET(self))->len;             /* re-read after any dispatch */
        if (UNLIKELY(dellen < 0)) return RESULT_OK(KORB_NIL);   /* (start, negative len) → nil */
        if (start < 0) start += n;
        subseq_form = true;
    } else {
        if (UNLIKELY(!korb_to_index(iv, &start))) {        /* coerce index via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &iv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(iv, &start)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
            n = VAL2ARY(VALUE_REF_GET(self))->len;
        }
        if (start < 0) start += n;
        if (start < 0 || start >= n) return RESULT_OK(KORB_NIL);
        slots[0] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[start];   /* removed elem */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));              /* empty replacement */
        CHECK(korb_ary_splice(c, slots + 2, self, start, 1, VALUE_REF_AT(&slots[1])));
        return RESULT_OK(slots[0]);
    }
    if (start < 0 || start > n) return RESULT_OK(KORB_NIL);
    if (dellen < 0) dellen = 0;
    if (start + dellen > n) dellen = n - start;
    slots[0] = UNWRAP(korb_ary_subseq(c, slots, self, (uint32_t)start, (uint32_t)dellen));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
    CHECK(korb_ary_splice(c, slots + 2, self, start, dellen, VALUE_REF_AT(&slots[1])));
    (void)subseq_form;
    return RESULT_OK(slots[0]);
}

static RESULT korb_m_ary_ltlt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, 0)));
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    uint32_t n = VALUE_SLICE_LEN(a);
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, i)));   /* slice rooted across grow */
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_pop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* pop(n): remove & return last n as array */
        korb_sword_t n;
        VALUE nv = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!korb_to_index(nv, &n))) {       /* coerce the count via #to_int */
            RESULT cr = korb_coerce_to_int(c, slots, &nv);
            if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
            if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        }
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t take = (uint32_t)n; if (take > SELF_ARY->len) take = SELF_ARY->len;
        uint32_t start = SELF_ARY->len - take;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(SELF_ARY->items)[start + i]));
        KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        for (uint32_t i = start; i < ary->len; i++) ARO_STORE(c, ary->items, &korb_items_data(ary->items)[i], KORB_NIL);
        ary->len = start;
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    (void)slots;
    KorbArray *ary = SELF_ARY;
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    ary->len--;
    VALUE v = korb_items_data(ary->items)[ary->len];
    ARO_STORE(c, ary->items, &korb_items_data(ary->items)[ary->len], KORB_NIL); /* drop the reference (nil needs no WB) */
    return RESULT_OK(v);
}

/* Array#== — same length, elements compared with #== (object/array/hash elements
 * dispatch, so user == and nested value-equality are honoured; korb_value_eq alone
 * cannot dispatch without a CTX). */
static RESULT korb_m_ary_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE other = VALUE_SLICE_GET(a, 0);
    if (VALUE_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_ARRAY_P(other)) {                            /* Array-like (responds to #to_ary) → delegate other == self */
        if (KORB_OBJECT_P(other) && korb_responds_to(c, other, korb_intern(c->vm, "to_ary", 6))) {
            slots[0] = other; slots[1] = VALUE_REF_GET(self);
            return korb_send_impl(c, slots + 2, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
        }
        return RESULT_OK(KORB_FALSE);
    }
    if (VAL2ARY(VALUE_REF_GET(self))->len != VAL2ARY(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2ARY(VALUE_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive: CRuby assumes equal */
    slots[0] = VALUE_REF_GET(self); slots[1] = other;     /* root both across element == dispatch */
    const uint32_t n = VAL2ARY(slots[0])->len;
    VAL2ARY(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    VALUE result = KORB_TRUE;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE v = korb_items_data(VAL2ARY(slots[0])->items)[i], v2 = korb_items_data(VAL2ARY(slots[1])->items)[i];
        if (KORB_OBJECT_P(v) || KORB_ARRAY_P(v) || KORB_HASH_P(v) ||
            KORB_OBJECT_P(v2) || KORB_ARRAY_P(v2) || KORB_HASH_P(v2)) {   /* dispatch == (recurses for nested) */
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (!korb_value_eq(v, v2)) {
            result = KORB_FALSE; break;
        }
    }
    VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;   /* re-deref: dispatch may have moved self */
    return RESULT_OK(result);
}
/* Array#eql? — same length, elements compared with #eql? (type-strict: 1 ≠ 1.0).
 * Object/array/hash elements dispatch #eql?; primitives use korb_value_eql. */
static RESULT korb_m_ary_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE other = VALUE_SLICE_GET(a, 0);
    if (VALUE_REF_GET(self) == other) return RESULT_OK(KORB_TRUE);
    if (!KORB_ARRAY_P(other)) return RESULT_OK(KORB_FALSE);
    if (VAL2ARY(VALUE_REF_GET(self))->len != VAL2ARY(other)->len) return RESULT_OK(KORB_FALSE);
    if (VAL2ARY(VALUE_REF_GET(self))->head.flags & KORB_FL_JOIN_VISITING) return RESULT_OK(KORB_TRUE);   /* recursive */
    slots[0] = VALUE_REF_GET(self); slots[1] = other;
    const uint32_t mid_eql = korb_intern(c->vm, "eql?", 4);
    const uint32_t n = VAL2ARY(slots[0])->len;
    VAL2ARY(slots[0])->head.flags |= KORB_FL_JOIN_VISITING;
    VALUE result = KORB_TRUE;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE v = korb_items_data(VAL2ARY(slots[0])->items)[i], v2 = korb_items_data(VAL2ARY(slots[1])->items)[i];
        if (KORB_OBJECT_P(v) || KORB_ARRAY_P(v) || KORB_HASH_P(v) ||
            KORB_OBJECT_P(v2) || KORB_ARRAY_P(v2) || KORB_HASH_P(v2)) {
            slots[2] = v; slots[3] = v2;
            RESULT r = korb_send_impl(c, slots + 4, mid_eql, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) { VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING; return r; }
            if (!KORB_TRUTHY(r.value)) { result = KORB_FALSE; break; }
        } else if (!korb_value_eql(v, v2)) {
            result = KORB_FALSE; break;
        }
    }
    VAL2ARY(slots[0])->head.flags &= ~KORB_FL_JOIN_VISITING;
    if (result == KORB_FALSE) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_ary_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VALUE_SLICE_GET(a, 0);                    /* needle (root across element == dispatches) */
    const uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* re-read each iter */
        if (e == slots[0]) return RESULT_OK(KORB_TRUE);     /* identity short-circuit (CRuby rb_equal; catches NaN's own bits) */
        if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[0])) {  /* user == → dispatch (element == needle) */
            slots[1] = e; slots[2] = slots[0];
            RESULT r = korb_send_impl(c, slots + 3, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) return RESULT_OK(KORB_TRUE);
        } else if (korb_value_eq(e, slots[0])) {
            return RESULT_OK(KORB_TRUE);
        }
    }
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_ary_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE elem = korb_items_data(SELF_ARY->items)[n - 1 - i];   /* push_val roots elem before any GC */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov))) {                   /* coerce the operand via #to_ary (self is a VALUE_REF) */
        RESULT cr = korb_coerce_to_ary(c, slots, &ov);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value != KORB_TRUE) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, 0)));
        slots[0] = ov;
        return korb_ary_plus_ref(c, slots + 1, self, VALUE_REF_AT(&slots[0]));
    }
    return korb_ary_plus_ref(c, slots, self, VALUE_SLICE_REF(a, 0));
}
static RESULT korb_m_ary_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mul_slow(c, slots, self, VALUE_SLICE_GET(a, 0), 0);   /* n→repeat, String→join */
}
static RESULT korb_m_str_mul(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mul_slow(c, slots, self, VALUE_SLICE_GET(a, 0), 0);   /* String * n → repeat */
}
static RESULT korb_m_str_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_plus_slow(c, slots, self, VALUE_SLICE_GET(a, 0), 0);  /* String + String → concat */
}

/* ---- yielding methods (drive a block) ------------------------------------ */

static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
static RESULT korb_m_ary_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the elements */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));   /* re-read each iter (GC) */
        if (i >= ary->len) break;
        VALUE elem = korb_items_data(ary->items)[i];                      /* copied into bf before GC */
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                                        /* no block → Enumerator over reversed elements */
        slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(VALUE_REF_GET(self))->len));
        VALUE_REF rev = VALUE_REF_AT(&slots[0]);
        for (int32_t k = (int32_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; k >= 0; k--)
            CHECK(korb_ary_push_val(c, slots + 1, rev, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[k]));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "reverse_each"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(rev), slots[1]);
    }
    uint32_t i = VAL2ARY(VALUE_REF_GET(self))->len;
    while (i > 0) {
        i--;
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) continue;                           /* shrunk during iteration */
        VALUE elem = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* each_slice(n): consecutive n-element slices.  block → yield each slice (nil);
 * no block → an Enumerator over the slices.  The slices are pre-built into a
 * rooted array so yielding is GC-safe. */
static RESULT korb_m_ary_each_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    korb_sword_t n;
    VALUE nv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!korb_to_index(nv, &n))) {              /* coerce the size via #to_int */
        RESULT cr = korb_coerce_to_int(c, slots, &nv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    }
    if (UNLIKELY(n <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid slice size");
    /* slots[0]=out, slots[1]=captured_self (heap VALUE — must be rooted across the
     * slice-building allocs, else STRESS GC moves `main` and the later yield uses a
     * stale self), slots[2]=current slice, yield/scratch at slots+3. */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                 /* array of slices */
    slots[1] = KORB_CSELF_VAL(captured_self);
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(self))->len; i += (uint32_t)n) {
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, (uint32_t)n));   /* one slice; slots_top now covers [0..2] */
        VALUE_REF slice = VALUE_REF_AT(&slots[2]);
        for (uint32_t j = 0; j < (uint32_t)n && i + j < VAL2ARY(VALUE_REF_GET(self))->len; j++)
            CHECK(korb_ary_push_val(c, slots + 3, slice, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i + j]));
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(slice)));
    }
    if (block == NULL) {
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "each_slice"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE sl = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &sl, 1, &slots[1]);   /* rooted self */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));    /* Ruby 4.0: block form returns the receiver */
}
/* each_cons(n): overlapping n-element windows.  block → yield each (nil); no block
 * → Enumerator.  Windows pre-built into a rooted array (GC-safe yields). */
static RESULT korb_m_ary_each_cons(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1)");
    korb_sword_t n;
    VALUE nv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!korb_to_index(nv, &n))) {              /* coerce the size via #to_int */
        RESULT cr = korb_coerce_to_int(c, slots, &nv);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (!korb_to_index(nv, &n)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    }
    if (UNLIKELY(n <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid size");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));
    slots[1] = KORB_CSELF_VAL(captured_self);                                    /* rooted across the build allocs */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i + (uint32_t)n <= VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, (uint32_t)n));   /* one window */
        VALUE_REF win = VALUE_REF_AT(&slots[2]);
        for (uint32_t j = 0; j < (uint32_t)n; j++)
            CHECK(korb_ary_push_val(c, slots + 3, win, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i + j]));
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(win)));
    }
    if (block == NULL) {
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "each_cons"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE w = korb_items_data(VAL2ARY(VALUE_REF_GET(out))->items)[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &w, 1, &slots[1]);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));    /* Ruby 4.0: block form returns the receiver */
}
/* chunk_while { |prev, cur| } — split between elements where the block is false;
 * returns an Enumerator over the chunks.  Block yields each adjacent pair. */
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
/* shared engine: chunk_while closes a chunk where the block is FALSE; slice_when
 * closes where it is TRUE (slice_when = chunk_while with the predicate negated). */
static RESULT korb_ary_chunk_impl(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool slice_when, const char *desc) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block given");
    /* slots[0]=out (chunks), slots[1]=captured_self (root across yields/allocs),
     * slots[2]=current chunk, yield args + scratch at slots+3. */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));
    slots[1] = KORB_CSELF_VAL(captured_self);
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (len > 0) {
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 1));            /* current chunk */
        VALUE_REF cur = VALUE_REF_AT(&slots[2]);
        CHECK(korb_ary_push_val(c, slots + 3, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[0]));
        for (uint32_t i = 1; i < len; i++) {
            VALUE argv[2] = { korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i - 1],
                              korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i] };
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, &slots[1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            bool boundary = KORB_TRUTHY(r.value) ? slice_when : !slice_when;
            if (boundary) {                                         /* close chunk */
                CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(cur)));
                slots[2] = UNWRAP(korb_ary_new(c, slots + 3, 1));
                cur = VALUE_REF_AT(&slots[2]);
            }
            CHECK(korb_ary_push_val(c, slots + 3, cur, korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i]));
        }
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(cur)));
    }
    slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), desc));
    return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
}
static RESULT korb_m_ary_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; return korb_ary_chunk_impl(c, slots, self, block, def_env, captured_self, false, "chunk_while");
}
/* chunk{|e| key} → Enumerator of [key, [consecutive elems with == key]] runs. */
static RESULT korb_m_ary_chunk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {                      /* chunk's block is a key-fn, not an iterator → plain enum */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), "chunk"));
        return korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));       /* pairs (result) */
    VALUE_REF pairs = VALUE_REF_AT(&slots[0]);
    slots[1] = KORB_NIL;                                /* current run array */
    slots[2] = KORB_NIL;                                /* current key */
    bool have = false;
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        slots[3] = korb_items_data(ary->items)[i];                /* elem (root across yield) */
        RESULT kr = korb_block_yield(c, slots + 4, block, def_env, &slots[3], 1, cself);
        if (UNLIKELY(kr.state != KORB_NORMAL)) return kr;
        slots[4] = kr.value;                           /* key (root) */
        if (!have || !korb_value_eq(slots[2], slots[4])) {
            if (have) {                                /* flush previous [key, run] */
                slots[5] = UNWRAP(korb_ary_new(c, slots + 5, 2));
                CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[2]));
                CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[1]));
                CHECK(korb_ary_push_val(c, slots + 6, pairs, slots[5]));
            }
            slots[2] = slots[4];                       /* new key */
            slots[1] = UNWRAP(korb_ary_new(c, slots + 5, 4));   /* new run */
            have = true;
        }
        slots[5] = korb_items_data(VAL2ARY(VALUE_REF_GET(self))->items)[i];   /* re-read elem (GC) */
        CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[1]), slots[5]));
    }
    if (have) {                                        /* flush last run */
        slots[5] = UNWRAP(korb_ary_new(c, slots + 5, 2));
        CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[2]));
        CHECK(korb_ary_push_val(c, slots + 6, VALUE_REF_AT(&slots[5]), slots[1]));
        CHECK(korb_ary_push_val(c, slots + 6, pairs, slots[5]));
    }
    slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "chunk"));
    return korb_enum_new(c, slots + 2, VALUE_REF_GET(pairs), slots[1]);
}
static RESULT korb_m_ary_slice_when(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; return korb_ary_chunk_impl(c, slots, self, block, def_env, captured_self, true, "slice_when");
}
static RESULT korb_m_ary_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator of [elem, index] pairs */
        slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(VALUE_REF_GET(self))->len));
        VALUE_REF pairs = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; ; i++) {
            const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
            if (i >= ary->len) break;
            slots[1] = korb_items_data(ary->items)[i];
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), LONG2FIX(i)));
            CHECK(korb_ary_push_val(c, slots + 3, pairs, slots[2]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_with_index"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(pairs), slots[1]);
    }
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        VALUE argv[2] = { korb_items_data(ary->items)[i], LONG2FIX(i) };
        RESULT r = korb_block_yield(c, slots, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the elements (for .with_index) */
        slots[0] = UNWRAP(korb_enum_desc(c, slots, VALUE_REF_GET(self), "map"));
        return korb_enum_new(c, slots + 1, VALUE_REF_GET(self), slots[0]);
    }
    uint32_t n0 = VAL2ARY(VALUE_REF_GET(self))->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n0)));  /* slots now past dst */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) break;
        VALUE elem = korb_items_data(ary->items)[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots, dst, r.value));      /* push roots r.value */
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_int_times(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    korb_sword_t n = FIX2LONG(VALUE_REF_GET(self));
    if (block == NULL) {                              /* → Enumerator over 0...n */
        slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)(n > 0 ? n : 0)));
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        for (korb_sword_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "times"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(dst), slots[1]);
    }
    for (korb_sword_t i = 0; i < n; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* up==true: self upto to (ascending); else downto (descending). */
static RESULT korb_int_iter(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool up, const char *meth) {
    VALUE lv = VALUE_SLICE_GET(a, 0);
    /* n.upto(Float::INFINITY) without a block → an endless ArithmeticSequence
     * (begin=n, step=+1); zip/first/take pull it lazily by index without
     * materializing.  (CRuby returns an Enumerator; an ArithSeq suffices here.) */
    if (block == NULL && up && !FIXNUM_P(lv)) {
        double d;
        if (korb_num_to_d(lv, &d) && isinf(d) && d > 0)
            return korb_arithseq_new(c, slots, VALUE_REF_GET(self), lv, LONG2FIX(1), 2, 0);
    }
    korb_sword_t to, from = FIX2LONG(VALUE_REF_GET(self));
    if (LIKELY(FIXNUM_P(lv))) {
        to = FIX2LONG(lv);
    } else if (KORB_FLOAT_P(lv) && isfinite(korb_float_val(lv))) {   /* Float endpoint: floor (upto) / ceil (downto) */
        const double d = korb_float_val(lv);
        to = (korb_sword_t)(up ? floor(d) : ceil(d));
    } else {
        double d;
        if (korb_num_to_d(lv, &d)) to = (korb_sword_t)(up ? floor(d) : ceil(d));   /* Bignum/Rational endpoint */
        else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of Integer with %s failed", korb_type_name(lv));   /* non-numeric */
    }
    if (block == NULL) {                              /* → Enumerator of the sequence */
        slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        if (up) for (korb_sword_t i = from; i <= to; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        else    for (korb_sword_t i = from; i >= to; i--) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), meth));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(dst), slots[1]);
    }
    if (up) for (korb_sword_t i = from; i <= to; i++) { VALUE iv = LONG2FIX(i); RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    else    for (korb_sword_t i = from; i >= to; i--) { VALUE iv = LONG2FIX(i); RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_int_upto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_int_iter(c, slots, self, a, block, def_env, cself, true, "upto"); }
static RESULT korb_m_int_downto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_int_iter(c, slots, self, a, block, def_env, cself, false, "downto"); }

