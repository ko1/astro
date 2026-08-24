/* koruby_precise — symbol.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Symbol methods ------------------------------------------------------ */

static RESULT korb_m_sym_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
static RESULT korb_m_sym_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* Symbol.all_symbols → an Array of every interned Symbol. */
static RESULT korb_m_sym_all_symbols(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    const uint32_t n = c->vm->sym_cnt;                /* snapshot: no interning happens in the loop */
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, ID2SYM(i)));
    return RESULT_OK(VALUE_REF_GET(dst));
}
static RESULT korb_m_sym_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    return RESULT_OK(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)))[0] == '\0' ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_sym_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return RESULT_OK(LONG2FIX(korb_utf8_count(nm, (uint32_t)strlen(nm))));   /* UTF-8 char count, like String#length */
}

/* ---- nil / true / false methods ------------------------------------------ */

/* Return a cached frozen String (created once, then the same object each call) —
 * matches CRuby's nil/true/false #to_s returning a shared frozen literal. */
static RESULT korb_cached_frozen_str(CTX *c, VALUE *slots, VALUE *cache, const char *s, uint32_t len) {
    if (LIKELY(KORB_STRING_P(*cache))) return RESULT_OK(*cache);
    RESULT r = korb_str_new(c, slots, s, len);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;
    *cache = r.value;                                    /* store in the vm (a GC root) */
    return RESULT_OK(*cache);
}
static RESULT korb_m_nil_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_nil_to_s, "", 0); }
static RESULT korb_m_nil_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(0)); }
static RESULT korb_m_nil_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_ary_new(c, slots, 0); }
static RESULT korb_m_nil_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_rat_new(c, slots, 0, 1); }   /* nil.to_r → (0/1) */
static RESULT korb_m_nil_rationalize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {   /* nil.rationalize([eps]) → (0/1); ignores eps, but >1 arg is an ArgumentError */
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", VALUE_SLICE_LEN(a));
    return korb_rat_new(c, slots, 0, 1);
}
static RESULT korb_m_nil_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_float_new(c, slots, 0.0); }   /* nil.to_f → 0.0 */
static RESULT korb_m_nil_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_hash_new(c, slots, 4); }       /* nil.to_h → {} */
static RESULT korb_m_nil_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_cpx_new(c, slots, LONG2FIX(0), LONG2FIX(0)); }   /* nil.to_c → (0+0i) */
static RESULT korb_m_true_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_true_to_s, "true", 4); }
static RESULT korb_m_false_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_false_to_s, "false", 5); }

/* ---- universal (Object) methods ------------------------------------------ */

static RESULT korb_m_obj_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
/* Default BasicObject#method_missing(name, *args) — raises NoMethodError.  Lets a
 * user method_missing call `super` for the unhandled case (CRuby semantics). */
static RESULT korb_m_obj_method_missing(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE name = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_NIL;
    char nmbuf[256];   /* copy the method name: korb_recv_desc may dispatch #name → GC → move a String arg */
    if (SYMBOL_P(name))       snprintf(nmbuf, sizeof nmbuf, "%s", korb_sym_name(c->vm, SYM2ID(name)));
    else if (KORB_STRING_P(name)) snprintf(nmbuf, sizeof nmbuf, "%.*s", (int)VAL2STR(name)->len, korb_strbuf_data(VAL2STR(name)->buf));
    else                      snprintf(nmbuf, sizeof nmbuf, "(unknown)");
    const char *const nm = nmbuf;
    const VALUE recv = VALUE_REF_GET(self);
    char buf[256];   /* CRuby-shaped: "class Foo" / "module Bar" / "an instance of Foo" / … */
    const char *tn = korb_recv_desc(c, slots, recv, buf, sizeof buf);
    RESULT r = korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method '%s' for %s", nm, tn);
    if (LIKELY(KORB_EXC_P(r.value))) {                    /* attach #name / #receiver metadata */
        slots[0] = r.value;
        VALUE_REF eref = VALUE_REF_AT(&slots[0]);
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern(c->vm, "@__name", 7)), name);   /* Symbol immediate */
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern(c->vm, "@__receiver", 11)), VALUE_REF_GET(self));
        const uint32_t na = VALUE_SLICE_LEN(a);            /* @__args = args after the name (method_missing(name, *args)) */
        RESULT ar = korb_ary_new(c, slots + 1, na > 1 ? na - 1 : 0);
        if (LIKELY(ar.state == KORB_NORMAL)) {
            slots[1] = ar.value;
            VALUE_REF argsref = VALUE_REF_AT(&slots[1]);
            for (uint32_t j = 1; j < na; j++)
                korb_ary_push_val(c, slots + 2, argsref, VALUE_SLICE_GET(a, j));
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern(c->vm, "@__args", 7)), VALUE_REF_GET(argsref));
        }
        r.value = VALUE_REF_GET(eref);
    }
    return r;
}
static RESULT korb_m_nil_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_obj_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }
/* Object#=== — the default is `self == other`, so a subclass's overridden #==
 * is honoured (dispatch rather than the identity korb_m_obj_eq). */
