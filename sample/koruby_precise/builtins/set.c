/* koruby_precise — set.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Set (array-backed, unique by korb_value_eq) -------------------------- */
static bool korb_arr_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eql(ar->items->data[i], v)) return true;
    return false;
}
/* Set membership honouring compare_by_identity: an identity set compares members
 * by object identity (equal?), a normal set by eql? (korb_arr_has).  For immediate
 * values (Symbol/Integer/nil/true/false) the two coincide. */
static bool korb_set_member(const KorbSet *s, VALUE v) {
    const KorbArray *ar = VAL2ARY(s->elems);
    if (!s->by_identity) return korb_arr_has(ar, v);
    for (uint32_t i = 0; i < ar->len; i++) if (ar->items->data[i] == v) return true;
    return false;
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
    for (uint32_t i = 0; i < VAL2ARY(VALUE_REF_GET(src))->len; i++) {
        VALUE e = VAL2ARY(VALUE_REF_GET(src))->items->data[i];
        if (!korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 1, dst, e));
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
static RESULT korb_m_set_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_set_member(SELF_SET, VALUE_SLICE_GET(a, 0)) ? KORB_TRUE : KORB_FALSE); }
/* compare_by_identity — switch the set to identity comparison (returns self). */
static RESULT korb_m_set_cbi(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    VAL2SET(VALUE_REF_GET(self))->by_identity = 1;
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_cbi_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(SELF_SET->by_identity ? KORB_TRUE : KORB_FALSE); }
/* disjoint?(o): no shared elements.  intersect?(o): some shared element. */
static RESULT korb_m_set_disjoint(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE oe = korb_set_elems_of(VALUE_SLICE_GET(a, 0));   /* Set/enumerable → elems array */
    if (oe == KORB_NIL) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be enumerable");
    const KorbArray *const ot = VAL2ARY(oe);
    const KorbArray *const me = VAL2ARY(SELF_SET->elems);        /* re-read self after the line above (no GC since) */
    for (uint32_t i = 0; i < me->len; i++)
        if (korb_arr_has((KorbArray *)ot, me->items->data[i])) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_set_intersect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const RESULT r = korb_m_set_disjoint(c, slots, self, a);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(r.value == KORB_TRUE ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_set_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE v = VALUE_SLICE_GET(a, 0);
    if (!korb_set_member(SELF_SET, v)) { slots[0] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&slots[0]), v)); }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_add_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (korb_set_member(SELF_SET, VALUE_SLICE_GET(a, 0))) return RESULT_OK(KORB_NIL);
    return korb_m_set_add(c, slots, self, a);
}
static RESULT korb_m_set_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbArray *ar = VAL2ARY(SELF_SET->elems);
    VALUE v = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ar->len; i++)
        if (korb_value_eq(ar->items->data[i], v)) {
            for (uint32_t j = i; j + 1 < ar->len; j++) ARO_STORE(c, ar->items, &ar->items->data[j], ar->items->data[j+1]);
            ARO_STORE(c, ar->items, &ar->items->data[--ar->len], KORB_NIL);
            break;
        }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    for (uint32_t i = 0; ; i++) {
        const KorbArray *ar = VAL2ARY(SELF_SET->elems);
        if (i >= ar->len) break;
        slots[0] = ar->items->data[i];
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
        VALUE e = VAL2ARY(SELF_SET->elems)->items->data[i];
        bool ino = korb_arr_has(VAL2ARY(slots[0]), e);
        bool keep = op == 0 ? true : op == 1 ? ino : !ino;       /* union / inter / (diff|xor) */
        if (keep && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 2, dst, e));
    }
    if (op == 0 || op == 3) {
        for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
            VALUE e = VAL2ARY(slots[0])->items->data[i];
            bool inme = korb_arr_has(VAL2ARY(SELF_SET->elems), e);
            bool keep = op == 0 ? true : !inme;
            if (keep && !korb_arr_has(VAL2ARY(VALUE_REF_GET(dst)), e)) CHECK(korb_ary_push_val(c, slots + 2, dst, e));
        }
    }
    return korb_set_new(c, slots + 2, VALUE_REF_GET(dst));
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
            VALUE e = VAL2ARY(slots[0])->items->data[i];
            if (!korb_set_member(SELF_SET, e)) { slots[1] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), e)); }
        }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
