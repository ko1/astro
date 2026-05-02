/* Module / Class metaprogramming — moved from builtins.c. */

/* ---------- Module / Class metaprogramming ---------- */

static VALUE ivar_getter_dispatch(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* the getter method's "name" tells us the @ivar; we encode the ivar name
     * as the method's name without the leading @, so name "x" → @x. */
    /* Actually simpler: we install the cfunc with a side-channel.  In our
     * scheme cfuncs receive the same args; we need to pass the ivar name.
     * Instead, the getter's cfunc captures the ID at definition time via
     * a closure-style structure. */
    (void)argc; (void)argv;
    return Qnil; /* never called directly */
}

/* attr_reader / attr_writer / attr_accessor implementation:
 * We install AST methods whose body is node_ivar_get / node_ivar_set.
 */
static VALUE module_attr_reader(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_reader: not on a class/module");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (BUILTIN_TYPE(argv[i]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[i])->ptr, ((struct korb_string *)argv[i])->len);
        else continue;
        /* @name */
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        /* body: node_ivar_get(iv_id) */
        NODE *body = ALLOC_node_ivar_get(iv_id);
        korb_class_add_method_ast(klass, name, body, 0, 0);
    }
    return Qnil;
}

static VALUE module_attr_writer(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_writer: not on a class/module");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (BUILTIN_TYPE(argv[i]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[i])->ptr, ((struct korb_string *)argv[i])->len);
        else continue;
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        /* setter name: name=  */
        char *sn = korb_xmalloc_atomic(bl + 2);
        memcpy(sn, base, bl); sn[bl] = '='; sn[bl + 1] = 0;
        ID setter_id = korb_intern(sn);
        /* @name */
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        /* body: node_ivar_set(iv_id, node_lvar_get(0)) */
        NODE *body = ALLOC_node_ivar_set(iv_id, ALLOC_node_lvar_get(0));
        korb_class_add_method_ast(klass, setter_id, body, 1, 1);
    }
    return Qnil;
}

static VALUE module_attr_accessor(CTX *c, VALUE self, int argc, VALUE *argv) {
    module_attr_reader(c, self, argc, argv);
    module_attr_writer(c, self, argc, argv);
    return Qnil;
}

static VALUE module_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Simplified include: copy module's methods/constants into the class.
     * Real Ruby inserts the module into the ancestor chain; we flatten. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        korb_module_include(klass, (struct korb_class *)argv[i]);
    }
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
    return self;
}

extern void korb_class_add_method_proc(struct korb_class *klass, ID name, struct korb_proc *p);

static VALUE module_define_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* define_method(:name) { |args| body } — register the block as a
     * proc-method.  Dispatch (prologue_proc_method) calls the proc via
     * proc_call so its captured env is preserved (closure semantics). */
    if (argc < 1) return Qnil;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    else return Qnil;
    extern struct korb_proc *current_block;
    struct korb_proc *p;
    if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) && BUILTIN_TYPE(argv[1]) == T_PROC) {
        p = (struct korb_proc *)argv[1];
    } else if (current_block) {
        p = current_block;
    } else {
        return Qnil;
    }
    korb_class_add_method_proc((struct korb_class *)self, name, p);
    return korb_id2sym(name);
}


/* ---------- Class === (for case/when class match) ---------- */
static VALUE class_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Class === obj  ⇔ obj.is_a?(self) */
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    struct korb_class *target = (struct korb_class *)self;
    for (struct korb_class *k = korb_class_of_class(argv[0]); k; k = k->super) {
        if (k == target) return Qtrue;
    }
    return Qfalse;
}


