/* Comparable — moved from builtins.c. */

/* ---------- Comparable ----------
 *
 * All cfuncs invoke `self.<=>(other)` and interpret the result.  If
 * <=> returns nil (incomparable), comparison ops raise ArgumentError. */

/* Returns the sign of `self <=> other`: -1, 0, or 1.  Fixnum is direct;
 * Float / Bignum / any other Numeric is reduced to its sign so that
 * comparators like Comparable#> work for non-canonical <=> returns
 * (e.g. user code that returns 0.1 to mean "greater"). */
static long korb_cmp_call(CTX *c, VALUE self, VALUE other) {
    VALUE r = korb_funcall(c, self, korb_intern("<=>"), 1, &other);
    if (NIL_P(r)) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        /* CRuby's "comparison of X with Y failed" uses the class name on
         * the LHS but inspect-style for non-builtin RHS values
         * (`"comparison of String with 7 failed"`). */
        VALUE oi = korb_inspect(other);
        const char *o_str = (!SPECIAL_CONST_P(oi) && BUILTIN_TYPE(oi) == T_STRING)
                                ? korb_str_cstr(oi)
                                : korb_id_name(korb_class_of_class(other)->name);
        korb_raise(c, (struct korb_class *)eArg, "comparison of %s with %s failed",
                   korb_id_name(korb_class_of_class(self)->name), o_str);
        return 0;
    }
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

static VALUE cmp_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_cmp_call(c, self, argv[0]) < 0);
}
static VALUE cmp_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_cmp_call(c, self, argv[0]) <= 0);
}
static VALUE cmp_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_cmp_call(c, self, argv[0]) > 0);
}
static VALUE cmp_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_cmp_call(c, self, argv[0]) >= 0);
}
static VALUE cmp_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* CRuby fast path: identical objects compare equal without
     * invoking #<=>.  Avoids infinite recursion when #<=> calls super
     * and the only resolved super is BasicObject (no <=>). */
    if (argc >= 1 && self == argv[0]) return Qtrue;
    /* Recursion guard: when a user-defined <=> ends up calling ==
     * (directly or through Object's eq dispatch), we'd recurse forever.
     * CRuby's rb_exec_recursive marks (recv, mid) and returns nil on
     * re-entry; we approximate with a thread-local depth counter,
     * returning false past a sane depth. */
    static __thread int cmp_eq_depth = 0;
    if (cmp_eq_depth >= 16) return Qfalse;
    cmp_eq_depth++;
    VALUE r = korb_funcall(c, self, korb_intern("<=>"), 1, argv);
    cmp_eq_depth--;
    if (c->state == KORB_RAISE) {
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
        return Qfalse;
    }
    if (NIL_P(r)) return Qfalse;
    if (FIXNUM_P(r)) return KORB_BOOL(FIX2LONG(r) == 0);
    if (FLONUM_P(r)) return KORB_BOOL(korb_flonum_to_double(r) == 0.0);
    if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT) {
        return KORB_BOOL(((struct korb_float *)r)->value == 0.0);
    }
    return Qfalse;
}
static VALUE cmp_between(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 2)", argc);
        return Qnil;
    }
    long lo = korb_cmp_call(c, self, argv[0]);
    long hi = korb_cmp_call(c, self, argv[1]);
    return KORB_BOOL(lo >= 0 && hi <= 0);
}
static VALUE cmp_clamp(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Two forms:
     *   clamp(min, max)  — both bounds explicit
     *   clamp(range)     — bounds taken from range.begin / range.end
     *                      (either end may be nil for half-bounded ranges) */
    VALUE lo, hi;
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        if (r->exclude_end) {
            /* `clamp(a...b)` is rejected (no clean cap on b's value). */
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                       "cannot clamp with an exclusive range");
            return Qnil;
        }
        lo = r->begin;
        hi = r->end;
    } else if (argc == 2) {
        lo = argv[0];
        hi = argv[1];
    } else {
        korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 1..2)", argc);
        return Qnil;
    }
    /* If both bounds are non-nil, verify lo <= hi.  Use the bounds'
     * own <=> (CRuby uses min.<=>(max)) so user-defined comparison
     * decides; nil result OR positive sign → ArgumentError. */
    if (!NIL_P(lo) && !NIL_P(hi)) {
        VALUE r = korb_funcall(c, lo, korb_intern("<=>"), 1, &hi);
        if (c->state == KORB_RAISE) return Qnil;
        bool bad;
        if (NIL_P(r)) bad = true;
        else if (FIXNUM_P(r)) bad = FIX2LONG(r) > 0;
        else if (FLONUM_P(r) || (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT))
            bad = korb_num2dbl(r) > 0;
        else bad = false;
        if (bad) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                       "min argument must be smaller than max argument");
            return Qnil;
        }
    }
    if (!NIL_P(lo) && korb_cmp_call(c, self, lo) < 0) return lo;
    if (!NIL_P(hi) && korb_cmp_call(c, self, hi) > 0) return hi;
    return self;
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
static VALUE module_undef_or_remove_method_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool is_undef) {
    if (argc < 1) return self;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name = SYMBOL_P(argv[i]) ? korb_sym2id(argv[i])
                                     : korb_intern(korb_str_cstr(argv[i]));
        struct korb_method *m = korb_class_find_method(klass, name);
        if (!m) {
            VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
            korb_raise(c, (struct korb_class *)eN,
                       "undefined method '%s' for class '%s'",
                       korb_id_name(name),
                       klass->name ? korb_id_name(klass->name) : "?");
            return Qnil;
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
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
    return self;
}

static VALUE module_undef_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_undef_or_remove_method_impl(c, self, argc, argv, true);
}

