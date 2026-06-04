/* Exception — moved from builtins.c. */

/* ---------- Exception ----------
 * Exception is a T_OBJECT now; message lives in @message and backtrace
 * lives in @__backtrace__ (set during raise — currently empty). */
/* Exception#message — calls #to_s and returns the resulting string.
 * Subclasses that override #to_s see their override applied here.  When
 * to_s isn't otherwise overridden it falls back to @message via
 * exc_to_s.  This is the CRuby-2.7+ behavior (`message` was equivalent
 * to `to_s`). */
static RESULT exc_to_s_internal(CTX *c, VALUE self);
static RESULT exc_message(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return korb_funcall(c, c->sp_top, self, korb_intern("to_s"), 0, NULL);
}
static RESULT exc_to_s_internal(CTX *c, VALUE self) {
    VALUE msg = korb_ivar_get(self, korb_intern("@message"));
    if (UNDEF_P(msg) || NIL_P(msg)) {
        if (!SPECIAL_CONST_P(self)) {
            struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
            /* Walk past singleton metaclasses — exc.class on a frozen
             * exception with a redefined method returns the user class
             * name, not "(singleton)". */
            while (k && (k->basic.head.flags & FL_SINGLETON)) k = k->super;
            const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
            return RESULT_OK(korb_str_new_cstr(c, c->sp_top, kn));
        }
        return RESULT_OK(korb_str_new_cstr(c, c->sp_top, ""));
    }
    /* CRuby calls #to_s on @message if it isn't a String. */
    if (SPECIAL_CONST_P(msg) || BUILTIN_TYPE(msg) != T_STRING) {
        return korb_funcall(c, c->sp_top, msg, korb_intern("to_s"), 0, NULL);
    }
    return RESULT_OK(msg);
}
static RESULT exc_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return exc_to_s_internal(c, self);
}
static RESULT exc_inspect(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self)) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, "#<Exception>"));
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    while (k && (k->basic.head.flags & FL_SINGLETON)) k = k->super;
    const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
    /* Drive the inspect output from #to_s so subclasses that override
     * to_s see their value used.  Three cases:
     *   to_s == ""           → just the class name  (no #<...:>)
     *   to_s == class name    → "#<Class: Class>"   (CRuby keeps it)
     *   to_s == anything else → "#<Class: <to_s>>" */
    RESULT _rt_s = korb_funcall(c, c->sp_top, self, korb_intern("to_s"), 0, NULL);
    if (_rt_s.state == KORB_RAISE) return RESULT_OK(Qnil);
    VALUE s = _rt_s.value;
    const char *ms = (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING)
                       ? korb_str_cstr(s) : "";
    if (ms[0] == '\0') return RESULT_OK(korb_str_new_cstr(c, c->sp_top, kn));
    char *buf = korb_xmalloc_atomic(strlen(kn) + strlen(ms) + 8);
    sprintf(buf, "#<%s: %s>", kn, ms);
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, buf));
}
static RESULT exc_backtrace(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE bt = korb_ivar_get(self, korb_intern("@__backtrace__"));
    if (!UNDEF_P(bt) && !NIL_P(bt)) return RESULT_OK(bt);
    return RESULT_OK(korb_ary_new(c, c->sp_top));
}

/* Exception#set_backtrace(arg) — accepts nil, a String, or an Array of
 * Strings.  Anything else is TypeError. */
static RESULT exc_set_backtrace(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    if (argc < 1) {
        return korb_raise_argument_error(c, "wrong number of arguments (given 0, expected 1)");
    }
    VALUE arg = sp[-1];
    VALUE bt;
    if (NIL_P(arg)) {
        bt = Qnil;
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) {
        bt = korb_ary_new_capa(c, sp, 1);
        korb_ary_push(c, c->sp_top, bt, arg);
    } else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)arg;
        for (long i = 0; i < a->len; i++) {
            VALUE e = korb_ary_items(a)[i];
            if (SPECIAL_CONST_P(e) || BUILTIN_TYPE(e) != T_STRING) {
                return korb_raise_type_error(c, "backtrace must be Array of String");
            }
        }
        bt = arg;
    } else {
        return korb_raise_type_error(c, "backtrace must be Array of String");
    }
    /* self went stale across the korb_ary_new_capa/push above; re-read the
     * forwarded staging slot.  Hoist the intern so the C arg-eval order
     * can't restage a moved self either. */
    const ID bt_id = korb_intern("@__backtrace__");
    korb_ivar_set(sp[-argc - 1], bt_id, bt);
    return RESULT_OK(arg);
}

/* Exception#backtrace_locations — koruby doesn't track Thread::Backtrace::
 * Location objects; for compatibility we return the same array as
 * #backtrace (or nil if no backtrace).  Tests that probe individual
 * Location attributes will fail, but the common nil-vs-set check works. */
static RESULT exc_backtrace_locations(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE bt = korb_ivar_get(self, korb_intern("@__backtrace__"));
    if (UNDEF_P(bt) || NIL_P(bt)) return RESULT_OK(Qnil);
    return RESULT_OK(bt);
}
static RESULT exc_initialize(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

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
            VALUE s = UNWRAP(korb_funcall(c, c->sp_top, msg, korb_intern("to_s"), 0, NULL));
            if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) msg = s;
            /* self went stale across the to_s funcall's GC — re-read the
             * forwarded staging slot before storing through it. */
            self = sp[-argc - 1];
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
    return RESULT_OK(self);
}

/* Exception#cause — returns the previously-rescued exception that was
 * "current" when this one was raised, or nil if none.  Set by
 * node_rescue when raise happens inside a rescue body. */
