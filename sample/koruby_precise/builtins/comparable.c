/* Comparable — moved from builtins.c. */

/* ---------- Comparable ----------
 *
 * All cfuncs invoke `self.<=>(other)` and interpret the result.  If
 * <=> returns nil (incomparable), comparison ops raise ArgumentError. */

/* Returns the sign of `self <=> other`: -1, 0, or 1.  Fixnum is direct;
 * Float / Bignum / any other Numeric is reduced to its sign so that
 * comparators like Comparable#> work for non-canonical <=> returns
 * (e.g. user code that returns 0.1 to mean "greater"). */
static long korb_cmp_call(CTX *c, VALUE self, VALUE other, RESULT *err) {
    /* self/other are by-value moving handles used in the error path AFTER the
     * <=> funcall (and korb_inspect) GC points.  Park them; the happy path
     * below doesn't need them, so pop once <=> succeeded with non-nil. */
    VALUE *const _csp = c->sp_top;
    _csp[0] = self; _csp[1] = other;
    c->sp_top = _csp + 2;
    RESULT _r = korb_funcall(c, _csp[0], korb_intern("<=>"), 1, &_csp[1]);
    if (_r.state != KORB_NORMAL) { c->sp_top = _csp; *err = _r; return 0; }
    VALUE r = _r.value;
    if (NIL_P(r)) {
        /* CRuby's "comparison of X with Y failed" uses the class name on
         * the LHS but inspect-style for non-builtin RHS values
         * (`"comparison of String with 7 failed"`). */
        VALUE oi = korb_inspect(c, c->sp_top, _csp[1]);
        const char *o_str = (!SPECIAL_CONST_P(oi) && BUILTIN_TYPE(oi) == T_STRING)
                                ? korb_str_cstr(oi)
                                : korb_id_name(korb_class_of_class(_csp[1])->name);
        /* Fetch ArgumentError AFTER korb_inspect's GC — a C-local fetched
         * before would be stale by the korb_raise below. */
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        *err = korb_raise(c, (struct korb_class *)eArg, "comparison of %s with %s failed",
                   korb_id_name(korb_class_of_class(_csp[0])->name), o_str);
        c->sp_top = _csp;
        return 0;
    }
    c->sp_top = _csp;   /* pop: self/other no longer needed past here */
    if (FIXNUM_P(r)) {
        long v = FIX2LONG(r);
        return v < 0 ? -1 : (v > 0 ? 1 : 0);
    }
    if (FLONUM_P(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT)) {
        double d = korb_num2dbl(r);
        if (d < 0.0) return -1;
        if (d > 0.0) return 1;
        return 0;
    }
    if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_BIGNUM) {
        int s = mpz_sgn((mpz_ptr)((struct korb_bignum *)r)->mpz);
        return s < 0 ? -1 : (s > 0 ? 1 : 0);
    }
    return 0;
}

static RESULT cmp_lt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    RESULT _err = RESULT_OK(Qnil);
    long cv = korb_cmp_call(c, self, argv[0], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    return RESULT_OK(KORB_BOOL(cv < 0));
}
static RESULT cmp_le(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    RESULT _err = RESULT_OK(Qnil);
    long cv = korb_cmp_call(c, self, argv[0], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    return RESULT_OK(KORB_BOOL(cv <= 0));
}
static RESULT cmp_gt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    RESULT _err = RESULT_OK(Qnil);
    long cv = korb_cmp_call(c, self, argv[0], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    return RESULT_OK(KORB_BOOL(cv > 0));
}
static RESULT cmp_ge(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    RESULT _err = RESULT_OK(Qnil);
    long cv = korb_cmp_call(c, self, argv[0], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    return RESULT_OK(KORB_BOOL(cv >= 0));
}
static RESULT cmp_eq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* CRuby fast path: identical objects compare equal without
     * invoking #<=>.  Avoids infinite recursion when #<=> calls super
     * and the only resolved super is BasicObject (no <=>). */
    if (argc >= 1 && self == argv[0]) return RESULT_OK(Qtrue);
    /* Recursion guard: when a user-defined <=> ends up calling ==
     * (directly or through Object's eq dispatch), we'd recurse forever.
     * CRuby's rb_exec_recursive marks (recv, mid) and returns nil on
     * re-entry; we approximate with a thread-local depth counter,
     * returning false past a sane depth. */
    static __thread int cmp_eq_depth = 0;
    if (cmp_eq_depth >= 16) return RESULT_OK(Qfalse);
    cmp_eq_depth++;
    /* If <=> raises, propagate the raise — CRuby's Comparable#== lets
     * exceptions go through. */
    RESULT _cr = korb_funcall(c, self, korb_intern("<=>"), 1, argv);
    cmp_eq_depth--;
    if (_cr.state != KORB_NORMAL) return _cr;
    VALUE r = _cr.value;
    if (NIL_P(r)) return RESULT_OK(Qfalse);
    if (FIXNUM_P(r)) return RESULT_OK(KORB_BOOL(FIX2LONG(r) == 0));
    if (FLONUM_P(r)) return RESULT_OK(KORB_BOOL(korb_flonum_to_double(r) == 0.0));
    if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT) {
        return RESULT_OK(KORB_BOOL(((struct korb_float *)r)->value == 0.0));
    }
    /* Non-nil non-numeric — CRuby raises ArgumentError.  Re-read self/other
     * from the (scanned) value stack: the #<=> funcall above fired GC, so the
     * C-local self/argv handles are stale → korb_class_of_class derefs a
     * retired plane under STRESS+PURGE (comparable/equal_value).  Nothing
     * between here and korb_raise allocates (intern is pre-interned, id_name /
     * class_of_class don't alloc; korb_raise formats before allocating). */
    const VALUE self_r  = sp[-argc - 1];
    const VALUE other_r = sp[-argc];
    return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError")),
                      "comparison of %s with %s failed",
                      korb_id_name(korb_class_of_class(self_r)->name),
                      korb_id_name(korb_class_of_class(other_r)->name));
}
static RESULT cmp_between(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 2) {
        return korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 2)", argc);
    }
    RESULT _err = RESULT_OK(Qnil);
    long lo = korb_cmp_call(c, self, argv[0], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    long hi = korb_cmp_call(c, self, argv[1], &_err);
    if (_err.state != KORB_NORMAL) return _err;
    return RESULT_OK(KORB_BOOL(lo >= 0 && hi <= 0));
}
static RESULT cmp_clamp(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Two forms:
     *   clamp(min, max)  — both bounds explicit
     *   clamp(range)     — bounds taken from range.begin / range.end
     *                      (either end may be nil for half-bounded ranges) */
    VALUE lo, hi;
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        /* `clamp(a...b)` is rejected — except when end is nil (endless
         * range), where exclusion is meaningless.  CRuby accepts endless
         * exclusive ranges for clamp. */
        if (r->exclude_end && !NIL_P(r->end)) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg,
                       "cannot clamp with an exclusive range");
        }
        lo = r->begin;
        hi = r->end;
    } else if (argc == 2) {
        lo = argv[0];
        hi = argv[1];
    } else {
        return korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    /* If both bounds are non-nil, verify lo <= hi.  Use the bounds'
     * own <=> (CRuby uses min.<=>(max)) so user-defined comparison
     * decides; nil result OR positive sign → ArgumentError. */
    if (!NIL_P(lo) && !NIL_P(hi)) {
        VALUE r = UNWRAP(korb_funcall(c, lo, korb_intern("<=>"), 1, &hi));
        bool bad;
        if (NIL_P(r)) bad = true;
        else if (FIXNUM_P(r)) bad = FIX2LONG(r) > 0;
        else if (FLONUM_P(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT))
            bad = korb_num2dbl(r) > 0;
        else bad = false;
        if (bad) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg,
                       "min argument must be smaller than max argument");
        }
    }
    RESULT _err2 = RESULT_OK(Qnil);
    if (!NIL_P(lo)) {
        long cv = korb_cmp_call(c, self, lo, &_err2);
        if (_err2.state != KORB_NORMAL) return _err2;
        if (cv < 0) return RESULT_OK(lo);
    }
    if (!NIL_P(hi)) {
        long cv = korb_cmp_call(c, self, hi, &_err2);
        if (_err2.state != KORB_NORMAL) return _err2;
        if (cv > 0) return RESULT_OK(hi);
    }
    return RESULT_OK(self);
}

