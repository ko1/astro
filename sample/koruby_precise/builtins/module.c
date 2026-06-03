/* Module / Class metaprogramming — moved from builtins.c. */

/* ---------- Module / Class metaprogramming ---------- */

static RESULT ivar_getter_dispatch(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* the getter method's "name" tells us the @ivar; we encode the ivar name
     * as the method's name without the leading @, so name "x" → @x. */
    /* Actually simpler: we install the cfunc with a side-channel.  In our
     * scheme cfuncs receive the same args; we need to pass the ivar name.
     * Instead, the getter's cfunc captures the ID at definition time via
     * a closure-style structure. */
    (void)argc; (void)argv;
    return RESULT_OK(Qnil); /* never called directly */
}

/* attr_reader / attr_writer / attr_accessor implementation:
 * We install AST methods whose body is node_ivar_get / node_ivar_set.
 */

/* Resolve an attr_*'s name argument: accept Symbol/String, fall back
 * to #to_str.  On NORMAL writes *out_id; on raise returns the RESULT. */
static RESULT attr_resolve_name(CTX *c, VALUE arg, ID *out_id, const char *meth) {
    VALUE v = arg;
    if (!SYMBOL_P(v) && (SPECIAL_CONST_P(v) || BUILTIN_TYPE(v) != T_STRING)) {
        if (!SPECIAL_CONST_P(v)) {
            VALUE rt = UNWRAP(korb_funcall(c, v, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) }));
            if (RTEST(rt)) {
                v = UNWRAP(korb_funcall(c, v, korb_intern("to_str"), 0, NULL));
            }
        }
    }
    ID name;
    if (SYMBOL_P(v)) name = korb_sym2id(v);
    else if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_STRING) {
        name = korb_intern_n(((struct korb_string *)v)->ptr,
                             ((struct korb_string *)v)->len);
    } else {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string",
                   SPECIAL_CONST_P(arg) ? "(special)"
                       : korb_id_name(korb_class_of_class(arg)->name));
    }
    const char *base = korb_id_name(name);
    if (!base || (!((base[0] >= 'a' && base[0] <= 'z') ||
                    (base[0] >= 'A' && base[0] <= 'Z') || base[0] == '_'))) {
        VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        return korb_raise(c, (struct korb_class *)eN,
                   "invalid attribute name '%s'", base ? base : "");
    }
    *out_id = name;
    (void)meth;
    return RESULT_OK(Qnil);
}

/* Resolve a method-name argument (method_defined? / *_method_defined? etc.):
 * accept Symbol/String, fall back to #to_str.  Raise TypeError for a
 * non-convertible arg or a #to_str that returns a non-String.  Unlike
 * attr_resolve_name this does NOT reject operator / [] names.  Writes *out_id
 * on success; returns the raise RESULT otherwise.  (The old inline pattern
 * blindly cast a non-Symbol arg to korb_string and crashed in korb_intern_n
 * on a fixnum/special — method_defined_spec's TypeError cases.) */
static RESULT name_arg_to_id(CTX *c, VALUE arg, ID *out_id) {
    VALUE v = arg;
    if (!SYMBOL_P(v) && (SPECIAL_CONST_P(v) || BUILTIN_TYPE(v) != T_STRING)) {
        if (!SPECIAL_CONST_P(v)) {
            VALUE rt = UNWRAP(korb_funcall(c, v, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) }));
            if (RTEST(rt)) v = UNWRAP(korb_funcall(c, v, korb_intern("to_str"), 0, NULL));
        }
    }
    if (SYMBOL_P(v)) { *out_id = korb_sym2id(v); return RESULT_OK(Qnil); }
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_STRING) {
        *out_id = korb_intern_n(((struct korb_string *)v)->ptr,
                                ((struct korb_string *)v)->len);
        return RESULT_OK(Qnil);
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eT, "%s is not a symbol nor a string",
               SPECIAL_CONST_P(arg) ? "(special)"
                   : korb_id_name(korb_class_of_class(arg)->name));
}

/* Frozen check: receiver class/module must not be frozen. */
static RESULT attr_check_frozen(CTX *c, VALUE self) {
    if (korb_obj_frozen_p(self)) {
        VALUE eF = korb_const_get(KORB_VM(c)->object_class, korb_intern("FrozenError"));
        struct korb_class *k = (struct korb_class *)self;
        const char *cn = (k->name != 0) ? korb_id_name(k->name) : "(anon)";
        return korb_raise(c, (struct korb_class *)eF,
                   "can't modify frozen %s: %s",
                   BUILTIN_TYPE(self) == T_MODULE ? "Module" : "Class", cn);
    }
    return RESULT_OK(Qnil);
}
static RESULT module_attr_reader(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return korb_raise(c, NULL, "attr_reader: not on a class/module");
    }
    CHECK(attr_check_frozen(c, self));
    /* Park self (the class) at sp[0]; the result array goes at sp[1].  Both
     * the array allocs and korb_class_add_method_ast fire GC now (arrays are
     * arena objects), so the class handle must be re-derived from sp[0] after
     * every GC point rather than held as a stale C-local. */
    sp[0] = self;
    sp[1] = korb_ary_new(c, sp + 2);
    for (int i = 0; i < argc; i++) {
        ID name;
        CHECK(attr_resolve_name(c, argv[i], &name, "attr_reader"));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        NODE *body = ALLOC_node_ivar_get(iv_id);
        korb_class_add_method_ast(c, (struct korb_class *)sp[0], name, body, 0, 0);
        korb_ary_push(c, sp + 2, sp[1], korb_id2sym(name));
    }
    return RESULT_OK(sp[1]);
}

static RESULT module_attr_writer(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return korb_raise(c, NULL, "attr_writer: not on a class/module");
    }
    CHECK(attr_check_frozen(c, self));
    /* Park self (class) at sp[0], result array at sp[1].  Re-derive the class
     * from sp[0] after each GC point (array allocs + add_method_ast). */
    sp[0] = self;
    sp[1] = korb_ary_new(c, sp + 2);
    for (int i = 0; i < argc; i++) {
        ID name;
        CHECK(attr_resolve_name(c, argv[i], &name, "attr_writer"));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *sn = korb_xmalloc_atomic(bl + 2);
        memcpy(sn, base, bl); sn[bl] = '='; sn[bl + 1] = 0;
        ID setter_id = korb_intern(sn);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        NODE *body = ALLOC_node_ivar_set(iv_id, ALLOC_node_lvar_get(0));
        korb_class_add_method_ast(c, (struct korb_class *)sp[0], setter_id, body, 1, 1);
        korb_ary_push(c, sp + 2, sp[1], korb_id2sym(setter_id));
    }
    return RESULT_OK(sp[1]);
}

