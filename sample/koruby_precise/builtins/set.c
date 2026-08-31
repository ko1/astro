/* koruby_precise — set.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Set (array-backed, unique by korb_value_eq) -------------------------- */
static bool korb_arr_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eql(korb_items_data(ar->items)[i], v)) return true;
    return false;
}
/* Set membership honouring compare_by_identity: an identity set compares members
 * by object identity (equal?), a normal set by eql? (korb_arr_has).  For immediate
 * values (Symbol/Integer/nil/true/false) the two coincide. */
static bool korb_set_member(const KorbSet *s, VALUE v) {
    const KorbArray *ar = VAL2ARY(s->elems);
    if (!s->by_identity) return korb_arr_has(ar, v);
    for (uint32_t i = 0; i < ar->len; i++) if (korb_items_data(ar->items)[i] == v) return true;
    return false;
}
/* Set membership dispatching #eql? for object elements (like CRuby's hash-backed
 * Set); GC-safe — elems/needle stay rooted in slots[0]/slots[1] and are re-read
 * after each dispatch.  Uses slots[0..4]. */
static RESULT korb_set_member_disp(CTX *c, VALUE *slots, VALUE elems, bool by_id, VALUE needle, bool *found) {
    slots[0] = elems; slots[1] = needle; *found = false;
    const uint32_t n = VAL2ARY(slots[0])->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE e = korb_items_data(VAL2ARY(slots[0])->items)[i];
        if (by_id) { if (e == slots[1]) { *found = true; return RESULT_OK(KORB_NIL); } continue; }
        if (KORB_OBJECT_P(e) || KORB_OBJECT_P(slots[1])) {          /* user #eql? → dispatch */
            slots[2] = e; slots[3] = slots[1];
            RESULT r = korb_send_impl(c, slots + 4, korb_intern(c->vm, "eql?", 4), 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            if (KORB_TRUTHY(r.value)) { *found = true; return RESULT_OK(KORB_NIL); }
        } else if (korb_value_eql(e, slots[1])) { *found = true; return RESULT_OK(KORB_NIL); }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_set_new(CTX *c, VALUE *slots, VALUE elems) {
    slots[0] = elems;
    KorbSet *s = korb_alloc(c, slots + 1, sizeof(KorbSet), KORB_OBJ_SET);
    ARO_STORE(c, s, (VALUE *)(uintptr_t)&s->elems, slots[0]);
    return RESULT_OK((VALUE)s);
}
/* build a deduped Set from an array of values (src rooted by caller). */
static RESULT korb_set_from_array(CTX *c, VALUE *slots, VALUE_REF src) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(VALUE_REF_GET(src))->len));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    const uint32_t sn = VAL2ARY(VALUE_REF_GET(src))->len;
    for (uint32_t i = 0; i < sn; i++) {
        const VALUE e = korb_items_data(VAL2ARY(VALUE_REF_GET(src))->items)[i];
        bool found; RESULT mr = korb_set_member_disp(c, slots + 1, VALUE_REF_GET(dst), false, e, &found);
        if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
        if (!found) CHECK(korb_ary_push_val(c, slots + 3, dst, slots[2]));   /* slots[2] = the rooted needle */
    }
    return korb_set_new(c, slots + 1, VALUE_REF_GET(dst));
}
static VALUE korb_set_elems_of(VALUE v) {
    if (KORB_SET_P(v)) return VAL2SET(v)->elems;
    if (KORB_ARRAY_P(v)) return v;
    return KORB_NIL;
}
static RESULT korb_m_set_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_SET->elems); }
static RESULT korb_m_set_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(LONG2FIX(VAL2ARY(SELF_SET->elems)->len)); }
static RESULT korb_m_set_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VAL2ARY(SELF_SET->elems)->len == 0 ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_set_self(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_set_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    bool found; RESULT r = korb_set_member_disp(c, slots, SELF_SET->elems, SELF_SET->by_identity, VALUE_SLICE_GET(a, 0), &found);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(found ? KORB_TRUE : KORB_FALSE);
}
/* compare_by_identity — switch the set to identity comparison (returns self). */
static RESULT korb_m_set_cbi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    VAL2SET(VALUE_REF_GET(self))->by_identity = 1;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_cbi_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_SET->by_identity ? KORB_TRUE : KORB_FALSE); }
/* The members of a Set-or-enumerable operand as a plain Array.  A "Set-like"
 * object (Enumerable, answers is_a?(Set)) is materialized with #to_a — CRuby's
 * Set methods accept those transparently.  Result at slots[0] (rooted). */
static RESULT korb_set_other_elems(CTX *c, VALUE *slots, VALUE o, VALUE *out) {
    const VALUE direct = korb_set_elems_of(o);
    if (direct != KORB_NIL) { *out = direct; return RESULT_OK(KORB_NIL); }
    slots[0] = o;                                       /* root across the respond_to?/to_a dispatch */
    if (KORB_OBJECT_P(slots[0]) && korb_responds_to_coerce(c, slots + 1, slots[0], korb_intern(c->vm, "each", 4))) {
        RESULT r = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_a", 4), 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (KORB_ARRAY_P(r.value)) { slots[0] = r.value; *out = slots[0]; return RESULT_OK(KORB_NIL); }
    }
    *out = KORB_NIL;
    return RESULT_OK(KORB_NIL);
}
/* disjoint?(o): no shared elements.  intersect?(o): some shared element. */
static RESULT korb_m_set_disjoint(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE oe; CHECK(korb_set_other_elems(c, slots, VALUE_SLICE_GET(a, 0), &oe));   /* Set/enumerable/set-like → elems array */
    if (oe == KORB_NIL) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be enumerable");
    slots[0] = oe;                                               /* root: SELF_SET re-read below */
    const KorbArray *const ot = VAL2ARY(slots[0]);
    const KorbArray *const me = VAL2ARY(SELF_SET->elems);
    for (uint32_t i = 0; i < me->len; i++)
        if (korb_arr_has(ot, korb_items_data(me->items)[i])) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_set_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const RESULT r = korb_m_set_disjoint(c, slots, self, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_TRUE ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_set_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    bool found; RESULT r = korb_set_member_disp(c, slots, SELF_SET->elems, SELF_SET->by_identity, VALUE_SLICE_GET(a, 0), &found);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!found) { slots[0] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1])); }   /* slots[1] = rooted needle */
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_add_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    bool found; RESULT r = korb_set_member_disp(c, slots, SELF_SET->elems, SELF_SET->by_identity, VALUE_SLICE_GET(a, 0), &found);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (found) return RESULT_OK(KORB_NIL);
    return korb_m_set_add(c, slots, self, a);
}
static RESULT korb_m_set_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbArray *ar = VAL2ARY(SELF_SET->elems);
    VALUE v = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ar->len; i++)
        if (korb_value_eq(korb_items_data(ar->items)[i], v)) {
            for (uint32_t j = i; j + 1 < ar->len; j++) ARO_STORE(c, ar->items, &korb_items_data(ar->items)[j], korb_items_data(ar->items)[j+1]);
            ARO_STORE(c, ar->items, &korb_items_data(ar->items)[--ar->len], KORB_NIL);
            break;
        }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Set#delete?(o) — delete o and return self if it was a member, else nil. */
static RESULT korb_m_set_delete_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbArray *ar = VAL2ARY(SELF_SET->elems);
    const VALUE v = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ar->len; i++)
        if (korb_value_eq(korb_items_data(ar->items)[i], v)) {
            for (uint32_t j = i; j + 1 < ar->len; j++) ARO_STORE(c, ar->items, &korb_items_data(ar->items)[j], korb_items_data(ar->items)[j+1]);
            ARO_STORE(c, ar->items, &korb_items_data(ar->items)[--ar->len], KORB_NIL);
            return RESULT_OK(VALUE_REF_GET(self));    /* was present → self */
        }
    return RESULT_OK(KORB_NIL);                        /* not present → nil */
}
static RESULT korb_m_set_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) REQUIRE_BLOCK("Set#each");
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = VAL2ARY(SELF_SET->elems);
        if (i >= ar->len) break;
        slots[0] = korb_items_data(ar->items)[i];
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* set ops: op 0=union 1=intersection 2=difference 3=xor */
static RESULT korb_set_binop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    VALUE ov = korb_set_elems_of(VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(ov == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be enumerable");
    slots[0] = ov;
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    const KorbArray *me = VAL2ARY(SELF_SET->elems);
    for (uint32_t i = 0; i < me->len; i++) {
        VALUE e = korb_items_data(VAL2ARY(SELF_SET->elems)->items)[i];
        bool ino = korb_arr_has(VAL2ARY(slots[0]), e);
        bool keep = op == 0 ? true : op == 1 ? ino : !ino;       /* union / inter / (diff|xor) */
        if (keep && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 2, dst, e));
    }
    if (op == 0 || op == 3) {
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            VALUE e = korb_items_data(VAL2ARY(slots[0])->items)[i];
            bool inme = korb_arr_has(VAL2ARY(SELF_SET->elems), e);
            bool keep = op == 0 ? true : !inme;
            if (keep && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 2, dst, e));
        }
    }
    RESULT nr = korb_set_new(c, slots + 2, VALUE_REF_GET(dst));
    /* CRuby: |, - and ^ keep the receiver's compare_by_identity flag; & does not. */
    if (nr.state == KORB_NORMAL && op != 1) VAL2SET(nr.value)->by_identity = SELF_SET->by_identity;
    return nr;
}
static RESULT korb_m_set_union(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_set_binop(c, slots, self, a, 0); }
static RESULT korb_m_set_inter(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_set_binop(c, slots, self, a, 1); }
static RESULT korb_m_set_diff(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_set_binop(c, slots, self, a, 2); }
static RESULT korb_m_set_xor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_set_binop(c, slots, self, a, 3); }
static RESULT korb_m_set_merge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    for (uint32_t k = 0; k < VALUE_SLICE_LEN(a); k++) {       /* merge(*enums) — each Set or Array */
        VALUE ov = korb_set_elems_of(VALUE_SLICE_GET(a, k));
        if (UNLIKELY(ov == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be enumerable");
        slots[0] = ov;
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            VALUE e = korb_items_data(VAL2ARY(slots[0])->items)[i];
            if (!korb_set_member(SELF_SET, e)) { slots[1] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), e)); }
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Set#join(sep="") — delegate to the member Array's join. */
static RESULT korb_m_set_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    slots[0] = SELF_SET->elems;                            /* receiver = member Array (slots[0]) */
    for (uint32_t i = 0; i < n; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);   /* args at slots[1..n] */
    return korb_send_impl(c, slots + 1 + n, korb_intern(c->vm, "join", 4), 0, n, NULL, NULL, NULL);   /* scratch base is above recv+args */
}
static RESULT korb_set_rel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int rel) {
    if (UNLIKELY(!KORB_SET_P(VALUE_SLICE_GET(a, 0))))   /* subset?/superset?/</<= require a Set (not just set-like) */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be a set");
    VALUE ov = korb_set_elems_of(VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(ov == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be a set");
    const KorbArray *me = VAL2ARY(SELF_SET->elems), *ot = VAL2ARY(ov);
    bool sub = true; for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, korb_items_data(me->items)[i])) { sub = false; break; }
    bool sup = true; for (uint32_t i = 0; i < ot->len; i++) if (!korb_arr_has(me, korb_items_data(ot->items)[i])) { sup = false; break; }
    bool t = rel == 0 ? sub : rel == 1 ? sup : rel == 2 ? (sub && me->len < ot->len) : (sup && me->len > ot->len);
    return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
}
/* Set#<=> — 0 equal, -1 proper subset, 1 proper superset, nil otherwise / non-Set. */
static RESULT korb_m_set_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots;
    const VALUE ov = VALUE_SLICE_GET(a, 0);
    if (!KORB_SET_P(ov)) return RESULT_OK(KORB_NIL);
    const KorbArray *me = VAL2ARY(SELF_SET->elems), *ot = VAL2ARY(VAL2SET(ov)->elems);
    bool sub = true; for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, korb_items_data(me->items)[i])) { sub = false; break; }
    bool sup = true; for (uint32_t i = 0; i < ot->len; i++) if (!korb_arr_has(me, korb_items_data(ot->items)[i])) { sup = false; break; }
    if (sub && sup) return RESULT_OK(LONG2FIX(0));
    if (sub) return RESULT_OK(LONG2FIX(-1));
    if (sup) return RESULT_OK(LONG2FIX(1));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_set_subset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_set_rel(c, slots, self, a, 0); }
static RESULT korb_m_set_superset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_set_rel(c, slots, self, a, 1); }
static RESULT korb_m_set_psubset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_set_rel(c, slots, self, a, 2); }
static RESULT korb_m_set_psuperset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a){ return korb_set_rel(c, slots, self, a, 3); }
static RESULT korb_m_set_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_SET_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbArray *me = VAL2ARY(SELF_SET->elems), *ot = VAL2ARY(VAL2SET(o)->elems);
    if (me->len != ot->len) return RESULT_OK(KORB_FALSE);
    for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, korb_items_data(me->items)[i])) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
/* Set Enumerable methods delegate to the elements array's Array method. */
static RESULT korb_m_ary_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
static RESULT korb_m_ary_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);
KORB_SET_DELEG_BLK(korb_m_set_map, korb_m_ary_map)
KORB_SET_DELEG_BLK(korb_m_set_select, korb_m_ary_select)
KORB_SET_DELEG_BLK(korb_m_set_reject, korb_m_ary_reject)
KORB_SET_DELEG_BLK(korb_m_set_find, korb_m_ary_find)
KORB_SET_DELEG_BLK(korb_m_set_sort, korb_m_ary_sort)
KORB_SET_DELEG(korb_m_set_sum, korb_m_ary_sum)
KORB_SET_DELEG_BLK(korb_m_set_minmax, korb_m_ary_minmax)
static RESULT korb_hash_first_n(CTX *c, VALUE *slots, VALUE_REF self, uint32_t limit);
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_ary_to_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return korb_set_from_array(c, slots, self);
    slots[0] = UNWRAP(korb_ary_new(c, slots, VAL2ARY(VALUE_REF_GET(self))->len));   /* map each through block first */
    VALUE_REF mapped = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = VAL2ARY(VALUE_REF_GET(self));
        if (i >= ar->len) break;
        slots[1] = korb_items_data(ar->items)[i];
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        CHECK(korb_ary_push_val(c, slots + 2, mapped, r.value));
    }
    return korb_set_from_array(c, slots + 1, mapped);
}
static RESULT korb_m_hash_to_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_hash_first_n(c, slots, self, 0xFFFFFFFFu));   /* [k,v] pairs */
    return korb_set_from_array(c, slots + 1, VALUE_REF_AT(&slots[0]));
}
static RESULT korb_m_range_to_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(VAL2RANGE(VALUE_REF_GET(self))->rend == KORB_NIL))    /* to_a would say "to an array" */
        return korb_raise(c, slots, KORB_E_RANGE, 0, "cannot convert endless range to a set");
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));          /* the range's elements as an Array */
    if (block == NULL) return korb_set_from_array(c, slots + 1, VALUE_REF_AT(&slots[0]));
    return korb_m_ary_to_set(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);   /* map each element through the block */
}

/* Proc#to_s / #inspect — the default object form, but CRuby tags it BINARY
 * (the text embeds a source path, which has no encoding of its own). */
static RESULT korb_m_obj_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_proc_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const RESULT r = korb_m_obj_to_s(c, slots, self, a);
    if (LIKELY(r.state == KORB_NORMAL) && KORB_STRING_P(r.value)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}
static RESULT korb_m_obj_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_to_s_s(c, slots, ms, VALUE_REF_GET(self));   /* containers dispatch element #inspect (may GC) */
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_obj_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v0 = VALUE_REF_GET(self);
    /* A plain object with instance variables inspects as "#<Class:0x.. @a=1, @b=2>"
     * (the default #inspect, distinct from #to_s which omits the ivars).  Struct/
     * Data render their members via the shared inspect formatter below. */
    if (KORB_OBJECT_P(v0) && VAL2OBJ(v0)->klass != KORB_NIL &&
        !KORB_ARRAY_P(VAL2CLASS(VAL2OBJ(v0)->klass)->members) &&
        c->vm->shapes[VAL2OBJ(v0)->shape_id].ivar_count > 0) {
        char *buf = NULL; size_t sz = 0;
        FILE *ms = open_memstream(&buf, &sz);
        if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
        const uint32_t sid0 = VAL2OBJ(v0)->shape_id;
        const uint32_t nvv = c->vm->shapes[sid0].ivar_count;
        const uint32_t n = nvv < 64 ? nvv : 64;
        uint32_t syms[64];
        for (uint32_t sid = sid0; sid; ) { const struct korb_shape *s = &c->vm->shapes[sid]; if (s->ivar_count >= 1 && s->ivar_count <= n) syms[s->ivar_count - 1] = s->edge_sym; sid = s->parent; }
        fputs("#<", ms);
        if (!korb_fprint_class_qname(c, ms, VAL2OBJ(v0)->klass)) fputs("Object", ms);
        fprintf(ms, ":0x%016lx", (unsigned long)(uintptr_t)v0);
        slots[0] = v0;                                       /* root the object across nested #inspect dispatch */
        const bool cyc = (((const AroObjectHeader *)(uintptr_t)v0)->flags & KORB_FL_JOIN_VISITING) != 0;
        if (cyc) {
            fputs(" ...", ms);                               /* already being inspected → recursion marker */
        } else {
            ((AroObjectHeader *)(uintptr_t)v0)->flags |= KORB_FL_JOIN_VISITING;
            for (uint32_t i = 0; i < n; i++) {
                fputs(i == 0 ? " " : ", ", ms);
                fputs(korb_sym_name(c->vm, syms[i]), ms); fputc('=', ms);
                const KorbObject *const oo = VAL2OBJ(slots[0]);  /* re-read: dispatch may move it */
                korb_fprint_inspect_d(c, slots + 1, ms, oo->ivars ? korb_items_data(oo->ivars)[i] : KORB_NIL, 1);
            }
            ((AroObjectHeader *)(uintptr_t)slots[0])->flags &= ~KORB_FL_JOIN_VISITING;
        }
        fputc('>', ms);
        fclose(ms);
        RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
        free(buf);
        return r;
    }
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_inspect_s(c, slots, ms, VALUE_REF_GET(self));   /* containers dispatch element #inspect */
    fclose(ms);
    /* CRuby tags an all-7-bit #inspect result US-ASCII (`[].inspect`, `{}.inspect`) */
    bool ascii = true;
    for (size_t i = 0; i < sz && ascii; i++) if ((unsigned char)buf[i] >= 0x80) ascii = false;
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL) && ascii && !KORB_STRING_P(VALUE_REF_GET(self)))
        KORB_STR_ENC_SET(r.value, KORB_ENC_USASCII);
    return r;
}
static RESULT korb_m_obj_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(korb_class_obj_of(c, VALUE_REF_GET(self)));
}
/* Object#object_id / __id__.  Immediates match CRuby exactly; heap objects use an
 * address-derived id (stable within a GC epoch — moving GC has no per-object id slot). */