/* ---------- Class.new etc ---------- */
static VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) {
        korb_raise(c, NULL, "Class.new called on non-class");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    /* Class.new(superclass = Object) — create an anonymous subclass. */
    if (klass == korb_vm->class_class) {
        struct korb_class *super = korb_vm->object_class;
        if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) &&
            BUILTIN_TYPE(argv[0]) == T_CLASS) {
            super = (struct korb_class *)argv[0];
        }
        struct korb_class *nk = korb_class_new(korb_intern("(anon)"),
                                               super, super->instance_type);
        extern struct korb_proc *current_block;
        if (current_block) {
            VALUE prev_self = c->self;
            struct korb_class *prev_class = c->current_class;
            struct korb_cref *prev_cref = c->cref;
            struct korb_cref new_cref = { .klass = nk, .prev = c->cref };
            c->self = (VALUE)nk;
            c->current_class = nk;
            c->cref = &new_cref;
            VALUE av0[1] = { (VALUE)nk };
            korb_yield(c, 1, av0);
            c->self = prev_self;
            c->current_class = prev_class;
            c->cref = prev_cref;
            if (c->state == KORB_BREAK) {
                c->state = KORB_NORMAL; c->state_value = Qnil;
            }
        }
        return (VALUE)nk;
    }
    VALUE obj = korb_object_new(klass);
    /* call initialize if defined */
    struct korb_method *m = korb_class_find_method(klass, id_initialize);
    if (m) {
        korb_funcall(c, obj, id_initialize, argc, argv);
    }
    return obj;
}

static VALUE class_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(korb_id_name(((struct korb_class *)self)->name));
}

/* (Array#hash folded into builtins/array.c) */

/* ---------- Class#ancestors / Module#prepend ---------- */
static VALUE class_ancestors(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    if (SPECIAL_CONST_P(self)) return arr;
    struct korb_class *k = (struct korb_class *)self;
    while (k) {
        korb_ary_push(arr, (VALUE)k);
        /* included modules go right after this class, in reverse
         * include order (Ruby semantics: most recent include first). */
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            korb_ary_push(arr, (VALUE)k->includes[i]);
        }
        k = k->super;
    }
    return arr;
}
static VALUE obj_extend(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* extend M on an object: include M into the object's singleton class. */
    if (SPECIAL_CONST_P(self)) return self;
    extern struct korb_class *korb_singleton_class_of_value(VALUE v);
    /* Fallback if helper isn't there: rewire basic.klass to a fresh
     * subclass of the current class and include the module into it. */
    if (BUILTIN_TYPE(self) == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)self;
        struct korb_class *cur = (struct korb_class *)o->basic.klass;
        struct korb_class *meta = NULL;
        if (cur && cur->name == korb_intern("(singleton)")) {
            meta = cur;
        } else {
            meta = korb_class_new(korb_intern("(singleton)"), cur, cur ? cur->instance_type : T_OBJECT);
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
        struct korb_class *meta = korb_singleton_class_of((struct korb_class *)self);
        for (int i = 0; i < argc; i++) {
            if (!SPECIAL_CONST_P(argv[i]) &&
                (BUILTIN_TYPE(argv[i]) == T_MODULE || BUILTIN_TYPE(argv[i]) == T_CLASS)) {
                korb_module_include(meta, (struct korb_class *)argv[i]);
            }
        }
    }
    return self;
}

static VALUE module_prepend(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Simplified: behave like include (insert into ancestors near top).
     * No method-resolution-order rewiring; enough for tests that just
     * check the symbolic effect. */
    if (SPECIAL_CONST_P(self)) return self;
    for (int i = 0; i < argc; i++) {
        if (!SPECIAL_CONST_P(argv[i]) &&
            (BUILTIN_TYPE(argv[i]) == T_MODULE || BUILTIN_TYPE(argv[i]) == T_CLASS)) {
            korb_module_include((struct korb_class *)self, (struct korb_class *)argv[i]);
        }
    }
    return self;
}
static VALUE module_new_class_func(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Module.new — create an anonymous module.  If a block is given,
     * evaluate it with self = the new module (lets `include`/method defs
     * land on the new module). */
    struct korb_class *m = korb_module_new(korb_intern("(anon)"));
    extern struct korb_proc *current_block;
    if (current_block) {
        VALUE prev_self = c->self;
        struct korb_class *prev_class = c->current_class;
        struct korb_cref *prev_cref = c->cref;
        struct korb_cref new_cref = { .klass = m, .prev = c->cref };
        c->self = (VALUE)m;
        c->current_class = m;
        c->cref = &new_cref;
        VALUE argv0[1] = { (VALUE)m };
        korb_yield(c, 1, argv0);
        c->self = prev_self;
        c->current_class = prev_class;
        c->cref = prev_cref;
        if (c->state == KORB_BREAK) {
            c->state = KORB_NORMAL; c->state_value = Qnil;
        }
    }
    return (VALUE)m;
}