static RESULT module_attr_accessor(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park the reader's result array (sp[0]) across the writer call,
     * which allocates and runs funcalls and can move the handle.  The
     * writer is invoked with its self/argv re-staged above sp[0] so it
     * doesn't clobber the parked reader result. */
    sp[0] = UNWRAP(module_attr_reader(c, argc, sp));   /* reader staged below sp[0]; park r1 in sp[0] */
    /* Re-derive self + argv from the GC-tracked arg slots: the reader call
     * above fired GC and may have moved the class and the arg values; the
     * C-locals `self`/`argv` would be stale. */
    sp[1] = sp[-argc - 1];                              /* writer self (re-read) */
    for (int i = 0; i < argc; i++) sp[2 + i] = (sp - argc)[i]; /* writer argv (re-read) */
    sp[1] = UNWRAP(module_attr_writer(c, argc, sp + argc + 2));  /* park r2 in sp[1] */
    /* Interleave readers and writers like CRuby: [a, a=, b, b=]. */
    sp[2] = korb_ary_new(c, sp + 3);                    /* park result in sp[2] */
    if (BUILTIN_TYPE(sp[0]) == T_ARRAY && BUILTIN_TYPE(sp[1]) == T_ARRAY) {
        long n = ((struct korb_array *)sp[0])->len;
        if (((struct korb_array *)sp[1])->len < n) n = ((struct korb_array *)sp[1])->len;
        for (long i = 0; i < n; i++) {
            korb_ary_push(c, sp + 3, sp[2], korb_ary_items((struct korb_array *)sp[0])[i]);
            korb_ary_push(c, sp + 3, sp[2], korb_ary_items((struct korb_array *)sp[1])[i]);
        }
    }
    return RESULT_OK(sp[2]);
}

static RESULT module_include(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    
    /* Top-level `include M` forwards to Object — that's how a file's
     * `include ConstantSpecs::ModuleA` (no enclosing class/module)
     * makes M's constants reachable as toplevel constants. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_OBJECT &&
            self == KORB_VM(c)->main_obj) {
            self = (VALUE)KORB_VM(c)->object_class;
        } else {
            return RESULT_OK(self);
        }
    }
    /* Simplified include: copy module's methods/constants into the class.
     * Real Ruby inserts the module into the ancestor chain; we flatten. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    struct korb_class *klass = (struct korb_class *)self;
    
    for (int i = 0; i < argc; i++) {
        
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        korb_module_include(klass, (struct korb_class *)argv[i]);
        /* Fire the module's `included` hook (defined as a class
         * method on the module).  The hook receives the including
         * class.  Skip silently if the module doesn't define it. */
        struct korb_class *meta = korb_class_of_class(argv[i]);
        if (meta && korb_class_find_method(meta, korb_intern("included"))) {
            VALUE klass_v = self;
            CHECK(korb_funcall(c, argv[i], korb_intern("included"), 1, &klass_v));
        }
    }
    if (KORB_VM(c)) { KORB_VM(c)->method_serial++; korb_g_method_serial = KORB_VM(c)->method_serial; }
    return RESULT_OK(self);
}

extern void korb_class_add_method_proc(struct korb_class *klass, ID name, struct korb_proc *p);

static RESULT module_define_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* define_method(:name) { |args| body } — register the block as a
     * proc-method.  Dispatch (prologue_proc_method) calls the proc via
     * proc_call so its captured env is preserved (closure semantics). */
    if (argc < 1) return RESULT_OK(Qnil);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    else return RESULT_OK(Qnil);
    
    struct korb_proc *p;
    /* UnboundMethod / Method whose receiver is a class — install the
     * underlying method directly into self by alias.  This preserves
     * the original method's body / arity / locals_cnt instead of
     * routing through a Proc shim that would re-dispatch through the
     * class itself (wrong receiver). */
    if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) &&
        BUILTIN_TYPE(argv[1]) == T_DATA &&
        ((struct RBasic *)argv[1])->klass == (VALUE)KORB_VM(c)->method_class) {
        struct korb_method_obj *m = (struct korb_method_obj *)argv[1];
        if (!SPECIAL_CONST_P(m->receiver) &&
            (BUILTIN_TYPE(m->receiver) == T_CLASS || BUILTIN_TYPE(m->receiver) == T_MODULE)) {
            struct korb_method *km = korb_class_find_method(
                (struct korb_class *)m->receiver, m->name);
            if (km) {
                /* Clone km with defining_class = target so `super` from
                 * inside the body walks past `self` (the install target)
                 * rather than past the source module. */
                struct korb_method *clone = korb_xmalloc(sizeof(*clone));
                *clone = *km;
                clone->name = name;
                clone->defining_class = (struct korb_class *)self;
                korb_class_alias_method((struct korb_class *)self, name, clone);
                return RESULT_OK(korb_id2sym(name));
            }
        }
        /* Bound Method (receiver is an instance): fall back to the
         * Proc-shim path so `m.receiver.send(m.name, *args)` runs. */
        VALUE pr = UNWRAP(korb_funcall(c, argv[1], korb_intern("to_proc"), 0, NULL));
        if (BUILTIN_TYPE(pr) != T_PROC) return RESULT_OK(Qnil);
        p = (struct korb_proc *)pr;
    } else if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) && BUILTIN_TYPE(argv[1]) == T_PROC) {
        p = (struct korb_proc *)argv[1];
    } else if (c->current_block) {
        p = c->current_block;
    } else {
        return RESULT_OK(Qnil);
    }
    korb_class_add_method_proc((struct korb_class *)self, name, p);
    return RESULT_OK(korb_id2sym(name));
}


/* Object#define_singleton_method — same as define_method but installs
 * on the receiver's singleton class instead of `self`'s class. */
static RESULT obj_define_singleton_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    extern struct korb_class *korb_singleton_class_of_value(CTX *c, VALUE *sp, VALUE v);
    struct korb_class *meta = korb_singleton_class_of_value(c, sp, self);
    if (!meta) return RESULT_OK(Qnil);
    /* Reuse module_define_method with self overridden to the meta class. */
    sp[-argc - 1] = (VALUE)meta;
    RESULT _r = module_define_method(c, argc, sp);
    sp[-argc - 1] = self;
    return _r;
}

/* Class#superclass */
static RESULT class_superclass(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS) return RESULT_OK(Qnil);
    struct korb_class *k = (struct korb_class *)self;
    /* Uninitialized class (`Class.allocate`) has no super yet — CRuby
     * raises TypeError on #superclass.  Detect via our sentinel name. */
    if (k->super == NULL && k->name == korb_intern("(uninitialized)")) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "uninitialized class");
    }
    return RESULT_OK(k->super ? (VALUE)k->super : Qnil);
}

