/* koruby_precise — set.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Set (array-backed, unique by korb_value_eq) -------------------------- */
static bool korb_arr_has(const KorbArray *ar, VALUE v) {
    for (uint32_t i = 0; i < ar->len; i++) if (korb_value_eql(ar->items->data[i], v)) return true;
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
static RESULT korb_m_set_include(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_arr_has(VAL2ARY(SELF_SET->elems), VALUE_SLICE_GET(a, 0)) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_set_add(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE v = VALUE_SLICE_GET(a, 0);
    if (!korb_arr_has(VAL2ARY(SELF_SET->elems), v)) { slots[0] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&slots[0]), v)); }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_add_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (korb_arr_has(VAL2ARY(SELF_SET->elems), VALUE_SLICE_GET(a, 0))) return RESULT_OK(KORB_NIL);
    return korb_m_set_add(c, slots, self, a);
}
static RESULT korb_m_set_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbArray *ar = VAL2ARY(SELF_SET->elems);
    VALUE v = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 0; i < ar->len; i++)
        if (korb_value_eq(ar->items->data[i], v)) {
            for (uint32_t j = i; j + 1 < ar->len; j++) ARO_STORE(c, ar->items, &ar->items->data[j], ar->items->data[j+1]);
            ar->items->data[--ar->len] = KORB_NIL;
            break;
        }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_set_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
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
    VALUE ov = korb_set_elems_of(VALUE_SLICE_GET(a, 0));
    if (UNLIKELY(ov == KORB_NIL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "value must be enumerable");
    slots[0] = ov;
    for (uint32_t i = 0; i < VAL2ARY(slots[0])->len; i++) {
        VALUE e = VAL2ARY(slots[0])->items->data[i];
        if (!korb_arr_has(VAL2ARY(SELF_SET->elems), e)) { slots[1] = SELF_SET->elems; CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), e)); }
    }
    return RESULT_OK(VALUE_REF_GET(self));
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
static RESULT korb_m_ary_map(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
static RESULT korb_m_ary_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
static RESULT korb_m_ary_reject(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
static RESULT korb_m_ary_find(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
static RESULT korb_m_ary_sort(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
static RESULT korb_m_ary_sum(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_ary_minmax(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself);
KORB_SET_DELEG_BLK(korb_m_set_map, korb_m_ary_map)
KORB_SET_DELEG_BLK(korb_m_set_select, korb_m_ary_select)
KORB_SET_DELEG_BLK(korb_m_set_reject, korb_m_ary_reject)
KORB_SET_DELEG_BLK(korb_m_set_find, korb_m_ary_find)
KORB_SET_DELEG_BLK(korb_m_set_sort, korb_m_ary_sort)
KORB_SET_DELEG(korb_m_set_sum, korb_m_ary_sum)
KORB_SET_DELEG_BLK(korb_m_set_minmax, korb_m_ary_minmax)
static RESULT korb_hash_first_n(CTX *c, VALUE *slots, VALUE_REF self, uint32_t limit);
static RESULT korb_m_range_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_m_ary_to_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
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
    if (KORB_OBJECT_P(sv) && VAL2OBJ(sv)->klass != KORB_NIL && korb_class_find_method(VAL2OBJ(sv)->klass, mid, NULL))
        return RESULT_OK(KORB_TRUE);
    if (AROH_IS_GC_OBJECT(sv) && (((const AroObjectHeader *)(uintptr_t)sv)->flags & KORB_FL_HAS_KLASS)) {
        VALUE ov = korb_klass_override_get(vm, sv);
        if (ov != KORB_NIL && korb_class_find_method(ov, mid, NULL)) return RESULT_OK(KORB_TRUE);
    }
    return RESULT_OK(korb_find_cmethod(vm, korb_class_of(sv), mid) != NULL ? KORB_TRUE : KORB_FALSE);
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
    if (((const AroObjectHeader *)(uintptr_t)obj)->flags & KORB_FL_HAS_KLASS) {
        VALUE ov = korb_klass_override_get(vm, obj);
        if (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton) return RESULT_OK(ov);   /* reuse */
    }
    slots[0] = obj;                                                              /* root self across class alloc */
    VALUE cur = (((const AroObjectHeader *)(uintptr_t)obj)->flags & KORB_FL_HAS_KLASS)
                  ? korb_klass_override_get(vm, obj) : korb_class_obj_of(c, obj);
    slots[1] = cur;
    VALUE sing = UNWRAP(korb_class_new(c, slots + 2, 0, slots[1]));               /* anonymous, super=cur */
    VAL2CLASS(sing)->is_singleton = 1;
    slots[2] = sing;
    korb_klass_override_set(c, slots[0], slots[2]);                               /* obj/sing rooted, no GC in set */
    return RESULT_OK(slots[2]);
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
/* Module#=== (`Klass === obj`): true iff obj.is_a?(Klass) — same test korb_case_eq uses. */
static RESULT korb_m_class_case_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    return RESULT_OK(korb_case_eq(c, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)) ? KORB_TRUE : KORB_FALSE);
}
/* Object#then / yield_self — yield self, return the block's value (no block → self). */
static RESULT korb_m_obj_then(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(self));
    slots[0] = VALUE_REF_GET(self);                   /* rooted across the yield */
    return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
}
/* Object#tap — yield self, return self. */
static RESULT korb_m_obj_tap(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE cself) {
    (void)a;
    if (block != NULL) {
        slots[0] = VALUE_REF_GET(self);
        RESULT r = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Exception#message / to_s — the stored message, or the class name if none. */
static RESULT korb_m_exc_message(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbException *e = VAL2EXC(VALUE_REF_GET(self));
    if (e->msg != KORB_NIL) return RESULT_OK(e->msg);
    const char *nm = korb_etype_name(e->etype);
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}