static RESULT korb_m_obj_case_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    slots[0] = VALUE_REF_GET(self); slots[1] = VALUE_SLICE_GET(a, 0);
    return korb_send_impl(c, slots + 2, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
}
static RESULT korb_m_obj_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eql(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }  /* type-strict: 1.eql?(1.0) => false */
static RESULT korb_m_obj_neq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* BasicObject#!= is !(self == other) — dispatch #== so a user-defined == is honoured. */
    slots[0] = VALUE_REF_GET(self); slots[1] = VALUE_SLICE_GET(a, 0);
    RESULT r = korb_send(c, slots + 2, c->vm->mid_eq, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_obj_equal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(VALUE_REF_GET(self) == VALUE_SLICE_GET(a,0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_itself(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* freeze: no-op (koruby has no frozen state) → self.  frozen?: true only for
 * immediates (Integer/Symbol/nil/true/false), false otherwise. */
static RESULT korb_m_obj_freeze(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (AROH_IS_GC_OBJECT(v)) {
        ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_FROZEN;
        /* A frozen Array gets capa=len so node_shl's existing `len < capa` room
         * check fails → the (rare) slow path does the FrozenError raise.  Keeps
         * the hot `a << x` fast path free of any frozen test. */
        if (KORB_ARRAY_P(v)) VAL2ARY(v)->capa = VAL2ARY(v)->len;
    }
    return RESULT_OK(v);
}
static RESULT korb_m_obj_frozen_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (!AROH_IS_GC_OBJECT(v)) return RESULT_OK(KORB_TRUE);   /* immediates are frozen */
    return RESULT_OK((((AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_FROZEN) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_obj_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* CRuby Object#<=>: 0 if self == other (dispatched, so a custom #== counts), else nil. */
    const VALUE s = VALUE_REF_GET(self), o = VALUE_SLICE_GET(a, 0);
    if (s == o) return RESULT_OK(LONG2FIX(0));                 /* identical */
    slots[0] = s; slots[1] = o;
    RESULT r = korb_send_impl(c, slots + 2, korb_intern(c->vm, "==", 2), 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK((r.value != KORB_NIL && r.value != KORB_FALSE) ? LONG2FIX(0) : KORB_NIL);
}
/* resolve a Symbol/String name arg to the ivar-key Symbol (`:@x`, `"@x"`). */
static bool korb_name_to_sym(CTX *c, VALUE name, VALUE *out) {
    if (SYMBOL_P(name)) { *out = name; return true; }
    if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); *out = ID2SYM(korb_intern(c->vm, korb_strbuf_data(s->buf), s->len)); return true; }
    return false;
}
/* valid instance-variable name: '@' + (letter|'_'|non-ASCII) + (alnum|'_'|
 * non-ASCII)*; not '@@x'.  A byte >= 0x80 is part of a multibyte identifier
 * character, which Ruby allows. */
static bool korb_valid_ivar_name(struct korb_vm *vm, uint32_t sym) {
    const char *const nm = korb_sym_name(vm, sym);
    const unsigned char f = (unsigned char)nm[1];
    if (nm[0] != '@' || f == '\0' || f == '@') return false;
    if (!((f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z') || f == '_' || f >= 0x80)) return false;
    for (const char *p = nm + 2; *p; p++) {
        const unsigned char ch = (unsigned char)*p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch >= 0x80)) return false;
    }
    return true;
}
/* Resolve an ivar-name argument to a Symbol: Symbol/String direct, else via
 * #to_str; *ok is set false when it's none of those. */
static RESULT korb_ivar_name_arg(CTX *c, VALUE *slots, VALUE name, VALUE *out_sym, bool *ok) {
    if (korb_name_to_sym(c, name, out_sym)) { *ok = true; return RESULT_OK(KORB_NIL); }
    if (KORB_OBJECT_P(name) && korb_responds_to_coerce_p(c, slots, &name, korb_intern(c->vm, "to_str", 6))) {
        slots[0] = name;
        RESULT sr = korb_send_impl(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (korb_name_to_sym(c, sr.value, out_sym)) { *ok = true; return RESULT_OK(KORB_NIL); }
    }
    *ok = false; return RESULT_OK(KORB_NIL);
}
/* Raise "'<name>' is not allowed as an instance variable name" (NameError) with
 * #name (the given name, verbatim — CRuby echoes the String/Symbol as passed)
 * and #receiver (self) attached. */
static RESULT korb_raise_bad_ivar_name(CTX *c, VALUE *slots, VALUE self, VALUE orig_name, uint32_t sym_id) {
    slots[0] = self; slots[1] = orig_name;
    RESULT ne = korb_raise(c, slots + 2, KORB_E_NAME, 0, "'%s' is not allowed as an instance variable name", korb_sym_name(c->vm, sym_id));
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
static RESULT korb_m_obj_ivar_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym, name = VALUE_SLICE_GET(a, 0);
    bool ok; RESULT nr = korb_ivar_name_arg(c, slots, name, &sym, &ok);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    if (UNLIKELY(!ok))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), SYM2ID(sym));
    if (UNLIKELY(!AROH_IS_GC_OBJECT(VALUE_REF_GET(self)) || KORB_BIGNUM_P(VALUE_REF_GET(self)) ||
                 KORB_FLOAT_P(VALUE_REF_GET(self))))                 /* numerics/Symbol/nil/... are frozen */
        return korb_raise_frozen(c, slots, VALUE_REF_GET(self));
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    CHECK(korb_ivar_set(c, slots, self, sym, VALUE_SLICE_GET(a, 1)));   /* class/exc/user-object/container all handled */
    return RESULT_OK(VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_obj_ivar_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym, name = VALUE_SLICE_GET(a, 0);
    bool ok; RESULT nr = korb_ivar_name_arg(c, slots, name, &sym, &ok);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    if (UNLIKELY(!ok))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), SYM2ID(sym));
    (void)slots;
    return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), sym));   /* handles class/exc/container/immediate */
}
/* Object#instance_variable_defined?(name) — true if the @ivar is set (non-nil). */
static RESULT korb_m_obj_ivar_defined(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym; bool ok;
    { RESULT nr = korb_ivar_name_arg(c, slots, VALUE_SLICE_GET(a, 0), &sym, &ok); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    if (UNLIKELY(!ok)) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), SYM2ID(sym));
    return RESULT_OK(korb_ivar_defined(c, VALUE_REF_GET(self), sym) ? KORB_TRUE : KORB_FALSE);   /* membership, not value (an ivar set to nil is defined) */
}
/* Object#remove_instance_variable(name) → the removed value; NameError if unset. */
static RESULT korb_m_obj_remove_ivar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym; bool ok;
    { RESULT nr = korb_ivar_name_arg(c, slots, VALUE_SLICE_GET(a, 0), &sym, &ok); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    if (UNLIKELY(!ok)) return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(VALUE_SLICE_GET(a, 0)));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0), SYM2ID(sym));
    KORB_CHECK_FROZEN(c, slots, VALUE_REF_GET(self));
    bool found;                                       /* true-remove from the shape / side hash */
    const VALUE old = korb_ivar_remove(c, VALUE_REF_GET(self), sym, &found);
    if (!found)
        return korb_raise(c, slots, KORB_E_NAME, 0, "instance variable %s not defined", korb_sym_name(c->vm, SYM2ID(sym)));
    return RESULT_OK(old);
}
/* Object#instance_variables → [:@a, :@b, ...] in definition order. */
static RESULT korb_m_obj_ivars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE sv = VALUE_REF_GET(self);
    if (KORB_EXC_P(sv)) {                                       /* exception ivars live in a side hash */
        if (VAL2EXC(sv)->ivars == KORB_NIL) return korb_ary_new(c, slots, 0);
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, VAL2HASH(VAL2EXC(sv)->ivars)->len));   /* alloc may GC → re-read self below */
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < VAL2HASH(VAL2EXC(VALUE_REF_GET(self))->ivars)->len; i++)
            CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2HASH(VAL2EXC(VALUE_REF_GET(self))->ivars)->items)[2 * i]));
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    if (!KORB_OBJECT_P(sv)) {                                   /* class / container / heap object → side-hash keys */
        const VALUE h0 = KORB_CLASS_P(sv) ? VAL2CLASS(sv)->class_ivars
                       : AROH_IS_GC_OBJECT(sv) ? korb_objivar_hash_of(c->vm, sv) : KORB_NIL;
        if (h0 == KORB_NIL) return korb_ary_new(c, slots, 0);
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, VAL2HASH(h0)->len));   /* alloc may GC → re-read via self below */
        VALUE_REF dst = VALUE_REF_AT(&slots[0]);
        /* re-read the hash after every push: the allocation may move it */
        #define KORB_IVH() (KORB_CLASS_P(VALUE_REF_GET(self)) ? VAL2CLASS(VALUE_REF_GET(self))->class_ivars \
                                                             : korb_objivar_hash_of(c->vm, VALUE_REF_GET(self)))
        for (uint32_t i = 0; i < VAL2HASH(KORB_IVH())->len; i++)
            CHECK(korb_ary_push_val(c, slots + 1, dst, korb_items_data(VAL2HASH(KORB_IVH())->items)[2 * i]));
        #undef KORB_IVH
        return RESULT_OK(VALUE_REF_GET(dst));
    }
    const uint32_t sid0 = VAL2OBJ(sv)->shape_id;                /* read shape BEFORE any alloc */
    const uint32_t n = c->vm->shapes[sid0].ivar_count;
    uint32_t *const syms = n ? (uint32_t *)malloc((size_t)n * sizeof(uint32_t)) : NULL;
    if (n && UNLIKELY(!syms)) return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "out of memory");
    for (uint32_t sid = sid0; sid; ) {                          /* leaf→root: place each ivar at its index */
        const struct korb_shape *s = &c->vm->shapes[sid];
        if (s->ivar_count >= 1 && s->ivar_count <= n) syms[s->ivar_count - 1] = s->edge_sym;
        sid = s->parent;
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, n));           /* (frees syms below; minor leak only on OOM) */
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = ID2SYM(syms[i]);                             /* syms is libc memory, stable across GC */
        CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    free(syms);
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Struct/Data#instance_variables — like Object#instance_variables but excluding
 * the members (stored internally as @<member> ivars, not user-visible). */