/* alias_method(:new_name, :existing_name) — register the existing
 * method under a new name on this class.  Reuses the resolved method
 * struct (methods are immutable in koruby). */
/* Module#undef_method / remove_method.  Real CRuby distinguishes
 * these (undef tombstones the slot to also block super dispatch);
 * koruby's method tables are simple, so both just unlink the entry
 * from this class.  Inherited methods remain reachable, which is
 * remove_method's semantics; for undef_method on a class that
 * doesn't override a super method this still raises NoMethodError
 * because Object doesn't define the name either. */
extern void korb_method_table_remove(struct korb_method_table *mt, ID name);
static RESULT module_undef_or_remove_method_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool is_undef) {
    if (argc < 1) return RESULT_OK(self);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name = SYMBOL_P(argv[i]) ? korb_sym2id(argv[i])
                                     : korb_intern(korb_str_cstr(argv[i]));
        struct korb_method *m = korb_class_find_method(klass, name);
        if (!m) {
            VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
            return korb_raise(c, (struct korb_class *)eN,
                       "undefined method '%s' for class '%s'",
                       korb_id_name(name),
                       klass->name ? korb_id_name(klass->name) : "?");
        }
        korb_method_table_remove(&klass->methods, name);
        if (is_undef) {
            struct korb_method *um = korb_xmalloc(sizeof(*um));
            um->type = KORB_METHOD_UNDEF;
            um->name = name;
            um->defining_class = klass;
            um->def_cref = NULL;
            um->is_simple_frame = false;
            um->visibility = KORB_VIS_PUBLIC;
            extern void korb_method_table_set(struct korb_method_table *, ID, struct korb_method *);
            korb_method_table_set(&klass->methods, name, um);
        }
    }
    if (KORB_VM(c)) { KORB_VM(c)->method_serial++; korb_g_method_serial = KORB_VM(c)->method_serial; }
    return RESULT_OK(self);
}

static RESULT module_undef_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return module_undef_or_remove_method_impl(c, self, argc, argv, true);
}

static RESULT module_remove_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return module_undef_or_remove_method_impl(c, self, argc, argv, false);
}

static RESULT module_undef_or_remove_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Backwards-compat shim — old binding.  Treat as remove_method semantics. */
    return module_undef_or_remove_method_impl(c, self, argc, argv, false);
}

static RESULT module_alias_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 2) {
        return korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 2)", argc);
    }
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    struct korb_class *klass = (struct korb_class *)self;
    ID new_name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern(korb_str_cstr(argv[0]));
    ID old_name = SYMBOL_P(argv[1]) ? korb_sym2id(argv[1]) : korb_intern(korb_str_cstr(argv[1]));
    struct korb_method *m = korb_class_find_method(klass, old_name);
    /* Module receiver: also check Object since included methods are
     * accessible via lookup on Module bodies
     * (e.g. `module Kernel; alias_method :a, :method_on_object; end`). */
    if (!m && BUILTIN_TYPE(self) == T_MODULE && KORB_VM(c)->object_class) {
        m = korb_class_find_method(KORB_VM(c)->object_class, old_name);
    }
    if (!m) {
        VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        return korb_raise(c, (struct korb_class *)eN,
                   "undefined method '%s' for %s",
                   korb_id_name(old_name),
                   klass->name ? korb_id_name(klass->name) : "?");
    }
    korb_class_alias_method(klass, new_name, m);
    return RESULT_OK(korb_id2sym(new_name));
}

static RESULT module_set_visibility_for_args(CTX *c, VALUE self, int argc, VALUE *argv,
                                            enum korb_visibility v)
{
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    struct korb_class *k = (struct korb_class *)self;
    extern struct korb_method *method_table_get(const struct korb_method_table *mt, ID name);
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING)
            name = korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                 ((struct korb_string *)argv[i])->len);
        else continue;
        /* `private :foo` on a subclass: if foo is inherited (not in this
         * class's own table), CLONE it into k's own table so mutating
         * visibility doesn't affect the parent class.  Without this,
         * `class H < A; private :foo; end` would also flip A#foo to
         * private. */
        struct korb_method *m_local = method_table_get(&k->methods, name);
        if (m_local) {
            m_local->visibility = v;
        } else {
            struct korb_method *m_inherited = korb_class_find_method(k, name);
            /* When the receiver is a Module (not Class), CRuby also
             * searches Object's methods (since Object is everything's
             * common ancestor and Module.new bodies can refer to Object
             * methods).  Fall back to Object so `private :Object_method`
             * on `Module.new` works. */
            if (!m_inherited && BUILTIN_TYPE(self) == T_MODULE && KORB_VM(c)->object_class) {
                m_inherited = korb_class_find_method(KORB_VM(c)->object_class, name);
            }
            if (m_inherited) {
                struct korb_method *cp = korb_xmalloc(sizeof(*cp));
                *cp = *m_inherited;
                cp->visibility = v;
                korb_class_alias_method(k, name, cp);
            } else {
                /* Method doesn't exist anywhere.  CRuby raises NameError
                 * for regular Modules / Classes, but is lenient for
                 * singleton classes (where mock objects route undefined
                 * methods through method_missing — `class << x; private
                 * :to_ary; end` shouldn't blow up just because to_ary
                 * isn't a real method). */
                bool is_singleton = (((struct RBasic *)k)->head.flags & FL_SINGLETON);
                if (is_singleton) continue;
                VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
                const char *cn = (k->name != 0) ? korb_id_name(k->name) : "(anon)";
                return korb_raise(c, (struct korb_class *)eN,
                           "undefined method '%s' for %s '%s'",
                           korb_id_name(name),
                           BUILTIN_TYPE(self) == T_MODULE ? "module" : "class",
                           cn);
            }
        }
    }
    return RESULT_OK(Qnil);
}
/* Top-level default visibility — initially PRIVATE (CRuby behavior:
 * top-level defs are private on Object).  Toggled by public/private
 * called at top-level. */
bool g_top_level_default_private = true;