/* Module#instance_methods([include_inherited=true]) — sym list. */
static RESULT module_instance_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return RESULT_OK(korb_ary_new(c, c->sp_top));
    }
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *root = (struct korb_class *)self;
    VALUE r = korb_ary_new(c, c->sp_top);
    /* Walk from root through includes / super if requested. */
    struct korb_class *k = root;
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                korb_ary_push(c, c->sp_top, r, korb_id2sym(e->name));
            }
        }
        if (!include_inherited) break;
        k = k->super;
    }
    return RESULT_OK(r);
}

/* Object#methods([include_inherited=true]) — list public + protected
 * methods accessible on the receiver, walking the class chain. */
/* Helper: collect methods of `vis` visibility from the receiver's class
 * chain.  vis = -1 means "all public + protected" (default for #methods).
 * vis = KORB_VIS_PUBLIC / PRIVATE / PROTECTED selects exactly that set. */
static VALUE methods_with_visibility(CTX *c, VALUE self, int vis, bool include_inherited) {
    struct korb_class *k = korb_class_of_class(self);
    VALUE r = korb_ary_new(c, c->sp_top);
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                if (vis < 0) {
                    if (e->method->visibility == KORB_VIS_PRIVATE) continue;
                } else {
                    if ((int)e->method->visibility != vis) continue;
                }
                bool dup = false;
                for (long j = 0; j < ((struct korb_array *)r)->len; j++) {
                    VALUE existing = korb_ary_items((struct korb_array *)r)[j];
                    if (SYMBOL_P(existing) && korb_sym2id(existing) == e->name) {
                        dup = true; break;
                    }
                }
                if (!dup) korb_ary_push(c, c->sp_top, r, korb_id2sym(e->name));
            }
        }
        if (!include_inherited) break;
        k = k->super;
    }
    return r;
}
static RESULT obj_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return RESULT_OK(methods_with_visibility(c, self, -1, include_inherited));
}
static RESULT obj_public_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return RESULT_OK(methods_with_visibility(c, self, KORB_VIS_PUBLIC, include_inherited));
}
static RESULT obj_private_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return RESULT_OK(methods_with_visibility(c, self, KORB_VIS_PRIVATE, include_inherited));
}
static RESULT obj_protected_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return RESULT_OK(methods_with_visibility(c, self, KORB_VIS_PROTECTED, include_inherited));
}

/* Object#singleton_methods — methods defined directly on this object's
 * singleton class (not inherited from regular class). */
static RESULT obj_singleton_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Park self (msp[0]), result (msp[1]) and the target class (msp[2])
     * across korb_ary_new / korb_singleton_class_of / per-method push GC —
     * self and classes are arena (moving).  Method-table buckets are libc
     * (non-moving) so entry pointers stay valid; re-read the class from
     * msp[2] each use. */
    VALUE *const msp = c->sp_top;
    msp[0] = self;                       /* park self BEFORE korb_ary_new */
    msp[1] = korb_ary_new(c, msp + 2);   /* result */
    msp[2] = 0;
    if (SPECIAL_CONST_P(msp[0])) return RESULT_OK(msp[1]);
    if (BUILTIN_TYPE(msp[0]) == T_CLASS || BUILTIN_TYPE(msp[0]) == T_MODULE) {
        /* For a class, singleton_methods returns the metaclass methods. */
        msp[2] = (VALUE)korb_singleton_class_of(c, c->sp_top, (struct korb_class *)msp[0]);
    } else if (BUILTIN_TYPE(msp[0]) == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)msp[0];
        struct korb_class *cur = (struct korb_class *)o->basic.klass;
        if (cur && cur->name == korb_intern("(singleton)")) msp[2] = (VALUE)cur;
    }
    if (!msp[2]) return RESULT_OK(msp[1]);
    uint32_t bcnt = ((struct korb_class *)msp[2])->methods.bucket_cnt;
    for (uint32_t b = 0; b < bcnt; b++) {
        struct korb_method_table_entry *e =
            ((struct korb_class *)msp[2])->methods.buckets[b];
        for (; e; e = e->next) {
            if (e->include_depth == 0) {
                VALUE sym = korb_id2sym(e->name);
                korb_ary_push(c, msp + 3, msp[1], sym);
            }
        }
    }
    return RESULT_OK(msp[1]);
}

/* Module#method_defined?(name [, inherit=true]) — true for public/
 * protected.  When inherit is false, only the receiver's own method
 * table (and its prepends/includes) is consulted, not super classes. */
static RESULT module_method_defined_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
    ID name; CHECK(name_arg_to_id(c, argv[0], &name));
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = NULL;
    if (inherit) {
        m = korb_class_find_method((struct korb_class *)self, name);
    } else {
        /* Without inherit, look only at methods defined directly on
         * self.  Module-included methods have include_depth > 0 and
         * are excluded.  Methods inherited via super aren't in the
         * table at all (we look up only this class's bucket). */
        struct korb_class *k = (struct korb_class *)self;
        for (uint32_t b = 0; !m && b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                if (e->name == name && e->include_depth == 0) { m = e->method; break; }
            }
        }
    }
    if (!m) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(m->visibility != KORB_VIS_PRIVATE));
}

/* Module#public_method_defined? / private_method_defined? /
 * protected_method_defined? — visibility-filtered counterparts.  Each
 * accepts an optional second `inherit` arg (default true).  When false,
 * only methods defined directly on self (not via include / super) match. */
static struct korb_method *find_method_with_inherit(struct korb_class *klass, ID name, bool inherit) {
    if (inherit) return korb_class_find_method(klass, name);
    for (uint32_t b = 0; b < klass->methods.bucket_cnt; b++) {
        for (struct korb_method_table_entry *e = klass->methods.buckets[b]; e; e = e->next) {
            if (e->name == name && e->include_depth == 0) return e->method;
        }
    }
    return NULL;
}
static RESULT module_public_method_defined_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
    ID name; CHECK(name_arg_to_id(c, argv[0], &name));
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return RESULT_OK(KORB_BOOL(m && m->visibility == KORB_VIS_PUBLIC));
}

static RESULT module_private_method_defined_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
    ID name; CHECK(name_arg_to_id(c, argv[0], &name));
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return RESULT_OK(KORB_BOOL(m && m->visibility == KORB_VIS_PRIVATE));
}

static RESULT module_protected_method_defined_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
    ID name; CHECK(name_arg_to_id(c, argv[0], &name));
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return RESULT_OK(KORB_BOOL(m && m->visibility == KORB_VIS_PROTECTED));
}