static RESULT korb_m_struct_ivars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT all = korb_m_obj_ivars(c, slots, self, a);
    if (UNLIKELY(all.state != KORB_NORMAL)) return all;
    slots[0] = all.value;                                /* all ivars (rooted) */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, 0));    /* result */
    VALUE_REF dst = VALUE_REF_AT(&slots[1]);
    const uint32_t n = VAL2ARY(slots[0])->len;
    for (uint32_t i = 0; i < n; i++) {
        const VALUE ivsym = korb_items_data(VAL2ARY(slots[0])->items)[i];
        const KorbArray *const mem = VAL2ARY(VAL2CLASS(VAL2OBJ(VALUE_REF_GET(self))->klass)->members);   /* re-read */
        bool is_member = false;
        for (uint32_t j = 0; j < mem->len; j++)
            if (korb_member_ivar_sym(c->vm, korb_items_data(mem->items)[j]) == ivsym) { is_member = true; break; }
        if (!is_member) CHECK(korb_ary_push_val(c, slots + 2, dst, ivsym));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
/* Object#method(:sym) → bound Method. */
static RESULT korb_m_obj_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t mid;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_arg_to_mid(c, slots, VALUE_SLICE_GET(a, 0), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    return korb_method_new(c, slots, VALUE_REF_GET(self), mid);   /* self re-read (coercion may GC) */
}
/* Method#call / #[] — re-dispatch to recv.mid(*args).  Stage [recv | args...]
 * below a fresh cursor and reuse the send machinery (polymorphic with Array#[]). */
static RESULT korb_m_meth_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    const KorbMethod *m = VAL2METH(VALUE_REF_GET(self));
    if (UNLIKELY(m->unbound)) return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'call' for an UnboundMethod (use #bind)");
    uint32_t mid = m->mid, argc = VALUE_SLICE_LEN(a);
    const VALUE owner = m->owner;
    if (UNLIKELY(owner != KORB_NIL && KORB_CLASS_P(owner))) {   /* bound-from-unbound: invoke the FIXED method from its owner (not virtual) */
        struct korb_method *const entry = korb_class_find_method(owner, mid, NULL);
        if (LIKELY(entry != NULL && entry->kind == KORB_METHOD_ISEQ)) {   /* korb_invoke_method handles ISEQ only; builtins fall to virtual */
            slots[0] = m->recv;                          /* self (the bound receiver) */
            slots[1] = owner;                            /* def_class for super resolution */
            for (uint32_t i = 0; i < argc; i++) slots[2 + i] = VALUE_SLICE_GET(a, i);
            return korb_invoke_method(c, slots + 2 + argc, entry, argc, 0, mid, slots[0], slots[1], block, def_env, KORB_CSELF_VAL(cself));
        }
    }
    slots[0] = m->recv;                                  /* recv below the args */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);   /* forward the block to the method */
}
static RESULT korb_m_meth_recv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2METH(VALUE_REF_GET(self))->recv);
}
/* Method#to_proc — a lambda Proc bound to the method's receiver.  Encoded like a
 * Symbol#to_proc proc (iseq == NULL, sym_mid = the method id) but with is_lambda = 1
 * and self = the bound receiver, which both marks it a "method proc" (vs a symbol
 * proc, self == nil) and roots the receiver.  korb_cproc_yield / Proc#call dispatch
 * recv.mid(args...). */
