/* koruby_precise — range.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Range methods ------------------------------------------------------- */


/* integer iteration bounds [lo, hi) ; false if endpoints aren't both Integer */
static bool korb_range_int_bounds(const KorbRange *r, intptr_t *lo, intptr_t *hi) {
    if (!FIXNUM_P(r->rbegin) || !FIXNUM_P(r->rend)) return false;
    intptr_t e = FIX2LONG(r->rend);
    *lo = FIX2LONG(r->rbegin);
    *hi = r->exclude_end ? e : e + 1;
    return true;
}

static RESULT korb_m_range_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rbegin); }
static RESULT korb_m_range_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->rend); }
static RESULT korb_m_range_exclude(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a){ (void)c;(void)slots;(void)a; return RESULT_OK(SELF_RANGE->exclude_end ? KORB_TRUE : KORB_FALSE); }

static RESULT korb_m_range_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
    return RESULT_OK(LONG2FIX(hi > lo ? hi - lo : 0));
}
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* defined below */
static RESULT korb_m_ary_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_count(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t lo, hi;
    if (block != NULL || !korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* block or non-int → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_count(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* count(obj): 1 if obj in the integer range else 0 */
        VALUE o = VALUE_SLICE_GET(a, 0);
        if (!FIXNUM_P(o)) return RESULT_OK(LONG2FIX(0));
        intptr_t v = FIX2LONG(o);
        return RESULT_OK(LONG2FIX(v >= lo && v < hi ? 1 : 0));
    }
    return RESULT_OK(LONG2FIX(hi > lo ? hi - lo : 0));
}

static RESULT korb_m_range_cover(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;
    const KorbRange *r = SELF_RANGE;
    VALUE x = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(x)) {                       /* cover?(other_range): self contains the whole range */
        const KorbRange *o = VAL2RANGE(x);
        int bc = korb_cmp_values(r->rbegin, o->rbegin);    /* self.begin <=> other.begin */
        int ec = korb_cmp_values(o->rend, r->rend);        /* other.end <=> self.end */
        if (bc == 2 || ec == 2) return RESULT_OK(KORB_FALSE);
        bool lo_ok = bc <= 0;
        bool hi_ok = (r->exclude_end && !o->exclude_end) ? (ec < 0) : (ec <= 0);
        return RESULT_OK((lo_ok && hi_ok) ? KORB_TRUE : KORB_FALSE);
    }
    int lc = korb_cmp_values(r->rbegin, x);     /* begin <=> x */
    int uc = korb_cmp_values(x, r->rend);       /* x <=> end */
    if (lc == 2 || uc == 2) return RESULT_OK(KORB_FALSE);
    bool lower = (lc <= 0);
    bool upper = r->exclude_end ? (uc < 0) : (uc <= 0);
    return RESULT_OK((lower && upper) ? KORB_TRUE : KORB_FALSE);
}

