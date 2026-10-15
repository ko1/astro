/* koruby_precise — symbol.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Symbol methods ------------------------------------------------------ */

/* Symbol#name / #to_s / #id2name — the *same* frozen String on every call
 * (CRuby 4.1 rb_sym2str hands back an fstring; to_s was chilled in 3.4/4.0). */
static RESULT korb_m_sym_name(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    struct korb_vm *const vm = c->vm;
    const uint32_t id = SYM2ID(KORB_REF_GET(self));
    if (LIKELY(id < vm->sym_fstr_cap && vm->sym_fstr[id] != 0)) return RESULT_OK(vm->fstr_vals[vm->sym_fstr[id] - 1]);
    const char *const nm = korb_sym_name(vm, id);
    const uint32_t len = vm->sym_lens[id];             /* the name may hold NUL bytes */
    const korb_value v = korb_fstr_get(c, slots, nm, len, vm->sym_encs[id]);
    /* remember the pool slot per symbol: the pool lookup hashed the name each call (railsbench 0.3%) */
    if (id >= vm->sym_fstr_cap) {
        uint32_t cap = vm->sym_fstr_cap ? vm->sym_fstr_cap : 1024;
        while (cap <= id) cap *= 2;
        uint32_t *const t = realloc(vm->sym_fstr, sizeof(uint32_t) * cap);
        if (t == NULL) { fprintf(stderr, "koruby_precise: oom (sym_fstr)\n"); abort(); }
        memset(t + vm->sym_fstr_cap, 0, sizeof(uint32_t) * (cap - vm->sym_fstr_cap));
        vm->sym_fstr = t; vm->sym_fstr_cap = cap;
    }
    vm->sym_fstr[id] = korb_fstr_slot(vm, v) + 1;
    return RESULT_OK(v);
}
static RESULT korb_m_sym_to_sym(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)a; return RESULT_OK(KORB_REF_GET(self)); }
/* Symbol.all_symbols → an Array of every interned Symbol. */
static RESULT korb_m_sym_all_symbols(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)self; (void)a;
    const uint32_t n = c->vm->sym_cnt;                /* snapshot: no interning happens in the loop */
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 1; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, dst, ID2SYM(i)));   /* id 0 is the reserved sentinel */
    return RESULT_OK(KORB_REF_GET(dst));
}
static RESULT korb_m_sym_empty(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a;
    return RESULT_OK(korb_sym_name(c->vm, SYM2ID(KORB_REF_GET(self)))[0] == '\0' ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_sym_len(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a;
    const uint32_t id = SYM2ID(KORB_REF_GET(self));   /* sym_lens, not strlen: a name may hold NUL */
    return RESULT_OK(LONG2FIX(korb_enc_charcount(c->vm, c->vm->sym_encs[id], korb_sym_name(c->vm, id), c->vm->sym_lens[id])));
}

/* ---- nil / true / false methods ------------------------------------------ */

/* Return a cached frozen String (created once, then the same object each call) —
 * matches CRuby's nil/true/false #to_s returning a shared frozen literal. */
static RESULT korb_cached_frozen_str(CTX *c, korb_value *slots, korb_value *cache, const char *s, uint32_t len) {
    if (LIKELY(KORB_STRING_P(*cache))) return RESULT_OK(*cache);
    RESULT r = korb_str_new(c, slots, s, len);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    ((AroObjectHeader *)(uintptr_t)r.value)->flags |= KORB_FL_FROZEN;
    KORB_STR_ENC_SET(r.value, KORB_ENC_USASCII);          /* nil/true/false #to_s (CRuby) */
    *cache = r.value;                                    /* store in the vm (a GC root) */
    return RESULT_OK(*cache);
}
static RESULT korb_m_nil_to_s(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_nil_to_s, "", 0); }
static RESULT korb_m_nil_to_i(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(0)); }
static RESULT korb_m_nil_to_a(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_ary_new(c, slots, 0); }
static RESULT korb_m_nil_to_r(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_rat_new(c, slots, 0, 1); }   /* nil.to_r → (0/1) */
static RESULT korb_m_nil_rationalize(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {   /* nil.rationalize([eps]) → (0/1); ignores eps, but >1 arg is an ArgumentError */
    (void)self;
    if (UNLIKELY(KORB_SLICE_LEN(a) > 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..1)", KORB_SLICE_LEN(a));
    return korb_rat_new(c, slots, 0, 1);
}
static RESULT korb_m_nil_to_f(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_float_new(c, slots, 0.0); }   /* nil.to_f → 0.0 */
static RESULT korb_m_nil_to_h(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_hash_new(c, slots, 4); }       /* nil.to_h → {} */
static RESULT korb_m_nil_to_c(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_cpx_new(c, slots, LONG2FIX(0), LONG2FIX(0)); }   /* nil.to_c → (0+0i) */
static RESULT korb_m_true_to_s(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_true_to_s, "true", 4); }
static RESULT korb_m_false_to_s(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)self;(void)a; return korb_cached_frozen_str(c, slots, &c->vm->str_false_to_s, "false", 5); }

/* ---- universal (Object) methods ------------------------------------------ */

static RESULT korb_m_obj_nil_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
/* Default BasicObject#method_missing(name, *args) — raises NoMethodError.  Lets a
 * user method_missing call `super` for the unhandled case (CRuby semantics). */