static RESULT korb_m_meth_to_proc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbMethod *m = VAL2METH(VALUE_REF_GET(self));
    if (UNLIKELY(m->unbound))
        return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'to_proc' for an UnboundMethod (use #bind)");
    slots[0] = m->recv;                                  /* root recv across the alloc */
    const uint32_t mid = m->mid;
    KorbProc *p = korb_alloc(c, slots + 1, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = NULL; p->sym_mid = mid; p->is_lambda = 1;
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->env, 0);
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, slots[0]);   /* bound receiver (non-nil ⇒ method proc) */
    return RESULT_OK((VALUE)p);
}
/* Proc#call / [] / .() / === — invoke the captured block body.  Stage A: env is
 * a tagged-odd live-slots pointer (correct while the defining frame is alive). */
static RESULT korb_m_proc_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *cself) {
    KorbProc *p = VAL2PROC(VALUE_REF_GET(self));
    const bool is_lam = p->is_lambda;                    /* captured before the body runs (a lambda's `return` returns from the lambda) */
    uint32_t argc = VALUE_SLICE_LEN(a);
    if (p->iseq == NULL) {                               /* no body: symbol proc / method proc */
        uint32_t mid = p->sym_mid;
        if (p->self != KORB_NIL) {                       /* Method#to_proc (bound receiver): recv.mid(args...) */
            slots[0] = p->self;
            for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
            return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);   /* forward the &block given at .call */
        }
        if (UNLIKELY(argc < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver is available");
        for (uint32_t i = 0; i < argc; i++) slots[i] = VALUE_SLICE_GET(a, i);   /* Symbol#to_proc: arg0.sym(arg1..) */
        return korb_send_impl(c, slots + argc, mid, 0, argc - 1, block, def_env, cself);
    }
    NODE *entry = p->iseq;
    /* Lambdas enforce arity (unlike plain procs/blocks).  Positional-only check
     * (skip when the lambda declares keywords — the trailing kw Hash would skew
     * the count). */
    if (UNLIKELY(p->is_lambda) && entry != KORB_BLK_CPROC && korb_entry_kw_info(entry) == NULL) {
        const uint32_t pc  = korb_entry_params_cnt(entry);
        const bool has_rest = korb_entry_rest_slot(entry) != -1;   /* named (>=0) or discard (-2) */
        const bool variable = (korb_entry_opt_defaults(entry) != NULL) || has_rest;   /* same basis as Proc#arity */
        const uint32_t req = variable ? korb_entry_req_cnt(entry) : pc;               /* non-variable → all required */
        if (UNLIKELY(argc < req || (!has_rest && argc > pc))) {
            char exp[32];
            if (has_rest)       snprintf(exp, sizeof exp, "%u+", req);
            else if (req == pc) snprintf(exp, sizeof exp, "%u", req);
            else                snprintf(exp, sizeof exp, "%u..%u", req, pc);
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected %s)", argc, exp);
        }
    }
    VALUE penv = p->env;                                 /* already tagged: odd=slots / even=KorbEnv */
    slots[0] = p->self;                                  /* captured self (rooted) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    /* If this proc has a `|&b|` param and the call site passed a (real) block,
     * forward it so &b binds to it — via the full path (the hot wrapper is
     * untouched; a no-block call leaves &b nil). */
    RESULT r;
    if (block != NULL && block != KORB_BLK_CPROC && block->head.kind == &kind_node_entry &&
        korb_entry_blk_param_slot(entry) >= 0)
        r = korb_block_yield_full(c, slots + 1 + argc, entry, (VALUE *)(uintptr_t)penv,
                                  &slots[1], argc, &slots[0], block, def_env, cself, is_lam);
    else if (is_lam)   /* a lambda enforces arity + never auto-splats a single Array → the full path with is_lam */
        r = korb_block_yield_full(c, slots + 1 + argc, entry, (VALUE *)(uintptr_t)penv,
                                  &slots[1], argc, &slots[0], NULL, NULL, NULL, 1);
    else
        r = korb_block_yield(c, slots + 1 + argc, entry, (VALUE *)(uintptr_t)penv, &slots[1], argc, &slots[0]);
    if (UNLIKELY(is_lam && r.state == KORB_RETURN)) {    /* lambda boundary consumes its own `return` */
        c->return_target = NULL;
        r.state = KORB_NORMAL;
    }
    return r;
}
/* Symbol#to_proc — a Proc that sends the symbol to its first argument. */
static RESULT korb_m_sym_to_proc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t mid = SYM2ID(VALUE_REF_GET(self));
    KorbProc *p = korb_alloc(c, slots, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = NULL; p->sym_mid = mid; p->is_lambda = 1;   /* CRuby: Symbol#to_proc returns a lambda */
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->env, 0);
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, KORB_NIL);
    return RESULT_OK((VALUE)p);
}
/* Proc#== / #eql? — CRuby 3.2+: two Procs are equal when they are the same
 * block with the same captured environment, so `p.dup == p`. */
static RESULT korb_m_proc_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots;
    const VALUE o = VALUE_SLICE_GET(a, 0);
    const VALUE s = VALUE_REF_GET(self);
    if (s == o) return RESULT_OK(KORB_TRUE);
    if (!KORB_PROC_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbProc *const x = VAL2PROC(s), *const y = VAL2PROC(o);
    const bool eq = x->iseq == y->iseq && x->env == y->env && x->self == y->self &&
                    x->sym_mid == y->sym_mid && x->is_lambda == y->is_lambda;
    return RESULT_OK(eq ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_proc_lambda_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2PROC(VALUE_REF_GET(self))->is_lambda ? KORB_TRUE : KORB_FALSE);
}
/* Proc#binding: the proc's captured env + self.  Local NAMES are not recorded
 * in the proc, so the binding exposes self/cref (enough for class-defining
 * eval) but no locals table. */