/* Module#private_instance_methods / public_instance_methods /
 * protected_instance_methods — visibility-filtered list.  Only own
 * methods (no inherited) since the existing instance_methods walks
 * super for inherited. */
static VALUE module_methods_by_vis(CTX *c, VALUE self, int argc, VALUE *argv,
                                     enum korb_visibility vis) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return korb_ary_new(c, c->sp_top);
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    /* Park the result array (msp[0]) and the current class (msp[1]) across
     * the per-method korb_ary_push / korb_id2sym GC points — classes are
     * arena (moving), so the C-locals would go stale.  Method-table buckets
     * are libc (non-moving), so an entry pointer `e` stays valid across GC;
     * only the class struct (holding the buckets ptr) moves, so re-read it
     * from msp[1] each time. */
    VALUE *const msp = c->sp_top;
    msp[0] = self;                          /* park class BEFORE korb_ary_new GC */
    msp[1] = korb_ary_new(c, msp + 1);      /* result array (msp[0] stays scanned) */
    bool first = true;
    while (msp[0]) {
        struct korb_class *k = (struct korb_class *)msp[0];
        uint32_t bcnt = k->methods.bucket_cnt;
        for (uint32_t b = 0; b < bcnt; b++) {
            struct korb_method_table_entry *e =
                ((struct korb_class *)msp[0])->methods.buckets[b];
            for (; e; e = e->next) {
                /* When inherit=false, only methods defined DIRECTLY on
                 * this class count — drop entries that came from an
                 * included module (include_depth > 0). */
                if (!include_inherited && first && e->include_depth > 0) continue;
                if (e->method && e->method->visibility == vis) {
                    VALUE sym = korb_id2sym(e->name);
                    korb_ary_push(c, msp + 2, msp[1], sym);
                }
            }
        }
        first = false;
        if (!include_inherited) break;
        msp[0] = (VALUE)((struct korb_class *)msp[0])->super;
    }
    return msp[1];
}
static RESULT module_private_instance_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(module_methods_by_vis(c, self, argc, argv, KORB_VIS_PRIVATE));
}
static RESULT module_public_instance_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(module_methods_by_vis(c, self, argc, argv, KORB_VIS_PUBLIC));
}
static RESULT module_protected_instance_methods(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(module_methods_by_vis(c, self, argc, argv, KORB_VIS_PROTECTED));
}

/* Module#constants — sym list of declared constants. */
static RESULT module_constants(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return RESULT_OK(korb_ary_new(c, c->sp_top));
    }
    VALUE r = korb_ary_new(c, c->sp_top);
    for (struct korb_const_entry *e = ((struct korb_class *)self)->constants; e; e = e->next) {
        korb_ary_push(c, c->sp_top, r, korb_id2sym(e->name));
    }
    return RESULT_OK(r);
}

/* Module#class_eval(string) / Module#class_eval { ... } — evaluate
 * the source string or block with self = the module. */
static RESULT module_class_eval(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    
    /* String-form: eval source in the module's context. */
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) &&
        BUILTIN_TYPE(argv[0]) == T_STRING) {
        extern NODE *koruby_parse(const char *src, size_t len, const char *filename);
        struct korb_string *s = (struct korb_string *)argv[0];
        struct korb_class *klass = (struct korb_class *)self;
        NODE *ast = koruby_parse(s->ptr, (size_t)s->len, "(eval)");
        if (!ast) return RESULT_OK(Qnil);
        VALUE *prev_fp = c->current_frame->fp;
        VALUE prev_self = c->current_frame->self;
        struct korb_class *prev_class = c->current_frame->current_class;
        struct korb_cref *prev_cref = c->current_frame->cref;
        c->current_frame->fp = c->sp_top + 1;
        c->current_frame->self = self;
        c->current_frame->current_class = klass;
        struct korb_cref top = { .klass = klass, .prev = NULL };
        c->current_frame->cref = &top;
        OPTIMIZE(ast);
        RESULT _br = EVAL(c, ast, c->current_frame->fp);
        c->current_frame->fp = prev_fp;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        return _br;
    }
    if (!c->current_block) return RESULT_OK(self);
    struct korb_proc *blk = c->current_block;
    /* Symbol-proc / Method-proc shim handling: dispatch as
     * `self.send(name)` rather than yielding into a NULL body.  Same
     * idea as obj_instance_eval. */
    if (blk->body == NULL) {
        if (SYMBOL_P(blk->self)) {
            return korb_funcall(c, self, korb_sym2id(blk->self), 0, NULL);
        }
        if (!SPECIAL_CONST_P(blk->self) &&
            BUILTIN_TYPE(blk->self) == T_DATA &&
            ((struct RBasic *)blk->self)->klass == (VALUE)KORB_VM(c)->method_class) {
            struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
            return korb_funcall(c, self, mo->name, 0, NULL);
        }
        return RESULT_OK(self);
    }
    struct korb_class *klass = (struct korb_class *)self;
    VALUE prev_self = c->current_frame->self;
    struct korb_class *prev_class = c->current_frame->current_class;
    struct korb_cref *prev_cref = c->current_frame->cref;
    struct korb_cref new_cref = { .klass = klass, .prev = c->current_frame->cref };
    VALUE prev_blk_self = blk->self;
    /* class_eval semantics: temporarily install klass at the head of the
     * block's lexical cref chain so `def` and constant lookup inside the
     * block resolve in klass.  Save and restore blk->cref so the proc
     * keeps its original cref for any post-eval calls. */
    struct korb_cref *prev_blk_cref = blk->cref;
    struct korb_cref blk_new_cref = { .klass = klass, .prev = blk->cref };
    blk->cref = &blk_new_cref;
    c->current_frame->self = self;
    c->current_frame->current_class = klass;
    c->current_frame->cref = &new_cref;
    blk->self = self;
    VALUE av0[1] = { self };
    RESULT _br = korb_yield(c, 1, av0);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
    c->current_frame->self = prev_self;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    /* class_eval consumes a `break` as nil; other non-NORMAL propagates. */
    if (_br.state == KORB_BREAK) return RESULT_OK(Qnil);
    return _br;
}

/* Module#class_exec(*args) { |*args| ... } — like class_eval but
 * passes args to the block.  module_exec is just an alias. */
