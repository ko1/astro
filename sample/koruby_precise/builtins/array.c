/* koruby_precise — array.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Array methods ------------------------------------------------------- */

static RESULT korb_m_ary_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(SELF_ARY->len)); }
static RESULT korb_m_ary_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_ARY->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_ary_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_ARRAY_P(o)) return RESULT_OK(KORB_NIL);
    int r = korb_cmp_full(c, VALUE_REF_GET(self), o);   /* element-wise <=> */
    return RESULT_OK(r == 2 ? KORB_NIL : LONG2FIX(r));
}
static RESULT korb_m_ary_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_ary_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* first(n) → first n as array */
        intptr_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t take = (uint32_t)n; if (take > SELF_ARY->len) take = SELF_ARY->len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, SELF_ARY->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[0] : KORB_NIL);
}
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* last(n) → last n as array */
        intptr_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        uint32_t len = SELF_ARY->len;
        uint32_t take = (uint32_t)n; if (take > len) take = len;
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
        for (uint32_t i = len - take; i < len; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, SELF_ARY->items->data[i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    const KorbArray *ary = SELF_ARY; return RESULT_OK(ary->len ? ary->items->data[ary->len - 1] : KORB_NIL);
}

/* fresh array = self[start, len) */
static RESULT korb_ary_subseq(CTX *c, VALUE *slots, VALUE_REF self, uint32_t start, uint32_t len) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, len)));
    for (uint32_t i = 0; i < len; i++)
        CHECK(korb_ary_push_val(c, slots + 1, dst, VAL2ARY(VALUE_REF_GET(self))->items->data[start + i]));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    uint32_t n = SELF_ARY->len;
    VALUE i0 = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(i0)) {                                 /* a[b..e] → subarray */
        const KorbRange *r = VAL2RANGE(i0);
        if (UNLIKELY(!FIXNUM_P(r->rbegin) || !FIXNUM_P(r->rend))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t b = FIX2LONG(r->rbegin), e = FIX2LONG(r->rend);
        if (b < 0) b += n;
        if (e < 0) e += n;
        if (b < 0 || b > (intptr_t)n) return RESULT_OK(KORB_NIL);
        intptr_t last = r->exclude_end ? e - 1 : e, cnt = last - b + 1;
        if (cnt < 0) cnt = 0;
        if (b + cnt > (intptr_t)n) cnt = (intptr_t)n - b;
        return korb_ary_subseq(c, slots, self, (uint32_t)b, (uint32_t)cnt);
    }
    intptr_t i;
    if (UNLIKELY(!korb_to_index(i0, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(i0));
    if (i < 0) i += n;
    if (VALUE_SLICE_LEN(a) >= 2) {                          /* a[start, len] → subarray */
        VALUE lv = VALUE_SLICE_GET(a, 1);
        intptr_t len;
        if (UNLIKELY(!korb_to_index(lv, &len))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
        if (len < 0 || i < 0 || i > (intptr_t)n) return RESULT_OK(KORB_NIL);
        if (i + len > (intptr_t)n) len = (intptr_t)n - i;
        return korb_ary_subseq(c, slots, self, (uint32_t)i, (uint32_t)len);
    }
    if (i < 0 || (uint32_t)i >= n) return RESULT_OK(KORB_NIL);
    return RESULT_OK(SELF_ARY->items->data[i]);
}

/* Replace self[start, dellen) with valref (spliced if Array, else single element).
 * `valref` must be a rooted slot. Returns the replacement value. */
static RESULT korb_ary_splice(CTX *c, VALUE *slots, VALUE_REF self, intptr_t start, intptr_t dellen, VALUE_REF valref) {
    intptr_t len = VAL2ARY(VALUE_REF_GET(self))->len;
    if (start < 0) start += len;
    if (UNLIKELY(start < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array; minimum: -%ld", (long)(start - len + len), (long)len);
    if (dellen < 0) dellen = 0;
    bool splat = KORB_ARRAY_P(VALUE_REF_GET(valref));
    /* build the new sequence in a temp array (rooted), then copy back into self */
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF tmp = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = 0; i < start; i++) {
        VALUE e = (i < len) ? VAL2ARY(VALUE_REF_GET(self))->items->data[i] : KORB_NIL;   /* pad gap with nil */
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    if (splat) {
        uint32_t vn = VAL2ARY(VALUE_REF_GET(valref))->len;
        for (uint32_t j = 0; j < vn; j++) {
            VALUE e = VAL2ARY(VALUE_REF_GET(valref))->items->data[j];
            CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
        }
    } else {
        CHECK(korb_ary_push_val(c, slots + 1, tmp, VALUE_REF_GET(valref)));
    }
    for (intptr_t i = start + dellen; i < len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(self))->items->data[i];
        CHECK(korb_ary_push_val(c, slots + 1, tmp, e));
    }
    /* overwrite self with tmp */
    VAL2ARY(VALUE_REF_GET(self))->len = 0;
    uint32_t tn = VAL2ARY(VALUE_REF_GET(tmp))->len;
    for (uint32_t j = 0; j < tn; j++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(tmp))->items->data[j];
        CHECK(korb_ary_push_val(c, slots + 1, self, e));
    }
    return RESULT_OK(VALUE_REF_GET(valref));
}
static RESULT korb_m_ary_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2..3)", VALUE_SLICE_LEN(a));
    VALUE iv = VALUE_SLICE_GET(a, 0);
    if (VALUE_SLICE_LEN(a) >= 3) {                        /* a[start, len] = val */
        intptr_t start, dellen;
        if (UNLIKELY(!korb_to_index(iv, &start)))               return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &dellen))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 1)));
        return korb_ary_splice(c, slots, self, start, dellen, VALUE_SLICE_REF(a, 2));
    }
    if (KORB_RANGE_P(iv)) {                               /* a[b..e] = val */
        const KorbRange *r = VAL2RANGE(iv);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        intptr_t len = VAL2ARY(VALUE_REF_GET(self))->len;
        if (b < 0) b += len;
        if (e < 0) e += len;
        intptr_t last = r->exclude_end ? e - 1 : e, dellen = last - b + 1;
        if (dellen < 0) dellen = 0;
        return korb_ary_splice(c, slots, self, b, dellen, VALUE_SLICE_REF(a, 1));
    }
    KorbArray *ary = SELF_ARY;
    intptr_t i;
    if (UNLIKELY(!korb_to_index(iv, &i))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
    if (i < 0) i += ary->len;
    if (UNLIKELY(i < 0)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "index %ld too small for array; minimum: -%u", (long)i, ary->len);
    if ((uint32_t)i >= ary->len) {
        CHECK(korb_ary_ensure(c, slots, self, (uint32_t)i + 1 - ary->len));
        ary = SELF_ARY;                                  /* re-read after grow GC */
        for (uint32_t k = ary->len; k <= (uint32_t)i; k++) ARO_STORE(c, ary->items, &ary->items->data[k], KORB_NIL);
        ary->len = (uint32_t)i + 1;
    }
    VALUE val = VALUE_SLICE_GET(a, 1);                    /* re-read (rooted) after GC */
    KorbArrayItems *it = ary->items;
    ARO_STORE(c, it, &it->data[i], val);
    return RESULT_OK(val);
}