static const uint32_t korb_proc_binding_scope_tbl[2] = { 1, 0 };   /* L=1, ns[0]=0 */
static RESULT korb_m_proc_binding(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    slots[0] = VALUE_REF_GET(self);                       /* root proc across alloc */
    KorbBinding *b = korb_alloc(c, slots + 1, sizeof(KorbBinding), KORB_OBJ_BINDING);
    const KorbProc *p = VAL2PROC(slots[0]);
    b->name_syms = korb_proc_binding_scope_tbl; b->name_cnt = 0; b->src_node = NULL;
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->env,  p->env);
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->self, p->self);
    ARO_STORE(c, b, (VALUE *)(uintptr_t)&b->extra, KORB_NIL);
    return RESULT_OK((VALUE)b);
}
static void korb_kw_arity_flags(const void *kwp, bool *req, bool *opt, bool *kwrest);   /* fwd (defined below) */
/* Proc#arity: #required positional, negated as -(req+1) when optional/rest make
 * it variable.  (Symbol#to_proc → -2, matching CRuby.) */
static korb_sword_t korb_method_arity(const struct korb_method *km);   /* fwd (defined below) */
static RESULT korb_m_proc_arity(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbProc *p = VAL2PROC(VALUE_REF_GET(self));
    if (p->iseq == NULL) {
        if (p->self != KORB_NIL) {                        /* Method#to_proc → the bound method's own arity */
            const VALUE klass = korb_dispatch_class(c, p->self);
            const struct korb_method *km = KORB_CLASS_P(klass) ? korb_class_find_method(klass, p->sym_mid, NULL) : NULL;
            if (km == NULL) km = korb_method_lookup(c->vm, p->sym_mid);   /* top-level def lives in the global table */
            return RESULT_OK(LONG2FIX(km ? korb_method_arity(km) : -1));
        }
        return RESULT_OK(LONG2FIX(-2));                   /* Symbol#to_proc */
    }
    const NODE *e = p->iseq;
    const bool lam = p->is_lambda;
    /* Count param kinds from the full param list (0=req 1=opt 2=rest 3=keyreq
     * 4=key 5=keyrest 6=block).  Required positionals = leading + post (both req).
     * Mandatory = required positionals + (any required kw ? 1 : 0).  The result is
     * negated when there is a rest, an optional positional in a *lambda*, or (for
     * a lambda) optional-only keyword args. A plain proc never negates for its
     * optionals/keywords. */
    const struct korb_param_info *const pi = (const struct korb_param_info *)e->u.node_entry.param_info;
    if (pi == NULL) {                                     /* no params → 0 */
        return RESULT_OK(LONG2FIX(0));
    }
    uint32_t reqp = 0, optp = 0; bool has_rest = false, kreq = false, kopt = false, kwrest = false;
    for (uint32_t i = 0; i < pi->n; i++) {
        switch (pi->e[i].kind) {
          case 0: reqp++; break;
          case 1: optp++; break;
          case 2: has_rest = true; break;
          case 3: kreq = true; break;
          case 4: kopt = true; break;
          case 5: kwrest = true; break;
          default: break;                                 /* block param: ignored */
        }
    }
    const uint32_t req = reqp + (kreq ? 1u : 0u);
    const bool negate = has_rest || (lam && optp > 0) || (lam && (kopt || kwrest) && !kreq);
    return RESULT_OK(LONG2FIX(negate ? -((korb_sword_t)req + 1) : (korb_sword_t)req));
}
/* Proc#source_location → [file, line] where the block was written, or nil. */
static RESULT korb_m_proc_source_location(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbProc *const p = VAL2PROC(VALUE_REF_GET(self));
    return korb_srcloc_result(c, slots, p->iseq);
}
/* Proc#parameters — [[kind, name], ...] from the parse-time param_info (cold;
 * never on the call/yield hot path).  A non-lambda proc reports a required
 * positional as :opt (CRuby semantics); keyreq/rest/block keep their kind. */
static RESULT korb_m_proc_parameters(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    /* Capture everything off the proc BEFORE the first alloc — korb_ary_new GCs
     * and may move the KorbObject, so `p` must not be dereferenced afterwards.
     * pi (node param_info) and the booleans are all stable values. */
    const KorbProc *const p = VAL2PROC(VALUE_REF_GET(self));
    const struct korb_param_info *const pi = p->iseq ? (const struct korb_param_info *)p->iseq->u.node_entry.param_info : NULL;
    bool lam = p->is_lambda;
    /* `lambda:` keyword overrides required-vs-optional treatment (nil = ignore). */
    if (VALUE_SLICE_LEN(a) >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, VALUE_SLICE_LEN(a) - 1))) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, VALUE_SLICE_LEN(a) - 1));
        const int32_t li = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "lambda", 6)));
        if (li >= 0 && korb_items_data(h->items)[2 * li + 1] != KORB_NIL) lam = KORB_TRUTHY(korb_items_data(h->items)[2 * li + 1]);
    }
    const bool symproc = (p->iseq == NULL);
    static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block", "nokey" };
    const uint32_t n = pi ? pi->n : 0;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n ? n : 1));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);
    if (!pi) {                                            /* symbol proc → [[:req], [:rest]]; no params → [] */
        if (symproc) {
            const char *const sp[] = { "req", "rest" };
            for (int j = 0; j < 2; j++) {
                slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));
                CHECK(korb_ary_push_val(c, slots + 2, VALUE_REF_AT(&slots[1]), ID2SYM(korb_intern(c->vm, sp[j], (uint32_t)strlen(sp[j])))));
                CHECK(korb_ary_push_val(c, slots + 2, res, slots[1]));
            }
        }
        return RESULT_OK(VALUE_REF_GET(res));
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t kind = pi->e[i].kind;
        const char *kn = (kind == 0 && !lam) ? "opt" : knames[kind];
        const uint32_t nm = pi->e[i].name;
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, nm ? 2 : 1));
        VALUE_REF sub = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(korb_intern(c->vm, kn, (uint32_t)strlen(kn)))));
        if (nm) CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(nm)));
        CHECK(korb_ary_push_val(c, slots + 2, res, VALUE_REF_GET(sub)));
    }
    return RESULT_OK(VALUE_REF_GET(res));
}
static RESULT korb_m_meth_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(ID2SYM(VAL2METH(VALUE_REF_GET(self))->mid));
}
/* Resolve a bound Method to its korb_method entry: receiver-class MRO first,
 * then the global function table (top-level def / builtin like `p`). */