/* Set#join(sep="") — delegate to the member Array's join. */
static RESULT korb_m_set_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t n = VALUE_SLICE_LEN(a);
    slots[0] = SELF_SET->elems;                            /* receiver = member Array */
    for (uint32_t i = 0; i < n; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);   /* forward args */
    return korb_send_impl(c, slots + 1, korb_intern(c->vm, "join", 4), 0, n, NULL, NULL, NULL);
}
static RESULT korb_set_rel(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int rel) {
    VALUE ov = korb_set_elems_of(VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(ov == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be a set");
    const KorbArray *me = VAL2ARY(SELF_SET->elems), *ot = VAL2ARY(ov);
    bool sub = true; for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, me->items->data[i])) { sub = false; break; }
    bool sup = true; for (uint32_t i = 0; i < ot->len; i++) if (!korb_arr_has(me, ot->items->data[i])) { sup = false; break; }
    bool t = rel == 0 ? sub : rel == 1 ? sup : rel == 2 ? (sub && me->len < ot->len) : (sup && me->len > ot->len);
    return RESULT_OK(t ? KORB_TRUE : KORB_FALSE);
}
/* Set#<=> — 0 equal, -1 proper subset, 1 proper superset, nil otherwise / non-Set. */
static RESULT korb_m_set_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots;
    const VALUE ov = VALUE_SLICE_GET(a, 0);
    if (!KORB_SET_P(ov)) return RESULT_OK(KORB_NIL);
    const KorbArray *me = VAL2ARY(SELF_SET->elems), *ot = VAL2ARY(VAL2SET(ov)->elems);
    bool sub = true; for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, me->items->data[i])) { sub = false; break; }
    bool sup = true; for (uint32_t i = 0; i < ot->len; i++) if (!korb_arr_has(me, ot->items->data[i])) { sup = false; break; }
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
    for (uint32_t i = 0; i < me->len; i++) if (!korb_arr_has(ot, me->items->data[i])) return RESULT_OK(KORB_FALSE);
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
        slots[1] = ar->items->data[i];
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
static RESULT korb_m_range_to_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = UNWRAP(korb_m_range_to_a(c, slots, self, a));
    return korb_set_from_array(c, slots + 1, VALUE_REF_AT(&slots[0]));
}

