/* Exception — moved from builtins.c. */

/* ---------- Exception ----------
 * Exception is a T_OBJECT now; message lives in @message and backtrace
 * lives in @__backtrace__ (set during raise — currently empty). */
/* Exception#message — calls #to_s and returns the resulting string.
 * Subclasses that override #to_s see their override applied here.  When
 * to_s isn't otherwise overridden it falls back to @message via
 * exc_to_s.  This is the CRuby-2.7+ behavior (`message` was equivalent
 * to `to_s`). */
static VALUE exc_to_s_internal(CTX *c, VALUE self);
static VALUE exc_message(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_funcall(c, self, korb_intern("to_s"), 0, NULL);
}
static VALUE exc_to_s_internal(CTX *c, VALUE self) {
    VALUE msg = korb_ivar_get(self, korb_intern("@message"));
    if (UNDEF_P(msg) || NIL_P(msg)) {
        if (!SPECIAL_CONST_P(self)) {
            struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
            /* Walk past singleton metaclasses — exc.class on a frozen
             * exception with a redefined method returns the user class
             * name, not "(singleton)". */
            while (k && (k->basic.head.flags & FL_SINGLETON)) k = k->super;
            const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
            return korb_str_new_cstr(kn);
        }
        return korb_str_new_cstr("");
    }
    /* CRuby calls #to_s on @message if it isn't a String. */
    if (SPECIAL_CONST_P(msg) || BUILTIN_TYPE(msg) != T_STRING) {
        return korb_funcall(c, msg, korb_intern("to_s"), 0, NULL);
    }
    return msg;
}
static VALUE exc_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return exc_to_s_internal(c, self);
}
static VALUE exc_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self)) return korb_str_new_cstr("#<Exception>");
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    while (k && (k->basic.head.flags & FL_SINGLETON)) k = k->super;
    const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
    /* Drive the inspect output from #to_s so subclasses that override
     * to_s see their value used.  Three cases:
     *   to_s == ""           → just the class name  (no #<...:>)
     *   to_s == class name    → "#<Class: Class>"   (CRuby keeps it)
     *   to_s == anything else → "#<Class: <to_s>>" */
    VALUE s = korb_funcall(c, self, korb_intern("to_s"), 0, NULL);
    if (c->state == KORB_RAISE) return Qnil;
    const char *ms = (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING)
                       ? korb_str_cstr(s) : "";
    if (ms[0] == '\0') return korb_str_new_cstr(kn);
    char *buf = korb_xmalloc_atomic(strlen(kn) + strlen(ms) + 8);
    sprintf(buf, "#<%s: %s>", kn, ms);
    return korb_str_new_cstr(buf);
}
static VALUE exc_backtrace(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE bt = korb_ivar_get(self, korb_intern("@__backtrace__"));
    if (!UNDEF_P(bt) && !NIL_P(bt)) return bt;
    return korb_ary_new();
}

/* Exception#set_backtrace(arg) — accepts nil, a String, or an Array of
 * Strings.  Anything else is TypeError. */
static VALUE exc_set_backtrace(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA, "wrong number of arguments (given 0, expected 1)");
        return Qnil;
    }
    VALUE arg = argv[0];
    VALUE bt;
    if (NIL_P(arg)) {
        bt = Qnil;
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) {
        bt = korb_ary_new_capa(1);
        korb_ary_push(bt, arg);
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)arg;
        for (long i = 0; i < a->len; i++) {
            VALUE e = a->ptr[i];
            if (SPECIAL_CONST_P(e) || BUILTIN_TYPE(e) != T_STRING) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT, "backtrace must be Array of String");
                return Qnil;
            }
        }
        bt = arg;
    } else {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT, "backtrace must be Array of String");
        return Qnil;
    }
    korb_ivar_set(self, korb_intern("@__backtrace__"), bt);
    return arg;
}

/* Exception#backtrace_locations — koruby doesn't track Thread::Backtrace::
 * Location objects; for compatibility we return the same array as
 * #backtrace (or nil if no backtrace).  Tests that probe individual
 * Location attributes will fail, but the common nil-vs-set check works. */
static VALUE exc_backtrace_locations(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE bt = korb_ivar_get(self, korb_intern("@__backtrace__"));
    if (UNDEF_P(bt) || NIL_P(bt)) return Qnil;
    return bt;
}
static VALUE exc_initialize(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Drop trailing FL_KWARGS hash and look for `:receiver` (NameError
     * / FrozenError accept it as a keyword). */
    int eff_argc = argc;
    VALUE recv_kw = Qundef;
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *kh = (struct korb_hash *)argv[argc - 1];
        for (struct korb_hash_entry *e = kh->first; e; e = e->next) {
            if (SYMBOL_P(e->key) && korb_sym2id(e->key) == korb_intern("receiver")) {
                recv_kw = e->value;
            }
        }
        eff_argc = argc - 1;
    }
    if (eff_argc >= 1) {
        VALUE msg = argv[0];
        if (!SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) != T_STRING && !NIL_P(msg)) {
            VALUE s = korb_funcall(c, msg, korb_intern("to_s"), 0, NULL);
            if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) msg = s;
        }
        if (!NIL_P(msg) || eff_argc >= 1) {
            korb_ivar_set(self, korb_intern("@message"), msg);
        }
    }
    /* NameError / NoMethodError accept a second positional arg as the
     * `name` (the missing identifier).  We unconditionally store argv[1]
     * into @name when present — Exception subclasses that don't expose
     * a `name` reader simply ignore it. */
    if (eff_argc >= 2) {
        korb_ivar_set(self, korb_intern("@name"), argv[1]);
    }
    if (!UNDEF_P(recv_kw)) {
        korb_ivar_set(self, korb_intern("@receiver"), recv_kw);
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