/* slice!: remove and return element (single index) or subarray (range/start,len). */
static RESULT korb_m_ary_slice_bang(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    VALUE iv = VALUE_SLICE_GET(a, 0);
    intptr_t n = VAL2ARY(VALUE_REF_GET(self))->len;
    intptr_t start, dellen; bool subseq_form = false;
    if (KORB_RANGE_P(iv)) {
        const KorbRange *r = VAL2RANGE(iv);
        intptr_t b, e;
        if (UNLIKELY(!korb_to_index(r->rbegin, &b) || !korb_to_index(r->rend, &e))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (b < 0) b += n;
        if (e < 0) e += n;
        intptr_t last = r->exclude_end ? e - 1 : e;
        start = b; dellen = last - b + 1; if (dellen < 0) dellen = 0;
        subseq_form = true;
    } else if (VALUE_SLICE_LEN(a) >= 2) {
        if (UNLIKELY(!korb_to_index(iv, &start) || !korb_to_index(VALUE_SLICE_GET(a, 1), &dellen))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (UNLIKELY(dellen < 0)) return RESULT_OK(KORB_NIL);   /* (start, negative len) → nil */
        if (start < 0) start += n;
        subseq_form = true;
    } else {
        if (UNLIKELY(!korb_to_index(iv, &start))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(iv));
        if (start < 0) start += n;
        if (start < 0 || start >= n) return RESULT_OK(KORB_NIL);
        slots[0] = VAL2ARY(VALUE_REF_GET(self))->items->data[start];   /* removed elem */
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
    CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, 0)));
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_ary_push_val(c, slots, self, VALUE_SLICE_GET(a, i)));   /* slice rooted across grow */
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_pop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    KorbArray *ary = SELF_ARY;
    if (ary->len == 0) return RESULT_OK(KORB_NIL);
    ary->len--;
    VALUE v = ary->items->data[ary->len];
    ARO_STORE(c, ary->items, &ary->items->data[ary->len], KORB_NIL); /* drop the reference (nil needs no WB) */
    return RESULT_OK(v);
}

static RESULT korb_m_ary_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbArray *ary = SELF_ARY;
    VALUE needle = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ary->len; i++)
        if (korb_value_eq(ary->items->data[i], needle)) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
}