static RESULT korb_m_obj_method_missing(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const korb_value name = KORB_SLICE_LEN(a) >= 1 ? KORB_SLICE_GET(a, 0) : KORB_NIL;
    char nmbuf[256];   /* copy the method name: korb_recv_desc may dispatch #name → GC → move a String arg */
    if (SYMBOL_P(name))       snprintf(nmbuf, sizeof nmbuf, "%s", korb_sym_name(c->vm, SYM2ID(name)));
    else if (KORB_STRING_P(name)) snprintf(nmbuf, sizeof nmbuf, "%.*s", (int)VAL2STR(name)->len, korb_str_ptr(VAL2STR(name)));
    else                      snprintf(nmbuf, sizeof nmbuf, "(unknown)");
    const char *const nm = nmbuf;
    const korb_value recv = KORB_REF_GET(self);
    char buf[256];   /* CRuby-shaped: "class Foo" / "module Bar" / "an instance of Foo" / … */
    const char *tn = korb_nomethod_recv_desc(c, slots, recv, buf, sizeof buf);
    /* A bare-name miss the user's #method_missing handed on with `super` keeps
     * CRuby's NameError shape; the mid must match, so a stale mark is inert. */
    const bool vcall = c->mm_vcall_mid != 0 && SYMBOL_P(name) && SYM2ID(name) == c->mm_vcall_mid;
    const bool vis = c->mm_vis_mid != 0 && SYMBOL_P(name) && SYM2ID(name) == c->mm_vis_mid;
    c->mm_vcall_mid = 0; c->mm_vis_mid = 0;
    RESULT r = vcall ? korb_raise(c, slots, KORB_E_NAME, 0, "undefined local variable or method '%s' for %s", nm, tn)
             : vis   ? korb_raise(c, slots, KORB_E_NOMETHOD, 0, "%s method '%s' called for %s", c->mm_vis == 1 ? "private" : "protected", nm, tn)
                     : korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method '%s' for %s", nm, tn);
    if (LIKELY(KORB_EXC_P(r.value))) {                    /* attach #name / #receiver metadata */
        slots[0] = r.value;
        korb_ref eref = KORB_REF_AT(&slots[0]);
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern_lit(c->vm, "@__name", 7)), name);   /* Symbol immediate */
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern_lit(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 1, eref, ID2SYM(korb_intern_lit(c->vm, "@__receiver", 11)), KORB_REF_GET(self));
        const uint32_t na = KORB_SLICE_LEN(a);            /* @__args = args after the name (method_missing(name, *args)) */
        RESULT ar = korb_ary_new(c, slots + 1, na > 1 ? na - 1 : 0);
        if (LIKELY(ar.state == KORB_NORMAL)) {
            slots[1] = ar.value;
            korb_ref argsref = KORB_REF_AT(&slots[1]);
            for (uint32_t j = 1; j < na; j++)
                korb_ary_push_val(c, slots + 2, argsref, KORB_SLICE_GET(a, j));
            korb_exc_ivar_set(c, slots + 2, eref, ID2SYM(korb_intern_lit(c->vm, "@__args", 7)), KORB_REF_GET(argsref));
        }
        r.value = KORB_REF_GET(eref);
    }
    return r;
}
static RESULT korb_m_nil_nil_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_obj_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a)  { (void)c;(void)slots; return RESULT_OK(korb_value_eq(KORB_REF_GET(self), KORB_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }
/* Object#=== — the default is `self == other`, so a subclass's overridden #==
 * is honoured (dispatch rather than the identity korb_m_obj_eq). */
static RESULT korb_m_obj_case_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    if (KORB_REF_GET(self) == KORB_SLICE_GET(a, 0)) return RESULT_OK(KORB_TRUE);   /* rb_equal: the same object, without #== */
    slots[0] = KORB_REF_GET(self); slots[1] = KORB_SLICE_GET(a, 0);
    const RESULT r = korb_send_impl(c, slots + 2, c->vm->mid_eq, 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_obj_eql(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots; return RESULT_OK(korb_value_eql(KORB_REF_GET(self), KORB_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }  /* type-strict: 1.eql?(1.0) => false */
static RESULT korb_m_obj_neq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    /* BasicObject#!= is !(self == other) — dispatch #== so a user-defined == is honoured. */
    slots[0] = KORB_REF_GET(self); slots[1] = KORB_SLICE_GET(a, 0);
    RESULT r = korb_send(c, slots + 2, c->vm->mid_eq, 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_FALSE : KORB_TRUE);
}
/* Kernel#!~ = !(self =~ other).  C, not Ruby: `$~` is frame-local, and a Ruby
 * body here would keep the match in ITS OWN frame and drop it on return —
 * CRuby's is an intrinsic, so `x !~ re` leaves `$~` set for the caller. */
static RESULT korb_m_obj_nmatch(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = KORB_REF_GET(self); slots[1] = KORB_SLICE_GET(a, 0);
    const RESULT r = korb_send(c, slots + 2, korb_intern_lit(c->vm, "=~", 2), 0, 1);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(KORB_TRUTHY(r.value) ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_obj_equal(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots; return RESULT_OK(KORB_REF_GET(self) == KORB_SLICE_GET(a,0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_itself(CTX *c, korb_value *slots, korb_ref self, korb_slice a) { (void)c;(void)slots;(void)a; return RESULT_OK(KORB_REF_GET(self)); }
/* freeze: no-op (koruby has no frozen state) → self.  frozen?: true only for
 * immediates (Integer/Symbol/nil/true/false), false otherwise. */
static RESULT korb_m_obj_freeze(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a; korb_value v = KORB_REF_GET(self);
    if (AROH_IS_GC_OBJECT(v)) {
        /* an explicit #freeze ends chilled status: from here a mutation is a
         * real FrozenError, not a deprecation warning */
        ((AroObjectHeader *)(uintptr_t)v)->flags &= (uint16_t)~(KORB_FL_CHILLED | KORB_FL_CHILLED_SYM);
        ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_FROZEN;
        /* A frozen Array gets capa=len so node_shl's existing `len < capa` room
         * check fails → the (rare) slow path does the FrozenError raise.  Keeps
         * the hot `a << x` fast path free of any frozen test. */
        if (KORB_ARRAY_P(v)) VAL2ARY(v)->capa = VAL2ARY(v)->len;
        /* an already-materialized singleton class freezes with its object (CRuby) */
        if (((const AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_HAS_KLASS) {
            const korb_value ov = korb_klass_override_get(c->vm, v);
            if (KORB_CLASS_P(ov) && VAL2CLASS(ov)->is_singleton)
                ((AroObjectHeader *)(uintptr_t)ov)->flags |= KORB_FL_FROZEN;
        }
    }
    return RESULT_OK(v);
}
static RESULT korb_m_obj_frozen_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; korb_value v = KORB_REF_GET(self);
    if (!AROH_IS_GC_OBJECT(v)) return RESULT_OK(KORB_TRUE);   /* immediates are frozen */
    const uint16_t fl = ((AroObjectHeader *)(uintptr_t)v)->flags;
    /* A CHILLED String carries FROZEN so the mutation guards catch it, but it is
     * not frozen yet as far as Ruby is concerned. */
    if (UNLIKELY(KORB_STRING_P(v) && (fl & KORB_FL_CHILLED))) return RESULT_OK(KORB_FALSE);
    return RESULT_OK((fl & KORB_FL_FROZEN) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_obj_cmp(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    /* CRuby Object#<=>: 0 if self == other (dispatched, so a custom #== counts), else nil. */
    const korb_value s = KORB_REF_GET(self), o = KORB_SLICE_GET(a, 0);
    if (s == o) return RESULT_OK(LONG2FIX(0));                 /* identical */
    slots[0] = s; slots[1] = o;
    RESULT r = korb_send_impl(c, slots + 2, korb_intern_lit(c->vm, "==", 2), 0, 1, NULL, NULL, NULL);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK((r.value != KORB_NIL && r.value != KORB_FALSE) ? LONG2FIX(0) : KORB_NIL);
}
/* resolve a Symbol/String name arg to the ivar-key Symbol (`:@x`, `"@x"`). */
static bool korb_name_to_sym(CTX *c, korb_value name, korb_value *out) {
    if (SYMBOL_P(name)) { *out = name; return true; }
    if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); *out = ID2SYM(korb_intern_enc(c->vm, korb_str_ptr(s), s->len, KORB_STR_ENC(name))); return true; }
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
static RESULT korb_ivar_name_arg(CTX *c, korb_value *slots, korb_value name, korb_value *out_sym, bool *ok) {
    if (korb_name_to_sym(c, name, out_sym)) { *ok = true; return RESULT_OK(KORB_NIL); }
    if (KORB_OBJECT_P(name) && korb_responds_to_coerce_p(c, slots, &name, korb_intern_lit(c->vm, "to_str", 6))) {
        slots[0] = name;
        RESULT sr = korb_send_impl(c, slots + 1, korb_intern_lit(c->vm, "to_str", 6), 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (korb_name_to_sym(c, sr.value, out_sym)) { *ok = true; return RESULT_OK(KORB_NIL); }
    }
    *ok = false; return RESULT_OK(KORB_NIL);
}
/* Raise "'<name>' is not allowed as an instance variable name" (NameError) with
 * #name (the given name, verbatim — CRuby echoes the String/Symbol as passed)
 * and #receiver (self) attached. */
static RESULT korb_raise_bad_ivar_name(CTX *c, korb_value *slots, korb_value self, korb_value orig_name, uint32_t sym_id) {
    slots[0] = self; slots[1] = orig_name;
    RESULT ne = korb_raise(c, slots + 2, KORB_E_NAME, 0, "'%s' is not allowed as an instance variable name", korb_sym_name(c->vm, sym_id));
    if (LIKELY(KORB_EXC_P(ne.value))) {
        slots[2] = ne.value;
        korb_ref eref = KORB_REF_AT(&slots[2]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern_lit(c->vm, "@__name", 7)), slots[1]);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern_lit(c->vm, "@__has_recv", 11)), KORB_TRUE);
        korb_exc_ivar_set(c, slots + 3, eref, ID2SYM(korb_intern_lit(c->vm, "@__receiver", 11)), slots[0]);
        ne.value = slots[2];
    }
    return ne;
}
static RESULT korb_m_obj_ivar_set(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value sym, name = KORB_SLICE_GET(a, 0);
    bool ok; RESULT nr = korb_ivar_name_arg(c, slots, name, &sym, &ok);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    if (UNLIKELY(!ok))
        return korb_raise_not_sym(c, slots, KORB_SLICE_GET(a, 0));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), SYM2ID(sym));
    if (UNLIKELY(!AROH_IS_GC_OBJECT(KORB_REF_GET(self)) || KORB_BIGNUM_P(KORB_REF_GET(self)) ||
                 KORB_FLOAT_P(KORB_REF_GET(self))))                 /* numerics/Symbol/nil/... are frozen */
        return korb_raise_frozen(c, slots, KORB_REF_GET(self));
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    CHECK(korb_ivar_set(c, slots, self, sym, KORB_SLICE_GET(a, 1)));   /* class/exc/user-object/container all handled */
    return RESULT_OK(KORB_SLICE_GET(a, 1));
}
static RESULT korb_m_obj_ivar_get(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value sym, name = KORB_SLICE_GET(a, 0);
    bool ok; RESULT nr = korb_ivar_name_arg(c, slots, name, &sym, &ok);
    if (UNLIKELY(nr.state != KORB_NORMAL)) return nr;
    if (UNLIKELY(!ok))
        return korb_raise_not_sym(c, slots, KORB_SLICE_GET(a, 0));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), SYM2ID(sym));
    (void)slots;
    return RESULT_OK(korb_ivar_get(c, KORB_REF_GET(self), sym));   /* handles class/exc/container/immediate */
}
/* Object#instance_variable_defined?(name) — true if the @ivar is set (non-nil). */
static RESULT korb_m_obj_ivar_defined(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value sym; bool ok;
    { RESULT nr = korb_ivar_name_arg(c, slots, KORB_SLICE_GET(a, 0), &sym, &ok); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    if (UNLIKELY(!ok)) return korb_raise_not_sym(c, slots, KORB_SLICE_GET(a, 0));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), SYM2ID(sym));
    return RESULT_OK(korb_ivar_defined(c, KORB_REF_GET(self), sym) ? KORB_TRUE : KORB_FALSE);   /* membership, not value (an ivar set to nil is defined) */
}
/* Object#remove_instance_variable(name) → the removed value; NameError if unset. */
static RESULT korb_m_obj_remove_ivar(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    korb_value sym; bool ok;
    { RESULT nr = korb_ivar_name_arg(c, slots, KORB_SLICE_GET(a, 0), &sym, &ok); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    if (UNLIKELY(!ok)) return korb_raise_not_sym(c, slots, KORB_SLICE_GET(a, 0));
    if (UNLIKELY(!korb_valid_ivar_name(c->vm, SYM2ID(sym))))
        return korb_raise_bad_ivar_name(c, slots, KORB_REF_GET(self), KORB_SLICE_GET(a, 0), SYM2ID(sym));
    if (UNLIKELY(!AROH_IS_GC_OBJECT(KORB_REF_GET(self)))) return korb_raise_frozen(c, slots, KORB_REF_GET(self));   /* nil, :sym, 1: always frozen */
    KORB_CHECK_FROZEN(c, slots, KORB_REF_GET(self));
    bool found;                                       /* true-remove from the shape / side hash */
    const korb_value old = korb_ivar_remove(c, KORB_REF_GET(self), sym, &found);
    if (!found)
        return korb_raise(c, slots, KORB_E_NAME, 0, "instance variable %s not defined", korb_sym_name(c->vm, SYM2ID(sym)));
    return RESULT_OK(old);
}
/* Object#instance_variables → [:@a, :@b, ...] in definition order. */
static RESULT korb_m_obj_ivars(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const korb_value sv = KORB_REF_GET(self);
    if (KORB_EXC_P(sv)) {                                       /* exception ivars live in a side hash */
        if (VAL2EXC(sv)->ivars == KORB_NIL) return korb_ary_new(c, slots, 0);
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, VAL2HASH(VAL2EXC(sv)->ivars)->len));   /* alloc may GC → re-read self below */
        korb_ref dst = KORB_REF_AT(&slots[0]);
        for (uint32_t i = 0; i < VAL2HASH(VAL2EXC(KORB_REF_GET(self))->ivars)->len; i++) {
            const korb_value k = korb_hash_pairs(VAL2HASH(VAL2EXC(KORB_REF_GET(self))->ivars))[2 * i];
            const char *const nm = SYMBOL_P(k) ? korb_sym_name(c->vm, SYM2ID(k)) : NULL;
            if (nm != NULL && nm[0] != '@') continue;           /* internal slot, as for objects below */
            CHECK(korb_ary_push_val(c, slots + 1, dst, k));
        }
        return RESULT_OK(KORB_REF_GET(dst));
    }
    if (!KORB_OBJECT_P(sv)) {                                   /* class / container / heap object → side-hash keys */
        const korb_value h0 = KORB_CLASS_P(sv) ? VAL2CLASS(sv)->class_ivars
                       : AROH_IS_GC_OBJECT(sv) ? korb_objivar_hash_of(c->vm, sv) : KORB_NIL;
        if (h0 == KORB_NIL) return korb_ary_new(c, slots, 0);
        slots[0] = UNWRAP(korb_ary_new(c, slots + 1, VAL2HASH(h0)->len));   /* alloc may GC → re-read via self below */
        korb_ref dst = KORB_REF_AT(&slots[0]);
        /* A module's autoload table is koruby's own state kept in an ivar; CRuby
         * has no such ivar, and `instance_variables.each { remove_instance_variable }`
         * (bundler's Plugin.reset!) would otherwise wipe every autoload. */
        const korb_value hidden = ID2SYM(c->vm->mid_autoloads);
        /* re-read the hash after every push: the allocation may move it */
        #define KORB_IVH() (KORB_CLASS_P(KORB_REF_GET(self)) ? VAL2CLASS(KORB_REF_GET(self))->class_ivars \
                                                             : korb_objivar_hash_of(c->vm, KORB_REF_GET(self)))
        for (uint32_t i = 0; i < VAL2HASH(KORB_IVH())->len; i++) {
            const korb_value k = korb_hash_pairs(VAL2HASH(KORB_IVH()))[2 * i];
            if (UNLIKELY(k == hidden)) continue;
            if (SYMBOL_P(k) && korb_sym_name(c->vm, SYM2ID(k))[0] != '@') continue;   /* a C extension's hidden ivar (rb_ivar_set(obj, rb_intern("x"), ...)) */
            CHECK(korb_ary_push_val(c, slots + 1, dst, k));
        }
        #undef KORB_IVH
        return RESULT_OK(KORB_REF_GET(dst));
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
    korb_ref dst = KORB_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        /* a name without a leading "@" is an internal slot (koruby stores IO's
         * fd / mode there), which CRuby also keeps out of #instance_variables */
        const char *const nm = korb_sym_name(c->vm, syms[i]);
        if (nm == NULL || nm[0] != '@') continue;
        /* koruby's own classes keep state in @__x ivars (Queue's @__items) that
         * CRuby's C classes do not have: a name the prelude interned is hidden */
        if (nm[1] == '_' && nm[2] == '_' && syms[i] < c->vm->boot_sym_cnt) continue;
        slots[1] = ID2SYM(syms[i]);                             /* syms is libc memory, stable across GC */
        CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    free(syms);
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Struct/Data#instance_variables — like Object#instance_variables but excluding
 * the members (stored internally as @<member> ivars, not user-visible). */
static RESULT korb_m_struct_ivars(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    RESULT all = korb_m_obj_ivars(c, slots, self, a);
    if (UNLIKELY(all.state != KORB_NORMAL)) return all;
    slots[0] = all.value;                                /* all ivars (rooted) */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 2, 0));    /* result */
    korb_ref dst = KORB_REF_AT(&slots[1]);
    const uint32_t n = VAL2ARY(slots[0])->len;
    for (uint32_t i = 0; i < n; i++) {
        const korb_value ivsym = korb_ary_ptr(VAL2ARY(slots[0]))[i];
        const KorbArray *const mem = VAL2ARY(VAL2CLASS(VAL2OBJ(KORB_REF_GET(self))->klass)->members);   /* re-read */
        bool is_member = false;
        for (uint32_t j = 0; j < mem->len; j++)
            if (korb_member_ivar_sym(c->vm, korb_ary_ptr(mem)[j]) == ivsym) { is_member = true; break; }
        if (!is_member) CHECK(korb_ary_push_val(c, slots + 2, dst, ivsym));
    }
    return RESULT_OK(KORB_REF_GET(dst));
}
/* Object#method(:sym) → bound Method. */
static RESULT korb_m_obj_method(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    uint32_t mid;   /* Symbol/String, or #to_str-coercible */
    { RESULT nr = korb_arg_to_mid(c, slots, KORB_SLICE_GET(a, 0), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    const korb_value recv = KORB_REF_GET(self);
    /* an unknown name is a NameError unless #respond_to_missing? claims it
     * (method_missing alone is not enough — CRuby asks respond_to?).
     * korb_responds_to also covers the dispatch special cases (new / send / …)
     * that have no table entry but are real methods all the same. */
    const bool defined = korb_responds_to(c, recv, mid) || korb_method_lookup(c->vm, mid) != NULL;
    if (!defined && !korb_responds_to_coerce(c, slots, recv, mid)) {
        /* the probe above can run #respond_to_missing?: name the class from the
         * rooted self cell, not from a C local read before it */
        char cnm[192]; korb_class_desc_into(c, korb_dispatch_class(c, KORB_REF_GET(self)), cnm, sizeof cnm);
        return korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for class '%s'",
                          korb_sym_name(c->vm, mid), cnm);
    }
    RESULT r = korb_method_new(c, slots, KORB_REF_GET(self), mid);   /* self re-read (coercion may GC) */
    if (!defined && LIKELY(r.state == KORB_NORMAL)) VAL2METH(r.value)->missing = 1;   /* method_missing-backed */
    return r;
}
/* Kernel#public_method — like #method, but only a public one: a private or
 * protected (or missing) name is a NameError, not a Method. */
static RESULT korb_m_obj_public_method(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    uint32_t mid;
    { RESULT nr = korb_arg_to_mid(c, slots, KORB_SLICE_GET(a, 0), &mid); if (UNLIKELY(nr.state != KORB_NORMAL)) return nr; }
    const korb_value cls = korb_dispatch_class(c, KORB_REF_GET(self));
    const struct korb_method *const m = KORB_CLASS_P(cls) ? korb_class_find_method(cls, mid, NULL) : NULL;
    if (m != NULL && m->visibility != 0) {
        char cnm[192]; korb_class_desc_into(c, cls, cnm, sizeof cnm);
        return korb_raise(c, slots, KORB_E_NAME, 0, "method '%s' for class '%s' is %s",
                          korb_sym_name(c->vm, mid), cnm, m->visibility == 1 ? "private" : "protected");
    }
    /* Same fallback as #method: an unknown name still answers when the object
     * claims it — but asked PUBLICLY (`respond_to?(name, false)`), so a name only
     * #respond_to_missing? admits for private access stays a NameError. */
    const bool defined = m != NULL || korb_responds_to(c, KORB_REF_GET(self), mid);
    if (!defined) {
        slots[0] = KORB_REF_GET(self); slots[1] = ID2SYM(mid); slots[2] = KORB_FALSE;
        const RESULT rr = korb_send_impl(c, slots + 3, korb_intern_lit(c->vm, "respond_to?", 11), 0, 2, NULL, NULL, NULL);
        if (UNLIKELY(rr.state != KORB_NORMAL)) return rr;
        if (!KORB_TRUTHY(rr.value)) {
            /* the probe ran Ruby: name the class from the rooted self cell */
            char cnm[192]; korb_class_desc_into(c, korb_dispatch_class(c, KORB_REF_GET(self)), cnm, sizeof cnm);
            return korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for class '%s'",
                              korb_sym_name(c->vm, mid), cnm);
        }
    }
    RESULT r = korb_method_new(c, slots, KORB_REF_GET(self), mid);
    if (!defined && LIKELY(r.state == KORB_NORMAL)) VAL2METH(r.value)->missing = 1;   /* method_missing-backed */
    return r;
}
/* Method#call / #[] — re-dispatch to recv.mid(*args).  Stage [recv | args...]
 * below a fresh cursor and reuse the send machinery (polymorphic with Array#[]). */
static RESULT korb_m_meth_call(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    const KorbMethod *m = VAL2METH(KORB_REF_GET(self));
    if (UNLIKELY(m->unbound)) return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'call' for an UnboundMethod (use #bind)");
    uint32_t mid = m->mid, argc = KORB_SLICE_LEN(a);
    /* Method#call is a frame CRuby shows, and the callee window built here has no
     * call site to link it — aim it at this C method's own argument window. */
    if (UNLIKELY(m->missing)) {   /* CRuby: always method_missing(name, *args), even if the name exists by now */
        slots[0] = m->recv;
        slots[1] = ID2SYM(mid);
        for (uint32_t i = 0; i < argc; i++) slots[2 + i] = KORB_SLICE_GET(a, i);
        korb_flink_hand_on_cframe(c, slots);
        return korb_send_impl(c, slots + 2 + argc, korb_intern_lit(c->vm, "method_missing", 14), 0, argc + 1, block, def_env, cself);
    }
    if (UNLIKELY(m->default_new)) {
        /* `Foo.method(:new)` captured the default allocator; a `def self.new`
         * added afterwards must not swallow the call (rspec-support does
         * exactly this and recursed forever).  Same one-shot marker the `super`
         * path uses to reach past such an override. */
        slots[0] = m->recv;
        for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, i);
        c->vm->super_new_skip = korb_dispatch_class(c, slots[0]);
        korb_flink_hand_on_cframe(c, slots);
        const RESULT nr = korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);
        c->vm->super_new_skip = KORB_NIL;
        return nr;
    }
    const korb_value owner = m->owner;
    if (UNLIKELY(owner != KORB_NIL && KORB_CLASS_P(owner))) {   /* invoke the PINNED method from its owner (not virtual) */
        /* the entry pinned at capture, not a fresh lookup: a redefinition of the
         * same name leaves this Method on the definition it was made from (CRuby) */
        struct korb_method *const entry = m->entry ? m->entry : korb_class_find_method(owner, mid, NULL);
        if (LIKELY(entry != NULL && entry->kind == KORB_METHOD_ISEQ)) {
            /* korb_invoke_method fills identity + self, so stage the other two
             * header cells (link, EP) below the receiver ourselves. */
            slots[0] = 0;                                /* base[-4] (identity) */
            slots[1] = korb_flink_cur_cframe(c, slots);  /* base[-3] (frame link) */
            slots[2] = 0;                                /* base[-2] (EP) */
            slots[3] = m->recv;                          /* base[-1] self (the bound receiver) */
            for (uint32_t i = 0; i < argc; i++) slots[4 + i] = KORB_SLICE_GET(a, i);
            return korb_invoke_method(c, slots + 4 + argc, entry, argc, 0, mid, slots[3], owner, block, def_env, KORB_CSELF_VAL(cself));
        }
        if (entry != NULL) {                             /* builtin (cfunc/attr): still the CAPTURED entry — the
                                                          * receiver may not even respond to the name (a Kernel
                                                          * method bound onto a BasicObject), and a same-named
                                                          * singleton override must not win. */
            slots[0] = 0;                                /* header, as above: a block-taking builtin reads base[-3] */
            slots[1] = korb_flink_cur_cframe(c, slots);
            slots[2] = 0;
            slots[3] = m->recv;                          /* recv, in the slot below the args */
            for (uint32_t i = 0; i < argc; i++) slots[4 + i] = KORB_SLICE_GET(a, i);
            return korb_dispatch_method(c, slots + 4 + argc, entry, mid, 0, argc, owner, block, def_env, cself, true);
        }
    }
    slots[0] = m->recv;                                  /* recv below the args */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, i);
    korb_flink_hand_on_cframe(c, slots);
    return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);   /* forward the block to the method */
}
static RESULT korb_m_meth_recv(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2METH(KORB_REF_GET(self))->recv);
}
/* Method#to_proc — a lambda Proc bound to the method's receiver.  Encoded like a
 * Symbol#to_proc proc (iseq == NULL, sym_mid = the method id) but with is_lambda = 1
 * and self = the bound receiver, which both marks it a "method proc" (vs a symbol
 * proc, self == nil) and roots the receiver.  korb_cproc_yield / Proc#call dispatch
 * recv.mid(args...). */