static RESULT korb_m_obj_object_id(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const VALUE v = VALUE_REF_GET(self);
    if (FIXNUM_P(v))     return RESULT_OK(LONG2FIX(2 * FIX2LONG(v) + 1));
    if (v == KORB_NIL)   return RESULT_OK(LONG2FIX(4));
    if (v == KORB_FALSE) return RESULT_OK(LONG2FIX(0));
    if (v == KORB_TRUE)  return RESULT_OK(LONG2FIX(20));
    if (SYMBOL_P(v))     return RESULT_OK(LONG2FIX((korb_sword_t)(((uintptr_t)SYM2ID(v) << 8) | 0x1c)));   /* consistent per symbol */
    return RESULT_OK(LONG2FIX((korb_sword_t)((uintptr_t)v >> 2)));
}
static RESULT korb_m_obj_is_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE target = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(target))) return korb_raise(c, slots, KORB_E_TYPE, 0, "class or module required");
    if (target == korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT])) return RESULT_OK(KORB_TRUE);
    VALUE sv = VALUE_REF_GET(self);
    /* start from the RAW override (singleton included) so extended modules count */
    VALUE cls = (AROH_IS_GC_OBJECT(sv) && (((const AroObjectHeader *)(uintptr_t)sv)->flags & KORB_FL_HAS_KLASS))
                  ? korb_klass_override_get(c->vm, sv)
                  : korb_class_obj_of(c, sv);
    while (KORB_CLASS_P(cls)) {
        if (cls == target) return RESULT_OK(KORB_TRUE);
        VALUE pre = VAL2CLASS(cls)->prepended;           /* prepended modules count */
        if (pre != KORB_NIL) {
            const KorbArray *pa = VAL2ARY(pre);
            for (uint32_t j = 0; j < pa->len; j++) if (korb_items_data(pa->items)[j] == target) return RESULT_OK(KORB_TRUE);
        }
        VALUE inc = VAL2CLASS(cls)->included;            /* included/extended modules count */
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (korb_items_data(ia->items)[j] == target) return RESULT_OK(KORB_TRUE);
        }
        cls = VAL2CLASS(cls)->superclass;
    }
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_obj_respond_to(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    uint32_t mid;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_arg_to_mid(c, slots, VALUE_SLICE_GET(a, 0), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    VALUE sv = VALUE_REF_GET(self);
    const VALUE incv = (VALUE_SLICE_LEN(a) >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_FALSE;
    const bool include_priv = (incv != KORB_NIL && incv != KORB_FALSE);
    if (korb_responds_to(c, sv, mid)) {
        /* private/protected methods are not "responded to" unless include_all. */
        const VALUE dcls = korb_dispatch_class(c, sv);
        VALUE mdef = KORB_NIL;
        const struct korb_method *const me = KORB_CLASS_P(dcls) ? korb_class_find_method(dcls, mid, &mdef) : NULL;
        if (me != NULL && me->visibility != 0 && !include_priv) return RESULT_OK(KORB_FALSE);
        return RESULT_OK(KORB_TRUE);
    }
    /* Kernel-private builtins (puts/print/require/... live in the global table,
     * not a class method table) are private methods of every object. */
    if (include_priv) {
        const struct korb_method *const gm = korb_method_lookup(vm, mid);
        if (gm != NULL && gm->kind == KORB_METHOD_BUILTIN) return RESULT_OK(KORB_TRUE);
    }
    /* respond_to_missing?(name, include_private) fallback (pairs with method_missing). */
    const uint32_t rtm = korb_intern(vm, "respond_to_missing?", 19);
    const VALUE dcls = korb_dispatch_class(c, sv);
    VALUE rtm_def = KORB_NIL;
    if (KORB_CLASS_P(dcls) && korb_class_find_method(dcls, rtm, &rtm_def)) {
        slots[0] = sv;
        slots[1] = ID2SYM(mid);
        slots[2] = (VALUE_SLICE_LEN(a) >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_FALSE;
        const RESULT r = korb_send_impl(c, slots + 3, rtm, 0, 2, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_TRUE : KORB_FALSE);   /* #respond_to? answers a boolean */
    }
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_obj_instance_of(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE target = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(target))) return korb_raise(c, slots, KORB_E_TYPE, 0, "class or module required");
    return RESULT_OK(korb_class_obj_of(c, VALUE_REF_GET(self)) == target ? KORB_TRUE : KORB_FALSE);
}
/* Get-or-create obj's singleton class (a per-instance class whose superclass is
 * obj's current class, is_singleton=1), recorded in the override table.  obj must
 * be a heap object.  Returns the singleton (rooted via the table). */
RESULT korb_obj_singleton(CTX *c, VALUE *slots, VALUE obj) {
    struct korb_vm *const vm = c->vm;
    if (UNLIKELY(!AROH_IS_GC_OBJECT(obj))) {
        /* nil/true/false: the singleton "is" their class (NilClass/TrueClass/FalseClass);
         * other immediates (Integer/Float/Symbol) can't have one (CRuby raises). */
        if (obj == KORB_NIL || obj == KORB_TRUE || obj == KORB_FALSE)
            return RESULT_OK(korb_class_obj_of(c, obj));
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't define singleton");
    }
    if (((const AroObjectHeader *)(uintptr_t)obj)->flags & KORB_FL_HAS_KLASS) {
        VALUE ov = korb_klass_override_get(vm, obj);
        if (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) return RESULT_OK(ov);   /* reuse */
    }
    slots[0] = obj;                                                              /* root self across class alloc */
    VALUE cur;
    if (KORB_CLASS_P(obj) && !VAL2CLASS(obj)->is_singleton) {
        /* metaclass hierarchy: a class's singleton inherits from its PARENT class's
         * singleton (built lazily + memoized), so subclasses inherit class methods. */
        const VALUE parent = VAL2CLASS(obj)->superclass;
        if (KORB_CLASS_P(parent)) {
            cur = UNWRAP(korb_obj_singleton(c, slots + 2, parent));              /* parent metaclass (recursion; GC may move obj) */
            obj = slots[0];                                                      /* re-read */
        } else {
            cur = korb_class_obj_of(c, slots[0]);                               /* top of the chain → Class */
        }
    } else if (((const AroObjectHeader *)(uintptr_t)obj)->flags & KORB_FL_HAS_KLASS) {
        cur = korb_klass_override_get(vm, obj);
    } else {
        cur = korb_class_obj_of(c, slots[0]);
    }
    slots[1] = cur;
    VALUE sing = UNWRAP(korb_class_new(c, slots + 2, 0, slots[1]));               /* anonymous, super=cur */
    VAL2CLASS(sing)->is_singleton = 1;
    slots[2] = sing;
    if (((const AroObjectHeader *)(uintptr_t)slots[0])->flags & KORB_FL_FROZEN)   /* a frozen object's singleton is frozen */
        ((AroObjectHeader *)(uintptr_t)slots[2])->flags |= KORB_FL_FROZEN;
    korb_klass_override_set(c, slots[0], slots[2]);                               /* obj/sing rooted, no GC in set */
    return RESULT_OK(slots[2]);
}
/* Object#singleton_class — the object's (lazily-created) singleton class. */
static RESULT korb_m_obj_singleton_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE sv = VALUE_REF_GET(self);
    if (sv == KORB_NIL || sv == KORB_TRUE || sv == KORB_FALSE)   /* their class IS their singleton */
        return RESULT_OK(korb_class_obj_of(c, sv));
    if (UNLIKELY(!AROH_IS_GC_OBJECT(sv)))                        /* Integer / Symbol / Float */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't define singleton");
    return korb_obj_singleton(c, slots, sv);
}
/* Object#initialize_copy / initialize_clone (private) — the default hook: type-
 * check + return self (the actual ivar copy is done by dup/clone). */
static RESULT korb_m_obj_initialize_copy(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE sv = VALUE_REF_GET(self), o = VALUE_SLICE_GET(a, 0);
    if (sv == o) return RESULT_OK(sv);
    if (UNLIKELY(!AROH_IS_GC_OBJECT(sv))) return korb_raise_frozen(c, slots, sv);   /* immediates (Integer/Symbol/...) are frozen */
    KORB_CHECK_FROZEN(c, slots, sv);
    if (korb_class_of(sv) != korb_class_of(o) ||
        (KORB_OBJECT_P(sv) && VAL2OBJ(sv)->klass != VAL2OBJ(o)->klass))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "initialize_copy should take same class object");
    return RESULT_OK(sv);
}
/* Object#define_singleton_method(name, &block|proc) — define_method on self's
 * singleton class. */
static RESULT korb_m_obj_define_singleton_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    RESULT sc = korb_m_obj_singleton_class(c, slots, self, VALUE_SLICE_MAKE(NULL, 0));
    if (UNLIKELY(sc.state != KORB_NORMAL)) return sc;
    slots[0] = sc.value;                                  /* root the singleton class across define_method */
    return korb_m_define_method(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, cself);
}
/* (singleton class)#attached_object — reverse the obj→singleton table. */
static RESULT korb_m_class_attached_object(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; struct korb_vm *const vm = c->vm; VALUE cls = VALUE_REF_GET(self);
    if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->is_singleton)
        for (uint32_t i = 0; i < vm->sklass_cnt; i++)
            if (vm->sklass_cls[i] == cls) return RESULT_OK(vm->sklass_obj[i]);
    return korb_raise(c, slots, KORB_E_TYPE, 0, "'attached_object' called on a non-singleton class");
}
/* Class#subclasses — direct (immediate) non-singleton subclasses, order unspecified. */
static RESULT korb_m_class_subclasses(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    const VALUE list = KORB_CLASS_P(VALUE_REF_GET(self)) ? VAL2CLASS(VALUE_REF_GET(self))->subclasses : KORB_NIL;
    if (list != KORB_NIL) {
        slots[1] = list;                                   /* root the source list */
        for (uint32_t i = 0; i < VAL2ARY(slots[1])->len; i++) {
            const VALUE sub = korb_items_data(VAL2ARY(slots[1])->items)[i];
            if (KORB_CLASS_P(sub) && !VAL2CLASS(sub)->is_singleton)
                CHECK(korb_ary_push_val(c, slots + 2, dst, sub));
        }
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Can this value take a singleton class?  nil/true/false borrow their own class;
 * numerics and Symbols cannot (CRuby raises), heap or not. */
static bool korb_singleton_able(VALUE v) {
    if (!AROH_IS_GC_OBJECT(v)) return v == KORB_NIL || v == KORB_TRUE || v == KORB_FALSE;
    return !(KORB_BIGNUM_P(v) || KORB_FLOAT_P(v) || KORB_RATIONAL_P(v) || KORB_COMPLEX_P(v));
}

/* Object#extend(*mods) — mix the modules into the object's singleton class. */
static RESULT korb_m_obj_extend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_REF_GET(self);
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    if (UNLIKELY(!AROH_IS_GC_OBJECT(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't define singleton");   /* immediates */
    slots[0] = sv;                                                               /* root self */
    slots[1] = UNWRAP(korb_obj_singleton(c, slots + 2, sv));                      /* singleton (rooted) */
    const uint32_t extended = korb_intern(c->vm, "extended", 8);
    const uint32_t extend_object = korb_intern(c->vm, "extend_object", 13);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {                           /* include each module */
        slots[2] = VALUE_SLICE_GET(a, i);
        /* CRuby routes every extend through the (private, overridable)
         * Module#extend_object; the prelude default does the singleton include. */
        if (LIKELY(korb_responds_to(c, slots[2], extend_object))) {
            slots[3] = slots[2];                                                  /* module = receiver */
            slots[4] = slots[0];                                                  /* the object = arg */
            RESULT er = korb_send_impl(c, slots + 5, extend_object, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(er.state != KORB_NORMAL)) return er;
        } else {
            CHECK(korb_do_include(c, slots + 3, slots[1], VALUE_SLICE_MAKE(&slots[2], 1)));
        }
        if (UNLIKELY(korb_responds_to(c, slots[2], extended))) {                  /* fire Module#extended(obj) hook */
            slots[3] = slots[2];                                                  /* module = receiver (sp[-2]) */
            slots[4] = slots[0];                                                  /* extended object = arg (sp[-1]) */
            RESULT hr = korb_send_impl(c, slots + 5, extended, 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        }
    }
    return RESULT_OK(slots[0]);
}
/* Module#private/public/protected/module_function — koruby doesn't enforce
 * visibility, so these are no-ops returning the arg (for `private :foo` /
 * `private def foo`) or nil (bare `private`). */
/* Module#deprecate_constant(*names) — mark each so a read warns. */
static RESULT korb_m_deprecate_constant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE owner = VALUE_REF_GET(self);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        uint32_t sym;
        { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, i), &sym); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
        if (UNLIKELY(korb_const_index_owned(c->vm, sym, owner) == UINT32_MAX &&
                     !korb_autoload_registered_p(c, owner, sym) &&
                     korb_const_in_ancestry(c->vm, owner, sym) == UINT32_MAX)) {
            char qn[256]; korb_class_desc_into(c, owner, qn, sizeof qn);
            return korb_raise(c, slots, KORB_E_NAME, 0, "constant %s::%s not defined", qn, korb_sym_name(c->vm, sym));
        }
        korb_const_set_deprecated(c, owner, sym);
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* private/protected/public: with no args set the class body's default visibility;
 * with args set those named methods' visibility.  vis: 1 priv / 2 prot / 0 pub. */
/* One name (Symbol/String) → set its visibility on self; NameError if undefined. */
static RESULT korb_set_visibility1(CTX *c, VALUE *slots, VALUE selfv, KorbClass *k, VALUE arg, uint8_t vis) {
    uint32_t mid;
    if (SYMBOL_P(arg)) mid = SYM2ID(arg);
    else if (KORB_STRING_P(arg)) { const KorbString *s = VAL2STR(arg); mid = korb_intern(c->vm, korb_strbuf_data(s->buf), s->len); }
    else return korb_raise_not_sym(c, slots, arg);
    for (uint32_t j = 0; j < k->method_cnt; j++)
        if (k->methods[j]->mid == mid) { k->methods[j]->visibility = vis; return RESULT_OK(KORB_NIL); }
    /* inherited/included method: CRuby adds a visibility-override entry on this
     * class — copy the ancestor definition into a local slot with the new
     * visibility, keeping the original owner so `super` still resolves above it. */
    VALUE adef = KORB_NIL;
    const struct korb_method *src = korb_class_find_method(selfv, mid, &adef);
    if (src == NULL) {
        /* a Kernel-level method (they all live on Kernel after boot), an Object
         * instance method, or a top-level def in the global function table:
         * `public :puts` inside a fresh Module has to copy from one of those,
         * since a Module has no ancestors of its own. */
        static const char *const fallbacks[] = { "Kernel", "Object" };
        for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0] && src == NULL; i++) {
            const VALUE m = korb_const_get(c->vm, korb_intern(c->vm, fallbacks[i], (uint32_t)strlen(fallbacks[i])));
            if (KORB_CLASS_P(m)) src = korb_class_find_method(m, mid, &adef);
        }
        if (src == NULL) src = korb_method_lookup(c->vm, mid);
    }
    if (src == NULL) {
        /* send/__send__/public_send/new/allocate are dispatch special-cases with
         * no table entry, and a singleton class inherits Class's own specials
         * (`class << self; private :new; end`) — no-op those.  Everything else
         * really is undefined here (CRuby: NameError). */
        const char *const nm = korb_sym_name(c->vm, mid);
        if (!strcmp(nm, "send") || !strcmp(nm, "__send__") || !strcmp(nm, "public_send")) return RESULT_OK(KORB_NIL);
        if (k->is_singleton && (!strcmp(nm, "new") || !strcmp(nm, "allocate"))) return RESULT_OK(KORB_NIL);
        char cnm[256]; korb_class_desc_into(c, selfv, cnm, sizeof cnm);
        RESULT ne = korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for %s '%s'",
                               nm, k->is_module ? "module" : "class", cnm);
        if (LIKELY(KORB_EXC_P(ne.value))) {                    /* NameError#name */
            slots[0] = ne.value;
            korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_intern(c->vm, "@__name", 7)), ID2SYM(mid));
            ne.value = slots[0];
        }
        return ne;
    }
    if (src->visibility == vis) return RESULT_OK(KORB_NIL);     /* same visibility → CRuby does not clone */
    struct korb_method *dst = korb_class_method_slot(k, mid);   /* libc alloc, no GC */
    const struct korb_method tmp = *src;                        /* snapshot (slot array may have grown) */
    *dst = tmp; dst->mid = mid; dst->visibility = vis;          /* keep tmp.owner for super */
    slots[0] = selfv;                                           /* the hook is Ruby code: root the class */
    return korb_fire_method_added(c, slots + 1, slots[0], mid); /* cloning an ancestor method IS a definition */
}
static RESULT korb_set_visibility(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, uint8_t vis) {
    const VALUE selfv = VALUE_REF_GET(self);
    const uint32_t argc = VALUE_SLICE_LEN(a);
    if (argc == 0) {                                  /* bare `private` → set the body's default */
        if (KORB_CLASS_P(selfv)) VAL2CLASS(selfv)->cur_visibility = vis;
        return RESULT_OK(KORB_NIL);
    }
    /* `private [:a, :b]` treats a lone Array argument as the list of names. */
    const bool array_arg = (argc == 1 && KORB_ARRAY_P(VALUE_SLICE_GET(a, 0)));
    if (KORB_CLASS_P(selfv)) {                         /* top-level / non-class: best-effort no-op on the names */
        KorbClass *const k = VAL2CLASS(selfv);
        if (array_arg) {
            slots[0] = VALUE_SLICE_GET(a, 0);         /* root the name array across NameError alloc */
            for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++)
                CHECK(korb_set_visibility1(c, slots + 1, selfv, k, korb_items_data(VAL2ARY(slots[0])->items)[i], vis));
        } else {
            for (uint32_t i = 0; i < argc; i++)
                CHECK(korb_set_visibility1(c, slots, selfv, k, VALUE_SLICE_GET(a, i), vis));
        }
        c->vm->method_serial++;
    }
    /* Return value (Ruby 3.0+): the Array arg / lone arg verbatim, else an Array of the args. */
    if (array_arg || argc == 1) return RESULT_OK(VALUE_SLICE_GET(a, 0));
    slots[0] = UNWRAP(korb_ary_new(c, slots, argc));
    for (uint32_t i = 0; i < argc; i++) CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&slots[0]), VALUE_SLICE_GET(a, i)));
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_private(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_set_visibility(c, slots, self, a, 1); }
static RESULT korb_m_protected(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_set_visibility(c, slots, self, a, 2); }
static RESULT korb_m_public(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_set_visibility(c, slots, self, a, 0); }
/* Module#module_function([name...]).  No args: switch the body's default so
 * subsequent defs become module functions (visibility mode 3, honoured by the
 * def path).  With names: for each, copy the instance method to the module's
 * singleton (public, so Mod.name works) and make the instance method private —
 * an independent copy, not a redirect.  Returns the name (1 arg) / names array. */
