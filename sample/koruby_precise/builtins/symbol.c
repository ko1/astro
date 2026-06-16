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
static RESULT korb_m_true_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "true", 4); }
static RESULT korb_m_false_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self;(void)a; return korb_str_new(c, slots, "false", 5); }

/* ---- universal (Object) methods ------------------------------------------ */

static RESULT korb_m_obj_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_FALSE); }
static RESULT korb_m_nil_nil_q(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)self;(void)a; return RESULT_OK(KORB_TRUE); }
static RESULT korb_m_obj_eq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_neq(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(korb_value_eq(VALUE_REF_GET(self), VALUE_SLICE_GET(a,0)) ? KORB_FALSE : KORB_TRUE); }
static RESULT korb_m_obj_equal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots; return RESULT_OK(VALUE_REF_GET(self) == VALUE_SLICE_GET(a,0) ? KORB_TRUE : KORB_FALSE); }
static RESULT korb_m_obj_itself(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)c;(void)slots;(void)a; return RESULT_OK(VALUE_REF_GET(self)); }
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
    return RESULT_OK(korb_ivar_get(VALUE_REF_GET(self), sym));
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
static RESULT korb_m_meth_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;(void)a; return RESULT_OK(ID2SYM(VAL2METH(VALUE_REF_GET(self))->mid));
}

/* generic to_s / inspect — render via the printer into a fresh String.
 * Specific types (Integer#to_s, String#to_s, ...) override via their own table. */