static RESULT korb_m_meth_to_proc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbMethod *m = VAL2METH(KORB_REF_GET(self));
    if (UNLIKELY(m->unbound))
        return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'to_proc' for an UnboundMethod (use #bind)");
    slots[0] = m->recv;                                  /* root recv across the alloc */
    const uint32_t mid = m->mid;
    KorbProc *p = korb_alloc(c, slots + 1, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = NULL; p->sym_mid = mid; p->is_lambda = 1;
    ARO_STORE(c, p, (korb_value *)(uintptr_t)&p->env, 0);
    ARO_STORE(c, p, (korb_value *)(uintptr_t)&p->self, slots[0]);   /* bound receiver (non-nil ⇒ method proc) */
    ARO_STORE(c, p, (korb_value *)(uintptr_t)&p->meth, KORB_REF_GET(self));   /* call the captured definition, not the name */
    return RESULT_OK((korb_value)p);
}
/* Proc#call / [] / .() / === — invoke the captured block body.  Stage A: env is
 * a tagged-odd live-slots pointer (correct while the defining frame is alive). */
/* LocalJumpError for a `break` whose target call is gone (CRuby's "break from proc-closure") */
static RESULT korb_raise_break_lje(CTX *c, korb_value *slots, korb_value value) {
    slots[0] = value;                                    /* the value given to `break` (rooted) */
    RESULT lj = korb_raise(c, slots + 1, KORB_E_LOCALJUMP, 0, "break from proc-closure");
    if (lj.state == KORB_RAISE && KORB_EXC_P(lj.value)) {   /* #reason / #exit_value (prelude readers) */
        slots[1] = lj.value;
        CHECK(korb_ivar_set(c, slots + 2, KORB_REF_AT(&slots[1]), ID2SYM(korb_intern_lit(c->vm, "@reason", 7)), ID2SYM(korb_intern_lit(c->vm, "break", 5))));
        CHECK(korb_ivar_set(c, slots + 2, KORB_REF_AT(&slots[1]), ID2SYM(korb_intern_lit(c->vm, "@exit_value", 11)), slots[0]));
        lj.value = slots[1];
    }
    return lj;
}
static RESULT korb_m_proc_call(CTX *c, korb_value *slots, korb_ref self, korb_slice a, NODE *block, korb_value *def_env, korb_value *cself) {
    KorbProc *p = VAL2PROC(KORB_REF_GET(self));
    const bool is_lam = p->is_lambda;                    /* captured before the body runs (a lambda's `return` returns from the lambda) */
    uint32_t argc = KORB_SLICE_LEN(a);
    if (p->iseq == NULL) {                               /* no body: symbol proc / method proc */
        uint32_t mid = p->sym_mid;
        if (p->meth != 0) {                              /* Method#to_proc: Method#call on the captured definition */
            slots[0] = p->meth;
            for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, i);
            return korb_m_meth_call(c, slots + 1 + argc, KORB_REF_AT(&slots[0]), KORB_SLICE_MAKE(&slots[1], argc), block, def_env, cself);
        }
        if (p->self != KORB_NIL) {                       /* bound receiver: recv.mid(args...) */
            slots[0] = p->self;
            for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, i);
            return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);   /* forward the &block given at .call */
        }
        if (UNLIKELY(argc < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver given");
        for (uint32_t i = 0; i < argc; i++) slots[i] = KORB_SLICE_GET(a, i);   /* Symbol#to_proc: arg0.sym(arg1..) */
        return korb_send_impl_vis(c, slots + argc, mid, 0, argc - 1, block, def_env, cself, KORB_VIS_OUTSIDE);   /* public only */
    }
    NODE *entry = p->iseq;
    /* `|&nil|` (Ruby 4.1): this proc refuses a block. */
    if (UNLIKELY(block != NULL) && entry != KORB_BLK_CPROC && korb_pi_no_block(korb_entry_param_info(entry)))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no block accepted");
    /* A lambda's arity is checked by korb_block_yield_full (is_lam), inside the block's own frame as CRuby does. */
    korb_value penv = p->env;                                 /* already tagged: odd=slots / even=KorbEnv */
    /* Proc#refined: the body runs under the set the copy carries.  Nothing may
     * raise between the push and the pop below. */
    const bool refined = p->refine_set != KORB_NIL;
    const bool sealed  = refined && p->refined_copy;   /* only a Proc#refined body refuses `using` */
    if (UNLIKELY(refined)) { korb_refine_push(c); c->refinements = p->refine_set;
                             if (sealed) { c->refine_sealed = c->refinements; c->vm->method_serial++; } }   /* the copy shares the original's AST and caches */
    /* Proc#ruby2_keywords: make the call-scoped mark persistent, so the Hash this
     * body collects into its *rest is keywords again when re-splatted. */
    const struct korb_param_info *const r2pi =
        (entry != KORB_BLK_CPROC) ? (const struct korb_param_info *)korb_entry_param_info(entry) : NULL;
    if (UNLIKELY(p->ruby2_kw || (r2pi != NULL && r2pi->r2k)) && argc >= 1 && korb_kwargs_hash_p(KORB_SLICE_GET(a, argc - 1)))
        korb_r2k_set(KORB_SLICE_GET(a, argc - 1));
    slots[0] = p->self;                                  /* captured self (rooted) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, i);
    /* If this proc has a `|&b|` param and the call site passed a (real) block,
     * forward it so &b binds to it — via the full path (the hot wrapper is
     * untouched; a no-block call leaves &b nil). */
    RESULT r;
    if (block != NULL && block != KORB_BLK_CPROC && block->head.kind == &kind_node_entry &&
        korb_entry_blk_param_slot(entry) >= 0)
        r = korb_block_yield_full(c, slots + 1 + argc, entry, (korb_value *)(uintptr_t)penv,
                                  &slots[1], argc, &slots[0], block, def_env, cself, is_lam);
    else if (is_lam)   /* a lambda enforces arity + never auto-splats a single Array → the full path with is_lam */
        r = korb_block_yield_full(c, slots + 1 + argc, entry, (korb_value *)(uintptr_t)penv,
                                  &slots[1], argc, &slots[0], NULL, NULL, NULL, 1);
    else
        r = korb_block_yield(c, slots + 1 + argc, entry, (korb_value *)(uintptr_t)penv, &slots[1], argc, &slots[0]);
    if (UNLIKELY(refined)) korb_refine_pop(c);
    /* A lambda consumes only a `return` aimed at a frame staged above this call's
     * cursor — its own body, or a block inside it.  One a plain proc the lambda
     * merely CALLED carries up aims at a frame below us and flows through
     * (KernelSpecs::Lambda#outer).  A NULL target is the nearest method = us. */
    if (UNLIKELY(is_lam && r.state == KORB_RETURN) &&
        (c->return_target == NULL || c->return_target >= slots)) {
        c->return_target = NULL;
        r.state = KORB_NORMAL;
    }
    if (UNLIKELY(is_lam && r.state == KORB_BREAK && c->break_blk == NULL)) r.state = KORB_NORMAL;   /* a lambda's own `break` acts like `return`; one from a block it called passes */
    if (UNLIKELY(r.state == KORB_BREAK && KORB_PROC_P(KORB_REF_GET(self)) &&   /* the call / method it would end is gone */
                 (c->break_blk == NULL || c->break_blk == VAL2PROC(KORB_REF_GET(self))->iseq) &&   /* its own break, not one passing through */
                 korb_proc_break_orphan(c, VAL2PROC(KORB_REF_GET(self)),
                                        c->cfunc_link ? korb_flink_ptr(c->cfunc_link) : NULL)))
        return korb_raise_break_lje(c, slots, r.value);
    if (UNLIKELY(r.state == KORB_RETURN && c->return_target == NULL && !is_lam)) {
        /* A non-local `return` always names its home method frame; a NULL target
         * here means the chain was closed — the method already returned, so this
         * is CRuby's "unexpected return", raised at the call, not unwound. */
        c->return_target = NULL;
        slots[0] = r.value;                                /* the value given to `return` (rooted) */
        RESULT lj = korb_raise(c, slots + 1, KORB_E_LOCALJUMP, 0, "unexpected return");
        if (lj.state == KORB_RAISE && KORB_EXC_P(lj.value)) {   /* #reason / #exit_value (prelude readers) */
            slots[1] = lj.value;
            CHECK(korb_ivar_set(c, slots + 2, KORB_REF_AT(&slots[1]), ID2SYM(korb_intern_lit(c->vm, "@reason", 7)), ID2SYM(korb_intern_lit(c->vm, "return", 6))));
            CHECK(korb_ivar_set(c, slots + 2, KORB_REF_AT(&slots[1]), ID2SYM(korb_intern_lit(c->vm, "@exit_value", 11)), slots[0]));
            lj.value = slots[1];
        }
        return lj;
    }
    return r;
}
/* Symbol#to_proc — a Proc that sends the symbol to its first argument. */
static RESULT korb_m_sym_to_proc(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    uint32_t mid = SYM2ID(KORB_REF_GET(self));
    KorbProc *p = korb_alloc(c, slots, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = NULL; p->sym_mid = mid; p->is_lambda = 1;   /* CRuby: Symbol#to_proc returns a lambda */
    ARO_STORE(c, p, (korb_value *)(uintptr_t)&p->env, 0);
    ARO_STORE(c, p, (korb_value *)(uintptr_t)&p->self, KORB_NIL);
    return RESULT_OK((korb_value)p);
}
/* Proc#== / #eql? — CRuby 3.2+: two Procs are equal when they are the same
 * block with the same captured environment, so `p.dup == p`. */
static RESULT korb_m_proc_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c; (void)slots;
    const korb_value o = KORB_SLICE_GET(a, 0);
    const korb_value s = KORB_REF_GET(self);
    if (s == o) return RESULT_OK(KORB_TRUE);
    if (!KORB_PROC_P(o)) return RESULT_OK(KORB_FALSE);
    const KorbProc *const x = VAL2PROC(s), *const y = VAL2PROC(o);
    const bool eq = x->iseq == y->iseq && x->env == y->env && x->self == y->self &&
                    x->sym_mid == y->sym_mid && x->is_lambda == y->is_lambda &&
                    x->refine_set == y->refine_set;   /* a Proc#refined copy is not its source */
    return RESULT_OK(eq ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_proc_lambda_q(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2PROC(KORB_REF_GET(self))->is_lambda ? KORB_TRUE : KORB_FALSE);
}
/* Proc#binding: the proc's captured env + self; its locals are the levels of
 * that env chain up to the method / file (a raw frame beyond it has no names). */
static struct Node *korb_method_srcloc_node(const struct korb_method *km);   /* fwd (defined below) */
static const uint32_t *korb_bind_tbl_from_levels(struct korb_vm *vm, const struct korb_locals_info *const *lis, uint32_t L,
                                                 korb_value key_id, uint32_t *out_cnt);   /* fwd (korb_runtime.c) */
static RESULT korb_binding_at_frame(CTX *c, korb_value *slots, korb_value *fbase, korb_value self, uint32_t bfile, uint32_t bline);   /* fwd */
static const uint32_t korb_proc_binding_scope_tbl[2] = { 1, 0 };   /* L=1, ns[0]=0 */
/* A closure that read no outer local keeps no real env: only its defining frame
 * has the locals, usable while that frame is on the current call chain (walked
 * from `cell`, the caller link of Proc#binding's own window).  NULL otherwise. */
static korb_value *korb_link_base(const CTX *c, const korb_value *cell);   /* fwd (korb_runtime.c) */
static bool korb_frame_on_chain(const CTX *c, const korb_value *from, const korb_value *target);   /* fwd (korb_runtime.c) */
static korb_value *korb_proc_live_home(CTX *c, const korb_value env, const korb_value *const cell) {
    korb_value *base;
    if (env & 1u) base = (korb_value *)(uintptr_t)(env & ~(korb_value)1u);
    else if (env != 0) {
        const KorbEnv *const e = VAL2ENV(env);
        if (e->closed || e->n != 0) return NULL;         /* a real env: the table walk handles it */
        base = e->loc;
    } else return NULL;
    korb_value *const fb = korb_link_base(c, cell);
    return (fb != NULL && korb_frame_on_chain(c, fb, base)) ? base : NULL;
}
static const uint32_t *korb_proc_binding_tbl(struct korb_vm *const vm, const korb_value env, uint32_t *const cnt) {
    const struct korb_locals_info *lis[KORB_SCOPE_DEPTH_MAX];
    uint32_t L = 0;
    for (korb_value ev = env; L < KORB_SCOPE_DEPTH_MAX && ev != 0 && !(ev & 1u); ) {
        const KorbEnv *const e = VAL2ENV(ev);
        const struct korb_scope_level lv = { e->id, NULL, (KorbEnv *)(uintptr_t)e };
        lis[L++] = e->n != 0 ? korb_scope_locals(vm, &lv) : NULL;   /* an identity-only env holds no locals */
        if ((e->id & 7u) != KORB_FID_BLOCK) break;      /* method / file: the lexical barrier */
        ev = e->prev;
    }
    *cnt = 0;
    if (L == 0) return korb_proc_binding_scope_tbl;
    return korb_bind_tbl_from_levels(vm, lis, L, VAL2ENV(env)->id, cnt);
}
static RESULT korb_m_proc_binding(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    slots[0] = KORB_REF_GET(self);                       /* root proc across alloc */
    {   const KorbProc *const p0 = VAL2PROC(slots[0]);     /* Symbol#to_proc has no Ruby body at all */
        if (p0->iseq == NULL && p0->self == KORB_NIL)
            return korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "Can't create Binding from C level Proc");   }
    korb_value *const home = korb_proc_live_home(c, VAL2PROC(slots[0])->env, a.p - 3);
    if (home != NULL) {
        const RESULT hr = korb_binding_at_frame(c, slots + 1, home, VAL2PROC(slots[0])->self, 0, 0);
        if (hr.state != KORB_NORMAL || !KORB_BINDING_P(hr.value)) return hr;
        const KorbProc *const hp = VAL2PROC(slots[0]);
        VAL2BIND(hr.value)->src_node = (hp->iseq != NULL && hp->iseq != KORB_BLK_CPROC) ? hp->iseq : NULL;
        return hr;
    }
    KorbBinding *b = korb_alloc(c, slots + 1, sizeof(KorbBinding), KORB_OBJ_BINDING);
    const KorbProc *p = VAL2PROC(slots[0]);
    b->name_syms = korb_proc_binding_tbl(c->vm, p->env, &b->name_cnt);
    /* #source_location of a proc's binding is where the BLOCK was written, the
     * same node Proc#source_location reports (bundler filters its own
     * post_reset_hooks with `proc.binding.source_location[0] == __FILE__`). */
    b->src_node = (p->iseq != NULL && p->iseq != KORB_BLK_CPROC) ? p->iseq : NULL;
    if (b->src_node == NULL && p->iseq == NULL) {         /* Method#to_proc: the bound method's own def */
        const korb_value mklass = korb_dispatch_class(c, p->self);
        const struct korb_method *mkm = KORB_CLASS_P(mklass) ? korb_class_find_method(mklass, p->sym_mid, NULL) : NULL;
        if (mkm == NULL) mkm = korb_method_lookup(c->vm, p->sym_mid);
        b->src_node = korb_method_srcloc_node(mkm);
    }
    b->src_file = 0; b->src_line = 0;
    ARO_STORE(c, b, (korb_value *)(uintptr_t)&b->env,  p->env);
    ARO_STORE(c, b, (korb_value *)(uintptr_t)&b->self, p->self);
    ARO_STORE(c, b, (korb_value *)(uintptr_t)&b->extra, KORB_NIL);
    ARO_STORE(c, b, (korb_value *)(uintptr_t)&b->refine_set, p->refine_set);
    return RESULT_OK((korb_value)b);
}
static void korb_kw_arity_flags(const void *kwp, bool *req, bool *opt, bool *kwrest);   /* fwd (defined below) */
/* Proc#arity: #required positional, negated as -(req+1) when optional/rest make
 * it variable.  (Symbol#to_proc → -2, matching CRuby.) */