static RESULT exc_cause(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = korb_ivar_get(self, korb_intern("@cause"));
    return RESULT_OK(UNDEF_P(v) ? Qnil : v);
}

/* Exception#full_message([highlight: false, order: :top]) —
 * formatted message + backtrace.  Simple concat: "<file:line>: <msg>
 * (Class)" + back-trace lines.  Ignore kwargs entirely. */
static RESULT exc_full_message(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    (void)argv;

    /* Park result (sp[0]) + a per-part scratch string (sp[1]) across the many
     * korb_str_new_cstr / korb_str_concat GC points: bare C-locals (msg, r)
     * would be relocated and the next concat would deref a moved-out handle
     * (STRESS+PURGE SEGV).  korb_str_concat may relocate its dest, so always
     * re-capture sp[0] from its return. */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[1] = UNWRAP(korb_funcall(c, c->sp_top, sp[-argc - 1], korb_intern("to_s"), 0, NULL));
    if (SPECIAL_CONST_P(sp[1]) || BUILTIN_TYPE(sp[1]) != T_STRING) {
        sp[1] = korb_str_new_cstr(c, sp + 2, "");
    }
    sp[0] = korb_str_new_cstr(c, sp + 2, "");
    sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
    VALUE self = sp[-argc - 1];   /* re-read after the to_s funcall GC */
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_OBJECT) {
        struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
        if (k && k->name) {
            const char *kn = korb_id_name(k->name);
            if (kn && kn[0] != 0) {
                sp[1] = korb_str_new_cstr(c, sp + 2, " (");
                sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
                sp[1] = korb_str_new_cstr(c, sp + 2, kn);
                sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
                sp[1] = korb_str_new_cstr(c, sp + 2, ")");
                sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
            }
        }
    }
    sp[1] = korb_str_new_cstr(c, sp + 2, "\n");
    sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
    VALUE r = sp[0];
    c->sp_top = sp;
    return RESULT_OK(r);
}
/* Exception#detailed_message — returns "message (Class)" by default,
 * or just the message if Class is RuntimeError. */
static RESULT exc_detailed_message(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    (void)argv;

    /* Park msg/result (sp[0]) + part scratch (sp[1]) across the to_s funcall
     * and the str_new/concat GC points; re-read self from its slot after the
     * funcall (a bare C-local self would be moved and the klass deref below
     * would fault under STRESS+PURGE). */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[0] = UNWRAP(korb_funcall(c, c->sp_top, sp[-argc - 1], korb_intern("to_s"), 0, NULL));
    VALUE self = sp[-argc - 1];
    if (SPECIAL_CONST_P(self)) { VALUE m = sp[0]; c->sp_top = sp; return RESULT_OK(m); }
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    const char *kn = k && k->name ? korb_id_name(k->name) : "Exception";
    if (strcmp(kn, "RuntimeError") == 0) { VALUE m = sp[0]; c->sp_top = sp; return RESULT_OK(m); }
    sp[0] = korb_str_dup(c, sp + 2, sp[0]);
    sp[1] = korb_str_new_cstr(c, sp + 2, " (");
    sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
    sp[1] = korb_str_new_cstr(c, sp + 2, kn);
    sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
    sp[1] = korb_str_new_cstr(c, sp + 2, ")");
    sp[0] = korb_str_concat(c, sp + 2, sp[0], sp[1]);
    VALUE r = sp[0];
    c->sp_top = sp;
    return RESULT_OK(r);
}

/* Exception#exception([msg]) — re-construct or return self. */
static RESULT exc_exception(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc == 0) return RESULT_OK(self);
    if (argc == 1 && argv[0] == self) return RESULT_OK(self);
    /* Build a new instance of self's class with msg = argv[0]. */
    if (SPECIAL_CONST_P(self)) return RESULT_OK(self);
    struct korb_class *k = (struct korb_class *)((struct RBasic *)self)->klass;
    VALUE n = korb_object_new(c, c->sp_top, k);
    VALUE msg = argv[0];
    if (!SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) != T_STRING) {
        VALUE s = UNWRAP(korb_funcall(c, c->sp_top, msg, korb_intern("to_s"), 0, NULL));
        if (!SPECIAL_CONST_P(s) && BUILTIN_TYPE(s) == T_STRING) msg = s;
    }
    korb_ivar_set(n, korb_intern("@message"), msg);
    return RESULT_OK(n);
}

/* NoMethodError#receiver — the object the missing method was called on.
 * Set by korb_dispatch_call / korb_dispatch_visibility_raise. */
static RESULT nme_receiver(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE v = korb_ivar_get(self, korb_intern("@receiver"));
    if (UNDEF_P(v)) {
        return korb_raise_argument_error(c, "no receiver is available");
    }
    return RESULT_OK(v);
}
static RESULT nme_name(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = korb_ivar_get(self, korb_intern("@name"));
    return RESULT_OK(UNDEF_P(v) ? Qnil : v);
}

/* SystemExit#status / #success? — set by Kernel#exit. */
static RESULT syx_status(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = korb_ivar_get(self, korb_intern("@status"));
    return RESULT_OK(UNDEF_P(v) ? INT2FIX(0) : v);
}
static RESULT syx_success_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = korb_ivar_get(self, korb_intern("@success"));
    if (UNDEF_P(v) || NIL_P(v)) {
        VALUE st = korb_ivar_get(self, korb_intern("@status"));
        if (FIXNUM_P(st)) return RESULT_OK(KORB_BOOL(FIX2LONG(st) == 0));
        return RESULT_OK(Qtrue);
    }
    return RESULT_OK(v);
}

/* (Range#exclude_end? folded into builtins/range.c) */
