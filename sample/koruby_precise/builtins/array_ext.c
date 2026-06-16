/* koruby_precise — array_ext.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- more Array methods -------------------------------------------------- */

static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, 0, (uint32_t)n);
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE nv = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (UNLIKELY(!korb_to_index(nv, &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(nv));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if ((uint32_t)n > len) n = len;
    return korb_ary_subseq(c, slots, self, (uint32_t)n, len - (uint32_t)n);
}
static RESULT korb_m_ary_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE v = VALUE_SLICE_GET(a, 0);
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    KorbArrayItems *it = ary->items;
    uint32_t w = 0; bool found = false;
    for (uint32_t r = 0; r < ary->len; r++) {
        if (korb_value_eq(it->data[r], v)) found = true;
        else { if (w != r) ARO_STORE(c, it, &it->data[w], it->data[r]); w++; }
    }
    for (uint32_t r = w; r < ary->len; r++) ARO_STORE(c, it, &it->data[r], KORB_NIL);
    ary->len = w;
    return RESULT_OK(found ? v : KORB_NIL);
}
static RESULT korb_m_ary_delete_at(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t i;
    if (!korb_to_index(iv, &i)) return RESULT_OK(KORB_NIL);
    KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    if (i < 0) i += ary->len;
    if (i < 0 || (uint32_t)i >= ary->len) return RESULT_OK(KORB_NIL);
    KorbArrayItems *it = ary->items;
    VALUE removed = it->data[i];
    for (uint32_t r = (uint32_t)i; r + 1 < ary->len; r++) ARO_STORE(c, it, &it->data[r], it->data[r + 1]);
    ary->len--; ARO_STORE(c, it, &it->data[ary->len], KORB_NIL);
    return RESULT_OK(removed);
}
static RESULT korb_m_ary_rindex(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    if (block != NULL) {                          /* block form: last index whose yield is truthy */
        for (int32_t i = (int32_t)VAL2ARY(VALUE_REF_GET(self))->len - 1; i >= 0; i--) {
            slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (r.value != KORB_NIL && r.value != KORB_FALSE) return RESULT_OK(LONG2FIX(i));
        }
        return RESULT_OK(KORB_NIL);
    }
    const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (int32_t i = (int32_t)ary->len - 1; i >= 0; i--)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(LONG2FIX(i));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_rotate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    intptr_t sh = 1;
    if (VALUE_SLICE_LEN(a) >= 1 && UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &sh))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (len == 0) return korb_ary_subseq(c, slots, self, 0, 0);
    intptr_t s = ((sh % (intptr_t)len) + (intptr_t)len) % (intptr_t)len;   /* normalized left rotation */
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[(s + i) % len];
        CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* zip rows: [ self[i], other0[i], other1[i], ... ]. With a block, yield each row
 * and return nil; otherwise collect rows into an array. dst lives at slots[1]
 * (block path leaves it nil/unused), rows built at slots[2]. */
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    uint32_t k = VALUE_SLICE_LEN(a);
    uint32_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, n)) : KORB_NIL;   /* dst */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, k + 1));              /* row at slots[1] */
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, row, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_zip_elem(VALUE_SLICE_GET(a, j), i)));
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else {
            CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
        }
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}

static bool korb_ary_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eql(ar->items->data[i], v)) return true;
    return false;
}
/* `|` union (in self then other, deduped) / `&` intersection (in both, self order, deduped) */
static RESULT korb_m_ary_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) { VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i]; if (!korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {  /* union(*others) */
        VALUE ov = VALUE_SLICE_GET(a, k);
        if (UNLIKELY(!KORB_ARRAY_P(ov))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
        uint32_t on = VAL2ARY(ov)->len;
        for (uint32_t i = 0; i < on; i++) { VALUE e = VAL2ARY(VALUE_SLICE_GET(a, k))->items->data[i]; if (!korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e)); }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++)
        if (UNLIKELY(!KORB_ARRAY_P(VALUE_SLICE_GET(a, k)))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(VALUE_SLICE_GET(a, k)));
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool no_args = VALUE_SLICE_LEN(a) == 0;             /* intersection() → plain copy of self, no dedup */
    uint32_t sn = VAL2ARY(VALUE_REF_GET(self))->len;
    for (uint32_t i = 0; i < sn; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        if (no_args) { CHECK(korb_ary_push_val(c, slots + 1, dst, e)); continue; }
        bool in_all = true;                              /* element must be in every other array */
        for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) if (!korb_arr_has(VAL2ARY(VALUE_SLICE_GET(a, k)), e)) { in_all = false; break; }
        if (in_all && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