static VALUE module_remove_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_undef_or_remove_method_impl(c, self, argc, argv, false);
}

static VALUE module_undef_or_remove_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Backwards-compat shim — old binding.  Treat as remove_method semantics. */
    return module_undef_or_remove_method_impl(c, self, argc, argv, false);
}

static VALUE module_alias_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise_argument_error(c, "wrong number of arguments (given %d, expected 2)", argc);
        return Qnil;
    }
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    ID new_name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern(korb_str_cstr(argv[0]));
    ID old_name = SYMBOL_P(argv[1]) ? korb_sym2id(argv[1]) : korb_intern(korb_str_cstr(argv[1]));
    struct korb_method *m = korb_class_find_method(klass, old_name);
    /* Module receiver: also check Object since included methods are
     * accessible via lookup on Module bodies
     * (e.g. `module Kernel; alias_method :a, :method_on_object; end`). */
    if (!m && BUILTIN_TYPE(self) == T_MODULE && korb_vm->object_class) {
        m = korb_class_find_method(korb_vm->object_class, old_name);
    }
    if (!m) {
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "undefined method '%s' for %s",
                   korb_id_name(old_name),
                   klass->name ? korb_id_name(klass->name) : "?");
        return Qnil;
    }
    korb_class_alias_method(klass, new_name, m);
    return korb_id2sym(new_name);
}

static void module_set_visibility_for_args(CTX *c, VALUE self, int argc, VALUE *argv,
                                            enum korb_visibility v)
{
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return;
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
            if (!m_inherited && BUILTIN_TYPE(self) == T_MODULE && korb_vm->object_class) {
                m_inherited = korb_class_find_method(korb_vm->object_class, name);
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
                VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
                const char *cn = (k->name != 0) ? korb_id_name(k->name) : "(anon)";
                korb_raise(c, (struct korb_class *)eN,
                           "undefined method '%s' for %s '%s'",
                           korb_id_name(name),
                           BUILTIN_TYPE(self) == T_MODULE ? "module" : "class",
                           cn);
                return;
            }
        }
    }
}
/* Top-level default visibility — initially PRIVATE (CRuby behavior:
 * top-level defs are private on Object).  Toggled by public/private
 * called at top-level. */
bool g_top_level_default_private = true;