static korb_sword_t korb_method_arity(const struct korb_method *km);   /* fwd (defined below) */
static RESULT korb_m_proc_arity(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)c;(void)slots;(void)a;
    const KorbProc *p = VAL2PROC(KORB_REF_GET(self));
    if (p->iseq == NULL) {
        if (p->self != KORB_NIL) {                        /* Method#to_proc → the bound method's own arity */
            const korb_value klass = korb_dispatch_class(c, p->self);
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
/* The NODE that carries a method entry's source location: define_method takes it
 * from the block it was given, everything else from the def body (NULL for C). */
static struct Node *korb_method_srcloc_node(const struct korb_method *km) {
    if (km == NULL) return NULL;
    if (km->kind == KORB_METHOD_DM && KORB_PROC_P(km->dm_proc)) return VAL2PROC(km->dm_proc)->iseq;
    return km->body;
}
/* Proc#source_location → [file, line] where the block was written, or nil.
 * A Method#to_proc proc has no body of its own and reports the method's. */
static RESULT korb_m_proc_source_location(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbProc *const p = VAL2PROC(KORB_REF_GET(self));
    if (p->iseq == NULL && p->self != KORB_NIL) {          /* method proc: recv + mid, no body */
        const korb_value klass = korb_dispatch_class(c, p->self);
        const struct korb_method *km = KORB_CLASS_P(klass) ? korb_class_find_method(klass, p->sym_mid, NULL) : NULL;
        if (km == NULL) km = korb_method_lookup(c->vm, p->sym_mid);
        return korb_srcloc_result(c, slots, korb_method_srcloc_node(km));
    }
    return korb_srcloc_result(c, slots, p->iseq);
}
/* Proc#__source_range_info — [path, start_line, start_column, end_line, end_column]
 * or nil.  Same body-node resolution as #source_location; the prelude wraps it. */
static RESULT korb_m_proc_source_range_info(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbProc *const p = VAL2PROC(KORB_REF_GET(self));
    if (p->iseq == NULL && p->self != KORB_NIL) {          /* method proc: recv + mid, no body */
        const korb_value klass = korb_dispatch_class(c, p->self);
        const struct korb_method *km = KORB_CLASS_P(klass) ? korb_class_find_method(klass, p->sym_mid, NULL) : NULL;
        if (km == NULL) km = korb_method_lookup(c->vm, p->sym_mid);
        return korb_srcrange_result(c, slots, korb_method_srcloc_node(km));
    }
    return korb_srcrange_result(c, slots, p->iseq);
}
/* Proc#parameters — [[kind, name], ...] from the parse-time param_info (cold;
 * never on the call/yield hot path).  A non-lambda proc reports a required
 * positional as :opt (CRuby semantics); keyreq/rest/block keep their kind. */
static RESULT korb_push_method_params(CTX *c, korb_value *slots, korb_ref res, const struct korb_method *km);   /* fwd (below) */
static RESULT korb_m_proc_parameters(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    /* Capture everything off the proc BEFORE the first alloc — korb_ary_new GCs
     * and may move the KorbObject, so `p` must not be dereferenced afterwards.
     * pi (node param_info) and the booleans are all stable values. */
    const KorbProc *const p = VAL2PROC(KORB_REF_GET(self));
    const struct korb_param_info *const pi = p->iseq ? (const struct korb_param_info *)p->iseq->u.node_entry.param_info : NULL;
    bool lam = p->is_lambda;
    /* `lambda:` keyword overrides required-vs-optional treatment (nil = ignore). */
    if (KORB_SLICE_LEN(a) >= 1 && KORB_HASH_P(KORB_SLICE_GET(a, KORB_SLICE_LEN(a) - 1))) {
        const KorbHash *const h = VAL2HASH(KORB_SLICE_GET(a, KORB_SLICE_LEN(a) - 1));
        const int32_t li = korb_hash_find(h, ID2SYM(korb_intern_lit(c->vm, "lambda", 6)));
        if (li >= 0 && korb_hash_pairs(h)[2 * li + 1] != KORB_NIL) lam = KORB_TRUTHY(korb_hash_pairs(h)[2 * li + 1]);
    }
    const bool symproc = (p->iseq == NULL);
    const uint32_t mproc_mid = symproc ? p->sym_mid : 0;
    static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block", "nokey", "noblock" };
    const uint32_t n = pi ? pi->n : 0;
    /* A Method#to_proc carries its receiver; that korb_value must be ROOTED, not held
     * in a C local — the Array alloc below moves the object and the dispatch
     * class lookup would read a freed address. */
    slots[0] = symproc ? p->self : KORB_NIL;              /* non-nil ⇒ a Method#to_proc, not a Symbol#to_proc */
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, n ? n : 1));
    korb_ref res = KORB_REF_AT(&slots[1]);
    if (!pi) {                                            /* symbol proc → [[:req], [:rest]]; no params → [] */
        if (symproc && slots[0] != KORB_NIL) {            /* Method#to_proc: report the bound method's own signature */
            const korb_value dc = korb_dispatch_class(c, slots[0]);
            const struct korb_method *const km =
                KORB_CLASS_P(dc) ? korb_class_find_method(dc, mproc_mid, NULL) : NULL;
            return korb_push_method_params(c, slots + 2, res, km);
        }
        if (symproc) {
            const char *const sp[] = { "req", "rest" };
            for (int j = 0; j < 2; j++) {
                slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 1));
                CHECK(korb_ary_push_val(c, slots + 3, KORB_REF_AT(&slots[2]), ID2SYM(korb_intern(c->vm, sp[j], (uint32_t)strlen(sp[j])))));
                CHECK(korb_ary_push_val(c, slots + 3, res, slots[2]));
            }
        }
        return RESULT_OK(KORB_REF_GET(res));
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t kind = pi->e[i].kind;
        const char *kn = (kind == 0 && !lam) ? "opt" : knames[kind];
        const uint32_t nm = pi->e[i].name;
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, nm ? 2 : 1));
        korb_ref sub = KORB_REF_AT(&slots[2]);
        CHECK(korb_ary_push_val(c, slots + 3, sub, ID2SYM(korb_intern(c->vm, kn, (uint32_t)strlen(kn)))));
        if (nm) CHECK(korb_ary_push_val(c, slots + 3, sub, ID2SYM(nm)));
        CHECK(korb_ary_push_val(c, slots + 3, res, KORB_REF_GET(sub)));
    }
    return RESULT_OK(KORB_REF_GET(res));
}
static RESULT korb_m_meth_name(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a; return RESULT_OK(ID2SYM(VAL2METH(KORB_REF_GET(self))->mid));
}
/* Resolve a bound Method to its korb_method entry: receiver-class MRO first,
 * then the global function table (top-level def / builtin like `p`). */