static RESULT module_set_visibility(CTX *c, VALUE self, int argc, VALUE *argv,
                                   enum korb_visibility v)
{
    if (argc == 0) {
        /* No-arg form: change the default visibility for subsequent
         * `def`s in this class body.  `protected` / `private` /
         * `public` toggle.  Returns nil per CRuby. */
        if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
            ((struct korb_class *)self)->default_visibility = v;
        }
        /* Also track top-level private/public state for `def` at the
         * implicit main-object scope.  self is the main object here. */
        if (self == KORB_VM(c)->main_obj) {
            g_top_level_default_private = (v == KORB_VIS_PRIVATE);
        }
        return RESULT_OK(Qnil);
    }
    /* Single Array form: `private([:foo, :bar])` (Ruby 3.x). */
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        CHECK(module_set_visibility_for_args(c, self, (int)a->len, korb_ary_items(a), v));
        return RESULT_OK(argv[0]);
    }
    CHECK(module_set_visibility_for_args(c, self, argc, argv, v));
    /* Ruby 3.0+: public/private/protected with args returns the symbol
     * (single arg) or array of symbols (multiple args).  String args
     * are converted to symbols for the return value. */
    if (argc == 1) {
        if (SYMBOL_P(argv[0])) return RESULT_OK(argv[0]);
        if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
            return RESULT_OK(korb_id2sym(korb_intern_n(((struct korb_string *)argv[0])->ptr,
                                              ((struct korb_string *)argv[0])->len)));
        }
        return RESULT_OK(argv[0]);
    }
    VALUE r = korb_ary_new_capa(c, c->sp_top, argc);
    for (int i = 0; i < argc; i++) {
        if (SYMBOL_P(argv[i])) korb_ary_push(c, c->sp_top, r, argv[i]);
        else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING) {
            korb_ary_push(c, c->sp_top, r, korb_id2sym(korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                                       ((struct korb_string *)argv[i])->len)));
        } else {
            korb_ary_push(c, c->sp_top, r, argv[i]);
        }
    }
    return RESULT_OK(r);
}
static RESULT module_private(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return module_set_visibility(c, self, argc, argv, KORB_VIS_PRIVATE);
}
static RESULT module_public(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return module_set_visibility(c, self, argc, argv, KORB_VIS_PUBLIC);
}
static RESULT module_protected(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return module_set_visibility(c, self, argc, argv, KORB_VIS_PROTECTED);
}
static RESULT module_const_defined_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE))
        return RESULT_OK(Qfalse);
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    else return RESULT_OK(Qfalse);
    /* Walks includes/super by default; the optional second arg `inherit`
     * (default true) controls whether to search ancestors.  When false,
     * only the receiver's own constant table is consulted. */
    bool inherit = true;
    if (argc >= 2) inherit = RTEST(argv[1]);
    extern bool korb_const_has_inherited(struct korb_class *klass, ID name);
    return RESULT_OK(KORB_BOOL(inherit
        ? korb_const_has_inherited((struct korb_class *)self, name)
        : korb_const_has((struct korb_class *)self, name)));
}
static RESULT module_module_function(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
 return RESULT_OK(self); }

/* Struct.new(:a, :b) → returns a new Class with attr_accessor for each */
static RESULT struct_initialize(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* attr_accessor for each member */
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(Qnil);
    struct korb_array *members = (struct korb_array *)members_v;
    long n = members->len;
    for (long i = 0; i < n && i < argc; i++) {
        ID name = SYMBOL_P(korb_ary_items(members)[i]) ? korb_sym2id(korb_ary_items(members)[i]) :
                  korb_intern(korb_str_cstr(korb_ary_items(members)[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ivar_set(self, iv_id, argv[i]);
    }
    return RESULT_OK(self);
}

static RESULT struct_to_a(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(korb_ary_new(c, c->sp_top));
    /* self, the __members__ array, and the result are moving handles, and
     * korb_ary_new_capa / korb_intern / korb_ary_push below all fire GC.
     * Park them at sp[0..2] (scanned) and re-derive each iteration. */
    sp[0] = self;
    sp[1] = members_v;
    sp[2] = 0;
    c->sp_top = sp + 3;
    sp[2] = korb_ary_new_capa(c, sp + 3, ((struct korb_array *)sp[1])->len);
    long mlen = ((struct korb_array *)sp[1])->len;
    for (long i = 0; i < mlen; i++) {
        VALUE mi = korb_ary_items((struct korb_array *)sp[1])[i];
        ID name = SYMBOL_P(mi) ? korb_sym2id(mi) :
                  korb_intern(korb_str_cstr(mi));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ary_push(c, sp + 3, sp[2], korb_ivar_get(sp[0], iv_id));
    }
    VALUE result = sp[2];
    c->sp_top = sp;
    return RESULT_OK(result);
}

/* Struct#[] — read by index or symbol. */
static RESULT struct_aref(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 1) {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError")),
                          "wrong number of arguments (given %d, expected 1)", argc);
    }
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(Qnil);
    struct korb_array *members = (struct korb_array *)members_v;
    long idx = -1;
    bool by_name = false;
    ID want_id = 0;
    if (FIXNUM_P(argv[0])) {
        idx = FIX2LONG(argv[0]);
        if (idx < 0) idx += members->len;
        if (idx < 0 || idx >= members->len) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError")),
                              "offset %ld too %s for struct", FIX2LONG(argv[0]),
                              FIX2LONG(argv[0]) < 0 ? "small" : "large");
        }
    } else if (SYMBOL_P(argv[0])) {
        by_name = true;
        want_id = korb_sym2id(argv[0]);
    } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        by_name = true;
        want_id = korb_intern(korb_str_cstr(argv[0]));
    } else {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                          "no implicit conversion of %s into Integer",
                          korb_id_name(korb_class_of_class(argv[0])->name));
    }
    if (by_name) {
        for (long i = 0; i < members->len; i++) {
            ID mid = SYMBOL_P(korb_ary_items(members)[i]) ? korb_sym2id(korb_ary_items(members)[i]) :
                      korb_intern(korb_str_cstr(korb_ary_items(members)[i]));
            if (mid == want_id) { idx = i; break; }
        }
        if (idx < 0) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError")),
                              "no member '%s' in struct", korb_id_name(want_id));
        }
    }
    ID name = SYMBOL_P(korb_ary_items(members)[idx]) ? korb_sym2id(korb_ary_items(members)[idx]) :
              korb_intern(korb_str_cstr(korb_ary_items(members)[idx]));
    const char *base = korb_id_name(name);
    long bl = strlen(base);
    char *iv = korb_xmalloc_atomic(bl + 2);
    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
    return RESULT_OK(korb_ivar_get(self, korb_intern(iv)));
}

/* Struct#[]= */
static RESULT struct_aset(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc != 2) {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError")),
                          "wrong number of arguments (given %d, expected 2)", argc);
    }
    CHECK_FROZEN_R(c, self);
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(Qnil);
    struct korb_array *members = (struct korb_array *)members_v;
    long idx = -1;
    bool by_name = false;
    ID want_id = 0;
    if (FIXNUM_P(argv[0])) {
        idx = FIX2LONG(argv[0]);
        if (idx < 0) idx += members->len;
        if (idx < 0 || idx >= members->len) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("IndexError")),
                              "offset %ld too %s for struct", FIX2LONG(argv[0]),
                              FIX2LONG(argv[0]) < 0 ? "small" : "large");
        }
    } else if (SYMBOL_P(argv[0])) {
        by_name = true;
        want_id = korb_sym2id(argv[0]);
    } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        by_name = true;
        want_id = korb_intern(korb_str_cstr(argv[0]));
    } else {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                          "no implicit conversion of %s into Integer",
                          korb_id_name(korb_class_of_class(argv[0])->name));
    }
    if (by_name) {
        for (long i = 0; i < members->len; i++) {
            ID mid = SYMBOL_P(korb_ary_items(members)[i]) ? korb_sym2id(korb_ary_items(members)[i]) :
                      korb_intern(korb_str_cstr(korb_ary_items(members)[i]));
            if (mid == want_id) { idx = i; break; }
        }
        if (idx < 0) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError")),
                              "no member '%s' in struct", korb_id_name(want_id));
        }
    }
    ID name = SYMBOL_P(korb_ary_items(members)[idx]) ? korb_sym2id(korb_ary_items(members)[idx]) :
              korb_intern(korb_str_cstr(korb_ary_items(members)[idx]));
    const char *base = korb_id_name(name);
    long bl = strlen(base);
    char *iv = korb_xmalloc_atomic(bl + 2);
    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
    korb_ivar_set(self, korb_intern(iv), argv[1]);
    return RESULT_OK(argv[1]);
}

