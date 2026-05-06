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

/* Exception#cause — returns the previously-rescued exception that was
 * "current" when this one was raised, or nil if none.  Set by
 * node_rescue when raise happens inside a rescue body. */
static VALUE exc_cause(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@cause"));
    return UNDEF_P(v) ? Qnil : v;
}

/* Exception#full_message([highlight: false, order: :top]) —
 * formatted message + backtrace.  Simple concat: "<file:line>: <msg>
 * (Class)" + back-trace lines.  Ignore kwargs entirely. */
static VALUE exc_full_message(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE msg = exc_message(c, self, 0, NULL);
    if (SPECIAL_CONST_P(msg) || BUILTIN_TYPE(msg) != T_STRING) {
        msg = korb_str_new_cstr("");
    }
    VALUE r = korb_str_new_cstr("");
    korb_str_concat(r, msg);
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_OBJECT) {
        struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
        if (k && k->name) {
            const char *kn = korb_id_name(k->name);
            if (kn && kn[0] != 0) {
                korb_str_concat(r, korb_str_new_cstr(" ("));
                korb_str_concat(r, korb_str_new_cstr(kn));
                korb_str_concat(r, korb_str_new_cstr(")"));
            }
        }
    }
    korb_str_concat(r, korb_str_new_cstr("\n"));
    return r;
}
/* Exception#detailed_message — returns "message (Class)" by default,
 * or just the message if Class is RuntimeError. */
static VALUE exc_detailed_message(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE msg = exc_message(c, self, 0, NULL);
    if (SPECIAL_CONST_P(self)) return msg;
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
    if (strcmp(kn, "RuntimeError") == 0) return msg;
    VALUE r = korb_str_dup(msg);
    korb_str_concat(r, korb_str_new_cstr(" ("));
    korb_str_concat(r, korb_str_new_cstr(kn));
    korb_str_concat(r, korb_str_new_cstr(")"));
    return r;
}

/* Exception#exception([msg]) — re-construct or return self. */
static VALUE exc_exception(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) return self;
    if (argc == 1 && argv[0] == self) return self;
    /* Build a new instance of self's class with msg = argv[0]. */
    if (SPECIAL_CONST_P(self)) return self;
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    VALUE n = korb_object_new(k);
    VALUE msg = argv[0];
    if (!SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) != T_STRING) {
        VALUE s = korb_funcall(c, msg, korb_intern("to_s"), 0, NULL);
        if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) msg = s;
    }
    korb_ivar_set(n, korb_intern("@message"), msg);
    return n;
}

/* NoMethodError#receiver — the object the missing method was called on.
 * Set by korb_dispatch_call / korb_dispatch_visibility_raise. */
static VALUE nme_receiver(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@receiver"));
    if (UNDEF_P(v)) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA, "no receiver is available");
        return Qnil;
    }
    return v;
}
static VALUE nme_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@name"));
    return UNDEF_P(v) ? Qnil : v;
}

/* SystemExit#status / #success? — set by Kernel#exit. */
static VALUE syx_status(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@status"));
    return UNDEF_P(v) ? INT2FIX(0) : v;
}
static VALUE syx_success_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@success"));
    if (UNDEF_P(v) || NIL_P(v)) {
        VALUE st = korb_ivar_get(self, korb_intern("@status"));
        if (FIXNUM_P(st)) return KORB_BOOL(FIX2LONG(st) == 0);
        return Qtrue;
    }
    return v;
}

/* (Range#exclude_end? folded into builtins/range.c) */