static RESULT module_class_exec(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    
    if (!c->current_block) return RESULT_OK(self);
    struct korb_proc *blk = c->current_block;
    if (blk->body == NULL) {
        if (SYMBOL_P(blk->self)) {
            return korb_funcall(c, self, korb_sym2id(blk->self), (uint32_t)argc, argv);
        }
        if (!SPECIAL_CONST_P(blk->self) &&
            BUILTIN_TYPE(blk->self) == T_DATA &&
            ((struct RBasic *)blk->self)->klass == (VALUE)KORB_VM(c)->method_class) {
            struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
            return korb_funcall(c, self, mo->name, (uint32_t)argc, argv);
        }
        return RESULT_OK(self);
    }
    struct korb_class *klass = (struct korb_class *)self;
    VALUE prev_self = c->current_frame->self;
    struct korb_class *prev_class = c->current_frame->current_class;
    struct korb_cref *prev_cref = c->current_frame->cref;
    struct korb_cref new_cref = { .klass = klass, .prev = c->current_frame->cref };
    VALUE prev_blk_self = blk->self;
    struct korb_cref *prev_blk_cref = blk->cref;
    struct korb_cref blk_new_cref = { .klass = klass, .prev = blk->cref };
    blk->cref = &blk_new_cref;
    c->current_frame->self = self;
    c->current_frame->current_class = klass;
    c->current_frame->cref = &new_cref;
    blk->self = self;
    RESULT _br = korb_yield(c, (uint32_t)argc, argv);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
    c->current_frame->self = prev_self;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    /* class_exec consumes a `break` as nil; other non-NORMAL propagates. */
    if (_br.state == KORB_BREAK) return RESULT_OK(Qnil);
    return _br;
}

/* Module#< — true if self is a subclass/submodule of other. */
extern bool korb_module_has_ancestor(struct korb_class *, struct korb_class *);
static RESULT module_lt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    if (SPECIAL_CONST_P(argv[0]) ||
        (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "compared with non class/module");
    }
    if (self == argv[0]) return RESULT_OK(Qfalse);  /* CRuby: same → false for `<` */
    struct korb_class *target = (struct korb_class *)argv[0];
    /* Use transitive include walk. */
    if (korb_module_has_ancestor((struct korb_class *)self, target)) return RESULT_OK(Qtrue);
    if (korb_module_has_ancestor(target, (struct korb_class *)self)) return RESULT_OK(Qfalse);
    return RESULT_OK(Qnil);
}
/* Module#<=> — -1 if self < target, 0 if equal, 1 if self > target,
 * nil if unrelated. */
static RESULT module_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return RESULT_OK(Qnil);
    if (self == argv[0]) return RESULT_OK(INT2FIX(0));
    VALUE lt = UNWRAP(module_lt(c, argc, sp));
    if (lt == Qtrue) return RESULT_OK(INT2FIX(-1));
    if (lt == Qfalse) return RESULT_OK(INT2FIX(1));
    return RESULT_OK(Qnil);
}
static RESULT module_le(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    if (self == argv[0]) return RESULT_OK(Qtrue);
    return module_lt(c, argc, sp);
}
static RESULT module_gt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    if (SPECIAL_CONST_P(argv[0]) ||
        (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "compared with non class/module");
    }
    if (self == argv[0]) return RESULT_OK(Qfalse);
    /* Swap receiver/arg for module_lt: stage [arg0, self] at sp. */
    VALUE saved_self = sp[-argc - 1];
    VALUE saved_arg0 = sp[-argc];
    sp[-argc - 1] = argv[0];  /* new self = original argv[0] */
    sp[-argc] = self;          /* new argv[0] = original self */
    RESULT _r = module_lt(c, 1, sp);
    sp[-argc - 1] = saved_self;
    sp[-argc] = saved_arg0;
    return _r;
}
static RESULT module_ge(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    if (self == argv[0]) return RESULT_OK(Qtrue);
    return module_gt(c, argc, sp);
}

/* Walk a module's transitive includes (and super chain).  Returns true
 * if `target` is in the module's ancestry. */
bool korb_module_has_ancestor(struct korb_class *m, struct korb_class *target) {
    if (!m) return false;
    for (struct korb_class *k = m; k; k = k->super) {
        if (k == target) return true;
        for (uint32_t i = 0; i < k->includes_cnt; i++) {
            if (korb_module_has_ancestor(k->includes[i], target)) return true;
        }
    }
    return false;
}

/* ---------- Class === (for case/when class match) ---------- */
static RESULT class_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Class === obj  ⇔ obj.is_a?(self).  Walks super chain + transitive
     * includes (Module includes Module).  `M === obj.extend(M)` and
     * `Basic === Child.new` (Child includes Super, Super includes Basic)
     * both return true. */
    if (argc < 1) return RESULT_OK(Qfalse);
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qfalse);
    struct korb_class *target = (struct korb_class *)self;
    return RESULT_OK(KORB_BOOL(korb_module_has_ancestor(korb_class_of_class(argv[0]), target)));
}


