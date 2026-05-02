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
    /* Two forms:
     *   clamp(min, max)  — both bounds explicit
     *   clamp(range)     — bounds taken from range.begin / range.end
     *                      (either end may be nil for half-bounded ranges) */
    VALUE lo, hi;
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)argv[0];
        lo = r->begin;
        hi = r->end;
        /* Exclusive-end Integer ranges: drop the upper bound by 1 so
         * `0.clamp(1...5)` behaves like `0.clamp(1, 4)`. */
        if (r->exclude_end && !NIL_P(hi) && FIXNUM_P(hi))
            hi = INT2FIX(FIX2LONG(hi) - 1);
    } else if (argc == 2) {
        lo = argv[0];
        hi = argv[1];
    } else {
        korb_raise(c, NULL, "wrong number of arguments to clamp (%d for 1..2)", argc);
        return Qnil;
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
static VALUE module_undef_or_remove_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return self;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name = SYMBOL_P(argv[i]) ? korb_sym2id(argv[i])
                                     : korb_intern(korb_str_cstr(argv[i]));
        korb_method_table_remove(&klass->methods, name);
    }
    /* Bump method serial so cached method-lookup entries invalidate. */
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
    return self;
}

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
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return korb_hash_new();
    struct korb_array *members = (struct korb_array *)members_v;
    VALUE h = korb_hash_new();
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
    if (BUILTIN_TYPE(self) != T_CLASS) return korb_ary_new();
    VALUE members_v = korb_const_get((struct korb_class *)self, korb_intern("__members__"));
    if (UNDEF_P(members_v)) return korb_ary_new();
    return members_v;
}

static VALUE struct_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Struct.new(:a, :b) — create new class */
    struct korb_class *klass = korb_class_new(korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    /* save members */
    VALUE members = korb_ary_new_from_values(argc, argv);
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
     * (Struct.new(:x) { def hello; ... end } pattern). */
    extern struct korb_proc *current_block;
    if (current_block) {
        VALUE prev_self = c->self;
        struct korb_class *prev_class = c->current_class;
        struct korb_cref *prev_cref = c->cref;
        struct korb_cref new_cref = { .klass = klass, .prev = c->cref };
        VALUE prev_blk_self = current_block->self;
        c->self = (VALUE)klass;
        c->current_class = klass;
        c->cref = &new_cref;
        current_block->self = (VALUE)klass;
        VALUE av0[1] = { (VALUE)klass };
        korb_yield(c, 1, av0);
        current_block->self = prev_blk_self;
        c->self = prev_self;
        c->current_class = prev_class;
        c->cref = prev_cref;
        if (c->state == KORB_BREAK) { c->state = KORB_NORMAL; c->state_value = Qnil; }
    }
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