/* Does this Method denote the DEFAULT allocator?  Either it was captured while
 * `new` still had no table entry, or it resolves to the trampoline that an
 * `alias` — or `private_class_method :new` — installs in its place. */
static bool korb_meth_default_new_p(const KorbMethod *m, const struct korb_method *e) {
    return m->default_new ||
           (e != NULL && e->kind == KORB_METHOD_CFUNC && e->rbfn == korb_m_class_new_alias);
}
static const struct korb_method *korb_meth_resolve(CTX *c, const KorbMethod *m) {
    if (m->missing) return NULL;                 /* no definition: it lives behind method_missing */
    if (m->default_new) return NULL;             /* the allocator has no table entry — never a later `def self.new` */
    if (LIKELY(m->entry != NULL)) return m->entry;   /* pinned at capture (korb_meth_pin) */
    /* unbound: recv IS the owner class → look the method up directly there. */
    const korb_value klass = m->unbound ? m->recv : korb_dispatch_class(c, m->recv);
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
        const bool varpos = (e->u.node_entry.opt_defaults != NULL) || (e->u.node_entry.rest_slot >= 0 || e->u.node_entry.rest_slot == -2);
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
static RESULT korb_m_meth_arity(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a;
    const struct korb_method *km = korb_meth_resolve(c, VAL2METH(KORB_REF_GET(self)));
    return RESULT_OK(LONG2FIX(km ? korb_method_arity(km) : -1));
}
/* Method/UnboundMethod#source_location → [file, line] of the def, or nil for a
 * C-defined (builtin/attr) method. */
static RESULT korb_m_meth_source_location(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const struct korb_method *const km = korb_meth_resolve(c, VAL2METH(KORB_REF_GET(self)));
    return korb_srcloc_result(c, slots, korb_method_srcloc_node(km));
}
/* Method/UnboundMethod#__source_range_info — see the Proc one. */
static RESULT korb_m_meth_source_range_info(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const struct korb_method *const km = korb_meth_resolve(c, VAL2METH(KORB_REF_GET(self)));
    return korb_srcrange_result(c, slots, korb_method_srcloc_node(km));
}
/* #original_name: the name at original definition, surviving aliases. */
static RESULT korb_m_meth_original_name(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;(void)a;
    const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    const struct korb_method *const km = korb_meth_resolve(c, m);
    return RESULT_OK(ID2SYM(km && km->orig_mid ? km->orig_mid : m->mid));
}
/* Method#== / UnboundMethod#==: same kind (bound vs unbound), same receiver/owner,
 * and the SAME underlying definition — so aliases (to_int↔to_i) compare equal. */
static RESULT korb_m_meth_eq(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots;
    const korb_value ov = KORB_SLICE_GET(a, 0);
    if (!KORB_METHOD_P(ov)) return RESULT_OK(KORB_FALSE);
    const KorbMethod *const m1 = VAL2METH(KORB_REF_GET(self));
    const KorbMethod *const m2 = VAL2METH(ov);
    if (m1->unbound != m2->unbound) return RESULT_OK(KORB_FALSE);
    if (m1->missing || m2->missing)   /* no definition to compare: same receiver + same name */
        return RESULT_OK((m1->missing && m2->missing && m1->recv == m2->recv && m1->mid == m2->mid) ? KORB_TRUE : KORB_FALSE);
    /* bound methods must share the receiver object; unbound ones only need the
     * same underlying definition (extracting from different subclasses is equal). */
    if (!m1->unbound && m1->recv != m2->recv) return RESULT_OK(KORB_FALSE);
    /* compare the actual definitions, even for the same owner + name: a method
     * redefined in between is a different one (state_machines tracks which
     * context methods changed that way), and a cross-owner pair (an override, a
     * superclass UnboundMethod rebound) must compare unequal — pry walks
     * `guess == guess.super` to find the frame's real method. */
    const struct korb_method *const e1 = korb_meth_resolve(c, m1);
    const struct korb_method *const e2 = korb_meth_resolve(c, m2);
    /* `.new` has no table entry (it is a dispatch special case), so an alias of
     * it — or `private_class_method :new` — resolves to a trampoline while the
     * original resolves to nothing.  Both denote the SAME definition, the default
     * allocator, and rspec-mocks decides how to verify a stubbed `new` with
     * exactly `klass.method(:new) == Class.instance_method(:new).bind(klass)`. */
    {   const bool d1 = korb_meth_default_new_p(m1, e1), d2 = korb_meth_default_new_p(m2, e2);
        if (d1 || d2) return RESULT_OK(d1 && d2 ? KORB_TRUE : KORB_FALSE);   }
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
#include "reflect_owner.h"
/* CRuby's owner for a method koruby defines on the core class `klass` itself
 * (Comparable#clamp on Integer, Enumerable's on Array/Hash/Range...), else nil.
 * Only the core class proper: a user class that happens to share the name is
 * not it (the identity check). */
static bool korb_reflect_raw_p(void) {                /* KORB_REFLECT_RAW: koruby's own tables (tools/gen_reflect_owner.rb) */
    static int raw = -1;
    if (raw < 0) raw = getenv("KORB_REFLECT_RAW") != NULL;
    return raw != 0;
}
static korb_value korb_reflect_owner_vm(struct korb_vm *const vm, const korb_value klass, const uint32_t mid) {
    if (!KORB_CLASS_P(klass) || VAL2CLASS(klass)->name_sym == 0 || korb_reflect_raw_p()) return KORB_NIL;
    const char *const kn = korb_sym_name(vm, VAL2CLASS(klass)->name_sym);
    const char *const mn = korb_sym_name(vm, mid);
    for (size_t i = 0; i < sizeof korb_reflect_owner_tab / sizeof korb_reflect_owner_tab[0]; i++) {
        if (strcmp(korb_reflect_owner_tab[i].klass, kn) != 0 || strcmp(korb_reflect_owner_tab[i].mid, mn) != 0) continue;
        if (korb_const_get(vm, korb_intern(vm, kn, strlen(kn))) != klass) return KORB_NIL;
        const char *const on = korb_reflect_owner_tab[i].owner;
        const korb_value o = korb_const_get_path(vm, korb_intern(vm, on, strlen(on)));
        return KORB_CLASS_P(o) ? o : KORB_NIL;
    }
    return KORB_NIL;
}
static korb_value korb_reflect_owner(CTX *c, const korb_value klass, const uint32_t mid) {
    return korb_reflect_owner_vm(c->vm, klass, mid);
}
/* Is (owner, mid) in the table, i.e. would a definition there shadow a core class's? */
static bool korb_reflect_owner_mid_p(struct korb_vm *const vm, const korb_value owner, const uint32_t mid) {
    if (!KORB_CLASS_P(owner) || VAL2CLASS(owner)->name_sym == 0) return false;
    const char *const on = korb_sym_name(vm, VAL2CLASS(owner)->name_sym);
    const char *const mn = korb_sym_name(vm, mid);
    for (size_t i = 0; i < sizeof korb_reflect_owner_tab / sizeof korb_reflect_owner_tab[0]; i++)
        if (strcmp(korb_reflect_owner_tab[i].owner, on) == 0 && strcmp(korb_reflect_owner_tab[i].mid, mn) == 0)
            return korb_const_get_path(vm, korb_intern(vm, on, strlen(on))) == owner;
    return false;
}
/* Does `klass` carry any such methods (a listing asks per method only then)? */
static bool korb_reflect_owner_class_p(CTX *c, const korb_value klass) {
    if (!KORB_CLASS_P(klass) || VAL2CLASS(klass)->name_sym == 0 || korb_reflect_raw_p()) return false;
    const char *const kn = korb_sym_name(c->vm, VAL2CLASS(klass)->name_sym);
    for (size_t i = 0; i < sizeof korb_reflect_owner_tab / sizeof korb_reflect_owner_tab[0]; i++)
        if (strcmp(korb_reflect_owner_tab[i].klass, kn) == 0)
            return korb_const_get(c->vm, korb_intern(c->vm, kn, strlen(kn))) == klass;
    return false;
}
static korb_value korb_meth_owner_raw(CTX *c, const KorbMethod *const m);
/* Method#owner — CRuby's owner for a method koruby keeps on the core class (Integer#clamp is Comparable's) */
static RESULT korb_m_meth_owner(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)slots; (void)a;
    const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    const korb_value o = korb_meth_owner_raw(c, m);
    const korb_value ro = korb_reflect_owner(c, o, m->mid);
    return RESULT_OK(ro != KORB_NIL ? ro : o);
}
static korb_value korb_meth_owner_raw(CTX *c, const KorbMethod *const m) {
    if (m->default_new)                                             /* the default allocator is Class#new, */
        return korb_builtin_class_obj(c->vm, KORB_C_CLASS);   /* never the singleton it was read from */
    if (m->owner != KORB_NIL) return m->owner;           /* fixed owner (e.g. from super_method) */
    if (m->missing) return korb_dispatch_class(c, m->recv);   /* CRuby: the receiver's class */
    /* the class whose table actually holds the entry: `public :m` on an
     * inherited private method (and alias_method) put a copy in the reopening
     * class and CRuby reports THAT as the owner — the entry's own ->owner still
     * points at the original, which is what super resolution needs. */
    const korb_value klass = m->unbound ? m->recv : korb_dispatch_class(c, m->recv);
    korb_value def = KORB_NIL;
    const struct korb_method *km = KORB_CLASS_P(klass) ? korb_class_find_method(klass, m->mid, &def) : NULL;
    if (km != NULL && KORB_CLASS_P(def)) return def;
    if (km == NULL) km = korb_method_lookup(c->vm, m->mid);
    if (km && km->owner != KORB_NIL) return km->owner;   /* defining class/module */
    if (m->unbound) return m->recv;                      /* unresolvable: the class it came from */
    return korb_builtin_class_obj(c->vm, KORB_C_OBJECT); /* global fn → Object */
}
/* Does `mk` hold a live (non-tombstone) entry for `mid`?  When `same_as` is
 * given, only an entry with that exact definition counts. */
static bool korb_class_owns_method(const KorbClass *mk, uint32_t mid, const struct korb_method *same_as) {
    for (uint32_t q = 0; q < mk->method_cnt; q++) {
        const struct korb_method *const e = mk->methods[q];
        if (e->mid != mid || e->mid == UINT32_MAX || e->kind == KORB_METHOD_UNDEF) continue;
        if (same_as == NULL) return true;
        if (e->kind != same_as->kind) continue;
        switch (e->kind) {
          case KORB_METHOD_ISEQ:    if (e->body == same_as->body) return true; break;
          case KORB_METHOD_CFUNC:   if (e->rfn == same_as->rfn && e->rbfn == same_as->rbfn) return true; break;
          case KORB_METHOD_BUILTIN: if (e->bfn == same_as->bfn) return true; break;
          case KORB_METHOD_ATTR_R:
          case KORB_METHOD_ATTR_W:  if (e->attr_ivar == same_as->attr_ivar) return true; break;
          case KORB_METHOD_DM:      if (e->dm_proc == same_as->dm_proc) return true; break;
          default: break;
        }
    }
    return false;
}
/* Method#super_method → a Method for the definition above this one in the
 * receiver's MRO (fixed super-owner), or nil if there's no super. */
static int korb_linearize_mro(korb_value klass, korb_value *buf, int max);   /* fwd (korb_runtime.c) */
static RESULT korb_m_meth_super_method(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    const struct korb_method *const km = korb_meth_resolve(c, m);
    /* The entry's own ->owner is where the definition really lives, which is
     * what super resolution needs: `public :m` on an inherited private method
     * puts a COPY in the reopening class, and #owner reports that copy's class
     * (m->owner), but super has to start from the original module. */
    const korb_value cur_owner = (km != NULL && km->owner != KORB_NIL) ? km->owner : m->owner;
    if (!KORB_CLASS_P(cur_owner)) return RESULT_OK(KORB_NIL);
    const korb_value start = m->unbound ? m->recv : korb_dispatch_class(c, m->recv);
    /* CRuby searches for the ORIGINAL name: an alias's super is what the
     * aliased definition's own super was, not what shadows the alias name. */
    const uint32_t mid = (km != NULL && km->orig_mid != 0) ? km->orig_mid : m->mid;
    const uint8_t unbound = m->unbound;
    const korb_value recv = m->recv;
    korb_value mro[256];
    int n = korb_linearize_mro(start, mro, 256);
    /* a class's singleton chain ends at #<Class:BasicObject>; CRuby's goes on into Class (Module for a module) */
    if (KORB_CLASS_P(start) && VAL2CLASS(start)->is_singleton && n < 256) {
        const korb_value at = korb_singleton_attached_vm(c->vm, start);
        if (KORB_CLASS_P(at)) {
            const korb_value tail = VAL2CLASS(at)->is_module ? korb_const_get(c->vm, korb_intern_lit(c->vm, "Module", 6))
                                                        : korb_builtin_class_obj(c->vm, KORB_C_CLASS);
            bool seen = false;
            for (int i = 0; i < n && !seen; i++) seen = mro[i] == tail;
            if (!seen && KORB_CLASS_P(tail)) n += korb_linearize_mro(tail, mro + n, 256 - n);
        }
    }
    int di = -1;
    for (int i = 0; i < n; i++) if (mro[i] == cur_owner) { di = i; break; }
    if (di < 0) return RESULT_OK(KORB_NIL);
    if (mid != m->mid)                                              /* alias: descend past the original definition first */
        for (int i = di + 1; i < n; i++)
            if (KORB_CLASS_P(mro[i]) && korb_class_owns_method(VAL2CLASS(mro[i]), mid, km)) { di = i; break; }
    for (int i = di + 1; i < n; i++) {
        if (!KORB_CLASS_P(mro[i])) continue;
        if (!korb_class_owns_method(VAL2CLASS(mro[i]), mid, NULL)) continue;
        slots[0] = mro[i];                                          /* root super-owner across the alloc */
        slots[1] = start;                                           /* the class whose MRO the chain walks */
        /* Keep the ORIGINAL class as the unbound recv: it is what the next
         * #super_method searches, and restarting from the super owner would end
         * the chain at the first module (its own MRO is just itself). */
        RESULT r = unbound ? korb_unbound_new(c, slots + 2, slots[1], mid)
                           : korb_method_new(c, slots + 2, recv, mid);       /* bound: keep recv, fix the super-owner */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        ARO_STORE(c, VAL2METH(r.value), (korb_value *)(uintptr_t)&VAL2METH(r.value)->owner, slots[0]);   /* #owner is where the super definition lives */
        VAL2METH(r.value)->from_super = 1;
        /* Pin to the definition mro[i] OWNS.  Resolving by name from that class
         * instead would walk its own prepends and hand back the very method we
         * are stepping off — `m = m.super_method` then never terminates
         * (dry-cli's .auto_initialize_keywords walks exactly that chain). */
        struct korb_method *const sm = korb_class_own_method(slots[0], mid);   /* slots[0] re-read: the alloc above can move it */
        if (LIKELY(sm != NULL)) { VAL2METH(r.value)->entry = sm; sm->pinned = 1; }
        else korb_meth_pin(c, r.value);
        return r;
    }
    /* above a class's own `new`s: Class#new, the allocator, which has no table entry */
    if (mid == c->vm->mid_new && KORB_CLASS_P(start) && VAL2CLASS(start)->is_singleton) {
        const korb_value at = korb_singleton_attached_vm(c->vm, start);
        if (KORB_CLASS_P(at) && !VAL2CLASS(at)->is_module) {
            slots[0] = start;
            RESULT r = unbound ? korb_unbound_new(c, slots + 1, slots[0], mid) : korb_method_new(c, slots + 1, recv, mid);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            KorbMethod *const nm = VAL2METH(r.value);
            nm->entry = NULL; nm->default_new = 1; nm->from_super = 1;
            ARO_STORE(c, nm, (korb_value *)(uintptr_t)&nm->owner, korb_builtin_class_obj(c->vm, KORB_C_CLASS));
            return r;
        }
    }
    return RESULT_OK(KORB_NIL);
}
/* push a [kind] (or [kind, name]) param descriptor onto the result array `res`. */
static RESULT korb_param_push(CTX *c, korb_value *slots, korb_ref res, const char *kind) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, 1));
    CHECK(korb_ary_push_val(c, slots + 1, KORB_REF_AT(&slots[0]), ID2SYM(korb_intern(c->vm, kind, (uint32_t)strlen(kind)))));
    return korb_ary_push_val(c, slots + 1, res, slots[0]);
}
/* [kind, name] — like korb_param_push, but with a name (an anonymous `**` etc.). */
static RESULT korb_param_push2(CTX *c, korb_value *slots, korb_ref res, const char *kind, uint32_t name) {
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    korb_ref sub = KORB_REF_AT(&slots[0]);
    CHECK(korb_ary_push_val(c, slots + 1, sub, ID2SYM(korb_intern(c->vm, kind, (uint32_t)strlen(kind)))));
    CHECK(korb_ary_push_val(c, slots + 1, sub, ID2SYM(name)));
    return korb_ary_push_val(c, slots + 1, res, KORB_REF_GET(sub));
}
/* Method/UnboundMethod#parameters — C methods are nameless: variadic → [[:rest]],
 * fixed-arity → [[:req]]×n; ISEQ → positional kinds (names not retained). */