static RESULT korb_m_obj_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_to_s(c, ms, VALUE_REF_GET(self));   /* no GC inside */
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_obj_inspect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    char *buf = NULL; size_t sz = 0;
    FILE *ms = open_memstream(&buf, &sz);
    if (!ms) { fprintf(stderr, "koruby_precise: open_memstream failed\n"); abort(); }
    korb_fprint_inspect(c, ms, VALUE_REF_GET(self));
    fclose(ms);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)sz);
    free(buf);
    return r;
}
static RESULT korb_m_obj_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(korb_class_obj_of(c, VALUE_REF_GET(self)));
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
            for (uint32_t j = 0; j < pa->len; j++) if (pa->items->data[j] == target) return RESULT_OK(KORB_TRUE);
        }
        VALUE inc = VAL2CLASS(cls)->included;            /* included/extended modules count */
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (ia->items->data[j] == target) return RESULT_OK(KORB_TRUE);
        }
        cls = VAL2CLASS(cls)->superclass;
    }
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_obj_respond_to(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    struct korb_vm *const vm = c->vm;
    VALUE mv = VALUE_SLICE_GET(a, 0);
    uint32_t mid;
    if (SYMBOL_P(mv)) mid = SYM2ID(mv);
    else if (KORB_STRING_P(mv)) mid = korb_intern(vm, VAL2STR(mv)->buf->data, VAL2STR(mv)->len);
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(mv));
    VALUE sv = VALUE_REF_GET(self);
    if (korb_responds_to(c, sv, mid)) return RESULT_OK(KORB_TRUE);
    /* respond_to_missing?(name, include_private) fallback (pairs with method_missing). */
    const uint32_t rtm = korb_intern(vm, "respond_to_missing?", 19);
    const VALUE dcls = korb_dispatch_class(c, sv);
    VALUE rtm_def = KORB_NIL;
    if (KORB_CLASS_P(dcls) && korb_class_find_method(dcls, rtm, &rtm_def)) {
        slots[0] = sv;
        slots[1] = ID2SYM(mid);
        slots[2] = (VALUE_SLICE_LEN(a) >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_FALSE;
        return korb_send_impl(c, slots + 3, rtm, 0, 2, NULL, NULL, NULL);
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
    korb_klass_override_set(c, slots[0], slots[2]);                               /* obj/sing rooted, no GC in set */
    return RESULT_OK(slots[2]);
}
/* Object#singleton_class — the object's (lazily-created) singleton class. */
static RESULT korb_m_obj_singleton_class(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; VALUE sv = VALUE_REF_GET(self);
    if (UNLIKELY(!AROH_IS_GC_OBJECT(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't define singleton");   /* immediates */
    return korb_obj_singleton(c, slots, sv);
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
/* Object#extend(*mods) — mix the modules into the object's singleton class. */
static RESULT korb_m_obj_extend(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sv = VALUE_REF_GET(self);
    if (UNLIKELY(!AROH_IS_GC_OBJECT(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't define singleton");   /* immediates */
    slots[0] = sv;                                                               /* root self */
    slots[1] = UNWRAP(korb_obj_singleton(c, slots + 2, sv));                      /* singleton (rooted) */
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {                           /* include each module */
        slots[2] = VALUE_SLICE_GET(a, i);
        CHECK(korb_do_include(c, slots + 3, slots[1], VALUE_SLICE_MAKE(&slots[2], 1)));
    }
    return RESULT_OK(slots[0]);
}
/* Module#private/public/protected/module_function — koruby doesn't enforce
 * visibility, so these are no-ops returning the arg (for `private :foo` /
 * `private def foo`) or nil (bare `private`). */
static RESULT korb_m_visibility_noop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)self;
    return RESULT_OK(VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL);
}
/* runtime attr_reader/writer/accessor (the dynamic `attr_reader id` form that
 * the parser can't desugar at parse time; self is the class). */
static RESULT korb_m_class_attr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, int reader, int writer) {
    struct korb_vm *const vm = c->vm;
    VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls))) return korb_raise(c, slots, KORB_E_TYPE, 0, "attr on a non-class");
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        VALUE sym = VALUE_SLICE_GET(a, i);
        if (KORB_STRING_P(sym)) sym = ID2SYM(korb_intern(vm, VAL2STR(sym)->buf->data, VAL2STR(sym)->len));
        if (!SYMBOL_P(sym)) continue;
        const char *nm = korb_sym_name(vm, SYM2ID(sym));
        char buf[256]; snprintf(buf, sizeof buf, "@%s", nm); uint32_t ivar = korb_intern(vm, buf, strlen(buf));
        if (reader) korb_class_def_attr(c, cls, korb_intern(vm, nm, strlen(nm)), ivar, 0);
        if (writer) { snprintf(buf, sizeof buf, "%s=", nm); korb_class_def_attr(c, cls, korb_intern(vm, buf, strlen(buf)), ivar, 1); }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_class_attr_reader(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_m_class_attr(c, slots, self, a, 1, 0); }
static RESULT korb_m_class_attr_writer(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { return korb_m_class_attr(c, slots, self, a, 0, 1); }
static RESULT korb_m_class_attr_accessor(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_class_attr(c, slots, self, a, 1, 1); }
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
/* Module#name → the class/module name (a frozen String), nil if anonymous. */
static RESULT korb_m_class_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t name_sym = VAL2CLASS(VALUE_REF_GET(self))->name_sym;
    if (name_sym == 0) return RESULT_OK(KORB_NIL);          /* anonymous class/module */
    const char *nm = korb_sym_name(c->vm, name_sym);        /* interned (stable across the alloc) */
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
/* Module#to_s / #inspect → the name; an anonymous class stringifies to a
 * placeholder rather than nil. */
static RESULT korb_m_class_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const uint32_t name_sym = VAL2CLASS(VALUE_REF_GET(self))->name_sym;
    if (name_sym == 0) return korb_str_new(c, slots, "#<Class>", 8);
    const char *nm = korb_sym_name(c->vm, name_sym);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
/* Module#ancestors — self, its included modules (most-recent first), then the
 * superclass chain (each followed by its modules).  Singleton classes skipped. */
static RESULT korb_m_class_ancestors(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 8));        /* result (rooted) */
    slots[1] = VALUE_REF_GET(self);                          /* current class (rooted) */
    while (KORB_CLASS_P(slots[1])) {
        VALUE pre = VAL2CLASS(slots[1])->prepended;   /* prepended modules precede the class */
        if (pre != KORB_NIL) {
            slots[2] = pre;
            for (uint32_t j = VAL2ARY(slots[2])->len; j-- > 0; )
                CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[0]), VAL2ARY(slots[2])->items->data[j]));
        }
        if (!VAL2CLASS(slots[1])->is_singleton)
            CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), slots[1]));
        VALUE inc = VAL2CLASS(slots[1])->included;
        if (inc != KORB_NIL) {
            slots[2] = inc;                                  /* root the module list */
            for (uint32_t j = VAL2ARY(slots[2])->len; j-- > 0; )
                CHECK(korb_ary_push_val(c, slots + 3, VALUE_REF_AT(&slots[0]), VAL2ARY(slots[2])->items->data[j]));
        }
        slots[1] = VAL2CLASS(slots[1])->superclass;
    }
    return RESULT_OK(slots[0]);
}
/* Module#instance_methods(inherit=true) → public/protected method names (symbols).
 * Excludes the private `initialize`; dedups across the ancestor chain. */
