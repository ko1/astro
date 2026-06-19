/* koruby_precise — symbol.c: builtin methods, #included into korb_runtime.c's TU
 * (inherits its includes + korb_runtime.h macros).  Split from korb_runtime.c. */
/* ---- Symbol methods ------------------------------------------------------ */

static RESULT korb_m_sym_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return korb_str_new(c, slots, nm, (uint32_t)strlen(nm));
}
static RESULT korb_m_sym_to_sym(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
static RESULT korb_m_sym_empty(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    return RESULT_OK(korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)))[0] == '\0' ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_sym_len(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const char *nm = korb_sym_name(c->vm, SYM2ID(VALUE_REF_GET(self)));
    return RESULT_OK(LONG2FIX((intptr_t)strlen(nm)));
}

/* ---- nil / true / false methods ------------------------------------------ */

static RESULT korb_m_nil_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "", 0); }
static RESULT korb_m_nil_to_i(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(LONG2FIX(0)); }
static RESULT korb_m_nil_to_a(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_ary_new(c, slots, 0); }
static RESULT korb_m_nil_to_r(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_rat_new(c, slots, 0, 1); }   /* nil.to_r / nil.rationalize([eps]) → (0/1); rationalize ignores its arg */
static RESULT korb_m_nil_to_f(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_float_new(c, slots, 0.0); }   /* nil.to_f → 0.0 */
static RESULT korb_m_nil_to_h(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_hash_new(c, slots, 4); }       /* nil.to_h → {} */
static RESULT korb_m_nil_to_c(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_cpx_new(c, slots, LONG2FIX(0), LONG2FIX(0)); }   /* nil.to_c → (0+0i) */
static RESULT korb_m_true_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "true", 4); }
static RESULT korb_m_false_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "false", 5); }

/* ---- universal (Object) methods ------------------------------------------ */

static RESULT korb_m_obj_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static RESULT korb_m_nil_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_obj_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_eql(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eql(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }  /* type-strict: 1.eql?(1.0) => false */
static RESULT korb_m_obj_neq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_FALSE : KORB_TRUE); }
static RESULT korb_m_obj_equal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(VALUE_REF_GET(self) == VALUE_SLICE_GET(a,0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_itself(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
/* freeze: no-op (koruby has no frozen state) → self.  frozen?: true only for
 * immediates (Integer/Symbol/nil/true/false), false otherwise. */
static RESULT korb_m_obj_freeze(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (AROH_IS_GC_OBJECT(v)) ((AroObjectHeader *)(uintptr_t)v)->flags |= KORB_FL_FROZEN;
    return RESULT_OK(v);
}
static RESULT korb_m_obj_frozen_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; VALUE v = VALUE_REF_GET(self);
    if (!AROH_IS_GC_OBJECT(v)) return RESULT_OK(KORB_TRUE);   /* immediates are frozen */
    return RESULT_OK((((AroObjectHeader *)(uintptr_t)v)->flags & KORB_FL_FROZEN) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_obj_cmp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a, 0)) ? LONG2FIX(0) : KORB_NIL); }
/* resolve a Symbol/String name arg to the ivar-key Symbol (`:@x`, `"@x"`). */
static bool korb_name_to_sym(CTX *c, VALUE name, VALUE *out) {
    if (SYMBOL_P(name)) { *out = name; return true; }
    if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); *out = ID2SYM(korb_intern(c->vm, s->buf->data, s->len)); return true; }
    return false;
}
static RESULT korb_m_obj_ivar_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym, name = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!korb_name_to_sym(c, name, &sym)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(name));
    if (UNLIKELY(!KORB_OBJECT_P(VALUE_REF_GET(self))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't set instance variable on %s", korb_type_name(VALUE_REF_GET(self)));
    CHECK(korb_ivar_set(c, slots, self, sym, VALUE_SLICE_GET(a, 1)));
    return RESULT_OK(VALUE_SLICE_GET(a, 1));
}
static RESULT korb_m_obj_ivar_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE sym, name = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!korb_name_to_sym(c, name, &sym)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(name));
    (void)slots;
    if (!KORB_OBJECT_P(VALUE_REF_GET(self))) return RESULT_OK(KORB_NIL);
    return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), sym));
}
/* Object#instance_variables → [:@a, :@b, ...] in definition order. */
static RESULT korb_m_obj_ivars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE sv = VALUE_REF_GET(self);
    if (!KORB_OBJECT_P(sv)) return korb_ary_new(c, slots, 0);   /* immediates / builtins: none */
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
/* Object#method(:sym) → bound Method. */
static RESULT korb_m_obj_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    VALUE name = VALUE_SLICE_GET(a, 0);
    uint32_t mid;
    if (SYMBOL_P(name)) mid = SYM2ID(name);
    else if (KORB_STRING_P(name)) { const KorbString *s = VAL2STR(name); mid = korb_intern(c->vm, s->buf->data, s->len); }
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Symbol", korb_type_name(name));
    return korb_method_new(c, slots, VALUE_REF_GET(self), mid);
}
/* Method#call / #[] — re-dispatch to recv.mid(*args).  Stage [recv | args...]
 * below a fresh cursor and reuse the send machinery (polymorphic with Array#[]). */
