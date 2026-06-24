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
    return RESULT_OK(LONG2FIX(korb_utf8_count(nm, (uint32_t)strlen(nm))));   /* UTF-8 char count, like String#length */
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
    if (UNLIKELY(m->unbound)) return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method 'call' for an UnboundMethod (use #bind)");
    uint32_t mid = m->mid, argc = VALUE_SLICE_LEN(a);
    slots[0] = m->recv;                                  /* recv below the args */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, NULL, NULL, NULL);
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
    uint32_t argc = VALUE_SLICE_LEN(a);
    if (p->iseq == NULL) {                               /* no body: symbol proc / method proc */
        uint32_t mid = p->sym_mid;
        if (p->is_lambda) {                              /* Method#to_proc: recv.mid(args...) */
            slots[0] = p->self;
            for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
            return korb_send_impl(c, slots + 1 + argc, mid, 0, argc, NULL, NULL, NULL);
        }
        if (UNLIKELY(argc < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no receiver is available");
        for (uint32_t i = 0; i < argc; i++) slots[i] = VALUE_SLICE_GET(a, i);   /* Symbol#to_proc: arg0.sym(arg1..) */
        return korb_send_impl(c, slots + argc, mid, 0, argc - 1, NULL, NULL, NULL);
    }
    NODE *entry = p->iseq;
    VALUE penv = p->env;                                 /* already tagged: odd=slots / even=KorbEnv */
    slots[0] = p->self;                                  /* captured self (rooted) */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, i);
    /* If this proc has a `|&b|` param and the call site passed a (real) block,
     * forward it so &b binds to it — via the full path (the hot wrapper is
     * untouched; a no-block call leaves &b nil). */
    if (block != NULL && block != KORB_BLK_CPROC && block->head.kind == &kind_node_entry &&
        korb_entry_blk_param_slot(entry) >= 0)
        return korb_block_yield_full(c, slots + 1 + argc, entry, (VALUE *)(uintptr_t)penv,
                                     &slots[1], argc, &slots[0], block, def_env, cself);
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
/* Proc#parameters — [[kind, name], ...] from the parse-time param_info (cold;
 * never on the call/yield hot path).  A non-lambda proc reports a required
 * positional as :opt (CRuby semantics); keyreq/rest/block keep their kind. */
static RESULT korb_m_proc_parameters(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    /* Capture everything off the proc BEFORE the first alloc — korb_ary_new GCs
     * and may move the KorbObject, so `p` must not be dereferenced afterwards.
     * pi (node param_info) and the booleans are all stable values. */
    const KorbProc *const p = VAL2PROC(VALUE_REF_GET(self));
    const struct korb_param_info *const pi = p->iseq ? (const struct korb_param_info *)p->iseq->u.node_entry.param_info : NULL;
    const bool lam = p->is_lambda;
    const bool symproc = (p->iseq == NULL);
    static const char *const knames[] = { "req", "opt", "rest", "keyreq", "key", "keyrest", "block" };
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
static intptr_t korb_method_arity(const struct korb_method *km) {
    switch (km->kind) {
      case KORB_METHOD_ATTR_R: return 0;
      case KORB_METHOD_ATTR_W: return 1;
      case KORB_METHOD_BUILTIN:
      case KORB_METHOD_CFUNC:  return km->params_cnt;          /* -1 = variadic */
      case KORB_METHOD_DM: {
        const KorbProc *p = VAL2PROC(km->dm_proc);
        if (p->iseq == NULL) return -2;
        const NODE *e = p->iseq;
        const bool var = (e->u.node_entry.opt_defaults != NULL) || (e->u.node_entry.rest_slot >= 0);
        const uint32_t req = var ? e->u.node_entry.req_cnt : e->u.node_entry.params_cnt;
        return var ? -((intptr_t)req + 1) : (intptr_t)req;
      }
      default: {                                               /* ISEQ */
        const bool var = (km->opt_defaults != NULL) || (km->rest_slot >= 0);
        const uint32_t req = km->req_cnt + km->post_cnt;
        return var ? -((intptr_t)req + 1) : (intptr_t)req;
      }
    }
}
static RESULT korb_m_meth_arity(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const struct korb_method *km = korb_meth_resolve(c, VAL2METH(VALUE_REF_GET(self)));
    return RESULT_OK(LONG2FIX(km ? korb_method_arity(km) : -1));
}
static RESULT korb_m_meth_owner(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a;
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (m->unbound) return RESULT_OK(m->recv);                      /* unbound: recv is the owner */
    const struct korb_method *km = korb_meth_resolve(c, m);
    if (km && km->owner != KORB_NIL) return RESULT_OK(km->owner);   /* defining class/module */
    return RESULT_OK(korb_builtin_class_obj(c->vm, KORB_C_OBJECT)); /* global fn → Object */
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
    /* ISEQ: required, optional, rest, post (names not stored) */
    for (uint32_t i = 0; i < km->req_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    const uint32_t nopt = (km->params_cnt >= 0 && (uint32_t)km->params_cnt > km->req_cnt) ? (uint32_t)km->params_cnt - km->req_cnt : 0;
    for (uint32_t i = 0; i < nopt; i++) CHECK(korb_param_push(c, slots + 1, res, "opt"));
    if (km->rest_slot >= 0) CHECK(korb_param_push(c, slots + 1, res, "rest"));
    for (uint32_t i = 0; i < km->post_cnt; i++) CHECK(korb_param_push(c, slots + 1, res, "req"));
    return RESULT_OK(VALUE_REF_GET(res));
}
/* Module#instance_method(name) → UnboundMethod owned by the class. */
static RESULT korb_m_class_instance_method(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE nv = VALUE_SLICE_GET(a, 0);
    uint32_t mid;
    if (SYMBOL_P(nv))           mid = SYM2ID(nv);
    else if (KORB_STRING_P(nv)) mid = korb_intern(c->vm, VAL2STR(nv)->buf->data, VAL2STR(nv)->len);
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", korb_type_name(nv));
    const VALUE cls = VALUE_REF_GET(self);
    if (UNLIKELY(!KORB_CLASS_P(cls) || korb_class_find_method(cls, mid, NULL) == NULL))
        return korb_raise(c, slots, KORB_E_NOMETHOD, 0, "undefined method '%s' for class '%s'",
                          korb_sym_name(c->vm, mid), korb_type_name(cls));
    return korb_unbound_new(c, slots, cls, mid);
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
    const VALUE obj = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!korb_case_eq(c, m->recv, obj)))                    /* owner === obj  ⇔  obj.is_a?(owner) */
        return korb_raise(c, slots, KORB_E_TYPE, 0, "bind argument must be an instance of %s", korb_type_name(m->recv));
    return korb_method_new(c, slots, obj, m->mid);
}
/* UnboundMethod#bind_call(obj, *args) → bind then call. */
static RESULT korb_m_meth_bind_call(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const KorbMethod *const m = VAL2METH(VALUE_REF_GET(self));
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    const uint32_t mid = m->mid, argc = VALUE_SLICE_LEN(a) - 1;
    slots[0] = VALUE_SLICE_GET(a, 0);                                /* recv */
    for (uint32_t i = 0; i < argc; i++) slots[1 + i] = VALUE_SLICE_GET(a, 1 + i);
    return korb_send(c, slots + 1 + argc, mid, 0, argc);
}

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