/* ---------- Class.new etc ---------- */
static RESULT class_new(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS) {
        return korb_raise(c, NULL, "Class.new called on non-class");
    }
    struct korb_class *klass = (struct korb_class *)self;
    /* Singleton classes can't be instantiated — CRuby raises TypeError. */
    if (klass->basic.head.flags & FL_SINGLETON) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "can't create instance of singleton class");
    }
    /* Reject .new on an uninitialized class (the result of Class.allocate
     * before its super has been wired up).  Without this, dispatching to
     * .allocate or .initialize on a half-built class crashes deep in
     * method lookup.  CRuby raises TypeError here. */
    if (klass->name == korb_intern("(uninitialized)") && klass->super == NULL) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "can't create instance of uninitialized class");
    }
    /* Class.new(superclass = Object) — create an anonymous subclass. */
    if (klass == KORB_VM(c)->class_class) {
        struct korb_class *super = KORB_VM(c)->object_class;
        if (argc >= 1) {
            /* Reject non-Class superclass with TypeError. */
            if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_CLASS) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "superclass must be an instance of Class (given an instance of %s)",
                           korb_id_name(korb_class_of_class(argv[0])->name));
            }
            super = (struct korb_class *)argv[0];
            /* Reject metaclass (FL_SINGLETON) — CRuby raises TypeError
             * "can't make subclass of singleton class". */
            if (super->basic.head.flags & FL_SINGLETON) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "can't make subclass of singleton class");
            }
            /* Reject an uninitialized superclass — same TypeError as
             * `klass.new` on it.  Uninitialized classes have no super
             * field set up yet, so subclassing them would inherit a
             * broken chain. */
            if (super->super == NULL &&
                super->name == korb_intern("(uninitialized)")) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "can't inherit uninitialized class");
            }
        }
        /* korb_class_new + the inherited funcall fire GC and classes are
         * arena (moving) — park super at sp[0] and the new class at sp[1]
         * and re-read them from those GC-scanned slots.  Pre-intern IDs so
         * no symbol-table GC fires while sp[0] sits at sp (not yet covered
         * by c->sp_top until korb_class_new bumps it). */
        const ID anon_id = korb_intern("(anon)");
        const ID inherited_id = korb_intern("inherited");
        sp[0] = (VALUE)super;
        struct korb_class *nk = korb_class_new(c, sp + 1, anon_id,
                                               (struct korb_class *)sp[0],
                                               ((struct korb_class *)sp[0])->instance_type);
        sp[1] = (VALUE)nk;
        /* Fire `inherited` on the parent — same as `class C < P; end`. */
        {
            struct korb_class *super_meta = korb_class_of_class(sp[0]);
            if (super_meta && korb_class_find_method(super_meta, inherited_id)) {
                VALUE child_v = sp[1];
                CHECK(korb_funcall(c, sp[0], inherited_id, 1, &child_v));
            }
        }
        super = (struct korb_class *)sp[0];   /* re-read after funcall GC */
        nk = (struct korb_class *)sp[1];

        if (c->current_block) {
            VALUE prev_self = c->current_frame->self;
            struct korb_class *prev_class = c->current_frame->current_class;
            struct korb_cref *prev_cref = c->current_frame->cref;
            struct korb_cref new_cref = { .klass = nk, .prev = c->current_frame->cref };
            VALUE prev_blk_self = c->current_block->self;
            struct korb_cref *prev_blk_cref = c->current_block->cref;
            struct korb_cref blk_new_cref = { .klass = nk, .prev = c->current_block->cref };
            c->current_block->cref = &blk_new_cref;
            c->current_frame->self = (VALUE)nk;
            c->current_frame->current_class = nk;
            c->current_frame->cref = &new_cref;
            c->current_block->self = (VALUE)nk;   /* class_eval semantics */
            VALUE av0[1] = { (VALUE)nk };
            RESULT _yr = korb_yield(c, 1, av0);
            c->current_block->cref = prev_blk_cref;
            c->current_block->self = prev_blk_self;
            c->current_frame->self = prev_self;
            c->current_frame->current_class = prev_class;
            c->current_frame->cref = prev_cref;
            /* BREAK from class body silently consumed; other non-NORMAL
             * propagates. */
            if (_yr.state != KORB_NORMAL && _yr.state != KORB_BREAK) return _yr;
            /* korb_yield ran the class-body block, which may have fired GC
             * (e.g. attr_reader → korb_class_add_method_ast) and moved the
             * arena class.  `nk` / `sp[1]` C-locals are stale; the block body
             * runs with frame->cref = blk_new_cref (korb_yield installs the
             * block's cref), so blk_new_cref.klass is the slot visit_roots
             * forwarded across the body and is the live handle.  (new_cref is
             * NOT walked during the body — korb_yield replaced it.) */
            return RESULT_OK((VALUE)blk_new_cref.klass);
        }
        return RESULT_OK((VALUE)nk);
    }
    /* Park klass (sp[0]) + new object (sp[1]) across alloc-can-GC.
     * korb_object_new sets c->sp_top = sp+2 internally; subsequent
     * korb_funcall_with_block inherits that root-scan boundary. */
    sp[0] = (VALUE)klass;
    sp[1] = 0;
    sp[1] = korb_object_new(c, sp + 2, (struct korb_class *)sp[0]);
    struct korb_method *m = korb_class_find_method((struct korb_class *)sp[0], id_initialize);
    RESULT init_r = RESULT_OK(Qnil);
    if (m) {
        VALUE blk = c->current_block ? (VALUE)c->current_block : Qnil;
        init_r = korb_funcall_with_block(c, sp[1], id_initialize, argc, argv, blk);
    }
    VALUE obj = sp[1];
    /* `break N` from the block passed to .new escapes .new with value N
     * (CRuby semantics).  Raises / returns also need to propagate. */
    if (init_r.state == KORB_BREAK) return RESULT_OK(init_r.value);
    if (init_r.state != KORB_NORMAL) return init_r;
    VALUE *fp_lo = c->current_frame->fp;
    VALUE *fp_hi = c->sp_top;
    /* Snapshot any Proc-typed ivar of obj whose env still points at
     * initialize's freshly-popped frame.  Inline gate: most classes
     * never store procs in ivars, so the CALL is itself the cost we
     * want to skip — `korb_proc_snapshot_env_maybe` is an inline that
     * checks FL_HAS_PROC_IVARS on obj's class first. */
    korb_proc_snapshot_env_maybe(obj, fp_lo, fp_hi + 1024);
    /* Also detach the caller's user block (c->current_block), in case it
     * was stored into an ivar of obj or used via @blk = blk from
     * &blk parameter — that block's env points at the *outer* frame. */
    if (c->current_block && c->current_block->env) {
        if (c->current_block->env >= fp_lo && c->current_block->env < fp_hi) {
            VALUE *snap = korb_xmalloc(c->current_block->env_size * sizeof(VALUE));
            for (uint32_t i = 0; i < c->current_block->env_size; i++) snap[i] = c->current_block->env[i];
            c->current_block->env = snap;
        }
    }
    return RESULT_OK(obj);
}

static RESULT class_name(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* CRuby: anonymous Class/Module returns nil; named ones return the
     * registered name string. */
    struct korb_class *k = (struct korb_class *)self;
    if (!k->name || k->name == korb_intern("(anon)")) return RESULT_OK(Qnil);
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, korb_id_name(k->name)));
}

/* (Array#hash folded into builtins/array.c) */

/* ---------- Class#ancestors / Module#prepend ---------- */

/* Append `m` and its transitive included modules to `arr`, dedup'd. */
static void ancestors_push_module(CTX *c, VALUE arr, struct korb_class *m) {
    if (!m) return;
    struct korb_array *a = (struct korb_array *)arr;
    for (long j = 0; j < a->len; j++) {
        if (korb_ary_items(a)[j] == (VALUE)m) return;
    }
    korb_ary_push(c, c->sp_top, arr, (VALUE)m);
    /* Recurse into m's own includes (latest-include first to match
     * CRuby's "module that is included later sits earlier in
     * ancestors"). */
    for (int32_t i = (int32_t)m->includes_cnt - 1; i >= 0; i--) {
        ancestors_push_module(c, arr, m->includes[i]);
    }
}