static RESULT korb_m_class_instance_methods(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const bool inherit = !(VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) == KORB_FALSE);
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 8));       /* result (rooted) */
    slots[1] = VALUE_REF_GET(self);                         /* current class (rooted) */
    while (KORB_CLASS_P(slots[1])) {
        const KorbClass *k = VAL2CLASS(slots[1]);
        for (uint32_t i = 0; i < k->method_cnt; i++) {
            const struct korb_method *m = k->methods[i];
            if (m->mid == c->vm->mid_initialize) continue;  /* private */
            const VALUE sym = ID2SYM(m->mid);
            const KorbArray *r = VAL2ARY(slots[0]);         /* dedup (a lower override shadows) */
            bool seen = false;
            for (uint32_t j = 0; j < r->len; j++) if (r->items->data[j] == sym) { seen = true; break; }
            if (!seen) CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[0]), sym));
        }
        if (!inherit) break;
        slots[1] = VAL2CLASS(slots[1])->superclass;
    }
    return RESULT_OK(slots[0]);
}
/* true if `sub` is `sup` or has `sup` among its ancestors (class chain + modules). */
static bool korb_class_is_descendant(VALUE sub, VALUE sup) {
    VALUE cls = sub;
    while (KORB_CLASS_P(cls)) {
        if (cls == sup) return true;
        VALUE pre = VAL2CLASS(cls)->prepended;
        if (pre != KORB_NIL) {
            const KorbArray *pa = VAL2ARY(pre);
            for (uint32_t j = 0; j < pa->len; j++) if (pa->items->data[j] == sup) return true;
        }
        VALUE inc = VAL2CLASS(cls)->included;
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (ia->items->data[j] == sup) return true;
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
static RESULT korb_m_class_le(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 1); }
static RESULT korb_m_class_gt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 2); }
static RESULT korb_m_class_ge(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_class_cmp_rel(c, slots, self, a, 3); }
/* Comparable mixin: derive </<=/>/>=/==/between?/clamp from the receiver's <=>.
 * `*out` = -1/0/1, or 2 when <=> returns nil (incomparable). */