/* include?/member?: membership of a single value (a Range/other container is not an element). */
static RESULT korb_m_range_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE x = VALUE_SLICE_GET(a, 0);
    if (KORB_RANGE_P(x) || KORB_ARRAY_P(x) || KORB_HASH_P(x)) return RESULT_OK(KORB_FALSE);
    return korb_m_range_cover(c, slots, self, a);
}
/* build an array of `take` consecutive ints from `from`, step +1 (asc) or -1 (desc). */
static RESULT korb_range_seq(CTX *c, VALUE *slots, intptr_t from, uint32_t take, int step) {
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, take)));
    for (uint32_t i = 0; i < take; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(from + step * (intptr_t)i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_min(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbRange *r = SELF_RANGE;
    intptr_t lo, hi;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* min(n) → first n ascending */
        intptr_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        if (!korb_range_int_bounds(r, &lo, &hi)) {
            slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
            return korb_m_ary_min(c, slots + 1, VALUE_REF_AT(&slots[0]), a, NULL, NULL, KORB_NIL);
        }
        uint32_t take = (uint32_t)n; if ((intptr_t)take > hi - lo) take = (uint32_t)(hi > lo ? hi - lo : 0);
        return korb_range_seq(c, slots, lo, take, 1);
    }
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(lo) : KORB_NIL);
    return RESULT_OK(r->rbegin);
}
static RESULT korb_m_ary_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_max(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbRange *r = SELF_RANGE;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(r, &lo, &hi)) {            /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_max(c, slots + 1, VALUE_REF_AT(&slots[0]), a, NULL, NULL, KORB_NIL);
    }
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {   /* max(n) → last n descending */
        intptr_t n;
        if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
        if (!korb_range_int_bounds(r, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
        uint32_t take = (uint32_t)n; if ((intptr_t)take > hi - lo) take = (uint32_t)(hi > lo ? hi - lo : 0);
        return korb_range_seq(c, slots, hi - 1, take, -1);
    }
    if (korb_range_int_bounds(r, &lo, &hi)) return RESULT_OK(hi > lo ? LONG2FIX(hi - 1) : KORB_NIL);
    if (r->exclude_end) return korb_raise(c, slots, KORB_E_TYPE, 0, "cannot exclude non Integer end value");
    return RESULT_OK(r->rend);
}
static RESULT korb_m_ary_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_take(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_take(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to take negative size");
    intptr_t end = lo + n; if (end > hi) end = hi;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(end > lo ? end - lo : 0))));
    for (intptr_t i = lo; i < end; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_first(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) return korb_m_range_take(c, slots, self, a);
    return RESULT_OK(SELF_RANGE->rbegin);
}
static RESULT korb_m_range_last(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  {
    if (VALUE_SLICE_LEN(a) == 0) return RESULT_OK(SELF_RANGE->rend);
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_last(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative array size");
    intptr_t start = hi - n; if (start < lo) start = lo;
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > start ? hi - start : 0))));
    for (intptr_t i = start; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_sum_b(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t lo, hi;
    if (block != NULL || !korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* block or non-int → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_sum_b(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    intptr_t init = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    intptr_t acc = init;
    for (intptr_t i = lo; i < hi; i++) acc += i;   /* small ranges; Bignum unneeded here */
    return RESULT_OK(LONG2FIX(acc));
}
static RESULT korb_m_range_frozen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE);   /* Range instances are frozen */
}

static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc);
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth);
static RESULT korb_m_range_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the range's elements */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-integer (e.g. String) range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        RESULT r = korb_m_ary_each(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
        return (r.state == KORB_NORMAL) ? RESULT_OK(VALUE_REF_GET(self)) : r;
    }
    for (intptr_t i = lo; i < hi; i++) {           /* bounds are plain ints — GC-safe */
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (defined below) */

static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    intptr_t lo, hi;
    if (korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
        for (intptr_t i = lo; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (KORB_STRING_P(SELF_RANGE->rbegin) && KORB_STRING_P(SELF_RANGE->rend)) {   /* String range via succ */
        slots[1] = SELF_RANGE->rbegin;                     /* cur  (rooted before any alloc) */
        slots[2] = SELF_RANGE->rend;                       /* end  (rooted) */
        const bool excl = SELF_RANGE->exclude_end != 0;
        slots[0] = UNWRAP(korb_ary_new(c, slots + 3, 8));
        VALUE_REF out = VALUE_REF_AT(&slots[0]);
        for (int guard = 0; guard < 100000000; guard++) {
            uint32_t curlen = VAL2STR(slots[1])->len, endlen = VAL2STR(slots[2])->len;
            if (curlen > endlen) break;                    /* succ grew past end length */
            int cmp = korb_cmp_values(slots[1], slots[2]);
            if (cmp > 0) break;
            if (excl && cmp == 0) break;
            CHECK(korb_ary_push_val(c, slots + 3, out, slots[1]));
            if (cmp == 0) break;                           /* inclusive end reached */
            slots[1] = UNWRAP(korb_m_str_succ(c, slots + 3, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(NULL, 0)));
        }
        return RESULT_OK(VALUE_REF_GET(out));
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate from %s", korb_type_name(SELF_RANGE->rbegin));
}

static RESULT korb_m_range_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (block == NULL) {                              /* → Enumerator over the range's elements */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "map"));
        return korb_enum_new(c, slots + 2, slots[0], slots[1]);
    }
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {   /* non-integer range → map over to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
        return korb_m_ary_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT korb_m_ary_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_tally(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) >= 1) {                    /* tally(hash) or String range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_tally(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    slots[0] = UNWRAP(korb_hash_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0)));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = lo; i < hi; i++) {              /* int range elements are unique → count 1 each */
        slots[1] = LONG2FIX(i);
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(1)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
static RESULT korb_m_range_each_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL || VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_each_with_object(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                 /* memo (rooted) */
    for (intptr_t i = lo; i < hi; i++) {
        VALUE argv[2] = { LONG2FIX(i), slots[0] };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_range_overlap(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_RANGE_P(o))) return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Range)", korb_type_name(o));
    intptr_t lo1, hi1, lo2, hi2;
    if (!korb_range_int_bounds(SELF_RANGE, &lo1, &hi1) || !korb_range_int_bounds(VAL2RANGE(o), &lo2, &hi2)) {
        const KorbRange *r1 = SELF_RANGE, *r2 = VAL2RANGE(o);     /* non-integer: compare endpoints */
        int c1 = korb_cmp_values(r1->rbegin, r2->rend);          /* r1.begin vs r2.end */
        int c2 = korb_cmp_values(r2->rbegin, r1->rend);          /* r2.begin vs r1.end */
        int e1 = korb_cmp_values(r1->rbegin, r1->rend), e2 = korb_cmp_values(r2->rbegin, r2->rend);
        if (c1 == 2 || c2 == 2 || e1 == 2 || e2 == 2) return RESULT_OK(KORB_FALSE);   /* incomparable */
        bool a_ok = r2->exclude_end ? (c1 < 0) : (c1 <= 0);
        bool b_ok = r1->exclude_end ? (c2 < 0) : (c2 <= 0);
        bool ne1  = r1->exclude_end ? (e1 < 0) : (e1 <= 0);
        bool ne2  = r2->exclude_end ? (e2 < 0) : (e2 <= 0);
        return RESULT_OK((a_ok && b_ok && ne1 && ne2) ? KORB_TRUE : KORB_FALSE);
    }
    bool ov = lo1 < hi2 && lo2 < hi1 && hi1 > lo1 && hi2 > lo2;   /* half-open [lo,hi) overlap, non-empty */
    return RESULT_OK(ov ? KORB_TRUE : KORB_FALSE);
}
/* min_by(want=-1)/max_by(want=1): element whose block key is the extreme. */
static RESULT korb_m_ary_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_by(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *cself, int want) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#min_by/max_by without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (want < 0 ? korb_m_ary_min_by : korb_m_ary_max_by)(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
    }
    if (hi <= lo) return RESULT_OK(KORB_NIL);
    bool have = false;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!have) { slots[0] = iv; slots[1] = r.value; have = true; continue; }
        int cmp = korb_cmp_full(c, r.value, slots[1]);
        if (cmp == want) { slots[0] = iv; slots[1] = r.value; }
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_range_min_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_range_by(c, slots, self, block, def_env, cself, -1); }
static RESULT korb_m_range_max_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { (void)a; return korb_range_by(c, slots, self, block, def_env, cself, 1); }
static RESULT korb_m_ary_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_sort_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#sort_by without a block is not supported");
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));      /* materialize, then Array#sort_by */
    return korb_m_ary_sort_by(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_reverse_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#reverse_each without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = hi - 1; i >= lo; i--) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Range#each_slice(n) — consecutive n-element slices over the integer range.
 * block → yield each slice (returns self); no block → Enumerator over the slices.
 * Slices are pre-built into a rooted array so yielding is GC-safe. */
static RESULT korb_m_range_each_slice(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t n;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || !korb_to_index(VALUE_SLICE_GET(a, 0), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (UNLIKELY(n <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid slice size");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_each_slice(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 0));                 /* array of slices */
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = lo; i < hi; i += n) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, (uint32_t)n));
        VALUE_REF slice = VALUE_REF_AT(&slots[1]);
        for (intptr_t j = 0; j < n && i + j < hi; j++)
            CHECK(korb_ary_push_val(c, slots + 2, slice, LONG2FIX(i + j)));
        CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_REF_GET(slice)));
    }
    if (block == NULL) {
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_slice"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(out), slots[1]);
    }
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(out))->len; i++) {
        VALUE sl = VAL2ARY(VALUE_REF_GET(out))->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &sl, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* bsearch (find-minimum mode): smallest i in [lo,hi) where block(i) is truthy. */
static RESULT korb_m_range_bsearch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#bsearch without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    bool found = false; intptr_t ans = 0;                 /* leftmost truthy index */
    while (lo < hi) {
        intptr_t mid = lo + (hi - lo) / 2;
        VALUE iv = LONG2FIX(mid);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (FIXNUM_P(r.value)) {                          /* find-any mode: 0=hit, <0 left, >0 right */
            intptr_t v = FIX2LONG(r.value);
            if (v == 0) return RESULT_OK(LONG2FIX(mid));
            if (v < 0) hi = mid; else lo = mid + 1;
        } else if (KORB_TRUTHY(r.value)) { found = true; ans = mid; hi = mid; }   /* find-minimum: go left */
        else lo = mid + 1;
    }
    return RESULT_OK(found ? LONG2FIX(ans) : KORB_NIL);
}
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_minmax(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), NULL, NULL, KORB_NIL);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    CHECK(korb_ary_push_val(c, slots + 1, dst, hi > lo ? LONG2FIX(lo) : KORB_NIL));
    CHECK(korb_ary_push_val(c, slots + 1, dst, hi > lo ? LONG2FIX(hi - 1) : KORB_NIL));
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Range#sort/min/max/minmax with a comparator block: materialize to_a (ascending)
 * then delegate to the block-aware Array method.  No block → the integer-range
 * fast paths.  A count arg on min/max (e.g. min(2)) also keeps the fast path. */