static RESULT korb_m_module_function(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE mod = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(mod) || !VAL2CLASS(mod)->is_module))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "module_function must be called for modules");
    const uint32_t argc = VALUE_SLICE_LEN(a);
    if (argc == 0) {
        slots[0] = mod;
        (void)UNWRAP(korb_obj_singleton(c, slots + 1, slots[0]));   /* create the singleton now so the (slots-less) def path can copy into it without allocating */
        VAL2CLASS(slots[0])->cur_visibility = 3;
        return RESULT_OK(KORB_NIL);
    }
    slots[0] = mod;                                            /* root module across singleton alloc */
    slots[1] = UNWRAP(korb_obj_singleton(c, slots + 2, slots[0]));   /* module's singleton (rooted) */
    for (uint32_t i = 0; i < argc; i++) {
        uint32_t mid = 0;
        { RESULT mr = korb_arg_to_mid(c, slots + 3, VALUE_SLICE_GET(a, i), &mid); if (UNLIKELY(mr.state != KORB_NORMAL)) return mr; }   /* #to_str coercion; slots[0]/[1] preserved */
        VALUE mdef = KORB_NIL;
        const struct korb_method *src = korb_class_find_method(slots[0], mid, &mdef);
        if (src == NULL)                                      /* a Kernel-private builtin (puts/require/…)
                                                               * lives in the global function table */
            src = korb_method_lookup(c->vm, mid);
        if (UNLIKELY(src == NULL))
            return korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for module '%s'",
                              korb_sym_name(c->vm, mid), korb_type_name(slots[0]));
        const struct korb_method tmp = *src;                  /* snapshot before any slot-array grow */
        struct korb_method *sm = korb_class_method_slot(VAL2CLASS(slots[1]), mid);   /* singleton = public copy */
        *sm = tmp; sm->mid = mid; sm->owner = slots[1]; sm->visibility = 0;
        struct korb_method *im = korb_class_method_slot(VAL2CLASS(slots[0]), mid);   /* module instance method = private copy */
        *im = tmp; im->mid = mid; im->owner = slots[0]; im->visibility = 1;
        c->vm->method_serial++;                               /* the hook below is Ruby code */
        CHECK(korb_fire_method_added(c, slots + 3, slots[1], mid));   /* the singleton copy fires #singleton_method_added */
    }
    c->vm->method_serial++;
    if (argc == 1) return RESULT_OK(VALUE_SLICE_GET(a, 0));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, argc));
    for (uint32_t i = 0; i < argc; i++)
        CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[2]), VALUE_SLICE_GET(a, i)));
    return RESULT_OK(slots[2]);
}
/* private_class_method / public_class_method :sym... — set the visibility of the
 * named singleton (class) methods, which live on self's singleton class. */
static RESULT korb_set_class_visibility(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, uint8_t vis) {
    const VALUE selfv = VALUE_REF_GET(self);
    if (!KORB_CLASS_P(selfv)) return RESULT_OK(selfv);            /* CRuby returns self */
    slots[0] = UNWRAP(korb_obj_singleton(c, slots + 1, selfv));   /* self's OWN singleton (created if absent → copy-down never touches a parent's) */
    if (!KORB_CLASS_P(slots[0])) return RESULT_OK(VALUE_REF_GET(self));
    /* a lone Array argument is the list of names (like private/public) */
    const bool array_arg = VALUE_SLICE_LEN(a) == 1 && KORB_ARRAY_P(VALUE_SLICE_GET(a, 0));
    slots[1] = array_arg ? VALUE_SLICE_GET(a, 0) : KORB_NIL;      /* root the name array across the loop's allocs */
    const uint32_t argc = array_arg ? VAL2ARY(slots[1])->len : VALUE_SLICE_LEN(a);
    for (uint32_t i = 0; i < argc; i++) {
        const VALUE arg = array_arg ? korb_items_data(VAL2ARY(slots[1])->items)[i] : VALUE_SLICE_GET(a, i);
        uint32_t mid;
        if (SYMBOL_P(arg)) mid = SYM2ID(arg);
        else if (KORB_STRING_P(arg)) { const KorbString *s = VAL2STR(arg); mid = korb_intern(c->vm, korb_strbuf_data(s->buf), s->len); }
        else return korb_raise_not_sym(c, slots + 2, arg);
        KorbClass *const k = VAL2CLASS(slots[0]);          /* re-read (a slot-array grow below may move nothing, but be safe) */
        bool set = false;
        for (uint32_t j = 0; j < k->method_cnt; j++)
            if (k->methods[j]->mid == mid) { k->methods[j]->visibility = vis; set = true; break; }
        if (set) continue;
        /* inherited class method (defined on a superclass' singleton) → copy it
         * down onto self's singleton with the requested visibility (CRuby). */
        const struct korb_method *src = korb_class_find_method(slots[0], mid, NULL);
        if (src != NULL) {
            const struct korb_method tmp = *src;           /* snapshot: the slot-array grow may dangle src */
            struct korb_method *dst = korb_class_method_slot(VAL2CLASS(slots[0]), mid);
            *dst = tmp; dst->mid = mid; dst->owner = slots[0]; dst->visibility = vis;
            continue;
        }
        /* .new / .allocate は method entry でなく korb_send_impl の特例 dispatch —
         * 可視性 mark 先が無い (Singleton module 等の private_class_method :new,
         * :allocate はこれ)。enforcement は失うが no-op で通す。 */
        { const char *const nm = korb_sym_name(c->vm, mid);
          if (nm && (strcmp(nm, "new") == 0 || strcmp(nm, "allocate") == 0)) continue; }
        return korb_raise(c, slots + 2, KORB_E_NAME, 0, "undefined method '%s' for class '%s'",   /* not a class method → NameError */
                          korb_sym_name(c->vm, mid), korb_type_name(VALUE_REF_GET(self)));
    }
    c->vm->method_serial++;                                /* invalidate call caches */
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_private_class_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_set_class_visibility(c, slots, self, a, 1); }
static RESULT korb_m_public_class_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { return korb_set_class_visibility(c, slots, self, a, 0); }
/* Kernel#throw(tag[, value]) — unwind (past rescue) to the matching catch.  With
 * no enclosing catch for the tag, raises a (rescuable) UncaughtThrowError. */
static RESULT korb_m_throw(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    const VALUE tag = VALUE_SLICE_GET(a, 0);
    bool active = false;
    for (uint32_t i = c->catch_n; i-- > 0; ) if (c->catch_tags[i] == tag) { active = true; break; }
    if (!active)
        return korb_raise(c, slots, KORB_E_UNCAUGHT_THROW, 0, "uncaught throw %s", korb_type_name(tag));
    c->throw_tag = tag;
    const VALUE val = (VALUE_SLICE_LEN(a) >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    return (RESULT){ val, KORB_THROW };
}
/* Kernel#catch([tag]) { |tag| ... } — a throw with the matching (identity) tag
 * returns its value here.  With no tag a fresh Object is used as the tag. */
static RESULT korb_m_catch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)self;
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    if (VALUE_SLICE_LEN(a) >= 1) slots[0] = VALUE_SLICE_GET(a, 0);
    else slots[0] = UNWRAP(korb_obj_new(c, slots, korb_builtin_class_obj(c->vm, KORB_C_OBJECT)));
    if (UNLIKELY(c->catch_n == c->catch_cap)) {        /* push our tag (active-catch stack) */
        const uint32_t nc = c->catch_cap ? c->catch_cap * 2 : 8;
        c->catch_tags = realloc(c->catch_tags, sizeof(VALUE) * nc);
        if (!c->catch_tags) { fprintf(stderr, "koruby_precise: oom (catch)\n"); abort(); }
        c->catch_cap = nc;
    }
    c->catch_tags[c->catch_n++] = slots[0];
    RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
    c->catch_n--;                                      /* pop on every exit path */
    if (r.state == KORB_THROW && c->throw_tag == slots[0]) {   /* our tag → caught (value in r.value) */
        c->throw_tag = KORB_NIL;
        r.state = KORB_NORMAL;
    }
    return r;
}
/* runtime attr_reader/writer/accessor (the dynamic `attr_reader id` form that
 * the parser can't desugar at parse time; self is the class). */
/* Define reader/writer accessors for a[0..n). Names may be Symbol/String, or a
 * #to_str-coercible object (TypeError otherwise). cls is kept rooted in slots[0]. */
static RESULT korb_m_class_attr_n(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, uint32_t n, int reader, int writer) {
    struct korb_vm *const vm = c->vm;
    slots[0] = VALUE_REF_GET(self);                          /* cls (rooted) */
    if (UNLIKELY(!KORB_CLASS_P(slots[0]))) return korb_raise(c, slots, KORB_E_TYPE, 0, "attr on a non-class");
    { RESULT fr = korb_check_def_frozen(c, slots, slots[0]); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* attr_* on a frozen class → FrozenError */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, n * 2));    /* defined method names → return value */
    VALUE_REF res = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < n; i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (!SYMBOL_P(sym) && !KORB_STRING_P(sym)) {          /* coerce name via #to_str */
            const uint32_t to_str = korb_intern(vm, "to_str", 6);
            VALUE nv = sym;
            if (KORB_OBJECT_P(nv) && korb_responds_to_coerce_p(c, slots + 2, &nv, to_str)) {
                slots[2] = nv;
                RESULT sr = korb_send_impl(c, slots + 3, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (!KORB_STRING_P(sr.value)) return korb_raise_not_sym(c, slots, VALUE_SLICE_GET(a, i));
                sym = sr.value;
            } else return korb_raise_not_sym(c, slots, sym);
        }
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, korb_strbuf_data(VAL2STR(sym)->buf), VAL2STR(sym)->len));
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256]; snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        if (reader) { const uint32_t rid = korb_intern(vm, nm, strlen(nm)); korb_class_def_attr(c, slots[0], rid, ivar, 0); CHECK(korb_ary_push_val(c, slots + 2, res, ID2SYM(rid))); CHECK(korb_fire_method_added(c, slots + 2, slots[0], rid)); }
        if (writer) { snprintf(buf, sizeof buf, "%s=", nm); const uint32_t wid = korb_intern(vm, buf, strlen(buf)); korb_class_def_attr(c, slots[0], wid, ivar, 1); CHECK(korb_ary_push_val(c, slots + 2, res, ID2SYM(wid))); CHECK(korb_fire_method_added(c, slots + 2, slots[0], wid)); }
    }
    return RESULT_OK(VALUE_REF_GET(res));
}
static RESULT korb_m_class_attr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int reader, int writer) {
    return korb_m_class_attr_n(c, slots, self, a, VALUE_SLICE_LEN(a), reader, writer);
}
static RESULT korb_m_class_attr_reader(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_m_class_attr(c, slots, self, a, 1, 0); }
static RESULT korb_m_class_attr_writer(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_m_class_attr(c, slots, self, a, 0, 1); }
static RESULT korb_m_class_attr_accessor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_class_attr(c, slots, self, a, 1, 1); }
/* Module#attr — like attr_reader, but a trailing true/false makes it also (or not) a writer. */
static RESULT korb_m_class_attr1(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    int writer = 0;
    if (n >= 1) {                                             /* attr(name, true|false) */
        const VALUE last = VALUE_SLICE_GET(a, n - 1);
        if (last == KORB_TRUE || last == KORB_FALSE) {
            writer = (last == KORB_TRUE) ? 1 : 0; n--;
            korb_warn(c, slots, "optional boolean argument is obsoleted");
        }
    }
    return korb_m_class_attr_n(c, slots, self, a, n, 1, writer);
}
/* Module#=== (`Klass === obj`): true iff obj.is_a?(Klass) — same test korb_case_eq uses. */
static RESULT korb_m_class_case_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    return RESULT_OK(korb_case_eq(c, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)) ? KORB_TRUE : KORB_FALSE);
}
/* Class#superclass — the immediate superclass (nil at the top). */
static RESULT korb_m_class_superclass(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    return RESULT_OK(VAL2CLASS(VALUE_REF_GET(self))->superclass);
}
/* Class#allocate — a bare instance with no #initialize call. */
static RESULT korb_m_class_allocate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    if (KORB_CLASS_P(VALUE_REF_GET(self))) {             /* immediate classes have no allocator */
        if (UNLIKELY(VAL2CLASS(VALUE_REF_GET(self))->is_singleton))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't create instance of singleton class");
        /* identity, not name: a user class nested in a namespace may well be
         * called "Float" without being ::Float */
        const VALUE cv = VALUE_REF_GET(self);
        const uint32_t cn = VAL2CLASS(cv)->name_sym;
        #define KORB_IS_BC(e) (cv == korb_builtin_class_obj(c->vm, (e)))
        if (KORB_IS_BC(KORB_C_NIL) || KORB_IS_BC(KORB_C_TRUE) || KORB_IS_BC(KORB_C_FALSE) ||
            KORB_IS_BC(KORB_C_INTEGER) || KORB_IS_BC(KORB_C_FLOAT) || KORB_IS_BC(KORB_C_SYMBOL) ||
            KORB_IS_BC(KORB_C_PROC) ||                    /* Proc is built from a block, not allocate'd */
            KORB_IS_BC(KORB_C_THREAD) || KORB_IS_BC(KORB_C_FIBER))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "allocator undefined for %s", korb_sym_name(c->vm, cn));
        if (KORB_IS_BC(KORB_C_MATCHDATA))   /* MatchData has no allocator (built only by a match) → NoMethodError */
            return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'allocate' for class %s", korb_sym_name(c->vm, cn));
        if (KORB_IS_BC(KORB_C_MUTEX))   return korb_mutex_s_new(c, slots);     /* 実 payload */
        if (KORB_IS_BC(KORB_C_CONDVAR)) return korb_condvar_s_new(c, slots);
        #undef KORB_IS_BC
    }
    /* Enumerator.allocate must produce a KorbEnumerator (not a generic object),
     * else enumerator methods VAL2ENUM-cast a too-small object → heap corruption.
     * A zeroed enumerator is mode 0 with a nil `values`; Enumerator#initialize
     * (given a block) turns it into a generator. */
    if (KORB_CLASS_P(VALUE_REF_GET(self)) &&
        korb_class_le(VALUE_REF_GET(self), korb_builtin_class_obj(c->vm, KORB_C_ENUMERATOR)) &&
        !korb_class_le(VALUE_REF_GET(self), korb_builtin_class_obj(c->vm, KORB_C_ARITHSEQ))) {
        slots[0] = VALUE_REF_GET(self);              /* Enumerator::Lazy and user subclasses (ArithmeticSequence has its own payload) */
        KorbEnumerator *const e = korb_alloc(c, slots + 1, sizeof(KorbEnumerator), KORB_OBJ_ENUMERATOR);
        e->mode = 0;                                 /* uninitialized-but-safe: empty eager enumerator */
        slots[1] = (VALUE)e;
        if (slots[0] != korb_builtin_class_obj(c->vm, KORB_C_ENUMERATOR))
            korb_klass_override_set(c, slots[1], slots[0]);
        return RESULT_OK(slots[1]);
    }
    /* A subclass of Module (`Class.new(Module).new`) must be a real module
     * object: prepend/include and the ancestry walk all need KorbClass. */
    if (KORB_CLASS_P(VALUE_REF_GET(self)) &&
        korb_class_le(VALUE_REF_GET(self), korb_const_get(c->vm, korb_intern(c->vm, "Module", 6))) &&
        !korb_class_le(VALUE_REF_GET(self), korb_const_get(c->vm, korb_intern(c->vm, "Class", 5)))) {
        slots[0] = VALUE_REF_GET(self);                          /* root the class across the alloc */
        RESULT mr = korb_class_new(c, slots + 1, 0, KORB_NIL);
        if (UNLIKELY(mr.state != KORB_NORMAL)) return mr;
        slots[1] = mr.value;
        VAL2CLASS(slots[1])->is_module = 1;
        /* the override table keys on the object, so set it AFTER the module is
         * parked in a scanned slot (STRESS moves it otherwise) */
        if (slots[0] != korb_const_get(c->vm, korb_intern(c->vm, "Module", 6)))
            korb_klass_override_set(c, slots[1], slots[0]);       /* report the subclass as #class */
        return RESULT_OK(slots[1]);
    }
    /* A subclass of a constructible builtin needs that builtin's payload, tagged
     * with the subclass — `SubHash.allocate` must answer a real Hash (Marshal and
     * other libraries build instances this way, bypassing #initialize). */
    if (KORB_CLASS_P(VALUE_REF_GET(self))) {
        slots[0] = VALUE_REF_GET(self);                  /* root the class across the alloc */
        const enum korb_class base = korb_builtin_base_class(c->vm, slots[0]);
        RESULT inst = RESULT_OK(KORB_NIL);
        switch (base) {
          case KORB_C_STRING: inst = korb_str_new(c, slots + 1, "", 0); break;
          case KORB_C_ARRAY:  inst = korb_ary_new(c, slots + 1, 0); break;
          case KORB_C_HASH:   inst = korb_hash_new(c, slots + 1, 0); break;
          case KORB_C_RANGE:  slots[1] = KORB_NIL;   /* nil..nil until #initialize; not frozen yet */
                              inst = korb_range_new(c, slots + 2, VALUE_REF_AT(&slots[1]), KORB_NIL, 0);
                              if (inst.state == KORB_NORMAL && inst.value != KORB_NIL)
                                  ((AroObjectHeader *)(uintptr_t)inst.value)->flags &= ~(uint32_t)KORB_FL_FROZEN;
                              break;
          default: break;
        }
        if (inst.value != KORB_NIL) {
            if (UNLIKELY(inst.state != KORB_NORMAL)) return inst;
            slots[1] = inst.value;
            korb_klass_override_set(c, slots[1], slots[0]);
            return RESULT_OK(slots[1]);
        }
    }
    return korb_obj_new(c, slots, VALUE_REF_GET(self));
}
/* Module#set_temporary_name(name|nil) — assign/clear a temporary name on an
 * (anonymous) module; the name may not be a constant path (contain "::"). */
