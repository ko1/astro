/* koruby_precise — enumerator.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Enumerator (eager): values materialized at creation ----------------- */
/* Build from a values Array + a desc String (or nil).  vals/desc must be rooted
 * by the caller's slots region; we root-copy into the new object. */
static RESULT
korb_enum_new(CTX *c, VALUE *slots, VALUE vals, VALUE desc)
{
    slots[0] = vals; slots[1] = desc;                  /* root across alloc */
    KorbEnumerator *e = korb_alloc(c, slots + 2, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->values, slots[0]);
    ARO_STORE(c, e, (VALUE *)(uintptr_t)&e->desc,   slots[1]);
    return RESULT_OK((VALUE)e);
}
/* build the inspect desc "#<Enumerator: RECV:meth>" (no koruby alloc during print). */
static RESULT korb_enum_desc(CTX *c, VALUE *slots, VALUE recv, const char *meth) {
    char *buf = NULL; size_t sz = 0; FILE *ms = open_memstream(&buf, &sz);
    if (ms) { fputs("#<Enumerator: ", ms); korb_fprint_inspect(c, ms, recv); fprintf(ms, ":%s>", meth); fclose(ms); }
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_enum_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_ENUM->values); }
static RESULT korb_m_enum_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(VAL2ARY(SELF_ENUM->values)->len)); }
static RESULT korb_m_enum_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    VALUE d = SELF_ENUM->desc;
    if (KORB_STRING_P(d)) return RESULT_OK(d);
    return korb_str_new(c, slots, "#<Enumerator>", 13);
}
/* each: yield every materialized value; with no block, return self. */
static RESULT korb_m_enum_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[0] = v->items->data[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* map: collect block results over the materialized values; no block → self. */
static RESULT korb_m_enum_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(SELF_ENUM->values)->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[1] = v->items->data[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* with_index(off=0): yield (value, off+i).  With a block → mapped array;
 * without → a new Enumerator of [value, off+i] pairs. */
static RESULT korb_m_enum_with_index(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    intptr_t off = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) off = FIX2LONG(VALUE_SLICE_GET(a, 0));
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(SELF_ENUM->values)->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        slots[1] = v->items->data[i];
        if (block != NULL) {
            VALUE argv[2] = { slots[1], LONG2FIX(off + (intptr_t)i) };
            RESULT r = korb_block_yield(c, slots + 2, block, def_env, argv, 2, cself);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            CHECK(korb_ary_push_val(c, slots + 2, dst, r.value));
        } else {                                       /* build [value, idx] pair */
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), slots[1]));
            CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), LONG2FIX(off + (intptr_t)i)));
            CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));
        }
    }
    if (block == NULL) return korb_enum_new(c, slots + 1, VALUE_REF_GET(dst), KORB_NIL);
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* with_object(o): yield (value, o) for each; return o. */
static RESULT korb_m_enum_with_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL || VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    slots[0] = VALUE_SLICE_GET(a, 0);                  /* the memo object (rooted) */
    for (uint32_t i = 0; ; i++) {
        const KorbArray *v = VAL2ARY(SELF_ENUM->values);
        if (i >= v->len) break;
        VALUE argv[2] = { v->items->data[i], slots[0] };
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, argv, 2, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_enum_next(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "iteration reached an end");
    return RESULT_OK(v->items->data[e->cursor++]);
}
static RESULT korb_m_enum_peek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbEnumerator *e = SELF_ENUM;
    const KorbArray *v = VAL2ARY(e->values);
    if (e->cursor >= v->len) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "iteration reached an end");
    return RESULT_OK(v->items->data[e->cursor]);
}