/* Push `km`'s parameter descriptors into `res` — shared by Method#parameters and
 * Proc#parameters for a Method#to_proc (which carries no iseq of its own). */
static RESULT korb_push_method_params(CTX *c, korb_value *slots, korb_ref res, const struct korb_method *km) {
    if (km == NULL) return RESULT_OK(KORB_REF_GET(res));
    if (km->kind == KORB_METHOD_ATTR_W) { CHECK(korb_param_push(c, slots + 1, res, "req")); return RESULT_OK(KORB_REF_GET(res)); }
    if (km->kind == KORB_METHOD_ATTR_R) return RESULT_OK(KORB_REF_GET(res));
    if (km->kind == KORB_METHOD_BUILTIN || km->kind == KORB_METHOD_CFUNC) {
        if (km->params_cnt < 0) { CHECK(korb_param_push(c, slots + 1, res, "rest")); }
        else for (int32_t i = 0; i < km->params_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
        return RESULT_OK(KORB_REF_GET(res));
    }
    /* ISEQ: the full param_info (kinds + names + kwargs + block), like Proc#parameters
     * — methods are strict (a required positional is :req, never :opt). param_info is
     * immortal libc (no GC). NULL (e.g. define_method) → fall back to the counts. */
    const struct korb_param_info *const pi = (const struct korb_param_info *)km->param_info;
    if ((km->kind == KORB_METHOD_ISEQ || km->kind == KORB_METHOD_DM) && pi != NULL) {   /* define_method carries its block's params */
        static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block", "nokey", "noblock" };
        /* ruby2_keywords adds an anonymous `**` before the block param (CRuby):
         * rspec-support reads #parameters to decide whether a double takes kwargs. */
        bool r2k_pending = km->ruby2_kw != 0;
        if (r2k_pending)                                   /* `def m(...)` already lists one */
            for (uint32_t i = 0; i < pi->n; i++) if (pi->e[i].kind == 5) { r2k_pending = false; break; }
        for (uint32_t i = 0; i < pi->n; i++) {
            const uint8_t kind = pi->e[i].kind;
            if (r2k_pending && (kind == 6 || kind == 8)) {
                CHECK(korb_param_push2(c, slots + 1, res, "keyrest", korb_intern_lit(c->vm, "**", 2)));
                r2k_pending = false;
            }
            const char *const kn = knames[kind];
            const uint32_t nm = pi->e[i].name;
            slots[1] = UNWRAP(korb_ary_new(c, slots + 1, nm ? 2 : 1));
            korb_ref sub = KORB_REF_AT(&slots[1]);
            CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(korb_intern(c->vm, kn, (uint32_t)strlen(kn)))));
            if (nm) CHECK(korb_ary_push_val(c, slots + 2, sub, ID2SYM(nm)));
            CHECK(korb_ary_push_val(c, slots + 2, res, KORB_REF_GET(sub)));
        }
        if (r2k_pending) CHECK(korb_param_push2(c, slots + 1, res, "keyrest", korb_intern_lit(c->vm, "**", 2)));
        return RESULT_OK(KORB_REF_GET(res));
    }
    for (uint32_t i = 0; i < km->req_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    const uint32_t nopt = (km->params_cnt >= 0 && (uint32_t)km->params_cnt > km->req_cnt) ? (uint32_t)km->params_cnt - km->req_cnt : 0;
    for (uint32_t i = 0; i < nopt; i++) CHECK(korb_param_push(c, slots + 1, res, "opt"));
    if (km->rest_slot >= 0) CHECK(korb_param_push(c, slots + 1, res, "rest"));
    for (uint32_t i = 0; i < km->post_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    return RESULT_OK(KORB_REF_GET(res));
}
static RESULT korb_m_meth_parameters(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const bool missing = VAL2METH(KORB_REF_GET(self))->missing;
    const struct korb_method *const km = korb_meth_resolve(c, VAL2METH(KORB_REF_GET(self)));
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    korb_ref res = KORB_REF_AT(&slots[0]);
    if (missing) { CHECK(korb_param_push(c, slots + 1, res, "rest")); return RESULT_OK(KORB_REF_GET(res)); }   /* method_missing forwards everything */
    if (km == NULL && VAL2METH(KORB_REF_GET(self))->default_new) {   /* Class#new is variadic (rspec verifies arity through it) */
        CHECK(korb_param_push(c, slots + 1, res, "rest"));
        return RESULT_OK(KORB_REF_GET(res));
    }
    return korb_push_method_params(c, slots, res, km);
}
/* Module#instance_method(name) → UnboundMethod owned by the class. */
static RESULT korb_m_class_instance_method(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    uint32_t mid;                                        /* Symbol/String, or #to_str-coercible */
    { RESULT r = korb_alias_argsym(c, slots, KORB_SLICE_GET(a, 0), &mid); if (UNLIKELY(r.state != KORB_NORMAL)) return r; }
    const korb_value cls = KORB_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls) || korb_class_find_method(cls, mid, NULL) == NULL)) {
        /* subsystems registered after boot can still land Kernel-level methods on
         * Object (korb_relocate_object_methods runs once); look there too. */
        /* `.new` is a dispatch special case with no table entry; Class still has
         * to hand out an UnboundMethod for it (Method#call re-dispatches). */
        if (KORB_CLASS_P(cls) && mid == c->vm->mid_new &&
            (cls == korb_builtin_class_obj(c->vm, KORB_C_CLASS) || VAL2CLASS(cls)->is_singleton))
            return korb_unbound_new(c, slots, cls, mid);
        /* same for send / __send__ / public_send: universal forms with no entry.
         * CRuby owns __send__ on BasicObject and the other two on Kernel. */
        if (KORB_CLASS_P(cls) && korb_universal_send_p(c->vm, mid)) {
            const korb_value own = korb_const_get(c->vm, korb_intern(c->vm,
                                  mid == c->vm->mid___send__ ? "BasicObject" : "Kernel",
                                  mid == c->vm->mid___send__ ? 11 : 6));
            if (!KORB_CLASS_P(own)) return korb_unbound_new(c, slots, cls, mid);
            if (cls == own || korb_class_has_ancestor(cls, own)) return korb_unbound_new(c, slots, own, mid);   /* not on a plain module */
        }
        if (KORB_CLASS_P(cls) && korb_toplevel_def(c->vm, mid) &&
            korb_class_has_ancestor(cls, korb_builtin_class_obj(c->vm, KORB_C_OBJECT)))
            return korb_unbound_new(c, slots, korb_builtin_class_obj(c->vm, KORB_C_OBJECT), mid);
        if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->name_sym == korb_intern_lit(c->vm, "Kernel", 6)) {
            const korb_value objc = korb_const_get(c->vm, c->vm->class_name[KORB_C_OBJECT]);
            if (KORB_CLASS_P(objc) && korb_class_find_method(objc, mid, NULL) != NULL)
                return korb_unbound_new(c, slots, cls, mid);
        }
        /* a refinement active in the running scope answers too, and the
         * UnboundMethod's #owner is the refinement (docs/refinements.md) */
        if (UNLIKELY(c->vm->refinements_active) && KORB_CLASS_P(cls)) {
            korb_value rdef = KORB_NIL;
            if (korb_refined_find(c, cls, mid, &rdef) != NULL && KORB_CLASS_P(rdef))
                return korb_unbound_new(c, slots, rdef, mid);
        }
        char cnm[256]; korb_class_desc_into(c, cls, cnm, sizeof cnm);
        RESULT ne = korb_raise(c, slots, KORB_E_NAME, 0, "undefined method '%s' for %s '%s'",   /* CRuby: NameError, not NoMethodError */
                          korb_sym_name(c->vm, mid),
                          (KORB_CLASS_P(cls) && VAL2CLASS(cls)->is_module) ? "module" : "class", cnm);
        if (LIKELY(KORB_EXC_P(ne.value))) {              /* NameError#name → the missing method symbol */
            slots[0] = ne.value;
            korb_exc_ivar_set(c, slots + 1, KORB_REF_AT(&slots[0]), ID2SYM(korb_intern_lit(c->vm, "@__name", 7)), ID2SYM(mid));
            ne.value = slots[0];
        }
        return ne;
    }
    return korb_unbound_new(c, slots, cls, mid);
}
/* Method#hash — equal for methods that are #eql? (same underlying definition,
 * and for bound methods the same receiver), consistent with korb_m_meth_eq. */