static RESULT korb_comparable_cmp(CTX *c, VALUE *slots, VALUE self, VALUE other, int *out) {
    slots[0] = self; slots[1] = other;
    RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "<=>", 3), 0, 1, NULL, NULL, KORB_NIL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (r.value == KORB_NIL) { *out = 2; return RESULT_OK(KORB_NIL); }
    if (UNLIKELY(!FIXNUM_P(r.value))) return korb_raise(c, slots, KORB_E_TYPE, 0, "comparison failed");
    intptr_t v = FIX2LONG(r.value);
    *out = v < 0 ? -1 : v > 0 ? 1 : 0;
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
static RESULT korb_m_cmpbl_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { return korb_m_cmpbl_rel(c, slots, self, a, 4); }
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
        const KorbRange *rg = VAL2RANGE(VALUE_SLICE_GET(a, 0)); slots[0] = rg->rbegin; slots[1] = rg->rend;
    } else if (VALUE_SLICE_LEN(a) >= 2) { slots[0] = VALUE_SLICE_GET(a, 0); slots[1] = VALUE_SLICE_GET(a, 1); }
    else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    /* slots[0]=lo, slots[1]=hi rooted across the (GC-causing) <=> dispatches */
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
    if (UNLIKELY(!KORB_CLASS_P(target)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Module)", korb_type_name(target));
    VALUE cls = VALUE_REF_GET(self);
    while (KORB_CLASS_P(cls)) {                              /* pure reads → no GC, no rooting */
        VALUE inc = VAL2CLASS(cls)->included;
        if (inc != KORB_NIL) {
            const KorbArray *ia = VAL2ARY(inc);
            for (uint32_t j = 0; j < ia->len; j++) if (ia->items->data[j] == target) return RESULT_OK(KORB_TRUE);
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
    (void)self;
    const uint32_t id = korb_bind_argsym(c, VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(id == UINT32_MAX))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    const VALUE val = VALUE_SLICE_GET(a, 1);
    korb_const_define(c, id, val);              /* libc realloc only → no GC move of val */
    return RESULT_OK(val);
}
/* Module#remove_method(sym...) — drop the named method(s) from THIS class (a
 * sentinel mid retires the slot; lookup then falls through to ancestors). Raises
 * NameError if a name isn't defined on the class itself. */
static RESULT korb_m_class_remove_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    KorbClass *const k = VAL2CLASS(cls);
    for (uint32_t ai = 0; ai < VALUE_SLICE_LEN(a); ai++) {
        const uint32_t mid = korb_bind_argsym(c, VALUE_SLICE_GET(a, ai));
        if (UNLIKELY(mid == UINT32_MAX))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, ai)));
        bool found = false;
        for (uint32_t i = 0; i < k->method_cnt; i++)
            if (k->methods[i]->mid == mid) { k->methods[i]->mid = UINT32_MAX; found = true; break; }
        if (!found)
            return korb_raise(c, slots, KORB_E_NAME, 0, "method '%s' not defined in %s", korb_sym_name(c->vm, mid), korb_type_name(cls));
    }
    c->vm->method_serial++;                     /* method table changed → flush caches */
    return RESULT_OK(cls);
}
/* Module#undef_method(sym...) — prevent the named method(s) from this class.
 * Approximated as remove-if-present (no inherited-block marker); a name absent
 * from this class is tolerated so it doesn't re-block the file. */
static RESULT korb_m_class_undef_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls))) return korb_raise(c, slots, KORB_E_TYPE, 0, "not a class/module");
    KorbClass *const k = VAL2CLASS(cls);
    for (uint32_t ai = 0; ai < VALUE_SLICE_LEN(a); ai++) {
        const uint32_t mid = korb_bind_argsym(c, VALUE_SLICE_GET(a, ai));
        if (UNLIKELY(mid == UINT32_MAX))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, ai)));
        for (uint32_t i = 0; i < k->method_cnt; i++)
            if (k->methods[i]->mid == mid) { k->methods[i]->mid = UINT32_MAX; break; }
    }
    c->vm->method_serial++;
    return RESULT_OK(cls);
}
/* Module#method_defined?(sym|str[, inherit]) — true if an instance method by
 * that name is defined on the class / its ancestors (koruby doesn't track
 * visibility, so any defined method counts; public_method_defined? aliases it). */
