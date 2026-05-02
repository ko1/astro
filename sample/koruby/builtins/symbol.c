/* Symbol — moved from builtins.c.  Includes Symbol#to_proc shim. */

/* ---------- Symbol#to_proc ---------- */
static VALUE sym_to_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Allocate a marker Proc whose body is NULL and whose `self` is the
     * Symbol value.  korb_yield/proc_call detect (body==NULL && SYMBOL_P(self))
     * and dispatch as `arg.send(sym, *rest)`. */
    struct korb_proc *p = korb_xcalloc(1, sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = (VALUE)korb_vm->proc_class;
    p->body = NULL;
    p->env = NULL;
    p->env_size = 0;
    p->params_cnt = 1;
    p->param_base = 0;
    p->self = self;            /* the symbol itself */
    p->is_lambda = false;
    return (VALUE)p;
}

/* (range ext folded into builtins/range.c) */
/* (integer ext folded into builtins/integer.c) */
/* (float ext folded into builtins/float.c) */

/* ---------- Symbol ---------- */
static VALUE sym_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(korb_id_name(korb_sym2id(self)));
}
static VALUE sym_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(self == argv[0]);
}
/* Symbol#<=> — compares by name. */
static VALUE sym_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!SYMBOL_P(argv[0])) return Qnil;
    const char *a = korb_id_name(korb_sym2id(self));
    const char *b = korb_id_name(korb_sym2id(argv[0]));
    int r = strcmp(a, b);
    return INT2FIX(r < 0 ? -1 : r > 0 ? 1 : 0);
}
/* Symbol#succ — name's #succ wrapped back into a Symbol. */
static VALUE sym_succ(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE s = korb_str_new_cstr(korb_id_name(korb_sym2id(self)));
    VALUE next_str = korb_funcall(c, s, korb_intern("succ"), 0, NULL);
    if (BUILTIN_TYPE(next_str) != T_STRING) return self;
    struct korb_string *ns = (struct korb_string *)next_str;
    return korb_id2sym(korb_intern_n(ns->ptr, ns->len));
}

/* Symbol#size / length — character count of the symbol's name. */
static VALUE sym_length(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)strlen(korb_id_name(korb_sym2id(self))));
}
/* Symbol#empty? */
static VALUE sym_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(*korb_id_name(korb_sym2id(self)) == '\0');
}
/* Symbol#upcase / downcase / capitalize / swapcase — return new Symbol. */
static VALUE sym_upcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    buf[n] = 0;
    VALUE r = korb_id2sym(korb_intern_n(buf, (long)n));
    return r;
}
static VALUE sym_downcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[n] = 0;
    VALUE r = korb_id2sym(korb_intern_n(buf, (long)n));
    return r;
}
static VALUE sym_capitalize(CTX *c, VALUE self, int argc, VALUE *argv) {
    const char *s = korb_id_name(korb_sym2id(self));
    size_t n = strlen(s);
    char *buf = korb_xmalloc_atomic(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = s[i];
        if (i == 0)  buf[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
        else         buf[i] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    buf[n] = 0;
    return korb_id2sym(korb_intern_n(buf, (long)n));
}
static VALUE sym_swapcase(CTX *c, VALUE self, int argc, VALUE *argv) {
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
    return korb_id2sym(korb_intern_n(buf, (long)n));
}