static RESULT korb_m_module_set_temp_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE sv = VALUE_REF_GET(self);
    if (!KORB_CLASS_P(sv)) return RESULT_OK(sv);
    if (UNLIKELY(korb_class_permanent_p(sv)))
        return korb_raise(c, slots, KORB_E_RUNTIME, 0, "can't change permanent name");
    const VALUE nm = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    /* clear → fully anonymous: CRuby drops the constant-derived name too, so a
     * module that was only reachable through an anonymous namespace has no name */
    if (nm == KORB_NIL) { VAL2CLASS(sv)->temp_name_sym = 0; VAL2CLASS(sv)->name_sym = 0; return RESULT_OK(sv); }
    if (UNLIKELY(!KORB_STRING_P(nm))) return korb_raise_not_sym(c, slots, nm);
    const KorbString *const s = VAL2STR(nm);
    if (UNLIKELY(s->len == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "empty class/module name");
    {   /* reject a valid constant path (each ::-segment is [A-Z][A-Za-z0-9_]*) to avoid confusion */
        const char *const d = korb_strbuf_data(s->buf); const uint32_t len = s->len; uint32_t i = 0; bool cpath = true;
        if (len >= 2 && d[0] == ':' && d[1] == ':') i = 2;          /* "::A" is a constant path too */
        for (;;) {
            if (i >= len || !(d[i] >= 'A' && d[i] <= 'Z')) { cpath = false; break; }
            i++;
            while (i < len && ((d[i]>='A'&&d[i]<='Z')||(d[i]>='a'&&d[i]<='z')||(d[i]>='0'&&d[i]<='9')||d[i]=='_')) i++;
            if (i == len) break;                                   /* ended on a valid segment */
            if (i + 1 < len && d[i] == ':' && d[i+1] == ':') { i += 2; continue; }
            cpath = false; break;
        }
        if (cpath) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "the temporary name must not be a constant path to avoid confusion");
    }
    VAL2CLASS(sv)->temp_name_sym = korb_intern(c->vm, korb_strbuf_data(s->buf), s->len);
    return RESULT_OK(sv);
}
/* Module#name → the class/module name (a frozen String), nil if anonymous. */
static RESULT korb_m_class_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const RESULT r = korb_class_qname_str(c, slots, VALUE_REF_GET(self));   /* fully-qualified (M::C); nil if anonymous */
    if (LIKELY(r.state == KORB_NORMAL) && KORB_STRING_P(r.value))
        ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;   /* CRuby: Module#name is frozen */
    return r;
}
/* Module#to_s / #inspect → the (qualified) name; an anonymous class stringifies
 * to a placeholder rather than nil. */
/* Class/Module #to_s into a memstream (no alloc — reads only): a named class →
 * its qualified name; a singleton class → "#<Class:<attached>>" where <attached>
 * is the attached class/module's own to_s (recursing) or, for an object, its
 * default "#<ClassName:0xADDR>" (NOT dispatching #inspect); else the anonymous
 * "#<Class:0xADDR>". */
static void korb_fprint_class_tostr(CTX *c, FILE *ms, VALUE cls) {
    const KorbClass *const k = VAL2CLASS(cls);
    if (k->name_sym != 0) { korb_fprint_class_qname(c, ms, cls); return; }
    if (k->is_singleton) {
        VALUE att = KORB_NIL;
        for (uint32_t i = 0; i < c->vm->sklass_cnt; i++)
            if (c->vm->sklass_cls[i] == cls) { att = c->vm->sklass_obj[i]; break; }
        if (att != KORB_NIL) {
            fputs("#<Class:", ms);
            if (KORB_CLASS_P(att)) korb_fprint_class_tostr(c, ms, att);   /* attached Class/Module/singleton → recurse */
            else {                                                         /* attached object → #<ClassName:0xADDR> */
                VALUE oc = korb_dispatch_class(c, att);
                while (KORB_CLASS_P(oc) && VAL2CLASS(oc)->is_singleton) oc = VAL2CLASS(oc)->superclass;   /* real class */
                fputs("#<", ms);
                if (KORB_CLASS_P(oc)) korb_fprint_class_tostr(c, ms, oc);   /* named → qname, anonymous → #<Class:0x…> */
                else fputs("Object", ms);
                fprintf(ms, ":0x%016zx>", (size_t)(uintptr_t)att);
            }
            fputc('>', ms);
            return;
        }
    }
    fprintf(ms, "#<%s:0x%016zx>", k->is_module ? "Module" : "Class", (size_t)(uintptr_t)cls);
}
static RESULT korb_m_class_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE cls = VALUE_REF_GET(self);
    const KorbClass *const k = VAL2CLASS(cls);
    if (k->name_sym == 0) {                                        /* anonymous / singleton → formatted form */
        char *buf = NULL; size_t sz = 0;
        FILE *ms = open_memstream(&buf, &sz);
        if (!ms) { char b[48]; int n = snprintf(b, sizeof b, "#<%s:0x%016zx>", k->is_module ? "Module" : "Class", (size_t)(uintptr_t)cls); return korb_str_new(c, slots, b, (uint32_t)n); }
        korb_fprint_class_tostr(c, ms, cls);
        fclose(ms);
        RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
        free(buf);
        return r;
    }
    return korb_class_qname_str(c, slots, cls);
}
/* Module#constants — the constant names defined directly in this module/class
 * (owner-tagged in the VM const table).  Globals ($) and cvars (@) share the
 * table and are excluded.  (inherit arg / ancestor constants not modelled.) */
/* Is `owner` in klass's own MRO segment (itself + prepended/included, which
 * bring their own mixins along)? */
static bool korb_const_seg_p(VALUE klass, VALUE owner, int depth) {
    if (!KORB_CLASS_P(klass) || depth > 64) return false;
    if (klass == owner) return true;
    const KorbClass *const k = VAL2CLASS(klass);
    const VALUE lists[2] = { k->prepended, k->included };
    for (int t = 0; t < 2; t++) {
        if (lists[t] == KORB_NIL) continue;
        const KorbArray *const l = VAL2ARY(lists[t]);
        for (uint32_t j = 0; j < l->len; j++)
            if (korb_const_seg_p(korb_items_data(l->items)[j], owner, depth + 1)) return true;
    }
    return false;
}
/* CRuby's rb_mod_const_of stops the ancestor walk AT Object, so neither
 * Object's constants nor those of a module included into Object show up in
 * some unrelated class's #constants. */
static bool korb_const_ancestor_p(VALUE klass, VALUE owner, VALUE objc) {
    for (VALUE k = klass; KORB_CLASS_P(k); k = VAL2CLASS(k)->superclass) {
        if (k == objc && klass != objc) return false;
        if (korb_const_seg_p(k, owner, 0)) return true;
    }
    return false;
}

static RESULT korb_m_mod_constants(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool inherit = VALUE_SLICE_LEN(a) < 1 || KORB_TRUTHY(VALUE_SLICE_GET(a, 0));   /* default: include ancestors */
    struct korb_vm *const vm = c->vm;
    slots[0] = korb_builtin_class_obj(vm, KORB_C_OBJECT);           /* rooted: the allocs below move it */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 8));
    VALUE_REF arr = VALUE_REF_AT(&slots[1]);
    for (uint32_t i = 0; i < vm->const_cnt; i++) {
        const VALUE owner = vm->const_owners[i];                    /* owners are root-updated across the push GC */
        const VALUE selfv = VALUE_REF_GET(self);
        const VALUE objc = slots[0];
        /* top-level constants (and the builtin classes) carry no owner; they are
         * Object's, which is what `Object.constants` must list. */
        bool match = (owner == selfv) || (owner == KORB_NIL && selfv == objc);
        /* inherit (default): own + included + prepended + ancestors (excl. Object).
         * non-inherit: only constants defined directly in self (owner == self). */
        if (!match && inherit && KORB_CLASS_P(selfv) && KORB_CLASS_P(owner) && owner != objc)
            match = korb_const_ancestor_p(selfv, owner, objc);
        if (!match) continue;
        if (korb_const_private_p(vm, owner, vm->const_names[i])) continue;   /* private_constant */
        const char *const nm = korb_sym_name(vm, vm->const_names[i]);
        /* only real constant names: an internal lowercase entry (CRuby's
         * `fatal`, IO::generic_readable) is not reported by #constants */
        if (!(nm[0] >= 'A' && nm[0] <= 'Z') && !((unsigned char)nm[0] & 0x80u)) continue;
        const VALUE csym = ID2SYM(vm->const_names[i]);
        bool dup = false;                                          /* a subclass constant shadows the ancestor's */
        const KorbArray *const d = VAL2ARY(VALUE_REF_GET(arr));
        for (uint32_t j = 0; j < d->len; j++) if (korb_items_data(d->items)[j] == csym) { dup = true; break; }
        if (dup) continue;
        CHECK(korb_ary_push_val(c, slots + 2, arr, csym));
    }
    /* a pending autoload is already a constant to CRuby — it shows up in
     * #constants before the file is required. */
    slots[0] = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_intern(vm, "@__autoloads", 12)));
    if (KORB_HASH_P(slots[0])) {
        for (uint32_t i = 0; i < VAL2HASH(slots[0])->len; i++) {
            const VALUE key = korb_items_data(VAL2HASH(slots[0])->items)[2 * i];   /* Symbol: immediate, GC-stable */
            if (!SYMBOL_P(key)) continue;
            bool dup = false;
            const KorbArray *const d = VAL2ARY(VALUE_REF_GET(arr));
            for (uint32_t j = 0; j < d->len; j++) if (korb_items_data(d->items)[j] == key) { dup = true; break; }
            if (!dup) CHECK(korb_ary_push_val(c, slots + 2, arr, key));
        }
    }
    return RESULT_OK(VALUE_REF_GET(arr));
}
/* Append one MRO segment (a class/module with its own prepended and included
 * modules) to `out`, in method-lookup order — the same walk korb_mro_seg_find
 * does, so `ancestors` and dispatch agree.  A prepended module brings its own
 * prepends along, hence the recursion.  Duplicates are dropped (a module keeps
 * its first, nearest position). */
static RESULT korb_ancestors_seg(CTX *c, VALUE *slots, VALUE_REF out, VALUE klass, int depth) {
    if (!KORB_CLASS_P(klass) || depth > 64) return RESULT_OK(KORB_NIL);
    slots[0] = klass;                                        /* the segment's class (rooted) */
    VALUE lst = VAL2CLASS(slots[0])->prepended;              /* prepended: most-recently-prepended first */
    if (lst != KORB_NIL) {
        slots[1] = lst;
        for (uint32_t j = VAL2ARY(slots[1])->len; j-- > 0; )
            CHECK(korb_ancestors_seg(c, slots + 2, out, korb_items_data(VAL2ARY(slots[1])->items)[j], depth + 1));
    }
    {   /* A module can appear twice — prepended to a subclass and included by a
         * superclass — so only an immediate repeat is dropped (CRuby). */
        const KorbArray *const d = VAL2ARY(VALUE_REF_GET(out));
        if (d->len == 0 || korb_items_data(d->items)[d->len - 1] != slots[0])
            CHECK(korb_ary_push_val(c, slots + 1, out, slots[0]));
    }
    lst = VAL2CLASS(slots[0])->included;                     /* included: most-recently-included first */
    if (lst != KORB_NIL) {
        slots[1] = lst;
        for (uint32_t j = VAL2ARY(slots[1])->len; j-- > 0; )
            CHECK(korb_ancestors_seg(c, slots + 2, out, korb_items_data(VAL2ARY(slots[1])->items)[j], depth + 1));
    }
    return RESULT_OK(KORB_NIL);
}
/* Module#ancestors — each class of the superclass chain as a segment (its
 * prepends, itself, its includes).  Singleton classes skipped. */
static RESULT korb_m_class_ancestors(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 8));        /* result (rooted) */
    slots[1] = VALUE_REF_GET(self);                          /* current class (rooted) */
    while (KORB_CLASS_P(slots[1])) {
        CHECK(korb_ancestors_seg(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1], 0));
        slots[1] = VAL2CLASS(slots[1])->superclass;
    }
    return RESULT_OK(slots[0]);
}
static RESULT korb_collect_methods_from(CTX *c, VALUE *slots, VALUE start_class, VALUE_SLICE a, uint8_t vis_mask);  /* fwd */
/* Module#instance_methods(inherit=true) → public/protected method names (symbols).
 * Excludes the private `initialize`; dedups across the ancestor chain. */
static RESULT korb_m_class_instance_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* public + protected (not private); walks prepended/included modules + the
     * superclass chain via the shared collector. */
    return korb_collect_methods_from(c, slots, VALUE_REF_GET(self), a, (1u << 0) | (1u << 2));
}
/* push the visibility-matching method names of `klass_ref`'s class into `result`
 * (with dedup).  vis_mask bit v set ⇒ include methods of visibility v.  Re-reads
 * the class each iteration since korb_ary_push_val can move it under a moving GC. */
/* `blocked` collects undef tombstones seen so far: a name undef'd nearer in the
 * MRO hides the ancestor definition from every listing below it. */
static RESULT korb_push_vis_methods(CTX *c, VALUE *slots, VALUE_REF result, VALUE_REF blocked, VALUE_REF klass_ref, uint8_t vis_mask, const uint32_t *priv_mids, uint32_t priv_n) {
    const uint32_t n = VAL2CLASS(VALUE_REF_GET(klass_ref))->method_cnt;
    for (uint32_t i = 0; i < n; i++) {
        const struct korb_method *const m = VAL2CLASS(VALUE_REF_GET(klass_ref))->methods[i];   /* entries are immortal (libc) */
        if (m->mid == UINT32_MAX) continue;
        const VALUE sym = ID2SYM(m->mid);
        if (m->kind == KORB_METHOD_UNDEF) { CHECK(korb_ary_push_val(c, slots, blocked, sym)); continue; }
        { const KorbArray *const b = VAL2ARY(VALUE_REF_GET(blocked));
          bool hid = false;
          for (uint32_t j = 0; j < b->len; j++) if (korb_items_data(b->items)[j] == sym) { hid = true; break; }
          if (hid) continue; }
        uint8_t v = m->visibility;
        /* initialize/initialize_{copy,clone,dup}/respond_to_missing? etc. read as
         * private wherever they are instance methods — but a singleton copy made
         * by module_function is public, so honour its own visibility there. */
        const bool on_singleton = KORB_CLASS_P(VALUE_REF_GET(klass_ref)) && VAL2CLASS(VALUE_REF_GET(klass_ref))->is_singleton;
        if (!on_singleton)
            for (uint32_t p = 0; p < priv_n; p++) if (m->mid == priv_mids[p]) { v = 1; break; }
        /* a definition nearer in the MRO hides the ancestors' — even when its own
         * visibility does not match, so `private :m` here keeps an ancestor's
         * public m out of public_instance_methods (CRuby). */
        if (!(vis_mask & (1u << v))) { CHECK(korb_ary_push_val(c, slots, blocked, sym)); continue; }
        const KorbArray *const r = VAL2ARY(VALUE_REF_GET(result));
        bool seen = false;
        for (uint32_t j = 0; j < r->len; j++) if (korb_items_data(r->items)[j] == sym) { seen = true; break; }
        if (!seen) CHECK(korb_ary_push_val(c, slots, result, sym));
    }
    return RESULT_OK(KORB_NIL);
}
/* Walk the MRO from `start_class` (class → included modules → super …) collecting
 * visibility-matching method names.  With inherit=false: only the singleton
 * class(es) + the first real class's own methods. */
/* Kernel's module_function block-methods (loop/catch/lambda/…) live on Object in
 * koruby for dispatch, so add them to Kernel's own method introspection to match
 * CRuby.  Private set = all; public-singleton set = the module_function subset. */