static RESULT korb_m_class_method_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const uint32_t mid = korb_bind_argsym(c, VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(mid == UINT32_MAX))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    const VALUE cls = VALUE_REF_GET(self);
    return RESULT_OK((KORB_CLASS_P(cls) && korb_class_find_method(cls, mid, NULL) != NULL) ? KORB_TRUE : KORB_FALSE);
}
/* Module#const_get(sym|str) — consts are a flat (global) table here, so the
 * receiver's namespace is ignored; rightmost name resolves. */
/* a valid constant name: [A-Z][A-Za-z0-9_]* */
static bool korb_valid_const_name(const char *p, uint32_t len) {
    if (len == 0 || !(p[0] >= 'A' && p[0] <= 'Z')) return false;
    for (uint32_t i = 1; i < len; i++) {
        const char ch = p[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}
static RESULT korb_m_class_const_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE name = VALUE_SLICE_GET(a, 0);   /* optional 2nd arg `inherit` is ignored (flat const table) */
    struct korb_vm *const vm = c->vm;
    /* qualified string "A::B::C" — resolve each component left to right (flat table). */
    if (KORB_STRING_P(name) && memchr(VAL2STR(name)->buf->data, ':', VAL2STR(name)->len)) {
        const KorbString *const s = VAL2STR(name);
        const char *p = s->buf->data, *const end = p + s->len;
        if (p + 2 <= end && p[0] == ':' && p[1] == ':') p += 2;   /* leading :: (top-level) */
        VALUE val = KORB_NIL;
        while (p < end) {
            const char *q = p;
            while (q < end && !(q[0] == ':' && q + 1 < end && q[1] == ':')) q++;
            const uint32_t clen = (uint32_t)(q - p);
            if (!korb_valid_const_name(p, clen))
                return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %.*s", (int)s->len, s->buf->data);
            const uint32_t cid = korb_intern(vm, p, clen);
            bool found = false;
            for (uint32_t i = 0; i < vm->const_cnt; i++)
                if (vm->const_names[i] == cid) { val = vm->const_vals[i]; found = true; break; }
            if (!found) return korb_raise(c, slots, KORB_E_NAME, 0, "uninitialized constant %.*s", clen, p);
            p = (q < end) ? q + 2 : end;
        }
        return RESULT_OK(val);
    }
    uint32_t id;
    if (SYMBOL_P(name)) id = SYM2ID(name);
    else if (KORB_STRING_P(name)) {
        const KorbString *const s = VAL2STR(name);
        if (!korb_valid_const_name(s->buf->data, s->len))
            return korb_raise(c, slots, KORB_E_NAME, 0, "wrong constant name %.*s", (int)s->len, s->buf->data);
        id = korb_intern(vm, s->buf->data, s->len);
    }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == id) return RESULT_OK(vm->const_vals[i]);
    return korb_raise(c, slots, KORB_E_NAME, 0, "uninitialized constant %s", korb_sym_name(vm, id));
}
/* Module#remove_const(sym|str) → the removed value (flat table tombstone). */
static RESULT korb_m_class_remove_const(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE name = VALUE_SLICE_GET(a, 0);
    uint32_t id;
    if (SYMBOL_P(name)) id = SYM2ID(name);
    else if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); id = korb_intern(c->vm, s->buf->data, s->len); }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == id) {
            const VALUE old = vm->const_vals[i];
            vm->const_names[i] = 0;            /* tombstone (interned ids are >0) */
            vm->const_vals[i] = KORB_NIL;
            vm->method_serial++;              /* invalidate const caches */
            return RESULT_OK(old);
        }
    return korb_raise(c, slots, KORB_E_NAME, 0, "constant %s not defined", korb_sym_name(vm, id));
}
/* Module#const_defined?(sym|str) — flat table membership. */
static RESULT korb_m_class_const_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)slots;
    VALUE name = VALUE_SLICE_GET(a, 0);
    uint32_t id;
    if (SYMBOL_P(name)) id = SYM2ID(name);
    else if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); id = korb_intern(c->vm, s->buf->data, s->len); }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(name));
    struct korb_vm *const vm = c->vm;
    for (uint32_t i = 0; i < vm->const_cnt; i++)
        if (vm->const_names[i] == id) return RESULT_OK(KORB_TRUE);
    return RESULT_OK(KORB_FALSE);
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
    (void)a; (void)self;
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "loop without a block (Enumerator) is not supported");
    for (;;) {
        RESULT r = korb_block_yield(c, slots, block, def_env, NULL, 0, cself);
        if (r.state == KORB_BREAK) return RESULT_OK(r.value);   /* break [v] → loop value */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
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

/* instance_exec(*args) { |*args| ... } — run the block with self rebound to the
 * receiver; the block's lexical env (def_env) is preserved so closures still work.
 * Method definitions inside (singleton def) are NOT redirected to the receiver. */
static RESULT korb_m_obj_instance_exec(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    if (UNLIKELY(block == NULL)) return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    const uint32_t argc = VALUE_SLICE_LEN(a);
    if (block == KORB_BLK_CPROC) {                       /* forwarded Symbol/Method#to_proc: fixed binding, self-rebind is moot */
        for (uint32_t i = 0; i < argc; i++) slots[i] = VALUE_SLICE_GET(a, i);
        return korb_block_yield(c, slots + argc, block, def_env, slots, argc, cself);
    }
    slots[0] = VALUE_REF_GET(self);                      /* new self = receiver (rooted self cell) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    if (def_env == KORB_BLK_FWD) {                        /* forwarded Proc: keep ITS closure env, rebind self only */
        slots[1 + argc] = VAL2PROC(*cself)->env;          /* proc's captured env (used as def_env below, not FWD) */
        return korb_block_yield(c, slots + 2 + argc, block, (VALUE *)(uintptr_t)slots[1 + argc], &slots[1], argc, &slots[0]);
    }
    return korb_block_yield(c, slots + 1 + argc, block, def_env, &slots[1], argc, &slots[0]);
}
/* instance_eval { |obj| ... } — like instance_exec but with the receiver as the
 * sole block argument (CRuby passes self).  The String form is not supported. */