static VALUE module_set_visibility(CTX *c, VALUE self, int argc, VALUE *argv,
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
        if (self == korb_vm->main_obj) {
            g_top_level_default_private = (v == KORB_VIS_PRIVATE);
        }
        return Qnil;
    }
    /* Single Array form: `private([:foo, :bar])` (Ruby 3.x). */
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        module_set_visibility_for_args(c, self, (int)a->len, a->ptr, v);
        if (c->state == KORB_RAISE) return Qnil;
        return argv[0];
    }
    module_set_visibility_for_args(c, self, argc, argv, v);
    if (c->state == KORB_RAISE) return Qnil;
    /* Ruby 3.0+: public/private/protected with args returns the symbol
     * (single arg) or array of symbols (multiple args).  String args
     * are converted to symbols for the return value. */
    if (argc == 1) {
        if (SYMBOL_P(argv[0])) return argv[0];
        if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
            return korb_id2sym(korb_intern_n(((struct korb_string *)argv[0])->ptr,
                                              ((struct korb_string *)argv[0])->len));
        }
        return argv[0];
    }
    VALUE r = korb_ary_new_capa(c, c->sp, argc);
    for (int i = 0; i < argc; i++) {
        if (SYMBOL_P(argv[i])) korb_ary_push(r, argv[i]);
        else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING) {
            korb_ary_push(r, korb_id2sym(korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                                       ((struct korb_string *)argv[i])->len)));
        } else {
            korb_ary_push(r, argv[i]);
        }
    }
    return r;
}
static VALUE module_private(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_set_visibility(c, self, argc, argv, KORB_VIS_PRIVATE);
}
static VALUE module_public(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_set_visibility(c, self, argc, argv, KORB_VIS_PUBLIC);
}
static VALUE module_protected(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_set_visibility(c, self, argc, argv, KORB_VIS_PROTECTED);
}
static VALUE module_const_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE))
        return Qfalse;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    else return Qfalse;
    /* Walks includes/super by default; the optional second arg `inherit`
     * (default true) controls whether to search ancestors.  When false,
     * only the receiver's own constant table is consulted. */
    bool inherit = true;
    if (argc >= 2) inherit = RTEST(argv[1]);
    extern bool korb_const_has_inherited(struct korb_class *klass, ID name);
    return KORB_BOOL(inherit
        ? korb_const_has_inherited((struct korb_class *)self, name)
        : korb_const_has((struct korb_class *)self, name));
}
static VALUE module_module_function(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }

/* Struct.new(:a, :b) → returns a new Class with attr_accessor for each */
static VALUE struct_initialize(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* attr_accessor for each member */
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return Qnil;
    struct korb_array *members = (struct korb_array *)members_v;
    long n = members->len;
    for (long i = 0; i < n && i < argc; i++) {
        ID name = SYMBOL_P(members->ptr[i]) ? korb_sym2id(members->ptr[i]) :
                  korb_intern(korb_str_cstr(members->ptr[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ivar_set(self, iv_id, argv[i]);
    }
    return self;
}

static VALUE struct_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return korb_ary_new(c, c->sp);
    struct korb_array *members = (struct korb_array *)members_v;
    VALUE r = korb_ary_new_capa(c, c->sp, members->len);
    for (long i = 0; i < members->len; i++) {
        ID name = SYMBOL_P(members->ptr[i]) ? korb_sym2id(members->ptr[i]) :
                  korb_intern(korb_str_cstr(members->ptr[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ary_push(r, korb_ivar_get(self, iv_id));
    }
    return r;
}

/* Struct#[] — read by index or symbol. */
static VALUE struct_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return Qnil;
    struct korb_array *members = (struct korb_array *)members_v;
    long idx = -1;
    if (FIXNUM_P(argv[0])) {
        idx = FIX2LONG(argv[0]);
        if (idx < 0) idx += members->len;
    } else if (SYMBOL_P(argv[0])) {
        ID want = korb_sym2id(argv[0]);
        for (long i = 0; i < members->len; i++) {
            if (SYMBOL_P(members->ptr[i]) && korb_sym2id(members->ptr[i]) == want) {
                idx = i; break;
            }
        }
    }
    if (idx < 0 || idx >= members->len) return Qnil;
    ID name = SYMBOL_P(members->ptr[idx]) ? korb_sym2id(members->ptr[idx]) :
              korb_intern(korb_str_cstr(members->ptr[idx]));
    const char *base = korb_id_name(name);
    long bl = strlen(base);
    char *iv = korb_xmalloc_atomic(bl + 2);
    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
    return korb_ivar_get(self, korb_intern(iv));
}

/* Struct#[]= */
static VALUE struct_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qnil;
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return Qnil;
    struct korb_array *members = (struct korb_array *)members_v;
    long idx = -1;
    if (FIXNUM_P(argv[0])) {
        idx = FIX2LONG(argv[0]);
        if (idx < 0) idx += members->len;
    } else if (SYMBOL_P(argv[0])) {
        ID want = korb_sym2id(argv[0]);
        for (long i = 0; i < members->len; i++) {
            if (SYMBOL_P(members->ptr[i]) && korb_sym2id(members->ptr[i]) == want) {
                idx = i; break;
            }
        }
    }
    if (idx < 0 || idx >= members->len) return Qnil;
    ID name = SYMBOL_P(members->ptr[idx]) ? korb_sym2id(members->ptr[idx]) :
              korb_intern(korb_str_cstr(members->ptr[idx]));
    const char *base = korb_id_name(name);
    long bl = strlen(base);
    char *iv = korb_xmalloc_atomic(bl + 2);
    iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
    korb_ivar_set(self, korb_intern(iv), argv[1]);
    return argv[1];
}

/* Struct#each — yield each value. */
static VALUE struct_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = struct_to_a(c, self, 0, NULL);
    struct korb_array *a = (struct korb_array *)arr;
    for (long i = 0; i < a->len; i++) {
        korb_yield(c, 1, &a->ptr[i]);
        if (c->state == KORB_RAISE) return Qnil;
    }
    return self;
}

/* Struct#== — same struct class + equal members. */
static VALUE struct_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(argv[0])) return Qfalse;
    if (((struct RBasic *)self)->klass != ((struct RBasic *)argv[0])->klass) return Qfalse;
    VALUE a = struct_to_a(c, self, 0, NULL);
    VALUE b = struct_to_a(c, argv[0], 0, NULL);
    return korb_funcall(c, a, korb_intern("=="), 1, &b);
}