/* Struct#each — yield each value. */
static RESULT struct_each(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;

    sp[0] = sp[-argc - 1];                       /* feed self to struct_to_a */
    VALUE arr = UNWRAP(struct_to_a(c, 0, sp + 1));
    /* Park the result array (last_line) + self (last_match) in a synthetic
     * frame: korb_yield lowers sp_top below value-stack slots, so the frame
     * chain (always walked) is the only durable parking across the yields. */
    struct korb_frame fr = {
        .prev = c->current_frame, .self = c->current_frame->self,
        .fp = c->current_frame->fp, .cref = c->current_frame->cref,
        .current_class = c->current_frame->current_class,
        .current_file = c->current_frame->current_file,
        .last_line = arr, .last_match = sp[-argc - 1],
    };
    c->current_frame = &fr;
    long n = ((struct korb_array *)fr.last_line)->len;
    for (long i = 0; i < n; i++) {
        VALUE el = korb_ary_items((struct korb_array *)fr.last_line)[i];
        RESULT _y = korb_yield_r(c, 1, &el);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
    }
    VALUE selfr = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(selfr);
}

/* Struct#== — same struct class + equal members. */
static RESULT struct_eq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(argv[0])) return RESULT_OK(Qfalse);
    if (((struct RBasic *)self)->klass != ((struct RBasic *)argv[0])->klass) return RESULT_OK(Qfalse);
    /* Recursion guard for self-referential Structs (CRuby: returns true). */
    static __thread VALUE struct_eq_stk_a[64];
    static __thread VALUE struct_eq_stk_b[64];
    static __thread int struct_eq_top = 0;
    for (int j = 0; j < struct_eq_top; j++) {
        if (struct_eq_stk_a[j] == self && struct_eq_stk_b[j] == argv[0]) return RESULT_OK(Qtrue);
    }
    if (struct_eq_top < 64) {
        struct_eq_stk_a[struct_eq_top] = self;
        struct_eq_stk_b[struct_eq_top] = argv[0];
        struct_eq_top++;
    }
    /* sp[0] receiver for first to_a; sp[1] holds its result (a) so it survives
     * GC during the second to_a; sp[2] receiver for second to_a. */
    sp[0] = self;
    RESULT ra = struct_to_a(c, 0, sp + 1);
    if (ra.state != KORB_NORMAL) { if (struct_eq_top > 0) struct_eq_top--; return ra; }
    sp[1] = ra.value;
    sp[2] = argv[0];
    RESULT rb = struct_to_a(c, 0, sp + 3);
    if (rb.state != KORB_NORMAL) { if (struct_eq_top > 0) struct_eq_top--; return rb; }
    sp[2] = rb.value; /* re-use sp[2] to hold b for funcall arg */
    RESULT res = korb_funcall_r(c, sp[1], korb_intern("=="), 1, &sp[2]);
    if (struct_eq_top > 0) struct_eq_top--;
    return res;
}

/* Struct#to_h — with optional block that transforms each [key, value] pair. */
static RESULT struct_to_h(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(korb_hash_new(c, c->sp_top));
    /* The hash header is arena-allocated (MOVING — the old "libc/non-moving"
     * note was stale).  Park it in this cfunc frame's last_line slot, not on
     * the value stack: the per-element block yield below lowers c->sp_top below
     * a value-stack slot, dropping it from the [stack_base, sp_top) scan range
     * so the hash goes stale → SEGV in korb_hash_aset under STRESS+PURGE.  The
     * frame chain is walked by koruby_visit_roots regardless of c->sp_top. */
    struct korb_frame *const hframe = c->current_frame;
    hframe->last_line = korb_hash_new(c, c->sp_top);
    hframe->last_match = sp[-argc - 1];   /* park struct receiver across yields too */
    c->sp_top = sp + 3;   /* cover sp[1]/sp[2] yield-arg staging below */
    bool has_block = (c->current_block != NULL);
    /* self and __members__ are moving handles, and korb_intern / korb_yield /
     * korb_funcall / korb_hash_aset below all fire GC.  Re-derive members
     * (loop top) and self (sp[-argc-1]) each iteration; member names are
     * Symbols (immediate) so a snapshot key stays valid. */
    for (long i = 0; ; i++) {
        VALUE self_i = hframe->last_match;   /* forwarded across yields (frame-parked) */
        struct korb_class *kl_i = (struct korb_class *)((struct korb_object *)self_i)->basic.klass;
        VALUE mv = korb_const_get_inherited(kl_i, korb_intern("__members__"));
        if (UNDEF_P(mv) || BUILTIN_TYPE(mv) != T_ARRAY) break;
        struct korb_array *members = (struct korb_array *)mv;
        if (i >= members->len) break;
        VALUE key = korb_ary_items(members)[i];
        ID name = SYMBOL_P(key) ? korb_sym2id(key) : korb_intern(korb_str_cstr(key));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        VALUE val = korb_ivar_get(hframe->last_match, korb_intern(iv));
        if (has_block) {
            sp[1] = key;
            sp[2] = val;
            RESULT yr = korb_yield_r(c, 2, &sp[1]);
            if (yr.state != KORB_NORMAL) return yr;
            VALUE pair = yr.value;
            if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY) {
                /* Try to_ary; if absent or returns non-Array, TypeError. */
                if (!SPECIAL_CONST_P(pair)) {
                    struct korb_class *pkl = korb_class_of_class(pair);
                    if (korb_class_find_method(pkl, korb_intern("to_ary"))) {
                        RESULT tr = korb_funcall_r(c, pair, korb_intern("to_ary"), 0, NULL);
                        if (tr.state != KORB_NORMAL) return tr;
                        pair = tr.value;
                    }
                }
                if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY) {
                    return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                      "wrong element type %s (expected array)",
                                      korb_id_name(korb_class_of_class(pair)->name));
                }
            }
            struct korb_array *pa = (struct korb_array *)pair;
            if (pa->len != 2) {
                return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError")),
                                  "element has wrong array length (expected 2, was %ld)", pa->len);
            }
            key = korb_ary_items(pa)[0];
            val = korb_ary_items(pa)[1];
        }
        korb_hash_aset(c, hframe->last_line, key, val);   /* re-read forwarded hash */
    }
    return RESULT_OK(hframe->last_line);
}

/* Struct#size / length */
static RESULT struct_size(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(INT2FIX(0));
    return RESULT_OK(INT2FIX(((struct korb_array *)members_v)->len));
}

/* Struct.members at the class level — return the members array. */
static RESULT struct_class_members(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS) return RESULT_OK(korb_ary_new(c, c->sp_top));
    VALUE members_v = korb_const_get((struct korb_class *)self, korb_intern("__members__"));
    if (UNDEF_P(members_v)) return RESULT_OK(korb_ary_new(c, c->sp_top));
    return RESULT_OK(members_v);
}