static RESULT class_ancestors(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self)) return RESULT_OK(korb_ary_new(c, c->sp_top));
    /* The result array and every class on the super/include/prepend walk are
     * moving arena handles, and each ancestors_push_module is a korb_ary_push
     * (GC point).  Park the array (sp[0]) and the walk cursor (sp[1]) and
     * re-derive the class from sp[1] before each push. */
    sp[0] = korb_ary_new(c, sp + 1);
    sp[1] = self;
    c->sp_top = sp + 2;
    while (sp[1] != 0 && !SPECIAL_CONST_P(sp[1])) {
        struct korb_class *k = (struct korb_class *)sp[1];
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            ancestors_push_module(c, sp[0], ((struct korb_class *)sp[1])->prepends[i]);
        }
        ancestors_push_module(c, sp[0], (struct korb_class *)sp[1]);
        k = (struct korb_class *)sp[1];
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            ancestors_push_module(c, sp[0], ((struct korb_class *)sp[1])->includes[i]);
        }
        sp[1] = (VALUE)((struct korb_class *)sp[1])->super;
    }
    VALUE result = sp[0];
    c->sp_top = sp;
    return RESULT_OK(result);
}
static RESULT obj_extend(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* extend M on an object: include M into the object's singleton class. */
    if (SPECIAL_CONST_P(self)) return RESULT_OK(self);
    extern struct korb_class *korb_singleton_class_of_value(CTX *c, VALUE *sp, VALUE v);
    /* Fallback if helper isn't there: rewire basic.klass to a fresh
     * subclass of the current class and include the module into it. */
    if (BUILTIN_TYPE(self) == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)self;
        struct korb_class *cur = (struct korb_class *)o->basic.klass;
        struct korb_class *meta = NULL;
        if (cur && cur->name == korb_intern("(singleton)")) {
            meta = cur;
        } else {
            meta = korb_class_new(c, c->sp_top, korb_intern("(singleton)"), cur, cur ? cur->instance_type : T_OBJECT);
            /* korb_class_new is a GC point: self/o and the old class cur are
             * moving and the C-locals are now stale.  Re-derive both from the
             * forwarded receiver slot before touching cur->ivar_* / o->klass. */
            o = (struct korb_object *)sp[-argc - 1];
            cur = (struct korb_class *)o->basic.klass;
            /* Copy ivar shape so @ivars set before extend remain
             * accessible: korb_ivar_get / korb_ivar_set look up slots
             * by name on the object's class, which is now meta. */
            if (cur && cur->ivar_count > 0) {
                meta->ivar_capa = cur->ivar_capa;
                meta->ivar_count = cur->ivar_count;
                meta->ivar_names = korb_xmalloc(cur->ivar_capa * sizeof(*meta->ivar_names));
                memcpy(meta->ivar_names, cur->ivar_names,
                       cur->ivar_count * sizeof(*meta->ivar_names));
            }
            o->basic.klass = (VALUE)meta;
        }
        for (int i = 0; i < argc; i++) {
            if (!SPECIAL_CONST_P(argv[i]) &&
                (BUILTIN_TYPE(argv[i]) == T_MODULE || BUILTIN_TYPE(argv[i]) == T_CLASS)) {
                korb_module_include(meta, (struct korb_class *)argv[i]);
            }
        }
    } else if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
        /* extending a class extends its metaclass — include into singleton */
        struct korb_class *meta = korb_singleton_class_of(c, c->sp_top, (struct korb_class *)self);
        for (int i = 0; i < argc; i++) {
            if (!SPECIAL_CONST_P(argv[i]) &&
                (BUILTIN_TYPE(argv[i]) == T_MODULE || BUILTIN_TYPE(argv[i]) == T_CLASS)) {
                korb_module_include(meta, (struct korb_class *)argv[i]);
            }
        }
    }
    /* Fire `extended` hook on each module (defined as a class method
     * on the module).  CRuby calls hooks after the inclusion takes
     * effect, mirroring how `included` is invoked. */
    for (int i = 0; i < argc; i++) {
        if (SPECIAL_CONST_P(argv[i])) continue;
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        struct korb_class *meta = korb_class_of_class(argv[i]);
        if (meta && korb_class_find_method(meta, korb_intern("extended"))) {
            /* Re-read self from its slot: a prior korb_class_new / extended
             * hook funcall (GC points) may have moved the receiver. */
            VALUE obj_v = sp[-argc - 1];
            CHECK(korb_funcall(c, argv[i], korb_intern("extended"), 1, &obj_v));
        }
    }
    return RESULT_OK(sp[-argc - 1]);
}

static RESULT module_prepend(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Real prepend: register the module in `prepends` so the dispatch
     * walk in korb_class_find_method finds prepended-module methods
     * BEFORE the class's own.  super from inside the prepended method
     * resolves to the class's own method via korb_class_find_super_method.
     * No method-table flattening — must stay symbolic. */
    if (SPECIAL_CONST_P(self)) return RESULT_OK(self);
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        if (SPECIAL_CONST_P(argv[i])) continue;
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        struct korb_class *mod = (struct korb_class *)argv[i];
        bool dup = false;
        for (uint32_t j = 0; j < klass->prepends_cnt; j++) {
            if (klass->prepends[j] == mod) { dup = true; break; }
        }
        if (dup) continue;
        if (klass->prepends_cnt >= klass->prepends_capa) {
            uint32_t nc = klass->prepends_capa ? klass->prepends_capa * 2 : 4;
            klass->prepends = korb_xrealloc(klass->prepends, nc * sizeof(*klass->prepends));
            klass->prepends_capa = nc;
        }
        klass->prepends[klass->prepends_cnt++] = mod;
        /* Also propagate the module's constants — module's CONSTS visible. */
        for (struct korb_const_entry *ce = mod->constants; ce; ce = ce->next) {
            if (!korb_const_has(klass, ce->name)) korb_const_set(klass, ce->name, ce->value);
        }
    }
    if (KORB_VM(c)) { KORB_VM(c)->method_serial++; korb_g_method_serial = KORB_VM(c)->method_serial; }
    return RESULT_OK(self);
}
/* Module#remove_const(:NAME) — remove a constant from the module.
 * Returns the previous value, or raises NameError if missing. */
static RESULT module_remove_const(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    if (argc < 1) return RESULT_OK(Qnil);
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              (BUILTIN_TYPE(argv[0]) == T_STRING ?
               korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len) : 0);
    if (!name) return RESULT_OK(Qnil);
    struct korb_class *k = (struct korb_class *)self;
    /* Walk the linked list of constants; unlink and return value. */
    extern bool korb_const_remove(struct korb_class *k, ID name, VALUE *out);
    VALUE prev = Qnil;
    if (!korb_const_remove(k, name, &prev)) {
        VALUE eName = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
        return korb_raise(c, (struct korb_class *)eName,
                   "constant %s::%s not defined",
                   korb_id_name(k->name), korb_id_name(name));
    }
    return RESULT_OK(prev);
}

/* Module#remove_class_variable(:@@name) — remove a cvar from the
 * module's own table.  Returns the previous value or raises NameError. */