static RESULT korb_append_names(CTX *c, VALUE *slots, VALUE_REF result, const char *const *names, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const VALUE sym = ID2SYM(korb_intern(c->vm, names[i], (uint32_t)strlen(names[i])));
        bool dup = false; const KorbArray *const r = VAL2ARY(VALUE_REF_GET(result));
        for (uint32_t j = 0; j < r->len; j++) if (korb_items_data(r->items)[j] == sym) { dup = true; break; }
        if (!dup) CHECK(korb_ary_push_val(c, slots, result, sym));
    }
    return RESULT_OK(KORB_NIL);
}
static const char *const korb_kernel_priv_funcs[] = { "loop", "catch", "throw", "lambda", "proc", "block_given?", "iterator?" };
static const char *const korb_kernel_pub_funcs[]  = { "loop", "catch", "throw", "lambda", "proc" };
static RESULT korb_collect_methods_from(CTX *c, VALUE *slots, VALUE start_class, VALUE_SLICE a, uint8_t vis_mask) {
    const bool inherit = !(VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_FALSE);
    const uint32_t priv_mids[] = {                                  /* method names that are always reported private */
        c->vm->mid_initialize,
        korb_intern(c->vm, "initialize_copy", 15),
        korb_intern(c->vm, "initialize_clone", 16),
        korb_intern(c->vm, "initialize_dup", 14),
        korb_intern(c->vm, "respond_to_missing?", 19),
        korb_intern(c->vm, "public", 6),                            /* Module's visibility helpers are private instance methods */
        korb_intern(c->vm, "private", 7),
        korb_intern(c->vm, "protected", 9),
        korb_intern(c->vm, "module_function", 15),
        korb_intern(c->vm, "marshal_dump", 12),                     /* Marshal hooks are private wherever defined */
        korb_intern(c->vm, "marshal_load", 12),
        korb_intern(c->vm, "method_added", 12),                     /* Module/Class definition-callback hooks are private */
        korb_intern(c->vm, "method_removed", 14),
        korb_intern(c->vm, "method_undefined", 16),
        korb_intern(c->vm, "included", 8),
        korb_intern(c->vm, "prepended", 9),
        korb_intern(c->vm, "extended", 8),
        korb_intern(c->vm, "inherited", 9),
        korb_intern(c->vm, "const_added", 11),
        korb_intern(c->vm, "append_features", 15),
        korb_intern(c->vm, "prepend_features", 16),
        korb_intern(c->vm, "extend_object", 13),
    };
    const uint32_t priv_n = (uint32_t)(sizeof priv_mids / sizeof priv_mids[0]);
    /* Compute the "bare module" test up front, while start_class is still fresh —
     * the collection loop below allocs (GC), after which start_class is stale. */
    const VALUE kernel_mod = korb_const_get(c->vm, korb_intern(c->vm, "Kernel", 6));
    const bool is_kernel = KORB_CLASS_P(start_class) && start_class == kernel_mod;
    const bool is_bare_module = KORB_CLASS_P(start_class) && VAL2CLASS(start_class)->is_module && !is_kernel;
    slots[1] = start_class;                                         /* MRO cursor (rooted) — set before any alloc */
    slots[0] = UNWRAP(korb_ary_new(c, slots + 2, 8));              /* result (rooted at slots[0]) */
    const VALUE_REF result = VALUE_REF_AT(&slots[0]);
    slots[2] = KORB_NIL;                                            /* module scratch (rooted; init so GC never scans garbage) */
    slots[3] = UNWRAP(korb_ary_new(c, slots + 4, 4));              /* undef'd names seen so far (rooted) */
    const VALUE_REF blocked = VALUE_REF_AT(&slots[3]);
    while (KORB_CLASS_P(slots[1])) {
        const bool is_sing = VAL2CLASS(slots[1])->is_singleton;
        if (inherit && VAL2CLASS(slots[1])->prepended != KORB_NIL) {   /* prepended modules precede the class (ancestors → only with inherit) */
            const uint32_t plen = VAL2ARY(VAL2CLASS(slots[1])->prepended)->len;
            for (uint32_t j = plen; j-- > 0; ) {
                slots[2] = korb_items_data(VAL2ARY(VAL2CLASS(slots[1])->prepended)->items)[j];   /* re-read (rooted class) */
                CHECK(korb_push_vis_methods(c, slots + 4, result, blocked, VALUE_REF_AT(&slots[2]), vis_mask, priv_mids, priv_n));
            }
        }
        CHECK(korb_push_vis_methods(c, slots + 4, result, blocked, VALUE_REF_AT(&slots[1]), vis_mask, priv_mids, priv_n));
        if (inherit && VAL2CLASS(slots[1])->included != KORB_NIL) { /* mixed-in modules */
            const uint32_t mlen = VAL2ARY(VAL2CLASS(slots[1])->included)->len;
            for (uint32_t j = mlen; j-- > 0; ) {
                slots[2] = korb_items_data(VAL2ARY(VAL2CLASS(slots[1])->included)->items)[j];   /* re-read (rooted class) */
                CHECK(korb_push_vis_methods(c, slots + 4, result, blocked, VALUE_REF_AT(&slots[2]), vis_mask, priv_mids, priv_n));
            }
        }
        if (!inherit && !is_sing) break;                           /* false: stop after the first non-singleton class */
        slots[1] = VAL2CLASS(slots[1])->superclass;
    }
    /* Kernel-private builtins (puts/require/... in the global table) are private
     * methods of Kernel, inherited by every object/class — but NOT by a bare
     * module (its ancestors are just itself + its own includes, not Kernel).
     * Kernel itself is the exception: it owns them. (is_bare_module computed at
     * function entry — start_class is stale here after the loop's GCs.) */
    if ((vis_mask & (1u << 1)) && (inherit ? !is_bare_module : is_kernel)) {   /* inherit=false lists only Kernel's OWN builtins */
        for (uint32_t i = 0; i < c->vm->method_cnt; i++) {
            if (c->vm->methods[i]->kind != KORB_METHOD_BUILTIN) continue;
            const VALUE sym = ID2SYM(c->vm->methods[i]->mid);
            bool dup = false;
            const KorbArray *const r = VAL2ARY(VALUE_REF_GET(result));
            for (uint32_t j = 0; j < r->len; j++) if (korb_items_data(r->items)[j] == sym) { dup = true; break; }
            if (!dup) CHECK(korb_ary_push_val(c, slots + 3, result, sym));
        }
        if (is_kernel)   /* Kernel's own private module_function block-methods */
            CHECK(korb_append_names(c, slots + 3, result, korb_kernel_priv_funcs, (uint32_t)(sizeof korb_kernel_priv_funcs / sizeof *korb_kernel_priv_funcs)));
    }
    return RESULT_OK(VALUE_REF_GET(result));
}
/* Object#*: walk the object's dispatch class (singleton + class + …). */
static RESULT korb_m_obj_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)           { return korb_collect_methods_from(c, slots, korb_dispatch_class(c, VALUE_REF_GET(self)), a, (1u<<0)|(1u<<2)); }
static RESULT korb_m_obj_public_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE sv = VALUE_REF_GET(self);
    RESULT r = korb_collect_methods_from(c, slots, korb_dispatch_class(c, sv), a, (1u << 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (sv == korb_const_get(c->vm, korb_intern(c->vm, "Kernel", 6))) {   /* Kernel.Array/.puts/... are public singleton methods */
        slots[0] = r.value;
        const VALUE_REF res = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < c->vm->method_cnt; i++) {
            if (c->vm->methods[i]->kind != KORB_METHOD_BUILTIN) continue;
            const VALUE sym = ID2SYM(c->vm->methods[i]->mid);
            bool dup = false; const KorbArray *const rr = VAL2ARY(VALUE_REF_GET(res));
            for (uint32_t j = 0; j < rr->len; j++) if (korb_items_data(rr->items)[j] == sym) { dup = true; break; }
            if (!dup) CHECK(korb_ary_push_val(c, slots + 1, res, sym));
        }
        CHECK(korb_append_names(c, slots + 1, res, korb_kernel_pub_funcs, (uint32_t)(sizeof korb_kernel_pub_funcs / sizeof *korb_kernel_pub_funcs)));   /* module_function block-methods */
        return RESULT_OK(VALUE_REF_GET(res));
    }
    return r;
}
static RESULT korb_m_obj_private_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_collect_methods_from(c, slots, korb_dispatch_class(c, VALUE_REF_GET(self)), a, (1u<<1)); }
static RESULT korb_m_obj_protected_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_collect_methods_from(c, slots, korb_dispatch_class(c, VALUE_REF_GET(self)), a, (1u<<2)); }
/* Object#singleton_methods(all=true) → only the object's singleton class methods
 * (public/protected); all=false excludes modules extended into the singleton.
 * Never includes the regular class's methods. */
static RESULT korb_m_obj_singleton_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool all = !(VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_FALSE);
    const uint32_t mid_init = c->vm->mid_initialize;
    const uint8_t mask = (1u << 0) | (1u << 2);                     /* public + protected */
    slots[0] = UNWRAP(korb_ary_new(c, slots + 2, 4));              /* result */
    const VALUE_REF result = VALUE_REF_AT(&slots[0]);
    slots[1] = KORB_NIL;                                            /* singleton (rooted) */
    slots[2] = KORB_NIL;                                            /* module scratch (rooted) */
    slots[3] = VALUE_REF_GET(self);                                 /* walk cursor (rooted) */
    slots[4] = UNWRAP(korb_ary_new(c, slots + 5, 4));              /* undef'd names (rooted) */
    const VALUE_REF blocked = VALUE_REF_AT(&slots[4]);
    /* A Class inherits class methods from its superclasses' singleton classes; walk
     * the (user) superclass chain when `all`, stopping before Object so builtin class
     * methods don't leak in. */
    const VALUE obj_cls = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    const bool is_class = KORB_CLASS_P(VALUE_REF_GET(self)) && !VAL2CLASS(VALUE_REF_GET(self))->is_singleton;
    for (;;) {
        slots[1] = korb_dispatch_class(c, slots[3]);                /* singleton (if any) or real class */
        if (KORB_CLASS_P(slots[1]) && VAL2CLASS(slots[1])->is_singleton) {
            CHECK(korb_push_vis_methods(c, slots + 5, result, blocked, VALUE_REF_AT(&slots[1]), mask, &mid_init, 1u));
            if (all && VAL2CLASS(slots[1])->included != KORB_NIL) { /* extended modules */
                const uint32_t mlen = VAL2ARY(VAL2CLASS(slots[1])->included)->len;
                for (uint32_t j = mlen; j-- > 0; ) {
                    slots[2] = korb_items_data(VAL2ARY(VAL2CLASS(slots[1])->included)->items)[j];
                    CHECK(korb_push_vis_methods(c, slots + 5, result, blocked, VALUE_REF_AT(&slots[2]), mask, &mid_init, 1u));
                }
            }
        }
        if (!all || !is_class) break;
        const VALUE sup = VAL2CLASS(slots[3])->superclass;
        if (!KORB_CLASS_P(sup) || sup == obj_cls) break;           /* stop before Object */
        slots[3] = sup;
    }
    return RESULT_OK(VALUE_REF_GET(result));
}
/* Module#*_instance_methods: walk the module/class itself (not a singleton). */
static RESULT korb_m_class_public_imethods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_collect_methods_from(c, slots, VALUE_REF_GET(self), a, (1u<<0)); }
static RESULT korb_m_class_private_imethods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_collect_methods_from(c, slots, VALUE_REF_GET(self), a, (1u<<1)); }
static RESULT korb_m_class_protected_imethods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_collect_methods_from(c, slots, VALUE_REF_GET(self), a, (1u<<2)); }
/* true if `sub` is `sup` or has `sup` among its ancestors (class chain + modules). */
static bool korb_class_is_descendant(VALUE sub, VALUE sup) {
    VALUE cls = sub;
    while (KORB_CLASS_P(cls)) {
        if (cls == sup) return true;
        VALUE pre = VAL2CLASS(cls)->prepended;
        if (pre != KORB_NIL) {
            const KorbArray *pa = VAL2ARY(pre);
            for (uint32_t j = 0; j < pa->len; j++) if (korb_items_data(pa->items)[j] == sup) return true;
        }
        VALUE inc = VAL2CLASS(cls)->included;
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (korb_items_data(ia->items)[j] == sup) return true;
        }
        cls = VAL2CLASS(cls)->superclass;
    }
    return false;
}
/* Module#< <= > >= : class/module hierarchy comparison (nil if unrelated).
 * rel: 0 '<', 1 '<=', 2 '>', 3 '>='. */
static RESULT korb_class_cmp_rel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int rel) {
    VALUE me = VALUE_REF_GET(self), other = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(other)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "compared with non class/module");
    if (me == other) return RESULT_OK((rel == 1 || rel == 3) ? KORB_TRUE : KORB_FALSE);
    if (korb_class_is_descendant(me, other)) return RESULT_OK((rel <= 1) ? KORB_TRUE : KORB_FALSE);   /* me < other */
    if (korb_class_is_descendant(other, me)) return RESULT_OK((rel >= 2) ? KORB_TRUE : KORB_FALSE);   /* me > other */
    return RESULT_OK(KORB_NIL);                                  /* unrelated */
}
static RESULT korb_m_class_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 0); }
/* Module#<=> — 0 if equal, -1 if self is a descendant of (includes/inherits)
 * other, 1 if an ancestor, nil if unrelated or other isn't a Module (no error). */
static RESULT korb_m_class_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    const VALUE me = VALUE_REF_GET(self), other = VALUE_SLICE_GET(a, 0);
    if (!KORB_CLASS_P(other)) return RESULT_OK(KORB_NIL);
    if (me == other) return RESULT_OK(LONG2FIX(0));
    if (korb_class_is_descendant(me, other)) return RESULT_OK(LONG2FIX(-1));
    if (korb_class_is_descendant(other, me)) return RESULT_OK(LONG2FIX(1));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_class_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 1); }
static RESULT korb_m_class_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 2); }
static RESULT korb_m_class_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 3); }
/* Comparable mixin: derive </<=/>/>=/==/between?/clamp from the receiver's <=>.
 * `*out` = -1/0/1, or 2 when <=> returns nil (incomparable). */
static RESULT korb_comparable_cmp(CTX *c, VALUE *slots, VALUE self, VALUE other, int *out) {
    slots[0] = self; slots[1] = other;
    RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "<=>", 3), 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (r.value == KORB_NIL) { *out = 2; return RESULT_OK(KORB_NIL); }
    /* rb_cmpint: Fixnum/Bignum/Float compare by sign; any other value by
     * dispatching > 0 / < 0.  (A NaN Float has neither sign → 0.) */
    if (FIXNUM_P(r.value)) {
        const korb_sword_t v = FIX2LONG(r.value);
        *out = v < 0 ? -1 : v > 0 ? 1 : 0;
        return RESULT_OK(KORB_TRUE);
    }
    double d;
    if (korb_num_to_d(r.value, &d)) {
        *out = d < 0.0 ? -1 : d > 0.0 ? 1 : 0;
        return RESULT_OK(KORB_TRUE);
    }
    /* general object: (val > 0) ? 1 : (val < 0) ? -1 : 0.  Re-stage the receiver
     * before each dispatch (slots[0]) with the literal 0 arg at slots[1]. */
    slots[2] = r.value;                                   /* park across dispatch */
    slots[0] = slots[2]; slots[1] = LONG2FIX(0);
    RESULT g = korb_send_impl(c, slots + 2, korb_intern(c->vm, ">", 1), 0, 1,
                              NULL, NULL, NULL);
    if (UNLIKELY(g.state != KORB_NORMAL)) return g;
    if (g.value != KORB_NIL && g.value != KORB_FALSE) { *out = 1; return RESULT_OK(KORB_TRUE); }
    slots[0] = slots[2]; slots[1] = LONG2FIX(0);
    RESULT l = korb_send_impl(c, slots + 2, korb_intern(c->vm, "<", 1), 0, 1,
                              NULL, NULL, NULL);
    if (UNLIKELY(l.state != KORB_NORMAL)) return l;
    *out = (l.value != KORB_NIL && l.value != KORB_FALSE) ? -1 : 0;
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_cmpbl_rel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int op) {
    int cmp; RESULT r = korb_comparable_cmp(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), &cmp);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (cmp == 2) {                                       /* <=> returned nil */
        if (op == 4) return RESULT_OK(KORB_FALSE);        /* == → false */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "comparison of %s with %s failed",
                          korb_type_name(VALUE_REF_GET(self)), korb_type_name(VALUE_SLICE_GET(a, 0)));
    }
    bool t;
    switch (op) { case 0: t = cmp < 0; break; case 1: t = cmp <= 0; break;
                  case 2: t = cmp > 0; break; case 3: t = cmp >= 0; break; default: t = cmp == 0; break; }
    return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_cmpbl_lt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cmpbl_rel(c, slots, self, a, 0); }
static RESULT korb_m_cmpbl_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cmpbl_rel(c, slots, self, a, 1); }
static RESULT korb_m_cmpbl_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cmpbl_rel(c, slots, self, a, 2); }
static RESULT korb_m_cmpbl_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cmpbl_rel(c, slots, self, a, 3); }
static RESULT korb_m_cmpbl_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE s = VALUE_REF_GET(self);   /* guard the == → <=> → Object#<=> → == cycle for a Comparable with no own <=> */
    if (AROH_IS_GC_OBJECT(s)) {
        AroObjectHeader *const h = (AroObjectHeader *)(uintptr_t)s;
        if (h->flags & KORB_FL_CMP_VISITING) return RESULT_OK(KORB_FALSE);   /* re-entrant → not comparable → false */
        h->flags |= KORB_FL_CMP_VISITING;
        RESULT r = korb_m_cmpbl_rel(c, slots, self, a, 4);
        ((AroObjectHeader *)(uintptr_t)VALUE_REF_GET(self))->flags &= ~KORB_FL_CMP_VISITING;   /* re-read: cmp may have moved self */
        return r;
    }
    return korb_m_cmpbl_rel(c, slots, self, a, 4);
}
/* Numeric#== — numeric value equality, but for a non-numeric object other it is
 * reflexive: `5 == obj` → `obj == 5` (CRuby).  Overrides Comparable#== (which
 * would just return false via <=>), so method-dispatched `num == obj` (e.g. from
 * Array#find_index) honors a custom #== on the other side. */
static RESULT korb_m_num_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE l = VALUE_REF_GET(self), r = VALUE_SLICE_GET(a, 0);
    if (korb_value_eq(l, r)) return RESULT_OK(KORB_TRUE);
    if (KORB_OBJECT_P(r)) {                              /* non-numeric other → reflexive delegation */
        slots[0] = r; slots[1] = l;
        RESULT res = korb_send(c, slots + 2, c->vm->mid_eq, 0, 1);
        if (UNLIKELY(res.state != KORB_NORMAL)) return res;
        return RESULT_OK(KORB_TRUTHY(res.value) ? KORB_TRUE : KORB_FALSE);
    }
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_cmpbl_between(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    int c1; RESULT r = korb_comparable_cmp(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), &c1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (c1 == 2 || c1 < 0) return RESULT_OK(KORB_FALSE);   /* self < min */
    int c2; r = korb_comparable_cmp(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 1), &c2);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK((c2 != 2 && c2 <= 0) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_cmpbl_clamp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (VALUE_SLICE_LEN(a) == 1 && KORB_RANGE_P(VALUE_SLICE_GET(a, 0))) {
        const KorbRange *rg = VAL2RANGE(VALUE_SLICE_GET(a, 0));
        if (UNLIKELY(rg->exclude_end && rg->rend != KORB_NIL))   /* endless exclusive is fine (no upper bound) */
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot clamp with an exclusive range");
        slots[0] = rg->rbegin; slots[1] = rg->rend;
    } else if (VALUE_SLICE_LEN(a) >= 2) { slots[0] = VALUE_SLICE_GET(a, 0); slots[1] = VALUE_SLICE_GET(a, 1); }
    else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    /* slots[0]=lo, slots[1]=hi rooted across the (GC-causing) <=> dispatches */
    if (slots[0] != KORB_NIL && slots[1] != KORB_NIL) {  /* both bounds present → min must be <= max */
        int clh; RESULT rb = korb_comparable_cmp(c, slots + 2, slots[0], slots[1], &clh);
        if (UNLIKELY(rb.state != KORB_NORMAL)) return rb;
        if (clh == 2 || clh > 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "min argument must be less than or equal to max argument");
    }
    int cl; RESULT r = korb_comparable_cmp(c, slots + 2, VALUE_REF_GET(self), slots[0], &cl);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (cl != 2 && cl < 0) return RESULT_OK(slots[0]);    /* self < lo → lo */
    int ch; r = korb_comparable_cmp(c, slots + 2, VALUE_REF_GET(self), slots[1], &ch);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (ch != 2 && ch > 0) return RESULT_OK(slots[1]);    /* self > hi → hi */
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Module#include?(mod) — true if mod is included in self or any ancestor
 * (superclasses themselves don't count, only included modules). */
static RESULT korb_m_class_include_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE target = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_CLASS_P(target) || !VAL2CLASS(target)->is_module))   /* a Class (not Module) or non-class → TypeError, like CRuby */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(target));
    VALUE cls = VALUE_REF_GET(self);
    while (KORB_CLASS_P(cls)) {                              /* pure reads → no GC, no rooting */
        VALUE inc = VAL2CLASS(cls)->included;
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (korb_items_data(ia->items)[j] == target) return RESULT_OK(KORB_TRUE);
        }
        cls = VAL2CLASS(cls)->superclass;
    }
    return RESULT_OK(KORB_FALSE);
}
/* Module#include(mod...) / Module#prepend(mod...) — explicit-receiver form (the
 * bare class-body `include`/`prepend` is special-cased in korb_call_impl). */