static RESULT korb_m_meth_hash(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a;
    const KorbMethod *m = VAL2METH(KORB_REF_GET(self));
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
        RESULT hr = korb_send(c, slots + 1, korb_intern_lit(c->vm, "hash", 4), 0, 0);
        if (UNLIKELY(hr.state != KORB_NORMAL)) return hr;
        if (FIXNUM_P(hr.value)) h ^= (korb_word_t)FIX2LONG(hr.value) * 0x100000001b3ULL;
    }
    return RESULT_OK(LONG2FIX((korb_sword_t)(h & (((korb_word_t)1 << 62) - 1))));
}
/* Method#unbind → UnboundMethod owned by the receiver's class. */
static RESULT korb_m_meth_unbind(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    (void)a; const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    if (m->unbound) return RESULT_OK(KORB_REF_GET(self));
    if (m->default_new)                                  /* Class#new, so it rebinds to any class (rspec-mocks) */
        return korb_unbound_new(c, slots, korb_builtin_class_obj(c->vm, KORB_C_CLASS), m->mid);
    const RESULT r = korb_unbound_new(c, slots, korb_dispatch_class(c, m->recv), m->mid);
    /* keep the definition this Method captured: re-resolving by name would pick
     * up a later redefinition (rspec-mocks unbinds the stashed original) */
    struct korb_method *const pinned = VAL2METH(KORB_REF_GET(self))->entry;   /* re-read after the alloc */
    if (LIKELY(r.state == KORB_NORMAL) && pinned != NULL) VAL2METH(r.value)->entry = pinned;
    return r;
}
/* CRuby's bind check, shared by #bind and #bind_call.  slots[0] holds the class
 * the UnboundMethod was retrieved from on entry and the class the method is
 * actually DEFINED in on return (an UnboundMethod taken from a subclass still
 * binds to any instance of the defining class); slots[1] holds the bind target.
 * Both must be caller slots — the singleton probe below can GC. */