static RESULT struct_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Struct.new(:a, :b, keyword_init: true) — strip trailing options
     * Hash before treating remaining args as member names.  We don't
     * implement keyword_init semantics differently from positional
     * (the generated initializer already handles both), but at least
     * the loader-time `Struct.new(:foo, keyword_init: true)` pattern
     * (test_marshal:776 etc.) needs to not blow up on the Hash arg. */
    VALUE kw_init_val = Qundef; /* used by Struct#keyword_init? */
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
        VALUE opts = argv[argc - 1];
        kw_init_val = korb_hash_aref(c, opts, korb_id2sym(korb_intern("keyword_init")));
        /* Normalize to an immediate boolean immediately: #keyword_init?
         * only reports true/false, and an immediate can't go stale across
         * the many method-add / attr_accessor GCs before the const_set far
         * below (a raw heap value held as a C-local there SEGVs under
         * STRESS+PURGE). */
        if (UNDEF_P(kw_init_val)) kw_init_val = Qundef;
        else kw_init_val = RTEST(kw_init_val) ? Qtrue : Qfalse;
        argc--;
    }
    /* Park the new class at sp[0] and members at sp[1].  klass is a moving
     * object and korb_ary_new_from_values is a GC point, so re-derive klass
     * from sp[0] afterwards.  Reserve c->sp_top = sp+2 so both stay scanned;
     * the method-add calls below are libc (no GC) so klass survives them, but
     * keep the reservation through the later attr_accessor / singleton / yield
     * GC points and read klass back from sp[0] at each. */
    sp[0] = 0; sp[1] = 0;
    c->sp_top = sp + 2;
    sp[0] = (VALUE)korb_class_new(c, sp + 2, korb_intern("Struct"), KORB_VM(c)->object_class, T_OBJECT);
    /* Reset name so const_set can rename anonymous Struct subclasses to
     * their constant path (Object → "Foo", or Mod → "Mod::Foo"). */
    ((struct korb_class *)sp[0])->name = 0;
    /* save members */
    sp[1] = korb_ary_new_from_values(c, sp + 2, argc, argv);
    /* Intern FIRST (may GC), then read klass from sp[0]: as args to one call,
     * korb_intern would otherwise move sp[0]'s class after klass was read. */
    ID _members_id = korb_intern("__members__");
    struct korb_class *klass = (struct korb_class *)sp[0];  /* re-derive after GC */
    korb_const_set(klass, _members_id, sp[1]);
    /* Install Struct's standard instance methods FIRST, then let
     * attr_accessor overwrite any collisions (e.g. Data.define(:length)
     * means user-given `length` accessor wins over Struct#length). */
    korb_class_add_method_cfunc_r(klass, korb_intern("initialize"), struct_initialize, -1);
    korb_class_add_method_cfunc_r(klass, korb_intern("to_a"),       struct_to_a,        0);
    korb_class_add_method_cfunc_r(klass, korb_intern("values"),     struct_to_a,        0);
    korb_class_add_method_cfunc_r(klass, korb_intern("members"),    struct_to_a,        0);
    korb_class_add_method_cfunc_r(klass, korb_intern("[]"),         struct_aref,        1);
    korb_class_add_method_cfunc_r(klass, korb_intern("[]="),        struct_aset,       -1);
    korb_class_add_method_cfunc_r(klass, korb_intern("each"),       struct_each,        0);
    /* Struct#each_pair — yield [member, value] for each member. */
    {
        RESULT _struct_each_pair(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
            VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
            if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return RESULT_OK(self);
            /* self + __members__ are moving handles and the loop yields / allocs
             * (GC) — re-derive from sp[-argc-1] / const table each iteration. */
            for (long i = 0; ; i++) {
                VALUE self_i = sp[-argc - 1];
                struct korb_class *kl_i = (struct korb_class *)((struct korb_object *)self_i)->basic.klass;
                VALUE mv = korb_const_get_inherited(kl_i, korb_intern("__members__"));
                if (UNDEF_P(mv) || BUILTIN_TYPE(mv) != T_ARRAY) break;
                struct korb_array *members = (struct korb_array *)mv;
                if (i >= members->len) break;
                VALUE key = korb_ary_items(members)[i];
                ID name = SYMBOL_P(key) ? korb_sym2id(key) : korb_intern(korb_str_cstr(key));
                const char *base = korb_id_name(name);
                long bl = strlen(base);
                char *iv = korb_xmalloc_atomic(bl + 2);
                iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
                VALUE val = korb_ivar_get(sp[-argc - 1], korb_intern(iv));
                sp[0] = key;
                sp[1] = val;
                /* yield [key, val] as a single array when block has 1 param,
                 * or as 2 args when 2 params; korb_yield_r handles arity. */
                sp[2] = korb_ary_new_capa(c, c->sp_top + 3, 2);  /* park pair in sp[2] */
                korb_ary_push(c, sp + 3, sp[2], sp[0]);
                korb_ary_push(c, sp + 3, sp[2], sp[1]);
                RESULT yr = korb_yield_r(c, 1, &sp[2]);
                if (yr.state != KORB_NORMAL) return yr;
            }
            return RESULT_OK(sp[-argc - 1]);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("each_pair"), _struct_each_pair, 0);
    }
    /* Struct#values_at: delegate to to_a.values_at(*indices). */
    {
        RESULT _struct_values_at(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            /* Stage self at c->sp_top[0] and call struct_to_a with sp+1. */
            c->sp_top[0] = self;
            VALUE arr = UNWRAP(struct_to_a(c, 0, c->sp_top + 1));
            return korb_funcall(c, arr, korb_intern("values_at"), argc, argv);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("values_at"), _struct_values_at, -1);
    }
    /* Struct#dig — chain dig into nested structures starting from
     * each member at the index/key given by the first arg. */
    {
        RESULT _struct_dig(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            if (argc < 1) {
                VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                return korb_raise(c, (struct korb_class *)eArg,
                           "wrong number of arguments (given %d, expected 1+)", argc);
            }
            /* First step: resolve member directly (unlike Struct#[], a
             * missing member here yields nil rather than raising — CRuby
             * semantics for Struct#dig). */
            struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
            VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
            VALUE v = Qnil;
            if (!UNDEF_P(members_v) && BUILTIN_TYPE(members_v) == T_ARRAY) {
                struct korb_array *members = (struct korb_array *)members_v;
                long idx = -1;
                ID want_id = 0; bool by_name = false;
                if (FIXNUM_P(argv[0])) {
                    idx = FIX2LONG(argv[0]);
                    if (idx < 0) idx += members->len;
                    if (idx < 0 || idx >= members->len) idx = -1;
                } else if (SYMBOL_P(argv[0])) {
                    by_name = true; want_id = korb_sym2id(argv[0]);
                } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
                    by_name = true; want_id = korb_intern(korb_str_cstr(argv[0]));
                }
                if (by_name) {
                    for (long i = 0; i < members->len; i++) {
                        ID mid = SYMBOL_P(korb_ary_items(members)[i]) ? korb_sym2id(korb_ary_items(members)[i]) :
                                  korb_intern(korb_str_cstr(korb_ary_items(members)[i]));
                        if (mid == want_id) { idx = i; break; }
                    }
                }
                if (idx >= 0) {
                    ID name = SYMBOL_P(korb_ary_items(members)[idx]) ? korb_sym2id(korb_ary_items(members)[idx]) :
                              korb_intern(korb_str_cstr(korb_ary_items(members)[idx]));
                    const char *base = korb_id_name(name);
                    long bl = strlen(base);
                    char *iv = korb_xmalloc_atomic(bl + 2);
                    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
                    v = korb_ivar_get(self, korb_intern(iv));
                }
            }
            if (argc == 1) return RESULT_OK(v);
            if (NIL_P(v)) return RESULT_OK(Qnil);
            /* Recurse: v.dig(*rest). */
            if (!SPECIAL_CONST_P(v) || FIXNUM_P(v) || FLONUM_P(v)) {
                struct korb_class *k = korb_class_of_class(v);
                if (!k || !korb_class_find_method(k, korb_intern("dig"))) {
                    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                    return korb_raise(c, (struct korb_class *)eT,
                               "%s does not have #dig method",
                               korb_id_name(korb_class_of_class(v)->name));
                }
            }
            return korb_funcall(c, v, korb_intern("dig"), argc - 1, &argv[1]);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("dig"), _struct_dig, -1);
    }
    /* Struct#deconstruct — returns array of values (= to_a). */
    {
        RESULT _struct_deconstruct(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            c->sp_top[0] = self;
            return struct_to_a(c, 0, c->sp_top + 1);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("deconstruct"), _struct_deconstruct, 0);
    }
    /* Struct#deconstruct_keys(keys) — returns hash subset by keys (or all if nil). */
    {
        RESULT _struct_deconstruct_keys(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            if (argc < 1) {
                VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                return korb_raise(c, (struct korb_class *)eArg,
                           "wrong number of arguments (given %d, expected 1)", argc);
            }
            /* nil → return full hash of members. */
            if (NIL_P(argv[0])) {
                c->sp_top[0] = self;
                return struct_to_h(c, 0, c->sp_top + 1);
            }
            if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_ARRAY) {
                return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                  "expected Array or nil");
            }
            struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
            VALUE members_v = korb_const_get_inherited(klass, korb_intern("__members__"));
            if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) {
                return RESULT_OK(korb_hash_new(c, c->sp_top));
            }
            struct korb_array *members = (struct korb_array *)members_v;
            struct korb_array *keys = (struct korb_array *)argv[0];
            VALUE result = korb_hash_new(c, c->sp_top);
            /* CRuby: if keys.length > members.length, return empty hash. */
            if (keys->len > members->len) return RESULT_OK(result);
            for (long i = 0; i < keys->len; i++) {
                VALUE k = korb_ary_items(keys)[i];
                long idx = -1;
                ID want_id = 0; bool by_name = false;
                if (FIXNUM_P(k)) {
                    idx = FIX2LONG(k);
                    if (idx < 0) idx += members->len;
                    if (idx < 0 || idx >= members->len) return RESULT_OK(result);
                } else if (SYMBOL_P(k)) {
                    by_name = true; want_id = korb_sym2id(k);
                } else if (!SPECIAL_CONST_P(k) && BUILTIN_TYPE(k) == T_STRING) {
                    by_name = true; want_id = korb_intern(korb_str_cstr(k));
                } else {
                    /* Try to_int coerce. */
                    if (!SPECIAL_CONST_P(k)) {
                        struct korb_class *kk = korb_class_of_class(k);
                        if (korb_class_find_method(kk, korb_intern("to_int"))) {
                            RESULT cr = korb_funcall_r(c, k, korb_intern("to_int"), 0, NULL);
                            if (cr.state != KORB_NORMAL) return cr;
                            if (!FIXNUM_P(cr.value)) {
                                return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                                  "can't convert %s to Integer (#to_int gave non-Integer)",
                                                  korb_id_name(korb_class_of_class(k)->name));
                            }
                            idx = FIX2LONG(cr.value);
                            if (idx < 0) idx += members->len;
                            if (idx < 0 || idx >= members->len) return RESULT_OK(result);
                            goto have_idx;
                        }
                    }
                    return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                                      "no implicit conversion of %s into Integer",
                                      korb_id_name(korb_class_of_class(k)->name));
                }
                if (by_name) {
                    for (long j = 0; j < members->len; j++) {
                        ID mid = SYMBOL_P(korb_ary_items(members)[j]) ? korb_sym2id(korb_ary_items(members)[j]) :
                                  korb_intern(korb_str_cstr(korb_ary_items(members)[j]));
                        if (mid == want_id) { idx = j; break; }
                    }
                    if (idx < 0) return RESULT_OK(result);
                }
            have_idx:;
                ID name = SYMBOL_P(korb_ary_items(members)[idx]) ? korb_sym2id(korb_ary_items(members)[idx]) :
                          korb_intern(korb_str_cstr(korb_ary_items(members)[idx]));
                const char *base = korb_id_name(name);
                long bl = strlen(base);
                char *iv = korb_xmalloc_atomic(bl + 2);
                iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
                VALUE v = korb_ivar_get(self, korb_intern(iv));
                korb_hash_aset(c, result, k, v);
            }
            return RESULT_OK(result);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("deconstruct_keys"), _struct_deconstruct_keys, 1);
    }
    korb_class_add_method_cfunc_r(klass, korb_intern("==" ),        struct_eq,          1);
    /* Struct#eql? — same struct class + all members eql?. */
    {
        RESULT _struct_eql(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE *argv = sp - argc;
            if (SPECIAL_CONST_P(argv[0])) return RESULT_OK(Qfalse);
            if (((struct RBasic *)self)->klass != ((struct RBasic *)argv[0])->klass) return RESULT_OK(Qfalse);
            static __thread VALUE eql_stk_a[64];
            static __thread VALUE eql_stk_b[64];
            static __thread int eql_top = 0;
            for (int j = 0; j < eql_top; j++) {
                if (eql_stk_a[j] == self && eql_stk_b[j] == argv[0]) return RESULT_OK(Qtrue);
            }
            if (eql_top < 64) { eql_stk_a[eql_top] = self; eql_stk_b[eql_top] = argv[0]; eql_top++; }
            sp[0] = self;
            RESULT ra = struct_to_a(c, 0, sp + 1);
            if (ra.state != KORB_NORMAL) { if (eql_top > 0) eql_top--; return ra; }
            sp[1] = ra.value;
            sp[2] = argv[0];
            RESULT rb = struct_to_a(c, 0, sp + 3);
            if (rb.state != KORB_NORMAL) { if (eql_top > 0) eql_top--; return rb; }
            sp[2] = rb.value;
            RESULT res = korb_funcall_r(c, sp[1], korb_intern("eql?"), 1, &sp[2]);
            if (eql_top > 0) eql_top--;
            return res;
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("eql?"), _struct_eql, 1);
    }
    /* Struct#hash — XOR of element hashes + class hash. */
    {
        RESULT _struct_hash(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            static __thread VALUE hash_stk[64];
            static __thread int hash_top = 0;
            for (int j = 0; j < hash_top; j++) {
                if (hash_stk[j] == self) return RESULT_OK(INT2FIX(0));
            }
            if (hash_top < 64) { hash_stk[hash_top] = self; hash_top++; }
            sp[0] = self;
            RESULT ra = struct_to_a(c, 0, sp + 1);
            if (ra.state != KORB_NORMAL) { if (hash_top > 0) hash_top--; return ra; }
            sp[0] = ra.value;   /* park the values array across the per-elem hash funcall */
            long h = (long)((struct RBasic *)sp[-argc - 1])->klass;   /* re-read self */
            long alen = ((struct korb_array *)sp[0])->len;
            for (long i = 0; i < alen; i++) {
                struct korb_array *a = (struct korb_array *)sp[0];   /* re-derive after funcall GC */
                RESULT rh = korb_funcall_r(c, korb_ary_items(a)[i], korb_intern("hash"), 0, NULL);
                if (rh.state != KORB_NORMAL) { if (hash_top > 0) hash_top--; return rh; }
                long eh = FIXNUM_P(rh.value) ? FIX2LONG(rh.value) : 0;
                h = (h * 31) ^ eh;
            }
            if (hash_top > 0) hash_top--;
            return RESULT_OK(INT2FIX(h & 0x3fffffffffffffffLL));
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("hash"), _struct_hash, 0);
    }
    /* Struct#inspect / #to_s — #<struct ClassName key=val, ...> */
    {
        RESULT _struct_inspect(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            static __thread VALUE ins_stk[64];
            static __thread int ins_top = 0;
            for (int j = 0; j < ins_top; j++) {
                if (ins_stk[j] == self) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, "#<struct ...>"));
            }
            if (ins_top < 64) { ins_stk[ins_top] = self; ins_top++; }
            VALUE result = korb_str_new_cstr(c, c->sp_top, "#<struct ");
            sp[0] = result;
            /* re-derive the class after the alloc GC (self moves; kl with it) */
            struct korb_class *kl = (struct korb_class *)((struct korb_object *)sp[-argc - 1])->basic.klass;
            /* Skip class name when class is anonymous or its name is a
             * tentative path through an anonymous ancestor (k->anon_parent
             * != NULL). CRuby Struct#inspect omits the class name in
             * both cases. */
            if (kl->name && kl->anon_parent == NULL && kl->name != korb_intern("(anon)")) {
                VALUE nm = korb_str_new_cstr(c, c->sp_top + 1, korb_id_name(kl->name));
                sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], nm);
                sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], korb_str_new_cstr(c, c->sp_top + 1, " "));
            }
            /* __members__ lives on the original Struct.new class; walk the
             * super chain so subclasses (`class Foo < Struct.new(:a)`)
             * still see it.  Re-derive kl: the class-name concats above are
             * GC points that moved self (and the class with it). */
            kl = (struct korb_class *)((struct korb_object *)sp[-argc - 1])->basic.klass;
            VALUE members_v = korb_const_get_inherited(kl, korb_intern("__members__"));
            if (!UNDEF_P(members_v) && BUILTIN_TYPE(members_v) == T_ARRAY) {
                /* self / class / __members__ are moving handles and the
                 * per-member str_concat / inspect / intern below all fire GC.
                 * Re-derive them from the (scanned) receiver slot + const
                 * table each iteration; sp[0] (result) is re-read by name. */
                for (long i = 0; ; i++) {
                    VALUE self_i = sp[-argc - 1];
                    struct korb_class *kl_i = (struct korb_class *)((struct korb_object *)self_i)->basic.klass;
                    VALUE mv = korb_const_get_inherited(kl_i, korb_intern("__members__"));
                    if (UNDEF_P(mv) || BUILTIN_TYPE(mv) != T_ARRAY) break;
                    struct korb_array *ms = (struct korb_array *)mv;
                    if (i >= ms->len) break;
                    VALUE mi = korb_ary_items(ms)[i];
                    if (i > 0) sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], korb_str_new_cstr(c, c->sp_top + 1, ", "));
                    ID mid = SYMBOL_P(mi) ? korb_sym2id(mi) : korb_intern(korb_str_cstr(mi));
                    const char *base = korb_id_name(mid);
                    long bl = strlen(base);
                    char *iv = korb_xmalloc_atomic(bl + 2);
                    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
                    sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], korb_str_new_cstr(c, c->sp_top + 1, base));
                    sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], korb_str_new_cstr(c, c->sp_top + 1, "="));
                    /* Read the ivar value AFTER the concats above (GC points)
                     * so it isn't a stale handle when inspected. */
                    VALUE val = korb_ivar_get(sp[-argc - 1], korb_intern(iv));
                    VALUE ins = korb_inspect(c, c->sp_top + 1, val);
                    sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], ins);
                }
            }
            sp[0] = korb_str_concat(c, c->sp_top + 1, sp[0], korb_str_new_cstr(c, c->sp_top + 1, ">"));
            if (ins_top > 0) ins_top--;
            return RESULT_OK(sp[0]);
        }
        korb_class_add_method_cfunc_r(klass, korb_intern("inspect"), _struct_inspect, 0);
        korb_class_add_method_cfunc_r(klass, korb_intern("to_s"),    _struct_inspect, 0);
    }
    korb_class_add_method_cfunc_r(klass, korb_intern("to_h"),       struct_to_h,        0);
    korb_class_add_method_cfunc_r(klass, korb_intern("size"),       struct_size,        0);
    korb_class_add_method_cfunc_r(klass, korb_intern("length"),     struct_size,        0);
    /* Now attr_accessor — overrides Struct#length etc. when a member
     * shadows a standard name. */
    {
        /* stage [klass, argv...] for module_attr_accessor cfunc_r ABI */
        c->sp_top[0] = (VALUE)klass;
        for (int i = 0; i < argc; i++) c->sp_top[1 + i] = argv[i];
        DROP_RESULT(module_attr_accessor(c, argc, c->sp_top + 1 + argc));
    }
    /* attr_accessor / method-add above may have fired GC; re-derive klass. */
    klass = (struct korb_class *)sp[0];
    /* Store keyword_init flag for #keyword_init? introspection.  Intern first
     * (may GC), then read klass from sp[0] (see __members__ above). */
    if (!UNDEF_P(kw_init_val)) {
        ID _kwi_id = korb_intern("__keyword_init__");
        korb_const_set((struct korb_class *)sp[0], _kwi_id, kw_init_val);
    }
    /* class-level .members and .[] (synonym for .new) */
    {
        struct korb_class *meta = korb_singleton_class_of(c, c->sp_top, klass);
        klass = (struct korb_class *)sp[0];   /* re-derive after singleton alloc */
        korb_class_add_method_cfunc_r(meta, korb_intern("members"),
                                     struct_class_members, 0);
        RESULT _struct_keyword_init_p(CTX *c, int argc, VALUE *sp) {
            (void)argc; c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            VALUE v = korb_const_get_inherited((struct korb_class *)self, korb_intern("__keyword_init__"));
            if (UNDEF_P(v) || NIL_P(v)) return RESULT_OK(Qnil);
            return RESULT_OK(RTEST(v) ? Qtrue : Qfalse);
        }
        korb_class_add_method_cfunc_r(meta, korb_intern("keyword_init?"), _struct_keyword_init_p, 0);
        RESULT _struct_class_aref(CTX *c, int argc, VALUE *sp) {
            c->sp_top = sp;
            VALUE self = sp[-argc - 1];
            return korb_funcall_r(c, self, korb_intern("new"), argc, sp - argc);
        }
        korb_class_add_method_cfunc_r(meta, korb_intern("[]"), _struct_class_aref, -1);
    }
    /* If a block was given, evaluate it with self = the new class
     * (Struct.new(:x) { def hello; ... end } pattern).  Crucially,
     * also temporarily swap the block's captured cref so `def` inside
     * the block targets the new Struct, NOT the lexical container.
     * Without this, `class TM; X = Struct.new(:y) { def foo; end };
     * end` would leak `foo` onto TM (and worse — `def method_missing`
     * inside the block would replace TM's method_missing, breaking
     * every method on TM). */
    
    klass = (struct korb_class *)sp[0];   /* re-derive before the block region */
    if (c->current_block) {
        VALUE prev_self = c->current_frame->self;
        struct korb_class *prev_class = c->current_frame->current_class;
        struct korb_cref *prev_cref = c->current_frame->cref;
        struct korb_cref new_cref = { .klass = klass, .prev = c->current_frame->cref };
        struct korb_cref blk_cref = { .klass = klass, .prev = c->current_block->cref };
        struct korb_cref *prev_blk_cref = c->current_block->cref;
        VALUE prev_blk_self = c->current_block->self;
        c->current_frame->self = (VALUE)klass;
        c->current_frame->current_class = klass;
        c->current_frame->cref = &new_cref;
        c->current_block->self = (VALUE)klass;
        c->current_block->cref = &blk_cref;
        VALUE av0[1] = { (VALUE)klass };
        RESULT _yr = korb_yield(c, 1, av0);
        c->current_block->self = prev_blk_self;
        c->current_block->cref = prev_blk_cref;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        /* BREAK from class body is silently consumed; other states propagate. */
        if (_yr.state != KORB_NORMAL && _yr.state != KORB_BREAK) { c->sp_top = sp; return _yr; }
    }
    /* klass may have moved during the block yield — return it from sp[0]. */
    VALUE result = sp[0];
    c->sp_top = sp;
    return RESULT_OK(result);
}