static RESULT korb_m_obj_instance_eval(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    (void)cself;
    if (UNLIKELY(block == NULL)) {
        if (VALUE_SLICE_LEN(a) >= 1)
            return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "instance_eval with a String is not supported");
        return korb_raise(c, slots, KORB_E_LOCALJUMP, 0, "no block given (yield)");
    }
    slots[0] = VALUE_REF_GET(self);
    if (block == KORB_BLK_CPROC)                          /* forwarded C-proc: fixed binding */
        return korb_block_yield(c, slots + 1, block, def_env, slots, 1, cself);
    slots[1] = slots[0];                                 /* arg0 = receiver */
    if (def_env == KORB_BLK_FWD) {                        /* forwarded Proc: keep ITS closure env, rebind self only */
        slots[2] = VAL2PROC(*cself)->env;
        return korb_block_yield(c, slots + 3, block, (VALUE *)(uintptr_t)slots[2], &slots[1], 1, &slots[0]);
    }
    return korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, &slots[0]);
}

/* Exception#message / to_s — the stored message, or the class name if none. */
static RESULT korb_m_exc_message(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbException *e = VAL2EXC(VALUE_REF_GET(self));
    if (e->msg != KORB_NIL) return RESULT_OK(e->msg);
    /* no explicit message → the class name (exc_class for an abstract/user class,
     * else the builtin etype name), matching CRuby's default. */
    const char *nm = (e->exc_class != KORB_NIL && KORB_CLASS_P(e->exc_class))
                         ? korb_sym_name(c->vm, VAL2CLASS(e->exc_class)->name_sym)
                         : korb_etype_name(e->etype);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
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