static bool korb_meth_bind_ok(CTX *c, korb_value *slots, uint32_t mid) {
    {   korb_value odef = KORB_NIL;
        const struct korb_method *const e = KORB_CLASS_P(slots[0]) ? korb_class_find_method(slots[0], mid, &odef) : NULL;
        if (e != NULL && KORB_CLASS_P(e->owner)) slots[0] = e->owner;
    }
    if (korb_case_eq(c, slots[0], slots[1])) return true;            /* owner === obj  ⇔  obj.is_a?(owner) */
    if (KORB_CLASS_P(slots[0]) && VAL2CLASS(slots[0])->is_module)
        return true;                                                 /* a module's method binds to any object (Ruby 3.0+) */
    if (KORB_CLASS_P(slots[0]) && VAL2CLASS(slots[0])->is_singleton && KORB_CLASS_P(slots[1])) {
        const RESULT sr = korb_obj_singleton(c, slots + 2, slots[1]);   /* a class method binds to a subclass */
        if (sr.state == KORB_NORMAL) return korb_class_has_ancestor(sr.value, slots[0]);
    }
    return false;
}
/* UnboundMethod#bind(obj) → Method bound to obj (obj must be a kind_of owner). */
static RESULT korb_m_meth_bind(CTX *c, korb_value *slots, korb_ref self, korb_slice a) {
    const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    if (UNLIKELY(!m->unbound)) return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'bind' for a Method");
    slots[0] = m->recv;                                             /* the class it was retrieved from (rooted) */
    slots[1] = KORB_SLICE_GET(a, 0);                               /* obj */
    const uint32_t mid = m->mid;
    if (UNLIKELY(!korb_meth_bind_ok(c, slots, mid))) {
        char onm[192];                                              /* the owner's NAME, not "Class"; #<Class:0x…> if anonymous */
        if (VAL2CLASS(slots[0])->name_sym) korb_class_qname_into(c, slots[0], onm, sizeof onm); else korb_desc_inspect(c, slots[0], onm, sizeof onm);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "bind argument must be an instance of %s", onm);
    }
    struct korb_method *const pinned = VAL2METH(KORB_REF_GET(self))->entry;   /* carry the unbound's definition over */
    RESULT r = korb_method_new(c, slots + 2, slots[1], mid);       /* bound: recv = obj */
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    ARO_STORE(c, VAL2METH(r.value), (korb_value *)(uintptr_t)&VAL2METH(r.value)->owner, slots[0]);   /* fix the dispatch to the unbound's owner */
    korb_meth_pin(c, r.value);
    if (pinned != NULL) VAL2METH(r.value)->entry = pinned;         /* the bind must not re-resolve a redefined name */
    if (VAL2METH(KORB_REF_GET(self))->default_new) {   /* rebinding Class#new runs the ALLOCATOR on the target, not its
                                             * own `def self.new` — rspec-mocks reaches the original this way */
        KorbMethod *const bm = VAL2METH(r.value);
        bm->default_new = 1; bm->entry = NULL;
        ARO_STORE(c, bm, (korb_value *)(uintptr_t)&bm->owner, KORB_NIL);
    }
    return r;
}
/* UnboundMethod#bind_call(obj, *args, &blk) → bind then call.  The block rides
 * along like Method#call's (tilt binds a compiled template method and yields). */
static RESULT korb_m_meth_bind_call(CTX *c, korb_value *slots, korb_ref self, korb_slice a,
                                    NODE *block, korb_value *def_env, korb_value *cself) {
    const KorbMethod *const m = VAL2METH(KORB_REF_GET(self));
    if (UNLIKELY(KORB_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const uint32_t mid = m->mid, argc = KORB_SLICE_LEN(a) - 1;
    slots[0] = m->unbound ? m->recv : m->owner;            /* unbound: recv is the owner class */
    slots[1] = KORB_SLICE_GET(a, 0);                      /* bind target */
    if (KORB_CLASS_P(slots[0]) && UNLIKELY(!korb_meth_bind_ok(c, slots, mid))) {   /* same test as #bind */
        char onm[192];
        if (VAL2CLASS(slots[0])->name_sym) korb_class_qname_into(c, slots[0], onm, sizeof onm); else korb_desc_inspect(c, slots[0], onm, sizeof onm);
        return korb_raise(c, slots, KORB_E_TYPE, 0, "bind argument must be an instance of %s", onm);
    }
    if (UNLIKELY(VAL2METH(KORB_REF_GET(self))->default_new)) {   /* Class#new rebound: run the allocator */
        slots[0] = KORB_SLICE_GET(a, 0);
        for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, 1 + i);
        c->vm->super_new_skip = korb_dispatch_class(c, slots[0]);
        korb_flink_hand_on_cframe(c, slots);
        const RESULT nr = korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);
        c->vm->super_new_skip = KORB_NIL;
        return nr;
    }
    const korb_value owner = slots[0];                          /* the resolved defining class */
    if (owner != KORB_NIL && KORB_CLASS_P(owner)) {        /* invoke the FIXED method from its owner */
        /* the entry PINNED at capture, not a fresh name lookup: tilt captures a
         * compiled template method and removes it right away, then binds it */
        struct korb_method *const entry = m->entry ? (struct korb_method *)(uintptr_t)m->entry
                                                   : korb_class_find_method(owner, mid, NULL);
        /* the callee window is built here, so its header (identity / link / EP)
         * is staged here too, as Method#call does: korb_invoke_method fills the
         * identity and relies on a zeroed EP */
        if (LIKELY(entry != NULL && entry->kind == KORB_METHOD_ISEQ)) {
            slots[0] = 0;                                 /* base[-4] (identity) */
            slots[1] = korb_flink_cur_cframe(c, slots);   /* base[-3] (frame link) */
            slots[2] = 0;                                 /* base[-2] (EP) */
            slots[3] = KORB_SLICE_GET(a, 0);             /* base[-1] self (the bind target) */
            for (uint32_t i = 0; i < argc; i++) slots[4 + i] = KORB_SLICE_GET(a, 1 + i);
            return korb_invoke_method(c, slots + 4 + argc, entry, argc, 0, mid, slots[3], owner, block, def_env, KORB_CSELF_VAL(cself));
        }
        if (entry != NULL) {                              /* builtin (cfunc/attr): still the CAPTURED entry, not a
                                                           * re-dispatch — a singleton override of the same name on
                                                           * the target must not win (Module#name is the classic). */
            slots[0] = 0;                                 /* header, as above: a block-taking builtin marks base[-4] */
            slots[1] = korb_flink_cur_cframe(c, slots);
            slots[2] = 0;
            slots[3] = KORB_SLICE_GET(a, 0);             /* recv, in the slot below the args */
            for (uint32_t i = 0; i < argc; i++) slots[4 + i] = KORB_SLICE_GET(a, 1 + i);
            return korb_dispatch_method(c, slots + 4 + argc, entry, mid, 0, argc, owner, block, def_env, cself, true);
        }
    }
    slots[0] = KORB_SLICE_GET(a, 0);                                /* recv */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = KORB_SLICE_GET(a, 1 + i);
    korb_flink_hand_on_cframe(c, slots);
    return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, block, def_env, cself);
}

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