static const struct korb_method *korb_meth_resolve(CTX *c, const KorbMethod *m) {
    /* unbound: recv IS the owner class → look the method up directly there. */
    const VALUE klass = m->unbound ? m->recv : korb_dispatch_class(c, m->recv);
    const struct korb_method *km = KORB_CLASS_P(klass) ? korb_class_find_method(klass, m->mid, NULL) : NULL;
    if (km == NULL) km = korb_method_lookup(c->vm, m->mid);
    return km;
}
/* CRuby Method#arity: required positional count, negated as -(req+1) when an
 * optional / rest param makes the count variable.  Variadic builtins → -1. */
/* keyword params → has-required-kw / has-optional-kw / has-**kwrest. */
static void korb_kw_arity_flags(const void *kwp, bool *req, bool *opt, bool *kwrest) {
    *req = *opt = *kwrest = false;
    if (kwp == NULL) return;
    const struct korb_kw_info *const ki = (const struct korb_kw_info *)kwp;
    *kwrest = ki->kwrest_slot >= 0;
    for (uint32_t i = 0; i < ki->count; i++) {
        if (ki->entries[i].deflt == NULL) *req = true; else *opt = true;
    }
}
/* CRuby arity: required keyword(s) add +1 (total) to the required count and keep
 * it positive; only-optional-kw or **kwrest (with no required kw) make it
 * variadic, like optional positionals / *rest. */
static korb_sword_t korb_method_arity(const struct korb_method *km) {
    switch (km->kind) {
      case KORB_METHOD_ATTR_R: return 0;
      case KORB_METHOD_ATTR_W: return 1;
      case KORB_METHOD_BUILTIN:
      case KORB_METHOD_CFUNC:  return km->params_cnt;          /* -1 = variadic */
      case KORB_METHOD_DM: {
        const KorbProc *p = VAL2PROC(km->dm_proc);
        if (p->iseq == NULL) return -2;
        const NODE *e = p->iseq;
        bool kreq, kopt, kwrest; korb_kw_arity_flags(e->u.node_entry.kw_info, &kreq, &kopt, &kwrest);
        const bool varpos = (e->u.node_entry.opt_defaults != NULL) || (e->u.node_entry.rest_slot >= 0);
        const uint32_t req = (varpos ? e->u.node_entry.req_cnt : e->u.node_entry.params_cnt) + (kreq ? 1u : 0u);
        const bool var = varpos || ((kopt || kwrest) && !kreq);
        return var ? -((korb_sword_t)req + 1) : (korb_sword_t)req;
      }
      default: {                                               /* ISEQ */
        bool kreq, kopt, kwrest; korb_kw_arity_flags(km->kw_info, &kreq, &kopt, &kwrest);
        const bool varpos = (km->opt_defaults != NULL) || (km->rest_slot >= 0);
        const uint32_t req = km->req_cnt + km->post_cnt + (kreq ? 1u : 0u);
        const bool var = varpos || ((kopt || kwrest) && !kreq);
        return var ? -((korb_sword_t)req + 1) : (korb_sword_t)req;
      }
    }
}
static RESULT korb_m_meth_arity(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const struct korb_method *km = korb_meth_resolve(c, VAL2METH(VALUE_REF_GET(self)));
    return RESULT_OK(LONG2FIX(km ? korb_method_arity(km) : -1));
}
/* Method/UnboundMethod#source_location → [file, line] of the def, or nil for a
 * C-defined (builtin/attr) method. */
static RESULT korb_m_meth_source_location(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const struct korb_method *const km = korb_meth_resolve(c, VAL2METH(VALUE_REF_GET(self)));
    return korb_srcloc_result(c, slots, km ? km->body : NULL);
}
/* #original_name: the name at original definition, surviving aliases. */
static RESULT korb_m_meth_original_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    const struct korb_method *const km = korb_meth_resolve(c, m);
    return RESULT_OK(ID2SYM(km && km->orig_mid ? km->orig_mid : m->mid));
}
/* Method#== / UnboundMethod#==: same kind (bound vs unbound), same receiver/owner,
 * and the SAME underlying definition — so aliases (to_int↔to_i) compare equal. */