static RESULT korb_m_class_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_do_include(c, slots, VALUE_REF_GET(self), a);
}
static RESULT korb_m_class_prepend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_do_prepend(c, slots, VALUE_REF_GET(self), a);
}
/* Module#const_set(name, value) — koruby's const table is flat (global), so this
 * defines/overwrites the named constant. Returns the value. */
static RESULT korb_m_class_const_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    const char *const cname = korb_sym_name(c->vm, id);   /* [A-Z][A-Za-z0-9_]* (or non-ASCII) */
    if (UNLIKELY(!((cname[0] >= 'A' && cname[0] <= 'Z') || (unsigned char)cname[0] >= 0x80)))
        return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %s", cname);
    for (const char *p = cname + 1; *p; p++)              /* reject '=', '?', etc. after the first char */
        if (UNLIKELY(!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || (unsigned char)*p >= 0x80)))
            return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %s", cname);
    { RESULT fr = korb_check_def_frozen(c, slots, VALUE_REF_GET(self)); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }   /* const_set on a frozen module → FrozenError */
    /* The warning below allocates, so keep BOTH the value and the owner in
     * scanned slots and re-read them afterwards (a bare VALUE goes stale). */
    slots[0] = VALUE_SLICE_GET(a, 1);
    slots[1] = KORB_CLASS_P(VALUE_REF_GET(self)) ? VALUE_REF_GET(self) : KORB_NIL;   /* cowner */
    if (UNLIKELY(korb_const_index_owned(c->vm, id, slots[1]) != UINT32_MAX))
        korb_warn_const_redef(c, slots + 2, id, slots[1]);   /* CRuby warns on reassignment */
    korb_const_define_owned(c, id, slots[0], slots[1]);   /* libc realloc only → no GC move */
    const VALUE owner = VALUE_REF_GET(self);              /* re-read: the warning may have moved it */
    if (UNLIKELY(KORB_CLASS_P(owner) && korb_mod_hook_custom(c, owner, korb_intern(c->vm, "const_added", 11)))) {
        slots[1] = VALUE_REF_GET(self); slots[2] = ID2SYM(id);   /* slots[0] still roots the value */
        const RESULT hr = korb_send(c, slots + 3, korb_intern(c->vm, "const_added", 11), 0, 1);
        if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        return RESULT_OK(slots[0]);
    }
    return RESULT_OK(slots[0]);
}
/* A class variable name must be @@-prefixed; otherwise NameError (CRuby), with
 * #name (the given name verbatim) and #receiver (self) attached. */
static RESULT korb_cvar_name_check(CTX *c, VALUE *slots, uint32_t id, VALUE self, VALUE orig_name) {
    const char *const nm = korb_sym_name(c->vm, id);
    if (LIKELY(nm[0] == '@' && nm[1] == '@')) return RESULT_OK(KORB_NIL);
    slots[0] = self; slots[1] = orig_name;
    RESULT ne = korb_raise(c, slots + 2, KORB_E_NAME, 0, "'%s' is not allowed as a class variable name", nm);
    if (LIKELY(KORB_EXC_P(ne.value))) {
        slots[2] = ne.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[2]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__name", 7)), slots[1]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
        ne.value = slots[2];
    }
    return ne;
}
/* Module#class_variable_get(name) — name is :@@x / "@@x" (koruby keys cvars by
 * the full @@-prefixed symbol); searches self + ancestors, NameError if absent. */
static RESULT korb_m_class_cvar_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    { RESULT r = korb_cvar_name_check(c, slots, id, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    int32_t idx = -1;
    const VALUE owner = KORB_CLASS_P(cls) ? korb_cvar_owner(cls, ID2SYM(id), &idx) : KORB_NIL;
    if (owner == KORB_NIL) {
        slots[0] = cls;                                   /* root the class across raise + ivar_set */
        RESULT ne = korb_raise(c, slots + 1, KORB_E_NAME, 0, "uninitialized class variable %s in %s",
                          korb_sym_name(c->vm, id), korb_type_name(cls));
        if (LIKELY(KORB_EXC_P(ne.value))) {               /* NameError#name → :@@x, #receiver → the class */
            slots[1] = ne.value;
            VALUE_REF eref = VALUE_REF_AT(&slots[1]);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__name", 7)), ID2SYM(id));
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
            ne.value = slots[1];
        }
        return ne;
    }
    return RESULT_OK(korb_items_data(VAL2HASH(VAL2CLASS(owner)->cvars)->items)[2 * idx + 1]);
}
/* Module#class_variable_set(name, val) — set on the defining ancestor if one
 * exists, else on the receiver (matches CRuby's rb_cvar_set). */
static RESULT korb_m_class_cvar_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    { RESULT r = korb_cvar_name_check(c, slots, id, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    return korb_cvar_set(c, slots, cls, KORB_UNDEF, id, VALUE_SLICE_GET(a, 1));   /* explicit receiver: cref = cls */
}
static RESULT korb_m_hash_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself);   /* fwd (hash.c) */
/* Module#remove_class_variable(name) → removes the class variable from self
 * (not ancestors) and returns its value; NameError if not defined on self. */
static RESULT korb_m_class_remove_cvar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    { RESULT r = korb_cvar_name_check(c, slots, id, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    const VALUE cvars = VAL2CLASS(cls)->cvars;
    if (!KORB_HASH_P(cvars) || korb_hash_find(VAL2HASH(cvars), ID2SYM(id)) < 0) {
        const char *cname = VAL2CLASS(cls)->name_sym ? korb_sym_name(c->vm, VAL2CLASS(cls)->name_sym) : korb_type_name(cls);
        return korb_raise(c, slots, KORB_E_NAME, 0, "class variable %s not defined for %s", korb_sym_name(c->vm, id), cname);
    }
    slots[0] = cvars;
    slots[1] = ID2SYM(id);
    return korb_m_hash_delete(c, slots + 2, VALUE_REF_AT(&slots[0]), VALUE_SLICE_MAKE(&slots[1], 1), NULL, NULL, NULL);
}
/* Module#class_variable_defined?(name) → true if defined on self or an ancestor. */
static RESULT korb_m_class_cvar_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    { RESULT r = korb_cvar_name_check(c, slots, id, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    int32_t idx = -1;
    const VALUE owner = KORB_CLASS_P(cls) ? korb_cvar_owner(cls, ID2SYM(id), &idx) : KORB_NIL;
    return RESULT_OK(owner != KORB_NIL ? KORB_TRUE : KORB_FALSE);
}
/* Module#class_variables(inherit = true) → symbols of the class variables of
 * self (and ancestors unless inherit is false), each listed once. */
static RESULT korb_m_class_cvars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool inherit = (VALUE_SLICE_LEN(a) < 1) || KORB_TRUTHY(VALUE_SLICE_GET(a, 0));
    slots[0] = UNWRAP(korb_ary_new(c, slots + 2, 8));   /* result (rooted at slots[0]) */
    slots[1] = VALUE_REF_GET(self);                      /* current class (rooted; push may move it) */
    while (KORB_CLASS_P(slots[1])) {
        /* re-derive the cvars hash from the rooted class each iteration: a push
         * below may GC/move both the class and its hash. len never shrinks, so a
         * plain index walk is stable; keys are immediate Symbols (no rooting). */
        for (uint32_t i = 0; VAL2CLASS(slots[1])->cvars != KORB_NIL && i < VAL2HASH(VAL2CLASS(slots[1])->cvars)->len; i++) {
            const VALUE key = korb_items_data(VAL2HASH(VAL2CLASS(slots[1])->cvars)->items)[2 * i];
            bool dup = false;                            /* de-dup across ancestors */
            for (uint32_t j = 0; j < VAL2ARY(slots[0])->len; j++)
                if (korb_items_data(VAL2ARY(slots[0])->items)[j] == key) { dup = true; break; }
            if (!dup) CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), key));
        }
        if (!inherit) break;
        slots[1] = VAL2CLASS(slots[1])->superclass;
    }
    return RESULT_OK(slots[0]);
}
/* Module#remove_method(sym...) — drop the named method(s) from THIS class (a
 * sentinel mid retires the slot; lookup then falls through to ancestors). Raises
 * NameError if a name isn't defined on the class itself. */
static RESULT korb_m_class_remove_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    for (uint32_t ai = 0; ai < VALUE_SLICE_LEN(a); ai++) {
        uint32_t mid;   /* Symbol/String, or #to_str-coercible — checked BEFORE frozen-ness (CRuby order) */
        { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, ai), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
        { RESULT fr = korb_check_def_frozen(c, slots, VALUE_REF_GET(self)); if (UNLIKELY(fr.state != KORB_NORMAL)) return fr; }
        KorbClass *const k = VAL2CLASS(VALUE_REF_GET(self));   /* re-read: the coercion may have GC-moved the class */
        bool found = false;
        for (uint32_t i = 0; i < k->method_cnt; i++)
            if (k->methods[i]->mid == mid) { k->methods[i]->mid = UINT32_MAX; found = true; break; }
        if (!found) {
            char cnm[256]; korb_class_desc_into(c, VALUE_REF_GET(self), cnm, sizeof cnm);
            return korb_raise(c, slots, KORB_E_NAME, 0, "method '%s' not defined in %s", korb_sym_name(c->vm, mid), cnm);
        }
        c->vm->method_serial++;
        slots[0] = VALUE_REF_GET(self);         /* the hook is Ruby code: re-root the class */
        CHECK(korb_fire_def_hook(c, slots + 1, slots[0], mid, "method_removed", 14));
    }
    c->vm->method_serial++;                     /* method table changed → flush caches */
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Module#undef_method(sym...) — prevent the named method(s) from this class.
 * Approximated as remove-if-present (no inherited-block marker); a name absent
 * from this class is tolerated so it doesn't re-block the file. */
/* The class name CRuby's undef/print_undef reports: the singleton class of a
 * class/module is named by that class ("String"), anything else by its #to_s. */
static void korb_undef_class_desc(CTX *c, VALUE cls, char *out, size_t outsz) {
    VALUE named = cls;
    if (VAL2CLASS(cls)->is_singleton)
        for (uint32_t i = 0; i < c->vm->sklass_cnt; i++)
            if (c->vm->sklass_cls[i] == cls) {
                if (KORB_CLASS_P(c->vm->sklass_obj[i])) named = c->vm->sklass_obj[i];
                break;
            }
    char *b = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&b, &sz);
    if (ms) { korb_fprint_class_tostr(c, ms, named); fclose(ms); }
    snprintf(out, outsz, "%s", b ? b : "");
    free(b);
}
static RESULT korb_m_class_undef_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(!KORB_CLASS_P(VALUE_REF_GET(self)))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    for (uint32_t ai = 0; ai < VALUE_SLICE_LEN(a); ai++) {
        uint32_t mid;                                 /* Symbol/String, or #to_str-coercible (bad type is checked before frozen-ness, CRuby order) */
        { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, ai), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
        const VALUE cls = VALUE_REF_GET(self);        /* re-read: the coercion may have GC-moved the class */
        KorbClass *const k = VAL2CLASS(cls);
        KORB_CHECK_FROZEN(c, slots, cls);             /* undef_method on a frozen class → FrozenError (even for a missing name) */
        VALUE mdef = KORB_NIL;                         /* NameError only when the method exists nowhere (self/ancestors/global/Object/intrinsic) */
        if (UNLIKELY(korb_class_find_method(cls, mid, &mdef) == NULL && korb_method_lookup(c->vm, mid) == NULL)) {
            /* to_s/inspect/!~/<=>/! are koruby-intrinsic universal methods (no
             * table entry); ==/===/hash/eql? live on Object.  undef_method of an
             * inherited/intrinsic method is legal (delegate.rb undefs exactly
             * these on a duped Kernel) — only a genuinely unknown name raises. */
            const VALUE objc = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
            const bool on_object = KORB_CLASS_P(objc) && korb_class_find_method(objc, mid, NULL) != NULL;
            const char *const nm = korb_sym_name(c->vm, mid);
            const bool intrinsic = !strcmp(nm, "to_s") || !strcmp(nm, "inspect") || !strcmp(nm, "!~") ||
                                   !strcmp(nm, "<=>") || !strcmp(nm, "!") || !strcmp(nm, "!=");
            if (!on_object && !intrinsic)
            {   char cnm[256]; korb_undef_class_desc(c, cls, cnm, sizeof cnm);
                return korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for %s '%s'",
                                  korb_sym_name(c->vm, mid), k->is_module ? "module" : "class", cnm); }
        }
        korb_class_undef_slot(k, cls, mid);
        c->vm->method_serial++;
        slots[0] = cls;                                  /* the hook is Ruby code: re-root the class */
        CHECK(korb_fire_def_hook(c, slots + 1, slots[0], mid, "method_undefined", 16));
    }
    c->vm->method_serial++;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Module#undefined_instance_methods — the class's own undef tombstones. */
static RESULT korb_m_class_undefined_imethods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 4));
    const VALUE_REF res = VALUE_REF_AT(&slots[0]);
    if (!KORB_CLASS_P(VALUE_REF_GET(self))) return RESULT_OK(VALUE_REF_GET(res));
    const uint32_t n = VAL2CLASS(VALUE_REF_GET(self))->method_cnt;
    for (uint32_t i = 0; i < n; i++) {
        const struct korb_method *const m = VAL2CLASS(VALUE_REF_GET(self))->methods[i];   /* entries are immortal */
        if (m->kind == KORB_METHOD_UNDEF) CHECK(korb_ary_push_val(c, slots + 1, res, ID2SYM(m->mid)));
    }
    return RESULT_OK(VALUE_REF_GET(res));
}
/* Module#method_defined?(sym|str[, inherit]) — true if an instance method by
 * that name is defined on the class / its ancestors (koruby doesn't track
 * visibility, so any defined method counts; public_method_defined? aliases it). */
/* want: -1 = method_defined? (public or protected, not private); 0/1/2 = exact. */
static RESULT korb_method_defined_vis(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int want) {
    uint32_t mid;                                        /* Symbol/String, or #to_str-coercible */
    { RESULT r = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &mid); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    /* Optional 2nd arg `inherit` (default true): when false, search only the
     * class's own method table — ignoring superclasses AND included/prepended
     * modules (CRuby semantics). */
    const bool inherit = (VALUE_SLICE_LEN(a) < 2) || KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    if (!inherit) {
        if (!KORB_CLASS_P(cls)) return RESULT_OK(KORB_FALSE);
        const KorbClass *const k = VAL2CLASS(cls);
        for (uint32_t j = 0; j < k->method_cnt; j++) {
            const struct korb_method *const om = k->methods[j];
            if (om->mid != mid) continue;
            if (om->kind == KORB_METHOD_UNDEF) return RESULT_OK(KORB_FALSE);
            const bool ok = (want < 0) ? (om->visibility != 1) : (om->visibility == want);
            return RESULT_OK(ok ? KORB_TRUE : KORB_FALSE);
        }
        return RESULT_OK(KORB_FALSE);
    }
    VALUE mdef = KORB_NIL;
    const struct korb_method *const me = KORB_CLASS_P(cls) ? korb_class_find_method(cls, mid, &mdef) : NULL;
    if (me == NULL) {
        /* Kernel-private builtins (global table) are private methods every class
         * inherits via Kernel: private_method_defined? true, public/method false. */
        const struct korb_method *const gm = korb_method_lookup(c->vm, mid);
        if (gm != NULL && gm->kind == KORB_METHOD_BUILTIN)
            return RESULT_OK((want == 1) ? KORB_TRUE : KORB_FALSE);
        return RESULT_OK(KORB_FALSE);
    }
    const bool ok = (want < 0) ? (me->visibility != 1) : (me->visibility == want);
    return RESULT_OK(ok ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_class_method_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)           { return korb_method_defined_vis(c, slots, self, a, -1); }
static RESULT korb_m_class_public_method_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)    { return korb_method_defined_vis(c, slots, self, a, 0); }
static RESULT korb_m_class_private_method_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_method_defined_vis(c, slots, self, a, 1); }
static RESULT korb_m_class_protected_method_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_method_defined_vis(c, slots, self, a, 2); }
/* Module#const_get(sym|str) — consts are a flat (global) table here, so the
 * receiver's namespace is ignored; rightmost name resolves. */
/* a valid constant name: [A-Z][A-Za-z0-9_]* */
static bool korb_valid_const_name(const char *p, uint32_t len) {
    if (len == 0 || !(p[0] >= 'A' && p[0] <= 'Z')) return false;
    for (uint32_t i = 1; i < len; i++) {
        const unsigned char ch = (unsigned char)p[i];   /* non-ASCII (>=0x80) allowed: CRuby permits unicode identifier chars */
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch >= 0x80)) return false;
    }
    return true;
}
/* Module#__lexical_parent (private) — the class/module this one was defined
 * inside, nil at top level.  The prelude's autoload lookup walks it: a bare
 * constant is searched in the lexical scopes before the ancestors. */
static RESULT korb_m_mod_lexical_parent(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const VALUE k = VALUE_REF_GET(self);
    if (!KORB_CLASS_P(k)) return RESULT_OK(KORB_NIL);
    const VALUE e = VAL2CLASS(k)->enclosing;
    return RESULT_OK(KORB_CLASS_P(e) ? e : KORB_NIL);
}
/* Module#const_source_location(name, inherit = true) — [file, line] where the
 * constant was assigned, [] for one defined in C (no position recorded), nil
 * when the constant is not defined at all. */