static RESULT korb_m_ary_reverse(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t n = SELF_ARY->len;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, n)));
    for (uint32_t i = 0; i < n; i++) {
        VALUE elem = SELF_ARY->items->data[n - 1 - i];   /* push_val roots elem before any GC */
        CHECK(korb_ary_push_val(c, slots, dst, elem));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_plus(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE ov = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_ARRAY_P(ov)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Array", korb_type_name(ov));
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
        VALUE elem = ary->items->data[i];                      /* copied into bf before GC */
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT korb_m_ary_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; REQUIRE_BLOCK("Array#reverse_each");
    uint32_t i = VAL2ARY(VALUE_REF_GET(self))->len;
    while (i > 0) {
        i--;
        const KorbArray *ary = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ary->len) continue;                           /* shrunk during iteration */
        VALUE elem = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* each_slice(n): consecutive n-element slices.  block → yield each slice (nil);
 * no block → an Enumerator over the slices.  The slices are pre-built into a
 * rooted array so yielding is GC-safe. */
static RESULT korb_m_ary_each_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    intptr_t n;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || !korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
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
            CHECK(korb_ary_push_val(c, slots + 3, slice, VAL2ARY(VALUE_REF_GET(self))->items->data[i + j]));
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(slice)));
    }
    if (block == NULL) {
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "each_slice"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE sl = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &sl, 1, &slots[1]);   /* rooted self */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));    /* Ruby 4.0: block form returns the receiver */
}
/* each_cons(n): overlapping n-element windows.  block → yield each (nil); no block
 * → Enumerator.  Windows pre-built into a rooted array (GC-safe yields). */
static RESULT korb_m_ary_each_cons(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    intptr_t n;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || !korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (UNLIKELY(n <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid size");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));
    slots[1] = KORB_CSELF_VAL(captured_self);                                    /* rooted across the build allocs */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i + (uint32_t)n <= VAL2ARY(VALUE_REF_GET(self))->len; i++) {
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, (uint32_t)n));   /* one window */
        VALUE_REF win = VALUE_REF_AT(&slots[2]);
        for (uint32_t j = 0; j < (uint32_t)n; j++)
            CHECK(korb_ary_push_val(c, slots + 3, win, VAL2ARY(VALUE_REF_GET(self))->items->data[i + j]));
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(win)));
    }
    if (block == NULL) {
        slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), "each_cons"));
        return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE w = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
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
        CHECK(korb_ary_push_val(c, slots + 3, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[0]));
        for (uint32_t i = 1; i < len; i++) {
            VALUE argv[2] = { VAL2ARY(VALUE_REF_GET(self))->items->data[i - 1],
                              VAL2ARY(VALUE_REF_GET(self))->items->data[i] };
            RESULT r = korb_block_yield(c, slots + 3, block, def_env, argv, 2, &slots[1]);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            bool boundary = KORB_TRUTHY(r.value) ? slice_when : !slice_when;
            if (boundary) {                                         /* close chunk */
                CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(cur)));
                slots[2] = UNWRAP(korb_ary_new(c, slots + 3, 1));
                cur = VALUE_REF_AT(&slots[2]);
            }
            CHECK(korb_ary_push_val(c, slots + 3, cur, VAL2ARY(VALUE_REF_GET(self))->items->data[i]));
        }
        CHECK(korb_ary_push_val(c, slots + 3, out, VALUE_REF_GET(cur)));
    }
    slots[2] = UNWRAP(korb_enum_desc(c, slots + 2, VALUE_REF_GET(self), desc));
    return korb_enum_new(c, slots + 3, VALUE_REF_GET(out), slots[2]);
}
static RESULT korb_m_ary_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; return korb_ary_chunk_impl(c, slots, self, block, def_env, captured_self, false, "chunk_while");
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
            slots[1] = ary->items->data[i];
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
        VALUE argv[2] = { ary->items->data[i], LONG2FIX(i) };
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
        VALUE elem = ary->items->data[i];
        RESULT r = korb_block_yield(c, slots, block, def_env, &elem, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots, dst, r.value));      /* push roots r.value */
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_int_times(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; REQUIRE_BLOCK("Integer#times");
    intptr_t n = FIX2LONG(VALUE_REF_GET(self));
    for (intptr_t i = 0; i < n; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* up==true: self upto to (ascending); else downto (descending). */
static RESULT korb_int_iter(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool up, const char *meth) {
    VALUE lv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(lv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(lv));
    intptr_t to = FIX2LONG(lv), from = FIX2LONG(VALUE_REF_GET(self));
    if (block == NULL) {                              /* → Enumerator of the sequence */
        slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        if (up) for (intptr_t i = from; i <= to; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        else    for (intptr_t i = from; i >= to; i--) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), meth));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(dst), slots[1]);
    }
    if (up) for (intptr_t i = from; i <= to; i++) { VALUE iv = LONG2FIX(i); RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    else    for (intptr_t i = from; i >= to; i--) { VALUE iv = LONG2FIX(i); RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_int_upto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_int_iter(c, slots, self, a, block, def_env, cself, true, "upto"); }
static RESULT korb_m_int_downto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_int_iter(c, slots, self, a, block, def_env, cself, false, "downto"); }

