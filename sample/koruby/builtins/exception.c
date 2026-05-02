/* Exception — moved from builtins.c. */

/* ---------- Exception ----------
 * Exception is a T_OBJECT now; message lives in @message and backtrace
 * lives in @__backtrace__ (set during raise — currently empty). */
static VALUE exc_message(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE msg = korb_ivar_get(self, korb_intern("@message"));
    if (UNDEF_P(msg) || NIL_P(msg)) {
        if (!SPECIAL_CONST_P(self)) {
            struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
            return korb_str_new_cstr(k && k->name ? korb_id_name(k->name) : "Exception");
        }
        return korb_str_new_cstr("");
    }
    return msg;
}
static VALUE exc_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return exc_message(c, self, argc, argv);
}
static VALUE exc_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self)) return korb_str_new_cstr("#<Exception>");
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
    char buf[256];
    VALUE msg = korb_ivar_get(self, korb_intern("@message"));
    const char *ms = (msg && !SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) == T_STRING)
                       ? korb_str_cstr(msg) : "";
    snprintf(buf, sizeof(buf), "#<%s: %s>", kn, ms);
    return korb_str_new_cstr(buf);
}
static VALUE exc_backtrace(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE bt = korb_ivar_get(self, korb_intern("@__backtrace__"));
    if (!UNDEF_P(bt) && !NIL_P(bt)) return bt;
    return korb_ary_new();
}
static VALUE exc_initialize(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc >= 1) {
        VALUE msg = argv[0];
        if (!SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) != T_STRING) {
            /* coerce via to_s */
            VALUE s = korb_funcall(c, msg, korb_intern("to_s"), 0, NULL);
            if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) msg = s;
        }
        korb_ivar_set(self, korb_intern("@message"), msg);
    }
    return self;
}

/* (Range#exclude_end? folded into builtins/range.c) */