/* Struct#to_h */
static VALUE struct_to_h(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return korb_hash_new(c, c->sp);
    struct korb_array *members = (struct korb_array *)members_v;
    VALUE h = korb_hash_new(c, c->sp);
    for (long i = 0; i < members->len; i++) {
        ID name = SYMBOL_P(members->ptr[i]) ? korb_sym2id(members->ptr[i]) :
                  korb_intern(korb_str_cstr(members->ptr[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        korb_hash_aset(h, members->ptr[i], korb_ivar_get(self, korb_intern(iv)));
    }
    return h;
}

/* Struct#size / length */
static VALUE struct_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return INT2FIX(0);
    return INT2FIX(((struct korb_array *)members_v)->len);
}

/* Struct.members at the class level — return the members array. */
static VALUE struct_class_members(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) return korb_ary_new(c, c->sp);
    VALUE members_v = korb_const_get((struct korb_class *)self, korb_intern("__members__"));
    if (UNDEF_P(members_v)) return korb_ary_new(c, c->sp);
    return members_v;
}

static VALUE struct_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Struct.new(:a, :b, keyword_init: true) — strip trailing options
     * Hash before treating remaining args as member names.  We don't
     * implement keyword_init semantics differently from positional
     * (the generated initializer already handles both), but at least
     * the loader-time `Struct.new(:foo, keyword_init: true)` pattern
     * (test_marshal:776 etc.) needs to not blow up on the Hash arg. */
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
        argc--;
    }
    struct korb_class *klass = korb_class_new(c, c->sp, korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    /* save members */
    VALUE members = korb_ary_new_from_values(c, c->sp, argc, argv);
    korb_const_set(klass, korb_intern("__members__"), members);
    /* Install Struct's standard instance methods FIRST, then let
     * attr_accessor overwrite any collisions (e.g. Data.define(:length)
     * means user-given `length` accessor wins over Struct#length). */
    korb_class_add_method_cfunc(klass, korb_intern("initialize"), struct_initialize, -1);
    korb_class_add_method_cfunc(klass, korb_intern("to_a"),       struct_to_a,        0);
    korb_class_add_method_cfunc(klass, korb_intern("values"),     struct_to_a,        0);
    korb_class_add_method_cfunc(klass, korb_intern("members"),    struct_to_a,        0);
    korb_class_add_method_cfunc(klass, korb_intern("[]"),         struct_aref,        1);
    korb_class_add_method_cfunc(klass, korb_intern("[]="),        struct_aset,       -1);
    korb_class_add_method_cfunc(klass, korb_intern("each"),       struct_each,        0);
    korb_class_add_method_cfunc(klass, korb_intern("==" ),        struct_eq,          1);
    korb_class_add_method_cfunc(klass, korb_intern("to_h"),       struct_to_h,        0);
    korb_class_add_method_cfunc(klass, korb_intern("size"),       struct_size,        0);
    korb_class_add_method_cfunc(klass, korb_intern("length"),     struct_size,        0);
    /* Now attr_accessor — overrides Struct#length etc. when a member
     * shadows a standard name. */
    module_attr_accessor(c, (VALUE)klass, argc, argv);
    /* class-level .members */
    {
        struct korb_class *meta = korb_singleton_class_of(klass);
        korb_class_add_method_cfunc(meta, korb_intern("members"),
                                     struct_class_members, 0);
    }
    /* If a block was given, evaluate it with self = the new class
     * (Struct.new(:x) { def hello; ... end } pattern).  Crucially,
     * also temporarily swap the block's captured cref so `def` inside
     * the block targets the new Struct, NOT the lexical container.
     * Without this, `class TM; X = Struct.new(:y) { def foo; end };
     * end` would leak `foo` onto TM (and worse — `def method_missing`
     * inside the block would replace TM's method_missing, breaking
     * every method on TM). */
    
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
        korb_yield(c, 1, av0);
        c->current_block->self = prev_blk_self;
        c->current_block->cref = prev_blk_cref;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        if (c->state == KORB_BREAK) { c->state = KORB_NORMAL; c->state_value = Qnil; }
    }
    return (VALUE)klass;
}

