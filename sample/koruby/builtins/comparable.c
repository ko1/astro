/* Comparable — moved from builtins.c. */

/* ---------- Comparable ----------
 *
 * All cfuncs invoke `self.<=>(other)` and interpret the result.  If
 * <=> returns nil (incomparable), comparison ops raise ArgumentError. */

static long korb_cmp_call(CTX *c, VALUE self, VALUE other) {
    VALUE r = korb_funcall(c, self, korb_intern("<=>"), 1, &other);
    if (NIL_P(r)) {
        korb_raise(c, NULL, "comparison of %s with %s failed",
                   korb_id_name(korb_class_of_class(self)->name),
                   korb_id_name(korb_class_of_class(other)->name));
        return 0;
    }
    if (FIXNUM_P(r)) return FIX2LONG(r);
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
    /* Comparable#== uses <=> too — returns true iff <=> returns 0. */
    VALUE r = korb_funcall(c, self, korb_intern("<=>"), 1, argv);
    if (NIL_P(r)) return Qfalse;
    return KORB_BOOL(FIXNUM_P(r) && FIX2LONG(r) == 0);
}
static VALUE cmp_between(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise(c, NULL, "wrong number of arguments to between? (%d for 2)", argc);
        return Qnil;
    }
    long lo = korb_cmp_call(c, self, argv[0]);
    long hi = korb_cmp_call(c, self, argv[1]);
    return KORB_BOOL(lo >= 0 && hi <= 0);
}
static VALUE cmp_clamp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise(c, NULL, "wrong number of arguments to clamp (%d for 2)", argc);
        return Qnil;
    }
    if (korb_cmp_call(c, self, argv[0]) < 0) return argv[0];
    if (korb_cmp_call(c, self, argv[1]) > 0) return argv[1];
    return self;
}

/* alias_method(:new_name, :existing_name) — register the existing
 * method under a new name on this class.  Reuses the resolved method
 * struct (methods are immutable in koruby). */
static VALUE module_alias_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise(c, NULL, "wrong number of arguments to alias_method (%d for 2)", argc);
        return Qnil;
    }
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    ID new_name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern(korb_str_cstr(argv[0]));
    ID old_name = SYMBOL_P(argv[1]) ? korb_sym2id(argv[1]) : korb_intern(korb_str_cstr(argv[1]));
    struct korb_method *m = korb_class_find_method(klass, old_name);
    if (!m) {
        korb_raise(c, NULL, "undefined method '%s' for %s", korb_id_name(old_name), korb_id_name(klass->name));
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
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (!SPECIAL_CONST_P(argv[i]) && BUILTIN_TYPE(argv[i]) == T_STRING)
            name = korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                 ((struct korb_string *)argv[i])->len);
        else continue;
        struct korb_method *m = korb_class_find_method(k, name);
        if (m) m->visibility = v;
    }
    /* No-arg `private` / `public` / `protected` would set the default
     * visibility for subsequent defs.  We don't track that yet; tests
     * pass with explicit `private :name` form. */
}
static VALUE module_set_visibility(CTX *c, VALUE self, int argc, VALUE *argv,
                                   enum korb_visibility v)
{
    if (argc == 0) {
        /* No-arg form: change the default visibility for subsequent
         * `def`s in this class body.  `protected` / `private` /
         * `public` toggle. */
        if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
            ((struct korb_class *)self)->default_visibility = v;
        }
    } else {
        module_set_visibility_for_args(c, self, argc, argv, v);
    }
    return self;
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
    return KORB_BOOL(korb_const_has((struct korb_class *)self, name));
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
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return korb_ary_new();
    struct korb_array *members = (struct korb_array *)members_v;
    VALUE r = korb_ary_new_capa(members->len);
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

static VALUE struct_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Struct.new(:a, :b) — create new class */
    struct korb_class *klass = korb_class_new(korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    /* save members */
    VALUE members = korb_ary_new_from_values(argc, argv);
    korb_const_set(klass, korb_intern("__members__"), members);
    /* attr_accessor for each member */
    module_attr_accessor(c, (VALUE)klass, argc, argv);
    /* initialize */
    korb_class_add_method_cfunc(klass, korb_intern("initialize"), struct_initialize, -1);
    korb_class_add_method_cfunc(klass, korb_intern("to_a"), struct_to_a, 0);
    korb_class_add_method_cfunc(klass, korb_intern("members"), struct_to_a, 0);
    return (VALUE)klass;
}

static VALUE module_const_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    VALUE v = korb_const_get((struct korb_class *)self, name);
    if (UNDEF_P(v)) return Qnil;
    return v;
}

static VALUE module_const_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 2) return Qnil;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    korb_const_set((struct korb_class *)self, name, argv[1]);
    return argv[1];
}

/* (string ext folded into builtins/string.c) */
/* (array ext folded into builtins/array.c) */
/* (hash ext folded into builtins/hash.c) */