static RESULT module_remove_class_variable(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(Qnil);
    if (argc < 1) return RESULT_OK(Qnil);
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              (BUILTIN_TYPE(argv[0]) == T_STRING ?
               korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len) : 0);
    if (!name) return RESULT_OK(Qnil);
    struct korb_class *k = (struct korb_class *)self;
    for (uint32_t i = 0; i < k->cvar_cnt; i++) {
        if (k->cvars[i].name == name) {
            VALUE prev = k->cvars[i].value;
            for (uint32_t j = i + 1; j < k->cvar_cnt; j++) k->cvars[j-1] = k->cvars[j];
            k->cvar_cnt--;
            return RESULT_OK(prev);
        }
    }
    VALUE eName = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
    return korb_raise(c, (struct korb_class *)eName,
               "class variable %s not defined for %s",
               korb_id_name(name), korb_id_name(k->name));
}

/* Module#private_class_method(:foo, ...) — mark singleton method as
 * private.  Stub-level: just set visibility flag.  We keep things
 * simple — return self regardless. */
static RESULT class_visibility_set(CTX *c, VALUE self, int argc, VALUE *argv,
                                  enum korb_visibility v) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    extern struct korb_class *korb_singleton_class_of(CTX *c, VALUE *sp, struct korb_class *);
    struct korb_class *meta = korb_singleton_class_of(c, c->sp_top, (struct korb_class *)self);
    /* Single-Array form: `private_class_method([:foo, :bar])` (Ruby 3.x). */
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        argv = korb_ary_items(a);
        argc = (int)a->len;
    }
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (!SYMBOL_P(arg) &&
            (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) &&
            !SPECIAL_CONST_P(arg)) {
            VALUE rt = UNWRAP(korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) }));
            if (RTEST(rt)) {
                arg = UNWRAP(korb_funcall(c, arg, korb_intern("to_str"), 0, NULL));
            }
        }
        ID name = 0;
        if (SYMBOL_P(arg)) name = korb_sym2id(arg);
        else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING)
            name = korb_intern_n(((struct korb_string *)arg)->ptr,
                                 ((struct korb_string *)arg)->len);
        if (!name) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "%s is not a symbol nor a string",
                       SPECIAL_CONST_P(argv[i]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[i])->name));
        }
        struct korb_method *m = meta ? korb_class_find_method(meta, name) : NULL;
        if (!m) {
            VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
            const char *cn = (((struct korb_class *)self)->name)
                              ? korb_id_name(((struct korb_class *)self)->name) : "(anon)";
            return korb_raise(c, (struct korb_class *)eN,
                       "undefined method '%s' for class '%s'",
                       korb_id_name(name), cn);
        }
        m->visibility = v;
    }
    return RESULT_OK(self);
}
static RESULT module_private_class_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return class_visibility_set(c, self, argc, argv, KORB_VIS_PRIVATE);
}

static RESULT module_public_class_method(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return class_visibility_set(c, self, argc, argv, KORB_VIS_PUBLIC);
}

/* Module#private_constant / public_constant — visibility on constants. */
static RESULT module_private_constant(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    struct korb_class *k = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name = SYMBOL_P(argv[i]) ? korb_sym2id(argv[i]) :
                  (BUILTIN_TYPE(argv[i]) == T_STRING ?
                   korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                  ((struct korb_string *)argv[i])->len) : 0);
        if (!name) continue;
        bool found = false;
        for (struct korb_const_entry *e = k->constants; e; e = e->next) {
            if (e->name == name) { e->is_private = true; found = true; break; }
        }
        if (!found) {
            VALUE eN = korb_const_get(KORB_VM(c)->object_class, korb_intern("NameError"));
            return korb_raise(c, (struct korb_class *)eN,
                       "constant %s::%s not defined",
                       k->name ? korb_id_name(k->name) : "?", korb_id_name(name));
        }
    }
    return RESULT_OK(self);
}
static RESULT module_public_constant(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return RESULT_OK(self);
    struct korb_class *k = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name = SYMBOL_P(argv[i]) ? korb_sym2id(argv[i]) :
                  (BUILTIN_TYPE(argv[i]) == T_STRING ?
                   korb_intern_n(((struct korb_string *)argv[i])->ptr,
                                  ((struct korb_string *)argv[i])->len) : 0);
        if (!name) continue;
        for (struct korb_const_entry *e = k->constants; e; e = e->next) {
            if (e->name == name) { e->is_private = false; break; }
        }
    }
    return RESULT_OK(self);
}

/* Module.nesting — array of the lexically enclosing class/module
 * stack, innermost first.  Just walks c->current_frame->cref which already mirrors
 * the source's `module M; class C; ...; end; end` nesting. */
static RESULT module_class_nesting(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE arr = korb_ary_new(c, c->sp_top);
    for (struct korb_cref *cur = c->current_frame->cref; cur; cur = cur->prev) {
        if (cur->klass && cur->klass != KORB_VM(c)->object_class) {
            korb_ary_push(c, c->sp_top, arr, (VALUE)cur->klass);
        }
    }
    return RESULT_OK(arr);
}

static RESULT module_new_class_func(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Module.new — create an anonymous module.  If a block is given,
     * evaluate it with self = the new module (lets `include`/method defs
     * land on the new module). */
    struct korb_class *m = korb_module_new(c, c->sp_top, korb_intern("(anon)"));
    
    if (c->current_block) {
        VALUE prev_self = c->current_frame->self;
        struct korb_class *prev_class = c->current_frame->current_class;
        struct korb_cref *prev_cref = c->current_frame->cref;
        struct korb_cref new_cref = { .klass = m, .prev = c->current_frame->cref };
        VALUE prev_blk_self = c->current_block->self;
        struct korb_cref *prev_blk_cref = c->current_block->cref;
        struct korb_cref blk_new_cref = { .klass = m, .prev = c->current_block->cref };
        c->current_block->cref = &blk_new_cref;
        c->current_frame->self = (VALUE)m;
        c->current_frame->current_class = m;
        c->current_frame->cref = &new_cref;
        /* `def self.X` inside the block needs the block's self to BE
         * the module, not the outer caller — same fix as Class.new. */
        c->current_block->self = (VALUE)m;
        VALUE argv0[1] = { (VALUE)m };
        RESULT _yr = korb_yield(c, 1, argv0);
        c->current_block->cref = prev_blk_cref;
        c->current_block->self = prev_blk_self;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        /* BREAK from module body silently consumed; other non-NORMAL
         * propagates. */
        if (_yr.state != KORB_NORMAL && _yr.state != KORB_BREAK) return _yr;
    }
    return RESULT_OK((VALUE)m);
}