static VALUE module_const_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 1) return Qnil;
    ID name;
    VALUE arg = argv[0];
    if (SYMBOL_P(arg)) name = korb_sym2id(arg);
    else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING) {
        name = korb_intern_n(((struct korb_string *)arg)->ptr,
                             ((struct korb_string *)arg)->len);
    } else if (!SPECIAL_CONST_P(arg)) {
        /* Coerce via #to_str (CRuby semantics: const_get accepts a
         * String-convertible name). */
        VALUE rt = korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return Qnil;
        if (RTEST(rt)) {
            VALUE r = korb_funcall(c, arg, korb_intern("to_str"), 0, NULL);
            if (c->state == KORB_RAISE) return Qnil;
            if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_STRING) {
                name = korb_intern_n(((struct korb_string *)r)->ptr,
                                     ((struct korb_string *)r)->len);
                goto have_name;
            }
        }
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into String",
                   korb_id_name(korb_class_of_class(arg)->name));
        return Qnil;
    } else {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of (special) into String");
        return Qnil;
    }
have_name:;
    bool inherit = true;
    if (argc >= 2) inherit = RTEST(argv[1]);
    extern VALUE korb_const_get_inherited(struct korb_class *klass, ID name);
    VALUE v = inherit
        ? korb_const_get_inherited((struct korb_class *)self, name)
        : korb_const_get((struct korb_class *)self, name);
    if (UNDEF_P(v)) {
        VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eName,
                   "uninitialized constant %s::%s",
                   korb_id_name(((struct korb_class *)self)->name),
                   korb_id_name(name));
        return Qnil;
    }
    return v;
}

static VALUE module_const_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 2) return Qnil;
    /* Frozen check before any side effects (CRuby semantics). */
    if (korb_obj_frozen_p(self)) {
        VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
        korb_raise(c, (struct korb_class *)eF, "can't modify frozen %s",
                   korb_id_name(korb_class_of_class(self)->name));
        return Qnil;
    }
    /* Coerce non-Symbol/String name via #to_str (CRuby semantics). */
    VALUE name_arg = argv[0];
    if (!SYMBOL_P(name_arg) &&
        (SPECIAL_CONST_P(name_arg) || BUILTIN_TYPE(name_arg) != T_STRING)) {
        if (!SPECIAL_CONST_P(name_arg)) {
            VALUE rt = korb_funcall(c, name_arg, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                name_arg = korb_funcall(c, name_arg, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
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
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        VALUE inspv = korb_funcall(c, argv[0], korb_intern("inspect"), 0, NULL);
        const char *insp = (!SPECIAL_CONST_P(inspv) && BUILTIN_TYPE(inspv) == T_STRING)
                              ? ((struct korb_string *)inspv)->ptr : "?";
        korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string", insp);
        return Qnil;
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
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "wrong constant name %.*s", namelen, namep);
        return Qnil;
    }
    ID name = korb_intern_n(namep, namelen);
    korb_const_set((struct korb_class *)self, name, argv[1]);
    return argv[1];
}

/* (string ext folded into builtins/string.c) */
/* (array ext folded into builtins/array.c) */
/* (hash ext folded into builtins/hash.c) */