static RESULT korb_m_range_sort_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                    NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
    if (block == NULL) return RESULT_OK(slots[0]);
    return korb_m_ary_sort(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_min_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL || (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL))
        return korb_m_range_min(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_min(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_max_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                   NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL || (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL))
        return korb_m_range_max(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_max(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_range_minmax_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                      NODE *block, VALUE *def_env, VALUE *cself) {
    if (block == NULL) return korb_m_range_minmax(c, slots, self, a);
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_minmax(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, cself);
}
static RESULT korb_m_ary_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_ary_slice_when(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_range_chunk_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_chunk_while(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_ary_chunk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_chunk(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_chunk(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_slice_when(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_slice_when(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
static RESULT korb_m_range_each_cons(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    RESULT r = korb_m_ary_each_cons(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    if (block != NULL && r.state == KORB_NORMAL) return RESULT_OK(VALUE_REF_GET(self));   /* block form → self (the range) */
    return r;
}
static RESULT korb_m_range_uniq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
    return korb_m_ary_uniq(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
}
static RESULT korb_m_range_minmax_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    slots[0] = UNWRAP(korb_range_by(c, slots, self, block, def_env, cself, -1));   /* min_by */
    slots[1] = UNWRAP(korb_range_by(c, slots + 1, self, block, def_env, cself, 1)); /* max_by */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}
static RESULT korb_m_range_partition(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#partition without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));         /* matching */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));     /* non-matching */
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(KORB_TRUTHY(r.value) ? &slots[0] : &slots[1]), iv));
    }
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
    return RESULT_OK(VALUE_REF_GET(VALUE_REF_AT(&slots[2])));
}
static RESULT korb_m_range_flat_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#flat_map without a block (Enumerator) is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_flat_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) {                 /* one level of flattening */
            slots[1] = r.value;
            uint32_t sublen = VAL2ARY(slots[1])->len;
            for (uint32_t j = 0; j < sublen; j++)    /* re-read sub each push (GC may move it) */
                CHECK(korb_ary_push_val(c, slots + 2, dst, VAL2ARY(slots[1])->items->data[j]));
        } else {
            CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_range_drop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_drop(c, slots + 1, VALUE_REF_AT(&slots[0]), a);
    }
    intptr_t n;
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 0), &n))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(n < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "attempt to drop negative size");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo + n; i < hi; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_drop_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#drop_while without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_drop_while(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    bool dropping = true;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        if (dropping) {
            RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) continue;
            dropping = false;
        }
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_take_while(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#take_while without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (!KORB_TRUTHY(r.value)) break;
        CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool keep) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (keep ? korb_m_ary_grep : korb_m_ary_grep_v)(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        slots[0] = LONG2FIX(i);
        if (korb_case_eq(c, VALUE_SLICE_GET(a, 0), slots[0]) == keep) {
            if (block != NULL) {
                RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
                if (UNLIKELY(r.state != KORB_NORMAL)) return r;
                CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
            } else CHECK(korb_ary_push_val(c, slots + 1, dst, slots[0]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_grep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_range_grep(c, slots, self, a, block, def_env, cself, true); }
static RESULT korb_m_range_grep_v(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_range_grep(c, slots, self, a, block, def_env, cself, false); }
static RESULT korb_m_range_group_by(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#group_by without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_group_by(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = UNWRAP(korb_hash_new(c, slots, 4));     VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = lo; i < hi; i++) {
        slots[1] = LONG2FIX(i);                        /* element */
        RESULT r = korb_block_yield(c, slots + 3, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[2] = r.value;                            /* key */
        int32_t idx = korb_hash_find(VAL2HASH(VALUE_REF_GET(h)), slots[2]);
        if (idx < 0) {
            slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
            CHECK(korb_hash_set(c, slots + 4, h, VALUE_REF_AT(&slots[2]), slots[3]));
        } else {
            slots[3] = VAL2HASH(VALUE_REF_GET(h))->items->data[2 * idx + 1];
            CHECK(korb_ary_push_val(c, slots + 4, VALUE_REF_AT(&slots[3]), slots[1]));
        }
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
static RESULT korb_m_range_filter_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#filter_map without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_filter_map(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) CHECK(korb_ary_push_val(c, slots + 1, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_each_wi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        RESULT r = korb_m_ary_each_wi(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
        return (r.state == KORB_NORMAL && block != NULL) ? RESULT_OK(VALUE_REF_GET(self)) : r;
    }
    if (block == NULL) {                              /* → Enumerator of [elem, index] pairs */
        slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0)));
        VALUE_REF pairs = VALUE_REF_AT(&slots[0]);
        for (intptr_t i = lo; i < hi; i++) {
            slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
            CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(i)));
            CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), LONG2FIX(i - lo)));
            CHECK(korb_ary_push_val(c, slots + 2, pairs, slots[1]));
        }
        slots[1] = UNWRAP(korb_enum_desc(c, slots + 1, VALUE_REF_GET(self), "each_with_index"));
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(pairs), slots[1]);
    }
    for (intptr_t i = lo; i < hi; i++) {
        VALUE argv[2] = { LONG2FIX(i), LONG2FIX(i - lo) };
        RESULT r = korb_block_yield(c, slots, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static VALUE korb_zip_elem(VALUE arg, uint32_t i);    /* array_int_ext.c */
static RESULT korb_m_ary_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_zip(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    uint32_t k = VALUE_SLICE_LEN(a);
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_zip(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    slots[0] = (block == NULL) ? UNWRAP(korb_ary_new(c, slots, (uint32_t)(hi > lo ? hi - lo : 0))) : KORB_NIL;
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (intptr_t i = lo; i < hi; i++) {
        uint32_t idx = (uint32_t)(i - lo);
        slots[1] = UNWRAP(korb_ary_new(c, slots + 2, k + 1));
        VALUE_REF row = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, row, LONG2FIX(i)));
        for (uint32_t j = 0; j < k; j++)
            CHECK(korb_ary_push_val(c, slots + 2, row, korb_zip_elem(VALUE_SLICE_GET(a, j), idx)));   /* Array/Range arg */
        if (block != NULL) {
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        } else CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(block != NULL ? KORB_NIL : VALUE_REF_GET(dst));
}
static RESULT korb_m_ary_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_one(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_one(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    uint32_t cnt = 0;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool t;
        if (has_pat) {
            t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), iv);
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else t = true;                              /* every Integer is truthy */
        if (t && ++cnt > 1) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(cnt == 1 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_ary_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_range_find_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_find_index(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
    }
    bool has_arg = VALUE_SLICE_LEN(a) >= 1;
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool hit;
        if (has_arg) hit = korb_value_eq(iv, VALUE_SLICE_GET(a, 0));
        else {
            if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#find_index without arg or block is not supported");
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            hit = KORB_TRUTHY(r.value);
        }
        if (hit) return RESULT_OK(LONG2FIX(i - lo));
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_ary_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self);
static RESULT korb_m_range_reduce(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → reduce over to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_reduce(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    if (block == NULL) {                                   /* symbol form */
        uint32_t na = VALUE_SLICE_LEN(a), op_mid;
        if (UNLIKELY(na < 1 || !korb_reduce_op(c, VALUE_SLICE_GET(a, na - 1), &op_mid)))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block or operator symbol given");
        intptr_t i = lo;
        if (na >= 2) slots[0] = VALUE_SLICE_GET(a, 0);
        else { if (lo >= hi) return RESULT_OK(KORB_NIL); slots[0] = LONG2FIX(lo); i = lo + 1; }
        for (; i < hi; i++) {
            slots[1] = slots[0]; slots[2] = LONG2FIX(i);
            RESULT r = korb_send_impl(c, slots + 3, op_mid, 0, 1, NULL, NULL, KORB_NIL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            slots[0] = r.value;
        }
        return RESULT_OK(slots[0]);
    }
    intptr_t i = lo;
    if (VALUE_SLICE_LEN(a) >= 1) {
        slots[0] = VALUE_SLICE_GET(a, 0);
    } else {
        if (lo >= hi) return RESULT_OK(KORB_NIL);
        slots[0] = LONG2FIX(lo); i = lo + 1;
    }
    for (; i < hi; i++) {
        VALUE argv[2] = { slots[0], LONG2FIX(i) };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    return RESULT_OK(slots[0]);
}

/* select (keep==true) / reject (keep==false) over an integer range */
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_filter(CTX *c, VALUE *slots, VALUE_REF self, NODE *block, VALUE *def_env, VALUE *captured_self, bool keep) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#select/reject without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (keep ? korb_m_ary_select : korb_m_ary_reject)(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(NULL, 0), block, def_env, captured_self);
    }
    VALUE_REF dst = SLOTS_PUSH(slots, UNWRAP(korb_ary_new(c, slots, 4)));
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value) == keep) CHECK(korb_ary_push_val(c, slots + 1, dst, LONG2FIX(i)));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_range_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, true); }
static RESULT korb_m_range_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { (void)a; return korb_range_filter(c, slots, self, block, def_env, captured_self, false); }

static RESULT korb_m_range_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Range#find without a block is not supported");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return korb_m_ary_find(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_TRUTHY(r.value)) return RESULT_OK(LONG2FIX(i));
    }
    return RESULT_OK(KORB_NIL);
}