static RESULT korb_m_meth_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    const VALUE ov = VALUE_SLICE_GET(a, 0);
    if (!KORB_METHOD_P(ov)) return RESULT_OK(KORB_FALSE);
    const KorbMethod *const m1 = VAL2METH(VALUE_REF_GET(self));
    const KorbMethod *const m2 = VAL2METH(ov);
    if (m1->unbound != m2->unbound) return RESULT_OK(KORB_FALSE);
    /* bound methods must share the receiver object; unbound ones only need the
     * same underlying definition (extracting from different subclasses is equal). */
    if (!m1->unbound && m1->recv != m2->recv) return RESULT_OK(KORB_FALSE);
    /* same owner + name is trivially the same method; cross-owner unbound
     * (e.g. a subclass that overrides) must fall through to resolve the actual
     * definition so an override compares unequal. */
    if (m1->recv == m2->recv && m1->mid == m2->mid) return RESULT_OK(KORB_TRUE);
    const struct korb_method *const e1 = korb_meth_resolve(c, m1);
    const struct korb_method *const e2 = korb_meth_resolve(c, m2);
    if (e1 == NULL || e2 == NULL) return RESULT_OK(e1 == e2 ? KORB_TRUE : KORB_FALSE);
    if (e1->kind != e2->kind) return RESULT_OK(KORB_FALSE);
    bool same;
    switch (e1->kind) {
      case KORB_METHOD_ISEQ:    same = e1->body == e2->body; break;
      case KORB_METHOD_CFUNC:   same = e1->rfn == e2->rfn && e1->rbfn == e2->rbfn; break;
      case KORB_METHOD_BUILTIN: same = e1->bfn == e2->bfn; break;
      case KORB_METHOD_ATTR_R:
      case KORB_METHOD_ATTR_W:  same = e1->attr_ivar == e2->attr_ivar; break;
      case KORB_METHOD_DM:      same = e1->dm_proc == e2->dm_proc; break;
      default:                  same = false;
    }
    return RESULT_OK(same ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_meth_owner(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (m->owner != KORB_NIL) return RESULT_OK(m->owner);           /* fixed owner (e.g. from super_method) */
    const struct korb_method *km = korb_meth_resolve(c, m);
    if (km && km->owner != KORB_NIL) return RESULT_OK(km->owner);   /* defining class/module */
    if (m->unbound) return RESULT_OK(m->recv);                      /* unresolvable: the class it came from */
    return RESULT_OK(korb_builtin_class_obj(c->vm, KORB_C_OBJECT)); /* global fn → Object */
}
/* Method#super_method → a Method for the definition above this one in the
 * receiver's MRO (fixed super-owner), or nil if there's no super. */
static int korb_linearize_mro(VALUE klass, VALUE *buf, int max);   /* fwd (korb_runtime.c) */
static RESULT korb_m_meth_super_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    const struct korb_method *const km = korb_meth_resolve(c, m);
    const VALUE cur_owner = (m->owner != KORB_NIL) ? m->owner : (km ? km->owner : KORB_NIL);
    if (!KORB_CLASS_P(cur_owner)) return RESULT_OK(KORB_NIL);
    const VALUE start = m->unbound ? m->recv : korb_dispatch_class(c, m->recv);
    const uint32_t mid = m->mid;
    const uint8_t unbound = m->unbound;
    const VALUE recv = m->recv;
    VALUE mro[256];
    const int n = korb_linearize_mro(start, mro, 256);
    int di = -1;
    for (int i = 0; i < n; i++) if (mro[i] == cur_owner) { di = i; break; }
    if (di < 0) return RESULT_OK(KORB_NIL);
    for (int i = di + 1; i < n; i++) {
        if (!KORB_CLASS_P(mro[i])) continue;
        const KorbClass *const mk = VAL2CLASS(mro[i]);
        bool has = false;
        for (uint32_t q = 0; q < mk->method_cnt; q++)
            if (mk->methods[q]->mid == mid && mk->methods[q]->mid != UINT32_MAX) { has = true; break; }
        if (!has) continue;
        slots[0] = mro[i];                                          /* root super-owner across the alloc */
        if (unbound)                                                /* unbound: recv IS the owner class */
            return korb_unbound_new(c, slots + 1, slots[0], mid);
        RESULT r = korb_method_new(c, slots + 1, recv, mid);        /* bound: keep recv, fix the super-owner */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        ARO_STORE(c, VAL2METH(r.value), (VALUE *)(uintptr_t)&VAL2METH(r.value)->owner, slots[0]);
        return r;
    }
    return RESULT_OK(KORB_NIL);
}
/* push a [kind] (or [kind, name]) param descriptor onto the result array `res`. */
static RESULT korb_param_push(CTX *c, VALUE *slots, VALUE_REF res, const char *kind) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, 1));
    CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_intern(c->vm, kind, (uint32_t)strlen(kind)))));
    return korb_ary_push_val(c, slots + 1, res, slots[0]);
}
/* Method/UnboundMethod#parameters — C methods are nameless: variadic → [[:rest]],
 * fixed-arity → [[:req]]×n; ISEQ → positional kinds (names not retained). */
static RESULT korb_m_meth_parameters(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const struct korb_method *km = korb_meth_resolve(c, VAL2METH(VALUE_REF_GET(self)));
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF res = VALUE_REF_AT(&slots[0]);
    if (km == NULL) return RESULT_OK(VALUE_REF_GET(res));
    if (km->kind == KORB_METHOD_ATTR_W) { CHECK(korb_param_push(c, slots + 1, res, "req")); return RESULT_OK(VALUE_REF_GET(res)); }
    if (km->kind == KORB_METHOD_ATTR_R) return RESULT_OK(VALUE_REF_GET(res));
    if (km->kind == KORB_METHOD_BUILTIN || km->kind == KORB_METHOD_CFUNC) {
        if (km->params_cnt < 0) { CHECK(korb_param_push(c, slots + 1, res, "rest")); }
        else for (int32_t i = 0; i < km->params_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
        return RESULT_OK(VALUE_REF_GET(res));
    }
    /* ISEQ: the full param_info (kinds + names + kwargs + block), like Proc#parameters
     * — methods are strict (a required positional is :req, never :opt). param_info is
     * immortal libc (no GC). NULL (e.g. define_method) → fall back to the counts. */
    const struct korb_param_info *const pi = (const struct korb_param_info *)km->param_info;
    if ((km->kind == KORB_METHOD_ISEQ || km->kind == KORB_METHOD_DM) && pi != NULL) {   /* define_method carries its block's params */
        static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block", "nokey" };
        for (uint32_t i = 0; i < pi->n; i++) {
            const uint8_t kind = pi->e[i].kind;
            const char *const kn = knames[kind];
            const uint32_t nm = pi->e[i].name;
            slots[1] = UNWRAP(korb_ary_new(c, slots + 1, nm ? 2 : 1));
            VALUE_REF sub = VALUE_REF_AT(&slots[1]);
            CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(korb_intern(c->vm, kn, (uint32_t)strlen(kn)))));
            if (nm) CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(nm)));
            CHECK(korb_ary_push_val(c, slots + 2, res, VALUE_REF_GET(sub)));
        }
        return RESULT_OK(VALUE_REF_GET(res));
    }
    for (uint32_t i = 0; i < km->req_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    const uint32_t nopt = (km->params_cnt >= 0 && (uint32_t)km->params_cnt > km->req_cnt) ? (uint32_t)km->params_cnt - km->req_cnt : 0;
    for (uint32_t i = 0; i < nopt; i++) CHECK(korb_param_push(c, slots + 1, res, "opt"));
    if (km->rest_slot >= 0) CHECK(korb_param_push(c, slots + 1, res, "rest"));
    for (uint32_t i = 0; i < km->post_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    return RESULT_OK(VALUE_REF_GET(res));
}
/* Module#instance_method(name) → UnboundMethod owned by the class. */
static RESULT korb_m_class_instance_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t mid;                                        /* Symbol/String, or #to_str-coercible */
    { RESULT r = korb_alias_argsym(c, slots, VALUE_SLICE_GET(a, 0), &mid); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls) || korb_class_find_method(cls, mid, NULL) == NULL)) {
        /* subsystems registered after boot can still land Kernel-level methods on
         * Object (korb_relocate_object_methods runs once); look there too. */
        if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->name_sym == korb_intern(c->vm, "Kernel", 6)) {
            const VALUE objc = korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
            if (KORB_CLASS_P(objc) && korb_class_find_method(objc, mid, NULL) != NULL)
                return korb_unbound_new(c, slots, cls, mid);
        }
        RESULT ne = korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for class '%s'",   /* CRuby: NameError, not NoMethodError */
                          korb_sym_name(c->vm, mid), korb_type_name(cls));
        if (LIKELY(KORB_EXC_P(ne.value))) {              /* NameError#name → the missing method symbol */
            slots[0] = ne.value;
            korb_exc_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_intern(c->vm, "@__name", 7)), ID2SYM(mid));
            ne.value = slots[0];
        }
        return ne;
    }
    return korb_unbound_new(c, slots, cls, mid);
}
/* Method#hash — equal for methods that are #eql? (same underlying definition,
 * and for bound methods the same receiver), consistent with korb_m_meth_eq. */