static RESULT module_const_get(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    if (argc < 1) return RESULT_OK(Qnil);
    ID name;
    VALUE arg = argv[0];
    if (SYMBOL_P(arg)) name = korb_sym2id(arg);
    else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) {
        name = korb_intern_n(((struct korb_string *)arg)->ptr,
                             ((struct korb_string *)arg)->len);
    } else if (!SPECIAL_CONST_P(arg)) {
        /* Coerce via #to_str (CRuby semantics: const_get accepts a
         * String-convertible name).  Park arg across the two funcalls so the
         * second (to_str) doesn't deref a handle moved by the first (IDIOM B). */
        VALUE *const asp = c->sp_top;
        asp[0] = arg;
        c->sp_top = asp + 1;
        RESULT rtr = korb_funcall(c, asp[0], korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (rtr.state != KORB_NORMAL) { c->sp_top = asp; return rtr; }
        if (RTEST(rtr.value)) {
            RESULT tsr = korb_funcall(c, asp[0], korb_intern("to_str"), 0, NULL);
            if (tsr.state != KORB_NORMAL) { c->sp_top = asp; return tsr; }
            VALUE r = tsr.value;
            if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) {
                name = korb_intern_n(((struct korb_string *)r)->ptr,
                                     ((struct korb_string *)r)->len);
                c->sp_top = asp;
                goto have_name;
            }
        }
        c->sp_top = asp;
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        /* arg went stale across the respond_to?/to_str funcalls — re-read the
         * forwarded receiver-arg slot for the error's class name. */
        return korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into String",
                   korb_id_name(korb_class_of_class(argv[0])->name));
    } else {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of (special) into String");
    }