/* any? (0) / all? (1) / none? (2) over an integer range, with block */
static RESULT korb_m_ary_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_range_quant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, int mode) {
    bool has_pat = VALUE_SLICE_LEN(a) >= 1;
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) {     /* non-integer range → via to_a */
        slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, VALUE_SLICE_MAKE(NULL, 0)));
        return (mode == 0 ? korb_m_ary_any : mode == 1 ? korb_m_ary_all : korb_m_ary_none)(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self);
    }
    for (intptr_t i = lo; i < hi; i++) {
        VALUE iv = LONG2FIX(i);
        bool t;
        if (has_pat) {
            t = korb_case_eq(c, VALUE_SLICE_GET(a, 0), iv);     /* pattern === element */
        } else if (block != NULL) {
            RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            t = KORB_TRUTHY(r.value);
        } else {
            t = KORB_TRUTHY(iv);                                /* int elements are always truthy */
        }
        if (mode == 0 && t) return RESULT_OK(KORB_TRUE);
        if (mode == 1 && !t) return RESULT_OK(KORB_FALSE);
        if (mode == 2 && t) return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(mode == 0 ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_range_any(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 0); }
static RESULT korb_m_range_all(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)  { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 1); }
static RESULT korb_m_range_none(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self) { return korb_range_quant(c, slots, self, a, block, def_env, captured_self, 2); }

static RESULT korb_range_step_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self, uint8_t is_pct) {
    uint8_t na = VALUE_SLICE_LEN(a) >= 1 ? 1 : 0;
    VALUE sv = na ? VALUE_SLICE_GET(a, 0) : LONG2FIX(1);
    if (block == NULL)                                /* → lazy ArithmeticSequence (recv = self range) */
        return korb_arithseq_new(c, slots, VALUE_REF_GET(self), sv, KORB_NIL, na, is_pct);
    if (UNLIKELY(!FIXNUM_P(sv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(sv));
    intptr_t st = FIX2LONG(sv);
    if (UNLIKELY(st <= 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "step can't be 0 or negative");
    intptr_t lo, hi;
    if (!korb_range_int_bounds(SELF_RANGE, &lo, &hi)) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't iterate");
    for (intptr_t i = lo; i < hi; i += st) {
        VALUE iv = LONG2FIX(i);
        RESULT r = korb_block_yield(c, slots, block, def_env, &iv, 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_range_step(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_range_step_impl(c, slots, self, a, block, def_env, cself, 0);
}
static RESULT korb_m_range_pct(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_range_step_impl(c, slots, self, a, block, def_env, cself, 1);
}