static RESULT korb_m_meth_hash(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbMethod *m = VAL2METH(VALUE_REF_GET(self));
    const struct korb_method *const e = korb_meth_resolve(c, m);
    /* Hash the SAME definition identity korb_m_meth_eq compares, so aliases
     * (foo / bar sharing one body) collide as CRuby requires. */
    uintptr_t def_id = (uintptr_t)e;
    if (e != NULL) switch (e->kind) {
        case KORB_METHOD_ISEQ:    def_id = (uintptr_t)e->body; break;
        case KORB_METHOD_CFUNC:   def_id = (uintptr_t)e->rfn ^ (uintptr_t)e->rbfn; break;
        case KORB_METHOD_BUILTIN: def_id = (uintptr_t)e->bfn; break;
        case KORB_METHOD_ATTR_R:
        case KORB_METHOD_ATTR_W:  def_id = (uintptr_t)e->attr_ivar; break;
        case KORB_METHOD_DM:      def_id = (uintptr_t)e->dm_proc; break;
        default: break;
    }
    uintptr_t h = def_id * 0x9e3779b97f4a7c15ULL;
    if (!m->unbound) {                                             /* bound: fold in the receiver's #hash */
        slots[0] = m->recv;
        RESULT hr = korb_send(c, slots + 1, korb_intern(c->vm, "hash", 4), 0, 0);
        if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        if (FIXNUM_P(hr.value)) h ^= (korb_word_t)FIX2LONG(hr.value) * 0x100000001b3ULL;
    }
    return RESULT_OK(LONG2FIX((korb_sword_t)(h & (((korb_word_t)1 << 62) - 1))));
}
/* Method#unbind → UnboundMethod owned by the receiver's class. */
static RESULT korb_m_meth_unbind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (m->unbound) return RESULT_OK(VALUE_REF_GET(self));
    return korb_unbound_new(c, slots, korb_dispatch_class(c, m->recv), m->mid);
}
/* UnboundMethod#bind(obj) → Method bound to obj (obj must be a kind_of owner). */
static RESULT korb_m_meth_bind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (UNLIKELY(!m->unbound)) return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'bind' for a Method");
    slots[0] = m->recv;                                             /* owner class (rooted across dispatch/alloc) */
    const uint32_t mid = m->mid;
    bool ok = korb_case_eq(c, slots[0], VALUE_SLICE_GET(a, 0));      /* owner === obj  ⇔  obj.is_a?(owner) */
    if (!ok && KORB_CLASS_P(slots[0]) && VAL2CLASS(slots[0])->is_module)
        ok = true;                                                   /* a module's method binds to any object (Ruby 3.0+) */
    if (!ok && KORB_CLASS_P(slots[0]) && VAL2CLASS(slots[0])->is_singleton && KORB_CLASS_P(VALUE_SLICE_GET(a, 0))) {
        slots[1] = VALUE_SLICE_GET(a, 0);                            /* a class method binds to a subclass */
        const RESULT sr = korb_obj_singleton(c, slots + 2, slots[1]);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        ok = korb_class_has_ancestor(sr.value, slots[0]);
    }
    if (UNLIKELY(!ok)) {
        char onm[192]; korb_class_qname_into(c, slots[0], onm, sizeof onm);   /* the owner's NAME, not "Class" */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "bind argument must be an instance of %s", onm);
    }
    slots[1] = VALUE_SLICE_GET(a, 0);                              /* obj (re-read after dispatch) */
    RESULT r = korb_method_new(c, slots + 2, slots[1], mid);       /* bound: recv = obj */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    ARO_STORE(c, VAL2METH(r.value), (VALUE *)(uintptr_t)&VAL2METH(r.value)->owner, slots[0]);   /* fix the dispatch to the unbound's owner */
    return r;
}
/* UnboundMethod#bind_call(obj, *args) → bind then call. */
static RESULT korb_m_meth_bind_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const uint32_t mid = m->mid, argc = VALUE_SLICE_LEN(a) - 1;
    const VALUE owner = m->unbound ? m->recv : m->owner;   /* unbound: recv is the owner class */
    if (owner != KORB_NIL && KORB_CLASS_P(owner)) {        /* invoke the FIXED method from its owner */
        struct korb_method *const entry = korb_class_find_method(owner, mid, NULL);
        if (LIKELY(entry != NULL && entry->kind == KORB_METHOD_ISEQ)) {
            slots[0] = VALUE_SLICE_GET(a, 0);             /* self (the bind target) */
            slots[1] = owner;
            for (uint32_t i = 0; i < argc; i++) slots[2 + i] = VALUE_SLICE_GET(a, 1 + i);
            return korb_invoke_method(c, slots + 2 + argc, entry, argc, 0, mid, slots[0], slots[1], NULL, NULL, KORB_NIL);
        }
        if (entry != NULL) {                              /* builtin (cfunc/attr): still the CAPTURED entry, not a
                                                           * re-dispatch — a singleton override of the same name on
                                                           * the target must not win (Module#name is the classic). */
            slots[0] = VALUE_SLICE_GET(a, 0);             /* recv, in the slot below the args */
            for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, 1 + i);
            return korb_dispatch_method(c, slots + 1 + argc, entry, mid, 0, argc, owner, NULL, NULL, NULL);
        }
    }
    slots[0] = VALUE_SLICE_GET(a, 0);                                /* recv */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, 1 + i);
    return korb_send(c, slots + 1 + argc, mid, 0, argc);
}

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
