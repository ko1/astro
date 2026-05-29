/* Symbol — moved from builtins.c.  Includes Symbol#to_proc shim. */

/* ---------- Symbol#to_proc ---------- */
static RESULT sym_to_proc(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Allocate a marker Proc whose body is NULL and whose `self` is the
     * Symbol value.  korb_yield/proc_call detect (body==NULL && SYMBOL_P(self))
     * and dispatch as `arg.send(sym, *rest)`. */
    struct korb_proc *p = korb_xcalloc(1, sizeof(*p));
    p->basic.head.flags = T_PROC;
    p->basic.klass = (VALUE)KORB_VM(c)->proc_class;
    p->body = NULL;
    p->env = NULL;
    p->env_size = 0;
    p->params_cnt = 1;
    p->param_base = 0;
    p->self = self;            /* the symbol itself */
    /* CRuby: Symbol#to_proc returns a lambda-style proc since 3.0. */
    p->is_lambda = true;
    extern void koruby_register_libc_obj(struct RBasic *);
    koruby_register_libc_obj(&p->basic);
    return RESULT_OK((VALUE)p);
}

/* (range ext folded into builtins/range.c) */
/* (integer ext folded into builtins/integer.c) */
/* (float ext folded into builtins/float.c) */

/* ---------- Symbol ---------- */
static RESULT sym_to_s(CTX *c, int argc, VALUE *sp) {
    /* sp[-1] = self.  CRuby 3.4+: Symbol#to_s returns a "chilled" String — frozen
     * by virtue of being interned but transparently mutable on demand. */
    VALUE s = korb_str_new_cstr(c, sp, korb_id_name(korb_sym2id(sp[-1])));
    if (!SPECIAL_CONST_P(s)) RBASIC(s)->head.flags |= FL_CHILLED;
    return RESULT_OK(s);
}
static RESULT sym_eq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(self == argv[0]));
}
/* Symbol#<=> — compares by name. */
static RESULT sym_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!SYMBOL_P(argv[0])) return RESULT_OK(Qnil);
    const char *a = korb_id_name(korb_sym2id(self));
    const char *b = korb_id_name(korb_sym2id(argv[0]));
    int r = strcmp(a, b);
    return RESULT_OK(INT2FIX(r < 0 ? -1 : r > 0 ? 1 : 0));
}
/* Symbol#succ — name's #succ wrapped back into a Symbol. */
static RESULT sym_succ(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE s = korb_str_new_cstr(c, c->sp_top, korb_id_name(korb_sym2id(self)));
    VALUE next_str = UNWRAP(korb_funcall(c, s, korb_intern("succ"), 0, NULL));
    if (BUILTIN_TYPE(next_str) != T_STRING) return RESULT_OK(self);
    struct korb_string *ns = (struct korb_string *)next_str;
    return RESULT_OK(korb_id2sym(korb_intern_n(ns->ptr, ns->len)));
}

/* Symbol#size / length — character count of the symbol's name. */
static RESULT sym_length(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX((long)strlen(korb_id_name(korb_sym2id(self)))));
}
/* Symbol#empty? */
static RESULT sym_empty_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(*korb_id_name(korb_sym2id(self)) == '\0'));
}
/* Symbol#upcase / downcase / capitalize / swapcase — return new Symbol. */
static RESULT sym_upcase(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    buf[n] = 0;
    VALUE r = korb_id2sym(korb_intern_n(buf, (long)n));
    return RESULT_OK(r);
}
static RESULT sym_downcase(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[n] = 0;
    VALUE r = korb_id2sym(korb_intern_n(buf, (long)n));
    return RESULT_OK(r);
}
static RESULT sym_capitalize(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        if (i == 0)  buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
        else         buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[n] = 0;
    return RESULT_OK(korb_id2sym(korb_intern_n(buf, (long)n)));
}
static RESULT sym_swapcase(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        if      (ch >= 'a' && ch <= 'z') buf[i] = ch - 32;
        else if (ch >= 'A' && ch <= 'Z') buf[i] = ch + 32;
        else                              buf[i] = ch;
    }
    buf[n] = 0;
    return RESULT_OK(korb_id2sym(korb_intern_n(buf, (long)n)));
}