static RESULT korb_m_meth_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbMethod *m = VAL2METH(VALUE_REF_GET(self));
    uint32_t mid = m->mid, argc = VALUE_SLICE_LEN(a);
    slots[0] = m->recv;                                  /* recv below the args */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, NULL, NULL, NULL);
}
static RESULT korb_m_meth_recv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2METH(VALUE_REF_GET(self))->recv);
}
/* Proc#call / [] / .() / === — invoke the captured block body.  Stage A: env is
 * a tagged-odd live-slots pointer (correct while the defining frame is alive). */
static RESULT korb_m_proc_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbProc *p = VAL2PROC(VALUE_REF_GET(self));
    uint32_t argc = VALUE_SLICE_LEN(a);
    if (p->iseq == NULL) {                               /* Symbol#to_proc: arg0.sym(arg1..) */
        uint32_t mid = p->sym_mid;
        if (UNLIKELY(argc < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver is available");
        for (uint32_t i = 0; i < argc; i++) slots[i] = VALUE_SLICE_GET(a, i);
        return korb_send_impl(c, slots + argc, mid, 0, argc - 1, NULL, NULL, NULL);
    }
    NODE *entry = p->iseq;
    VALUE penv = p->env;                                 /* already tagged: odd=slots / even=KorbEnv */
    slots[0] = p->self;                                  /* captured self (rooted) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    return korb_block_yield(c, slots + 1 + argc, entry, (VALUE *)(uintptr_t)penv, &slots[1], argc, &slots[0]);
}
/* Symbol#to_proc — a Proc that sends the symbol to its first argument. */
static RESULT korb_m_sym_to_proc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    uint32_t mid = SYM2ID(VALUE_REF_GET(self));
    KorbProc *p = korb_alloc(c, slots, sizeof(KorbProc), KORB_OBJ_PROC);
    p->iseq = NULL; p->sym_mid = mid; p->is_lambda = 0;
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->env, 0);
    ARO_STORE(c, p, (VALUE *)(uintptr_t)&p->self, KORB_NIL);
    return RESULT_OK((VALUE)p);
}
static RESULT korb_m_proc_lambda_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a; return RESULT_OK(VAL2PROC(VALUE_REF_GET(self))->is_lambda ? KORB_TRUE : KORB_FALSE);
}
/* Proc#arity: #required positional, negated as -(req+1) when optional/rest make
 * it variable.  (Symbol#to_proc → -2, matching CRuby.) */
static RESULT korb_m_proc_arity(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c;(void)slots;(void)a;
    const KorbProc *p = VAL2PROC(VALUE_REF_GET(self));
    if (p->iseq == NULL) return RESULT_OK(LONG2FIX(-2));   /* symbol proc */
    const NODE *e = p->iseq;
    const bool variable = (e->u.node_entry.opt_defaults != NULL) || (e->u.node_entry.rest_slot >= 0);
    const uint32_t reqc = variable ? e->u.node_entry.req_cnt : e->u.node_entry.params_cnt;
    return RESULT_OK(LONG2FIX(variable ? -((intptr_t)reqc + 1) : (intptr_t)reqc));
}
static RESULT korb_m_meth_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(ID2SYM(VAL2METH(VALUE_REF_GET(self))->mid));
}

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