static RESULT korb_m_mod_const_source_location(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    VALUE name = VALUE_SLICE_GET(a, 0);
    const bool inherit = !(VALUE_SLICE_LEN(a) >= 2 && !KORB_TRUTHY(VALUE_SLICE_GET(a, 1)));
    if (!SYMBOL_P(name) && !KORB_STRING_P(name)) {          /* #to_str, else TypeError */
        const uint32_t to_str = korb_intern(vm, "to_str", 6);
        if (!KORB_OBJECT_P(name) || !korb_responds_to(c, name, to_str))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, name));
        slots[0] = name;
        const RESULT r = korb_send(c, slots + 1, to_str, 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (UNLIKELY(!KORB_STRING_P(r.value)))            /* #to_str that is not a String */
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
        name = r.value;
    }
    const char *nm; uint32_t nlen;
    if (SYMBOL_P(name)) { nm = korb_sym_name(vm, SYM2ID(name)); nlen = (uint32_t)strlen(nm); }
    else { const KorbString *const ns = VAL2STR(name); nm = korb_strbuf_data(ns->buf); nlen = ns->len; }
    /* a String name may be scoped ("A::B" / "::Top"): walk to the owner of the
     * final segment and answer for that one */
    VALUE scope = VALUE_REF_GET(self);
    if (!SYMBOL_P(name)) {
        if (nlen >= 2 && nm[0] == ':' && nm[1] == ':') { nm += 2; nlen -= 2; scope = KORB_NIL; }
        for (;;) {
            uint32_t sep = 0;
            while (sep + 1 < nlen && !(nm[sep] == ':' && nm[sep + 1] == ':')) sep++;
            if (sep + 1 >= nlen) break;                    /* last segment */
            if (UNLIKELY(!korb_valid_const_name(nm, sep)))
                return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %.*s", (int)sep, nm);
            const uint32_t ssym = korb_intern(vm, nm, sep);
            uint32_t six = KORB_CLASS_P(scope) ? korb_const_index_owned(vm, ssym, scope) : UINT32_MAX;
            if (six == UINT32_MAX && KORB_CLASS_P(scope)) six = korb_const_in_ancestry(vm, scope, ssym);
            if (six == UINT32_MAX) six = korb_const_index_owned(vm, ssym, KORB_NIL);
            if (six == UINT32_MAX) return RESULT_OK(KORB_NIL);
            scope = vm->const_vals[six];
            nm += sep + 2; nlen -= sep + 2;
        }
    }
    if (UNLIKELY(!korb_valid_const_name(nm, nlen)))
        return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %.*s", (int)nlen, nm);
    const uint32_t sym = korb_intern(vm, nm, nlen);
    const VALUE owner = scope;
    uint32_t idx = korb_const_index_owned(vm, sym, owner);
    VALUE loc_owner = owner;
    if (idx == UINT32_MAX && inherit) {
        idx = korb_const_in_ancestry(vm, owner, sym);
        if (idx != UINT32_MAX) loc_owner = vm->const_owners[idx];
        else {                                             /* top-level (owner nil) constants */
            idx = korb_const_index_owned(vm, sym, KORB_NIL);
            if (idx != UINT32_MAX) loc_owner = KORB_NIL;
        }
    }
    if (idx == UINT32_MAX && !korb_autoload_registered_p(c, owner, sym)) return RESULT_OK(KORB_NIL);
    uint32_t fsym = 0, line = 0;
    /* read the table BEFORE allocating: loc_owner is a bare C local, so the
     * array below would leave it pointing at the pre-GC copy of the module */
    const bool have_loc = korb_const_get_loc(vm, sym, loc_owner, &fsym, &line);
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    if (!have_loc) return RESULT_OK(slots[0]);   /* defined in C → [] */
    const char *const f = korb_sym_name(vm, fsym);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, f, (uint32_t)strlen(f)));
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), LONG2FIX((korb_sword_t)line)));
    return RESULT_OK(slots[0]);
}
static RESULT korb_m_class_const_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    VALUE name = VALUE_SLICE_GET(a, 0);
    /* inherit (2nd arg, default true): false → search only the receiver's own
     * constants (no ancestors / no top-level fallback). */
    const bool inherit = !(VALUE_SLICE_LEN(a) >= 2 && !KORB_TRUTHY(VALUE_SLICE_GET(a, 1)));
    if (!SYMBOL_P(name) && !KORB_STRING_P(name)) {          /* coerce a name via #to_str */
        if (UNLIKELY(!korb_responds_to_coerce_p(c, slots, &name, korb_intern(vm, "to_str", 6))))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
        slots[0] = name;
        RESULT sr = korb_send(c, slots + 1, korb_intern(vm, "to_str", 6), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (UNLIKELY(!KORB_STRING_P(sr.value)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s to String", korb_type_name(VALUE_SLICE_GET(a, 0)));
        name = sr.value;
    }
    /* copy the name bytes off the movable String/interned name; resolution below
     * does no allocation (read-only const-table scans). */
    char buf[512]; uint32_t len;
    if (SYMBOL_P(name)) { const char *nm = korb_sym_name(vm, SYM2ID(name)); len = (uint32_t)strlen(nm); }
    else                { len = VAL2STR(name)->len; }
    if (len >= sizeof buf) len = sizeof buf - 1;
    if (SYMBOL_P(name)) memcpy(buf, korb_sym_name(vm, SYM2ID(name)), len);
    else                memcpy(buf, korb_strbuf_data(VAL2STR(name)->buf), len);
    buf[len] = 0;

    /* Only a String name is parsed for `::` scope separators; a Symbol name is
     * treated atomically (so :"A::B" / :"::A" are "wrong constant name"). */
    const bool is_sym = SYMBOL_P(VALUE_SLICE_GET(a, 0));
    const char *p = buf, *const end = buf + len;
    if (!is_sym && p + 2 <= end && p[0] == ':' && p[1] == ':') { p += 2; } /* leading :: → start at top-level */
    VALUE owner = (p != buf) ? KORB_NIL : VALUE_REF_GET(self);           /* nil owner = top-level namespace */
    const bool leading_top = (p != buf);
    if (UNLIKELY(p >= end))
        return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %s", buf);
    VALUE result = KORB_NIL;
    bool first = true;
    while (p < end) {
        const char *q = p;
        if (is_sym) q = end;                                             /* atomic: whole Symbol is one component */
        else while (q < end && !(q[0] == ':' && q + 1 < end && q[1] == ':')) q++;
        const uint32_t clen = (uint32_t)(q - p);
        if (UNLIKELY(!korb_valid_const_name(p, clen)))
            return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %s", buf);
        const uint32_t cid = korb_intern(vm, p, clen);
        uint32_t idx = UINT32_MAX;
        if (KORB_CLASS_P(owner)) {
            if (inherit) for (VALUE o = owner; KORB_CLASS_P(o) && idx == UINT32_MAX; o = VAL2CLASS(o)->superclass) {
                             idx = korb_const_index_owned(vm, cid, o);
                             const VALUE inc = VAL2CLASS(o)->included;   /* also search included modules */
                             if (idx == UINT32_MAX && inc != KORB_NIL)
                                 for (int32_t j = (int32_t)VAL2ARY(inc)->len - 1; j >= 0 && idx == UINT32_MAX; j--)
                                     if (KORB_CLASS_P(korb_items_data(VAL2ARY(inc)->items)[j]))
                                         idx = korb_const_index_owned(vm, cid, korb_items_data(VAL2ARY(inc)->items)[j]);
                         }
            else         idx = korb_const_index_owned(vm, cid, owner);
        }
        /* top-level fallback: for a leading `::`, or (with inherit) a first
         * component whose owner search missed — but never for inherit=false. */
        if (idx == UINT32_MAX && (leading_top || (inherit && first)))
            idx = korb_const_index_owned(vm, cid, KORB_NIL);
        if (idx == UINT32_MAX) {
            /* const_missing hook on the (original) receiver for a bare name. */
            if (first && !leading_top && q >= end) {
                const uint32_t cm = korb_intern(vm, "const_missing", 13);
                VALUE cmdef = KORB_NIL;
                if (KORB_CLASS_P(VALUE_REF_GET(self)) &&
                    korb_mcache_find(vm, korb_dispatch_class(c, VALUE_REF_GET(self)), cm, &cmdef)) {
                    slots[0] = VALUE_REF_GET(self);
                    slots[1] = ID2SYM(cid);
                    return korb_send_impl(c, slots + 2, cm, 0, 1, NULL, NULL, NULL);
                }
            }
            RESULT nr = korb_raise(c, slots, KORB_E_NAME, 0, "uninitialized constant %.*s", (int)clen, buf + (p - buf));
            if (LIKELY(KORB_EXC_P(nr.value))) {           /* NameError#name = the missing constant symbol */
                slots[0] = nr.value;
                korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_intern(vm, "@__name", 7)), ID2SYM(cid));
                nr.value = slots[0];
            }
            return nr;
        }
        if (UNLIKELY(vm->deprconst_cnt != 0)) {
            slots[0] = vm->const_vals[idx];                 /* park: the warning path allocates */
            korb_const_deprecated_warn(c, slots + 1, vm->const_owners[idx], cid);
            result = slots[0];
        } else result = vm->const_vals[idx];
        owner = result;                                     /* next component resolves within this */
        p = (q < end) ? q + 2 : end;
        first = false;
    }
    return RESULT_OK(result);
}
/* Module#remove_const(sym|str) → the removed value (flat table tombstone). */
static RESULT korb_m_class_remove_const(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t id;
    { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &id);   /* Symbol / String / #to_str */
      if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    struct korb_vm *const vm = c->vm;
    if (korb_autoload_registered_p(c, VALUE_REF_GET(self), id)) {   /* pending autoload → just unregister */
        slots[0] = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_intern(vm, "@__autoloads", 12)));
        slots[1] = ID2SYM(id);
        const RESULT dr = korb_send(c, slots + 2, korb_intern(vm, "delete", 6), 0, 1);
        if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
        vm->const_serial++;
        return RESULT_OK(KORB_NIL);
    }
    /* only a constant defined DIRECTLY in this module can be removed; the
     * top-level ones (owner nil) belong to Object. */
    const VALUE selfv = VALUE_REF_GET(self);
    const VALUE objc = korb_builtin_class_obj(vm, KORB_C_OBJECT);
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == id &&
            (vm->const_owners[i] == selfv || (vm->const_owners[i] == KORB_NIL && selfv == objc))) {
            slots[0] = vm->const_vals[i];      /* park: the deprecation warning allocates */
            korb_const_deprecated_warn(c, slots + 1, vm->const_owners[i], id);
            vm->const_names[i] = 0;            /* tombstone (interned ids are >0) */
            vm->const_vals[i] = KORB_NIL;
            vm->method_serial++;
            vm->const_serial++;               /* invalidate const caches */
            return RESULT_OK(slots[0]);
        }
    return korb_raise(c, slots, KORB_E_NAME, 0, "constant %s not defined", korb_sym_name(vm, id));
}
/* Module#const_defined?(sym|str) — flat table membership. */
/* Module#private_constant(*names) / #public_constant(*names). */
static RESULT korb_mod_const_vis(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, bool private_p) {
    const VALUE owner = VALUE_REF_GET(self);
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        uint32_t sym;
        { RESULT nr = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, i), &sym); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
        /* the constant must be defined in self — an inherited one is a NameError */
        if (UNLIKELY(korb_const_index_owned(c->vm, sym, owner) == UINT32_MAX &&
                     !korb_autoload_registered_p(c, owner, sym))) {
            char qn[256]; korb_class_desc_into(c, owner, qn, sizeof qn);
            return korb_raise(c, slots, KORB_E_NAME, 0, "constant %s::%s not defined", qn, korb_sym_name(c->vm, sym));
        }
        korb_const_set_private(c, owner, sym, private_p);
    }
    return RESULT_OK(owner);
}
static RESULT korb_m_mod_private_constant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mod_const_vis(c, slots, self, a, true);
}
static RESULT korb_m_mod_public_constant(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    return korb_mod_const_vis(c, slots, self, a, false);
}
static RESULT korb_m_class_const_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    VALUE name = VALUE_SLICE_GET(a, 0);
    if (!SYMBOL_P(name) && !KORB_STRING_P(name)) {         /* coerce a non-Symbol/String name via #to_str */
        const uint32_t to_str = korb_intern(vm, "to_str", 6);
        if (KORB_OBJECT_P(name) && korb_responds_to_coerce_p(c, slots, &name, to_str)) {
            slots[0] = name;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (UNLIKELY(!KORB_STRING_P(sr.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, 0)));
            name = sr.value;
        } else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
    }
    uint32_t id;
    if (SYMBOL_P(name)) {
        id = SYM2ID(name);
        const char *const sn = korb_sym_name(vm, id);      /* a Symbol name is atomic — validate the whole thing */
        if (UNLIKELY(!korb_valid_const_name(sn, (uint32_t)strlen(sn))))
            return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %s", sn);
        if (korb_autoload_registered_p(c, VALUE_REF_GET(self), id)) return RESULT_OK(KORB_TRUE);
    }
    else if (KORB_STRING_P(name)) {
        const KorbString *const s = VAL2STR(name);
        const char *p = korb_strbuf_data(s->buf); uint32_t len = s->len;
        /* scoped name "A::B::C" / leading "::Top": flat model resolves the final
         * segment (and requires every intermediate segment to also be defined). */
        if (len >= 2 && p[0] == ':' && p[1] == ':') { p += 2; len -= 2; }
        uint32_t seg = 0;
        for (uint32_t i = 0; i <= len; i++) {
            if (i == len || (i + 1 < len && p[i] == ':' && p[i + 1] == ':')) {
                if (i == seg) return RESULT_OK(KORB_FALSE);   /* empty segment */
                if (UNLIKELY(!korb_valid_const_name(p + seg, i - seg)))
                    return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %.*s", (int)(i - seg), p + seg);
                const uint32_t sid = korb_intern(vm, p + seg, i - seg);
                bool found = korb_autoload_registered_p(c, VALUE_REF_GET(self), sid);   /* a pending autoload counts */
                for (uint32_t k = 0; !found && k < vm->const_cnt; k++) if (vm->const_names[k] == sid) found = true;
                if (!found) return RESULT_OK(KORB_FALSE);
                i++; seg = i + 1;                             /* skip the second ':' */
            }
        }
        return RESULT_OK(KORB_TRUE);
    }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
    /* owner-aware: the receiver + (unless inherit=false) its ancestors, then a
     * top-level fallback for inherited/Object constants. */
    const bool inherit = (VALUE_SLICE_LEN(a) < 2) || KORB_TRUTHY(VALUE_SLICE_GET(a, 1));   /* coerce the inherit flag to a boolean (nil/false → no inherit) */
    const VALUE owner = VALUE_REF_GET(self);
    if (KORB_CLASS_P(owner)) {
        if (korb_const_index_owned(vm, id, owner) != UINT32_MAX) return RESULT_OK(KORB_TRUE);
        if (!inherit) return RESULT_OK(KORB_FALSE);
        /* the whole ancestry (superclasses AND included/prepended modules) */
        if (korb_const_in_ancestry(vm, owner, id) != UINT32_MAX) return RESULT_OK(KORB_TRUE);
    }
    /* then the top-level constants (owner nil), which every class inherits from
     * Object — but NOT constants owned by some unrelated namespace. */
    return RESULT_OK(korb_const_index_owned(vm, id, KORB_NIL) != UINT32_MAX ? KORB_TRUE : KORB_FALSE);
}
/* Object#then / yield_self — yield self, return the block's value (no block → self). */
static RESULT korb_m_obj_then(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = VALUE_REF_GET(self);                   /* rooted across the yield */
    return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
}
/* Kernel#loop — yield with no args forever; `break` ends it (and is the value).
 * (StopIteration-based termination isn't modelled; koruby has no such class.) */
static RESULT korb_m_loop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (UNLIKELY(block == NULL)) {                        /* no block → an Enumerator */
        slots[0] = VALUE_REF_GET(self);
        slots[1] = ID2SYM(korb_intern(c->vm, "loop", 4));
        RESULT er = korb_send(c, slots + 2, korb_intern(c->vm, "__to_enum_sized", 15), 0, 1);
        if (LIKELY(er.state == KORB_NORMAL) && KORB_ENUM_P(er.value)) VAL2ENUM(er.value)->size_inf = 1;   /* loop.size → Infinity */
        return er;
    }
    uint32_t tick = 0;
    for (;;) {
        RESULT r = korb_block_yield(c, slots, block, def_env, NULL, 0, cself);
        if (r.state == KORB_NORMAL) {
            /* same preemption point the while nodes have: a spinning `loop` must
             * not starve the other green threads */
            if (UNLIKELY(c->vm->runq_head != NULL || c->vm->blop_npending != 0) &&
                (++tick & 63u) == 0) CHECK(korb_loop_yield(c, slots));
            continue;
        }
        if (r.state == KORB_BREAK && korb_break_owned(c, block, def_env)) return RESULT_OK(r.value);   /* break [v] → loop value (only ours) */
        if (r.state == KORB_RAISE && KORB_EXC_P(r.value) && VAL2EXC(r.value)->etype == KORB_E_STOP_ITERATION)
            return RESULT_OK(KORB_NIL);                         /* StopIteration ends the loop */
        return r;
    }
}
/* Object#tap — yield self, return self. */
static RESULT korb_m_obj_tap(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block != NULL) {
        slots[0] = VALUE_REF_GET(self);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Kernel#lambda / #proc — turn the given block into a Proc (lambda-flavoured for
 * #lambda).  No block → ArgumentError. */
static RESULT korb_m_kernel_makeproc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, uint32_t is_lambda) {
    (void)a; (void)self;
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create Proc object without a block");
    VALUE *const denv = (VALUE *)((uintptr_t)def_env & ~(uintptr_t)1u);   /* block-arg def_env arrives tagged (base|1) */
    return korb_make_proc(c, slots, block, denv, KORB_CSELF_VAL(cself), is_lambda);
}
static RESULT korb_m_kernel_lambda(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) { return korb_m_kernel_makeproc(c, slots, self, a, block, def_env, cself, 1); }
static RESULT korb_m_kernel_proc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself)   { return korb_m_kernel_makeproc(c, slots, self, a, block, def_env, cself, 0); }

/* instance_exec(*args) { |*args| ... } — run the block with self rebound to the
 * receiver; the block's lexical env (def_env) is preserved so closures still work.
 * Method definitions inside (singleton def) are NOT redirected to the receiver. */