have_name:;
    /* Name derivation above hits GC points (korb_intern_n / respond_to? +
     * to_str funcalls); re-read self from the receiver slot (IDIOM A). */
    self = sp[-argc - 1];
    argv = sp - argc;
    bool inherit = true;
    if (argc >= 2) inherit = RTEST(argv[1]);
    extern VALUE korb_const_get_inherited(struct korb_class *klass, ID name);
    VALUE v = inherit
        ? korb_const_get_inherited((struct korb_class *)self, name)
        : korb_const_get((struct korb_class *)self, name);
    if (UNDEF_P(v)) {
        VALUE eName = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        return korb_raise(c, (struct korb_class *)eName,
                   "uninitialized constant %s::%s",
                   korb_id_name(((struct korb_class *)self)->name),
                   korb_id_name(name));
    }
    return RESULT_OK(v);
}

static RESULT module_const_set(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    if (argc < 2) return RESULT_OK(Qnil);
    /* Frozen check before any side effects (CRuby semantics). */
    if (korb_obj_frozen_p(self)) {
        VALUE eF = korb_const_get(KORB_VM(c)->object_class, korb_intern("FrozenError"));
        return korb_raise(c, (struct korb_class *)eF, "can't modify frozen %s",
                   korb_id_name(korb_class_of_class(self)->name));
    }
    /* Coerce non-Symbol/String name via #to_str (CRuby semantics). */
    VALUE name_arg = argv[0];
    if (!SYMBOL_P(name_arg) &&
        (SPECIAL_CONST_P(name_arg) || BUILTIN_TYPE(name_arg) != T_STRING)) {
        if (!SPECIAL_CONST_P(name_arg)) {
            /* Two funcalls on the same by-value moving handle — park name_arg
             * across both so the second (to_str) doesn't deref a moved handle
             * (IDIOM B). */
            VALUE *const nsp = c->sp_top;
            nsp[0] = name_arg;
            c->sp_top = nsp + 1;
            RESULT rtr = korb_funcall(c, nsp[0], korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (rtr.state != KORB_NORMAL) { c->sp_top = nsp; return rtr; }
            if (RTEST(rtr.value)) {
                RESULT tsr = korb_funcall(c, nsp[0], korb_intern("to_str"), 0, NULL);
                if (tsr.state != KORB_NORMAL) { c->sp_top = nsp; return tsr; }
                nsp[0] = tsr.value;
            }
            name_arg = nsp[0];
            c->sp_top = nsp;
        }
    }
    const char *namep = NULL;
    int namelen = 0;
    if (SYMBOL_P(name_arg)) {
        const char *s = korb_id_name(korb_sym2id(name_arg));
        namep = s; namelen = (int)strlen(s);
    } else if (!SPECIAL_CONST_P(name_arg) && BUILTIN_TYPE(name_arg) == T_STRING) {
        struct korb_string *str = (struct korb_string *)name_arg;
        namep = str->ptr; namelen = str->len;
    } else {
        VALUE inspv = UNWRAP(korb_funcall(c, argv[0], korb_intern("inspect"), 0, NULL));
        const char *insp = (!SPECIAL_CONST_P(inspv) && BUILTIN_TYPE(inspv) == T_STRING)
                              ? ((struct korb_string *)inspv)->ptr : "?";
        /* Fetch TypeError AFTER the inspect funcall's GC — a class fetched
         * before would be a stale (retired-plane) handle by the korb_raise. */
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string", insp);
    }
    /* Constant name must start with an uppercase ASCII letter and consist
     * of word characters; mirror MRI's rb_is_const_id check. */
    bool valid = (namelen > 0 && namep[0] >= 'A' && namep[0] <= 'Z');
    for (int i = 1; valid && i < namelen; i++) {
        char ch = namep[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || (unsigned char)ch >= 0x80)) {
            valid = false;
        }
    }
    if (!valid) {
        VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        return korb_raise(c, (struct korb_class *)eN,
                   "wrong constant name %.*s", namelen, namep);
    }
    ID name = korb_intern_n(namep, namelen);   /* GC point: may grow symtab */
    /* self / argv[1] are moving handles — re-read from the (scanned) receiver
     * and arg slots after korb_intern_n, which can move them (IDIOM A). */
    self = sp[-argc - 1];
    argv = sp - argc;
    korb_const_set((struct korb_class *)self, name, argv[1]);
    return RESULT_OK(argv[1]);
}

/* (string ext folded into builtins/string.c) */
/* (array ext folded into builtins/array.c) */
/* (hash ext folded into builtins/hash.c) */