/* CRuby keeps the bare-`private`/`public`/`module_function` default on the
 * cref, so a class_eval/module_eval body starts public and its changes don't
 * leak back out.  koruby stores it on the class, hence the save/reset pair. */
#define KORB_VIS_NONE 0xffu
static uint8_t korb_vis_enter(VALUE cls) {
    if (!KORB_CLASS_P(cls)) return KORB_VIS_NONE;
    const uint8_t sv = VAL2CLASS(cls)->cur_visibility;
    VAL2CLASS(cls)->cur_visibility = 0;
    return sv;
}
static void korb_vis_leave(VALUE cls, uint8_t saved) {
    if (saved != KORB_VIS_NONE && KORB_CLASS_P(cls)) VAL2CLASS(cls)->cur_visibility = saved;
}

static RESULT korb_obj_exec_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool singleton_definee) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    const uint32_t argc = VALUE_SLICE_LEN(a);
    if (block == KORB_BLK_CPROC) {                       /* forwarded Symbol/Method#to_proc: fixed binding, self-rebind is moot */
        for (uint32_t i = 0; i < argc; i++) slots[i] = VALUE_SLICE_GET(a, i);
        return korb_block_yield(c, slots + argc, block, def_env, slots, argc, cself);
    }
    slots[0] = VALUE_REF_GET(self);                      /* new self = receiver (rooted self cell) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    /* same default definee as instance_eval: `def` lands on the singleton */
    const VALUE saved_definee = c->def_definee;
    if (singleton_definee) {
        /* a value with no singleton is left as the definee itself: `def` inside
         * raises then, not on entry (CRuby only fails at the definition) */
        if (korb_singleton_able(slots[0])) {
            const RESULT sr = korb_obj_singleton(c, slots + 1 + argc, slots[0]);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            c->def_definee = sr.value;
        } else c->def_definee = slots[0];
    } else c->def_definee = slots[0];              /* class_exec/module_exec: the class itself */
    const uint8_t saved_vis = korb_vis_enter(slots[0]);
    /* @@vars keep resolving against the block's definition scope, which self no
     * longer names once the receiver is bound in. */
    const VALUE saved_cvar_cref = c->cvar_cref;
    if (singleton_definee && cself != NULL)                /* a forwarded Proc carries its own captured self */
        c->cvar_cref = korb_cvar_self_class_pub(c, def_env == KORB_BLK_FWD || block == KORB_BLK_CPROC
                                                   ? VAL2PROC(*cself)->self : KORB_CSELF_VAL(cself));
    RESULT r;
    if (def_env == KORB_BLK_FWD) {                        /* forwarded Proc: keep ITS closure env, rebind self only */
        slots[1 + argc] = VAL2PROC(*cself)->env;          /* proc's captured env (used as def_env below, not FWD) */
        r = korb_block_yield(c, slots + 2 + argc, block, (VALUE *)(uintptr_t)slots[1 + argc], &slots[1], argc, &slots[0]);
    } else {
        r = korb_block_yield(c, slots + 1 + argc, block, def_env, &slots[1], argc, &slots[0]);
    }
    c->cvar_cref = saved_cvar_cref;
    korb_vis_leave(slots[0], saved_vis);
    c->def_definee = saved_definee;
    return r;
}
static RESULT korb_m_obj_instance_exec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_obj_exec_impl(c, slots, self, a, block, def_env, cself, true);
}
static RESULT korb_m_mod_class_exec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_obj_exec_impl(c, slots, self, a, block, def_env, cself, false);
}

/* instance_eval { |obj| ... } — like instance_exec but with the receiver as the
 * sole block argument (CRuby passes self).  The String form is not supported. */
static RESULT korb_obj_eval_impl(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself, bool singleton_definee) {
    (void)cself;
    if (UNLIKELY(block == NULL)) {
        VALUE *bind_ptr = NULL;              /* hidden caller binding appended by the parser (caller locals) */
        if (VALUE_SLICE_LEN(a) >= 2 && KORB_BINDING_P(VALUE_SLICE_GET(a, VALUE_SLICE_LEN(a) - 1))) {
            bind_ptr = &a.p[VALUE_SLICE_LEN(a) - 1];   /* stays rooted in the arg slots */
            a = VALUE_SLICE_MAKE(a.p, VALUE_SLICE_LEN(a) - 1);
        }
        if (VALUE_SLICE_LEN(a) == 0 || VALUE_SLICE_LEN(a) > 3)   /* no block: 1..3 args (CRuby) */
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..3)",
                              VALUE_SLICE_LEN(a));
        /* String form: eval the source with self = the receiver, so `def` in
         * class_eval/module_eval attaches to the class (self is a Class there),
         * and instance_eval's code sees the receiver as self.  2nd/3rd args
         * (filename, lineno) are accepted and ignored. */
        VALUE src = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_STRING_P(src))) {
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(src) && korb_responds_to_coerce_p(c, slots, &src, to_str)) {
                slots[0] = VALUE_REF_GET(self);              /* root receiver across the dispatch */
                slots[1] = src;
                RESULT sr = korb_send_impl(c, slots + 2, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (KORB_STRING_P(sr.value)) { VALUE_REF_SET(VALUE_SLICE_REF(a, 0), sr.value); src = sr.value; }
            }
            if (UNLIKELY(!KORB_STRING_P(src)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        }
        /* optional 2nd/3rd args: the filename and first line the source is
         * reported as (CRuby uses them for __FILE__ / __LINE__ / backtraces) */
        char fbuf[256]; const char *fname = "(eval)"; int32_t line = 1;
        if (VALUE_SLICE_LEN(a) >= 2) {
            slots[0] = VALUE_SLICE_GET(a, 1);
            if (!KORB_STRING_P(slots[0])) {
                const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
                if (!KORB_OBJECT_P(slots[0]) || !korb_responds_to(c, slots[0], to_str))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
                const RESULT fr = korb_send(c, slots + 1, to_str, 0, 0);
                if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
                if (!KORB_STRING_P(fr.value))
                    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
                slots[0] = fr.value;
            }
            const KorbString *const fs = VAL2STR(slots[0]);
            const uint32_t n = fs->len < sizeof fbuf - 1 ? fs->len : (uint32_t)(sizeof fbuf - 1);
            memcpy(fbuf, korb_strbuf_data(fs->buf), n); fbuf[n] = '\0';
            fname = korb_sym_name(c->vm, korb_intern(c->vm, fbuf, n));   /* interned → outlives this frame, like the AST */
        }
        if (VALUE_SLICE_LEN(a) >= 3) {
            slots[0] = VALUE_SLICE_GET(a, 2);
            CHECK(korb_coerce_to_int_pub(c, slots + 1, &slots[0]));   /* #to_int is honoured */
            korb_sword_t l = 1;
            if (!korb_to_index(slots[0], &l))
                return korb_raise_no_int(c, slots, VALUE_SLICE_GET(a, 2));
            line = (int32_t)l;
        }
        /* the eval'd string runs under a cref: instance_eval → the receiver's
         * singleton class, class_eval/module_eval → the module itself */
        VALUE cref = KORB_NIL;
        if (singleton_definee) {
            if (korb_singleton_able(VALUE_REF_GET(self))) {
                slots[0] = src;                            /* park across the singleton's alloc */
                const RESULT sr = korb_obj_singleton(c, slots + 1, VALUE_REF_GET(self));
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                cref = sr.value; src = slots[0];
            }
        } else if (KORB_CLASS_P(VALUE_REF_GET(self))) cref = VALUE_REF_GET(self);
        const uint8_t saved_vis = korb_vis_enter(VALUE_REF_GET(self));
        /* @@vars in the source resolve against the CALLER's class, which the
         * hidden caller binding is the only record of here. */
        const VALUE saved_cvar_cref = c->cvar_cref;
        if (singleton_definee && bind_ptr)
            c->cvar_cref = korb_cvar_self_class_pub(c, VAL2BIND(*bind_ptr)->self);
        RESULT er;
        if (bind_ptr) {                                    /* caller binding → its locals are visible + written back */
            slots[0] = src;
            slots[1] = VALUE_REF_GET(self);
            slots[2] = cref;
            er = korb_eval_binding_core(c, slots + 3, &slots[0], bind_ptr, fname, line, &slots[1], slots[2]);
        } else {
            er = korb_eval_str_self(c, slots, src, VALUE_REF_GET(self), fname, line, cref);
        }
        c->cvar_cref = saved_cvar_cref;
        korb_vis_leave(VALUE_REF_GET(self), saved_vis);
        return er;
    }
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 0))                     /* a block AND positional args → ArgumentError (CRuby) */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0)", VALUE_SLICE_LEN(a));
    slots[0] = VALUE_REF_GET(self);
    /* default definee: instance_eval → the receiver's SINGLETON class (a `def`
     * there is a singleton method); class_eval/module_eval → the class itself
     * (a plain instance method).  Restored on the way out. */
    const VALUE saved_definee = c->def_definee;
    if (singleton_definee) {
        if (korb_singleton_able(slots[0])) {       /* see korb_obj_exec_impl: `def` fails lazily */
            const RESULT sr = korb_obj_singleton(c, slots + 1, slots[0]);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            c->def_definee = sr.value;
        } else c->def_definee = slots[0];
    } else c->def_definee = slots[0];
    const uint8_t saved_vis = korb_vis_enter(slots[0]);
    const VALUE saved_cvar_cref = c->cvar_cref;           /* @@vars: the block's definition scope (see korb_obj_exec_impl) */
    if (singleton_definee && cself != NULL)                /* a forwarded Proc carries its own captured self */
        c->cvar_cref = korb_cvar_self_class_pub(c, def_env == KORB_BLK_FWD || block == KORB_BLK_CPROC
                                                   ? VAL2PROC(*cself)->self : KORB_CSELF_VAL(cself));
    RESULT r;
    if (block == KORB_BLK_CPROC)                          /* forwarded C-proc: fixed binding */
        r = korb_block_yield(c, slots + 1, block, def_env, slots, 1, cself);
    else {
        slots[1] = slots[0];                             /* arg0 = receiver */
        if (def_env == KORB_BLK_FWD) {                    /* forwarded Proc: keep ITS closure env, rebind self only */
            slots[2] = VAL2PROC(*cself)->env;
            r = korb_block_yield(c, slots + 3, block, (VALUE *)(uintptr_t)slots[2], &slots[1], 1, &slots[0]);
        } else {
            r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, &slots[0]);
        }
    }
    c->cvar_cref = saved_cvar_cref;
    korb_vis_leave(slots[0], saved_vis);
    c->def_definee = saved_definee;
    return r;
}

static RESULT korb_m_obj_instance_eval(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_obj_eval_impl(c, slots, self, a, block, def_env, cself, true);
}
static RESULT korb_m_mod_class_eval(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    return korb_obj_eval_impl(c, slots, self, a, block, def_env, cself, false);
}

/* Exception#message / to_s — the stored message, or the class name if none. */
static RESULT korb_m_exc_cause(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const VALUE v = VALUE_REF_GET(self);
    return RESULT_OK(KORB_EXC_P(v) ? VAL2EXC(v)->cause : KORB_NIL);
}
static RESULT korb_m_exc_backtrace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const VALUE v = VALUE_REF_GET(self);
    return RESULT_OK(KORB_EXC_P(v) ? VAL2EXC(v)->backtrace : KORB_NIL);
}
/* NameError#name → the missing constant/method/variable name (a Symbol), or nil. */
static RESULT korb_m_exc_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_exc_ivar_get(VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__name", 7))));
}
/* NameError#receiver / NoMethodError#receiver → the object the failed call/lookup
 * targeted.  Raises ArgumentError (like CRuby) when no receiver was recorded. */
/* Tag a NameError with the missing name and the module it was looked up in. */
void korb_name_error_where(CTX *c, VALUE *slots, VALUE *excp, uint32_t name, VALUE recv) {
    slots[0] = recv;                                  /* root: the ivar sets allocate */
    korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(excp), ID2SYM(korb_intern(c->vm, "@__name", 7)), ID2SYM(name));
    korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(excp), ID2SYM(korb_intern(c->vm, "@__receiver", 11)), slots[0]);
    korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(excp), ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
}
static RESULT korb_m_exc_receiver(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE r = korb_exc_ivar_get(VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__receiver", 11)));
    if (r == KORB_NIL && korb_exc_ivar_get(VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__has_recv", 11))) != KORB_TRUE)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver is available");
    return RESULT_OK(r);
}
/* NoMethodError#args → the arguments passed to the missing method (or []). */
static RESULT korb_m_nme_args(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE v = korb_exc_ivar_get(VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__args", 7)));
    return KORB_ARRAY_P(v) ? RESULT_OK(v) : korb_ary_new(c, slots, 0);
}
/* Exception#backtrace_locations — the Location Array a `raise obj, msg, locs`
 * stored; nil for a String backtrace or none at all (CRuby). */
static RESULT korb_m_exc_backtrace_locations(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const VALUE v = VALUE_REF_GET(self);
    if (!KORB_EXC_P(v)) return RESULT_OK(KORB_NIL);
    const VALUE bt = VAL2EXC(v)->backtrace;
    if (!KORB_ARRAY_P(bt)) return RESULT_OK(KORB_NIL);
    const KorbArray *const ary = VAL2ARY(bt);
    for (uint32_t i = 0; i < ary->len; i++)
        if (KORB_STRING_P(korb_items_data(ary->items)[i])) return RESULT_OK(KORB_NIL);
    return RESULT_OK(bt);
}
static RESULT korb_m_exc_set_backtrace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE v = VALUE_REF_GET(self);
    if (!KORB_EXC_P(v)) return RESULT_OK(KORB_NIL);
    VALUE bt = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    /* nil, a String (wrapped in a one-element Array), or an Array whose elements
     * are all Strings / Thread::Backtrace::Location. */
    if (KORB_STRING_P(bt)) {
        slots[0] = bt;                                    /* root across the Array alloc */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
        CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), slots[0]));
        bt = slots[1];
    } else if (KORB_ARRAY_P(bt)) {
        const KorbArray *const ary = VAL2ARY(bt);
        for (uint32_t i = 0; i < ary->len; i++) {
            const VALUE el = korb_items_data(ary->items)[i];
            if (KORB_STRING_P(el)) continue;
            if (KORB_OBJECT_P(el) && korb_responds_to(c, el, korb_intern(c->vm, "lineno", 6))) continue;
            return korb_raise(c, slots, KORB_E_TYPE, 0, "backtrace must be an Array of String or an Array of Thread::Backtrace::Location");
        }
    } else if (bt != KORB_NIL) {
        return korb_raise(c, slots, KORB_E_TYPE, 0, "backtrace must be an Array of String or an Array of Thread::Backtrace::Location");
    }
    slots[0] = bt;                                        /* the exception may have moved above */
    KorbException *const e = VAL2EXC(VALUE_REF_GET(self));
    ARO_STORE(c, e, &e->backtrace, slots[0]);
    return RESULT_OK(slots[0]);
}
/* The message as STORED (nil when never set) — Marshal dumps that, not the
 * class-name fallback #message produces. */
static RESULT korb_m_exc_raw_mesg(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)a;
    const VALUE v = VALUE_REF_GET(self);
    return RESULT_OK(KORB_EXC_P(v) ? VAL2EXC(v)->msg : KORB_NIL);
}
static RESULT korb_m_exc_message(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbException *e = VAL2EXC(VALUE_REF_GET(self));
    if (e->msg != KORB_NIL) {
        if (KORB_STRING_P(e->msg)) return RESULT_OK(e->msg);
        slots[0] = e->msg;                                /* non-String message → #to_s it */
        return korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
    }
    /* no explicit message → the class name (exc_class for an abstract/user class,
     * else the builtin etype name), matching CRuby's default.  Use the *qualified*
     * name so a namespaced exception reads "Mod::Err", not "Err". */
    if (e->exc_class != KORB_NIL && KORB_CLASS_P(e->exc_class)) {
        char qn[256]; korb_class_qname_into(c, e->exc_class, qn, sizeof qn);
        return korb_str_new(c, slots, qn, (uint32_t)strlen(qn));
    }
    const char *nm = korb_etype_name(e->etype);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
/* Exception#message → self.to_s (so a subclass overriding #to_s is honoured;
 * the default #to_s is korb_m_exc_message above, so no recursion). */
static RESULT korb_m_exc_message_via_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VALUE_REF_GET(self);
    return korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
}
/* Exception#inspect → "#<ClassName: message>" (or "#<ClassName>" if #to_s is
 * empty).  Uses the dispatched (overridable) #to_s for the message. */
static RESULT korb_m_exc_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE sv = VALUE_REF_GET(self);
    const KorbException *const e = VAL2EXC(sv);
    char cnbuf[256];
    if (e->exc_class != KORB_NIL && KORB_CLASS_P(e->exc_class))   /* qualified name (Mod::Err), not the bare Err */
        korb_class_qname_into(c, e->exc_class, cnbuf, sizeof cnbuf);
    else
        snprintf(cnbuf, sizeof cnbuf, "%s", korb_etype_name(e->etype));
    slots[0] = sv;
    RESULT tr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0, NULL, NULL, NULL);
    if (UNLIKELY(tr.state != KORB_NORMAL)) return tr;
    slots[0] = KORB_STRING_P(tr.value) ? tr.value : KORB_NIL; /* park the message string */
    const KorbString *const ms = KORB_STRING_P(slots[0]) ? VAL2STR(slots[0]) : NULL;
    const size_t mlen = ms ? ms->len : 0;
    const size_t need = strlen(cnbuf) + mlen + 8;
    char *const buf = malloc(need);
    if (!buf) abort();
    int n = (mlen == 0) ? snprintf(buf, need, "%s", cnbuf)      /* empty message → bare class name */
                        : snprintf(buf, need, "#<%s: %.*s>", cnbuf, (int)mlen, korb_strbuf_data(ms->buf));
    RESULT r = korb_str_new(c, slots + 1, buf, (uint32_t)n);
    free(buf);
    return r;
}
/* Exception#initialize([msg]) — stores msg (the super target for a user
 * exception's `def initialize(msg); super; end`). */
static RESULT korb_m_exc_initialize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    const VALUE sv = VALUE_REF_GET(self);
    if (KORB_EXC_P(sv) && VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL)
        ARO_STORE(c, VAL2EXC(sv), &VAL2EXC(sv)->msg, VALUE_SLICE_GET(a, 0));
    return RESULT_OK(sv);
}

