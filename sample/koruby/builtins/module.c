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
    } else if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) &&
               BUILTIN_TYPE(argv[1]) == T_DATA &&
               ((struct RBasic *)argv[1])->klass == (VALUE)korb_vm->method_class) {
        /* Method object: convert via Method#to_proc. */
        VALUE pr = korb_funcall(c, argv[1], korb_intern("to_proc"), 0, NULL);
        if (BUILTIN_TYPE(pr) != T_PROC) return Qnil;
        p = (struct korb_proc *)pr;
    } else if (current_block) {
        p = current_block;
    } else {
        return Qnil;
    }
    korb_class_add_method_proc((struct korb_class *)self, name, p);
    return korb_id2sym(name);
}


/* Object#define_singleton_method — same as define_method but installs
 * on the receiver's singleton class instead of `self`'s class. */
static VALUE obj_define_singleton_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    extern struct korb_class *korb_singleton_class_of_value(VALUE v);
    struct korb_class *meta = korb_singleton_class_of_value(self);
    if (!meta) return Qnil;
    /* Reuse module_define_method with self overridden to the meta class. */
    return module_define_method(c, (VALUE)meta, argc, argv);
}

/* Class#superclass */
static VALUE class_superclass(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) return Qnil;
    struct korb_class *k = (struct korb_class *)self;
    return k->super ? (VALUE)k->super : Qnil;
}

/* Module#instance_methods([include_inherited=true]) — sym list. */
static VALUE module_instance_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return korb_ary_new();
    }
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *root = (struct korb_class *)self;
    VALUE r = korb_ary_new();
    /* Walk from root through includes / super if requested. */
    struct korb_class *k = root;
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                korb_ary_push(r, korb_id2sym(e->name));
            }
        }
        if (!include_inherited) break;
        k = k->super;
    }
    return r;
}

/* Object#methods([include_inherited=true]) — list public + protected
 * methods accessible on the receiver, walking the class chain. */
static VALUE obj_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *k = korb_class_of_class(self);
    VALUE r = korb_ary_new();
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                if (e->method->visibility == KORB_VIS_PRIVATE) continue;
                /* Avoid duplicates from super chain. */
                bool dup = false;
                for (long j = 0; j < ((struct korb_array *)r)->len; j++) {
                    VALUE existing = ((struct korb_array *)r)->ptr[j];
                    if (SYMBOL_P(existing) && korb_sym2id(existing) == e->name) {
                        dup = true; break;
                    }
                }
                if (!dup) korb_ary_push(r, korb_id2sym(e->name));
            }
        }
        if (!include_inherited) break;
        k = k->super;
    }
    return r;
}

/* Object#singleton_methods — methods defined directly on this object's
 * singleton class (not inherited from regular class). */
static VALUE obj_singleton_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new();
    if (SPECIAL_CONST_P(self)) return r;
    struct korb_class *k = NULL;
    if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
        /* For a class, singleton_methods returns the metaclass methods. */
        struct korb_class *meta = korb_singleton_class_of((struct korb_class *)self);
        k = meta;
    } else if (BUILTIN_TYPE(self) == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)self;
        struct korb_class *cur = (struct korb_class *)o->basic.klass;
        if (cur && cur->name == korb_intern("(singleton)")) k = cur;
    }
    if (!k) return r;
    for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
        for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
            if (e->include_depth == 0) {
                korb_ary_push(r, korb_id2sym(e->name));
            }
        }
    }
    return r;
}

/* Module#method_defined?(name) — true for public/protected. */
static VALUE module_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    struct korb_method *m = korb_class_find_method((struct korb_class *)self, name);
    if (!m) return Qfalse;
    /* CRuby: method_defined? returns true only for public / protected
     * methods (not private).  Match that. */
    return KORB_BOOL(m->visibility != KORB_VIS_PRIVATE);
}

/* Module#public_method_defined? / private_method_defined? /
 * protected_method_defined? — visibility-filtered counterparts. */
static VALUE module_public_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    struct korb_method *m = korb_class_find_method((struct korb_class *)self, name);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PUBLIC);
}

static VALUE module_private_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    struct korb_method *m = korb_class_find_method((struct korb_class *)self, name);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PRIVATE);
}

static VALUE module_protected_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    struct korb_method *m = korb_class_find_method((struct korb_class *)self, name);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PROTECTED);
}

/* Module#private_instance_methods / public_instance_methods /
 * protected_instance_methods — visibility-filtered list.  Only own
 * methods (no inherited) since the existing instance_methods walks
 * super for inherited. */
static VALUE module_methods_by_vis(CTX *c, VALUE self, int argc, VALUE *argv,
                                     enum korb_visibility vis) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return korb_ary_new();
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *k = (struct korb_class *)self;
    VALUE r = korb_ary_new();
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                if (e->method && e->method->visibility == vis) korb_ary_push(r, korb_id2sym(e->name));
            }
        }
        if (!include_inherited) break;
        k = k->super;
    }
    return r;
}
static VALUE module_private_instance_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_methods_by_vis(c, self, argc, argv, KORB_VIS_PRIVATE);
}
static VALUE module_public_instance_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_methods_by_vis(c, self, argc, argv, KORB_VIS_PUBLIC);
}
static VALUE module_protected_instance_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    return module_methods_by_vis(c, self, argc, argv, KORB_VIS_PROTECTED);
}

/* Module#constants — sym list of declared constants. */
static VALUE module_constants(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return korb_ary_new();
    }
    VALUE r = korb_ary_new();
    for (struct korb_const_entry *e = ((struct korb_class *)self)->constants; e; e = e->next) {
        korb_ary_push(r, korb_id2sym(e->name));
    }
    return r;
}

/* Module#class_eval { ... } — evaluate the block with self = the module. */
static VALUE module_class_eval(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    extern struct korb_proc *current_block;
    if (!current_block) return self;
    struct korb_class *klass = (struct korb_class *)self;
    VALUE prev_self = c->self;
    struct korb_class *prev_class = c->current_class;
    struct korb_cref *prev_cref = c->cref;
    struct korb_cref new_cref = { .klass = klass, .prev = c->cref };
    VALUE prev_blk_self = current_block->self;
    c->self = self;
    c->current_class = klass;
    c->cref = &new_cref;
    current_block->self = self;
    VALUE av0[1] = { self };
    VALUE r = korb_yield(c, 1, av0);
    current_block->self = prev_blk_self;
    c->self = prev_self;
    c->current_class = prev_class;
    c->cref = prev_cref;
    if (c->state == KORB_BREAK) { c->state = KORB_NORMAL; c->state_value = Qnil; }
    return r;
}

/* Module#< — true if self is a subclass/submodule of other. */
static VALUE module_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return Qnil;
    if (self == argv[0]) return Qnil;  /* CRuby: same → nil, not false */
    struct korb_class *target = (struct korb_class *)argv[0];
    for (struct korb_class *k = (struct korb_class *)self; k; k = k->super) {
        if (k == target) return Qtrue;
        for (uint32_t i = 0; i < k->includes_cnt; i++) {
            if (k->includes[i] == target) return Qtrue;
        }
    }
    /* If target has self as a strict ancestor, return false (we are
     * the ancestor, target is the subclass).  Else nil (unrelated). */
    for (struct korb_class *k = target; k; k = k->super) {
        if (k == (struct korb_class *)self) return Qfalse;
        for (uint32_t i = 0; i < k->includes_cnt; i++) {
            if (k->includes[i] == (struct korb_class *)self) return Qfalse;
        }
    }
    return Qnil;
}

/* Module#<=> — -1 if self < target, 0 if equal, 1 if self > target,
 * nil if unrelated. */
static VALUE module_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return Qnil;
    if (self == argv[0]) return INT2FIX(0);
    VALUE lt = module_lt(c, self, argc, argv);
    if (lt == Qtrue) return INT2FIX(-1);
    if (lt == Qfalse) return INT2FIX(1);
    return Qnil;
}
static VALUE module_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (self == argv[0]) return Qtrue;
    return module_lt(c, self, argc, argv);
}
static VALUE module_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) return Qnil;
    if (self == argv[0]) return Qfalse;
    VALUE swap[1] = {self};
    return module_lt(c, argv[0], 1, swap);
}
static VALUE module_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (self == argv[0]) return Qtrue;
    return module_gt(c, self, argc, argv);
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
            VALUE prev_blk_self = current_block->self;
            c->self = (VALUE)nk;
            c->current_class = nk;
            c->cref = &new_cref;
            current_block->self = (VALUE)nk;   /* class_eval semantics */
            VALUE av0[1] = { (VALUE)nk };
            korb_yield(c, 1, av0);
            current_block->self = prev_blk_self;
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
    /* Dedupe: an included module that also appears further up the
     * super chain (because the super class includes it too) should
     * only appear at its first/most-specific position. */
    while (k) {
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            VALUE v = (VALUE)k->prepends[i];
            bool dup = false;
            for (long j = 0; j < ((struct korb_array *)arr)->len; j++) {
                if (((struct korb_array *)arr)->ptr[j] == v) { dup = true; break; }
            }
            if (!dup) korb_ary_push(arr, v);
        }
        bool dup_self = false;
        for (long j = 0; j < ((struct korb_array *)arr)->len; j++) {
            if (((struct korb_array *)arr)->ptr[j] == (VALUE)k) { dup_self = true; break; }
        }
        if (!dup_self) korb_ary_push(arr, (VALUE)k);
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            VALUE v = (VALUE)k->includes[i];
            bool dup = false;
            for (long j = 0; j < ((struct korb_array *)arr)->len; j++) {
                if (((struct korb_array *)arr)->ptr[j] == v) { dup = true; break; }
            }
            if (!dup) korb_ary_push(arr, v);
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
    /* Real prepend: register the module in `prepends` so the dispatch
     * walk in korb_class_find_method finds prepended-module methods
     * BEFORE the class's own.  super from inside the prepended method
     * resolves to the class's own method via korb_class_find_super_method.
     * No method-table flattening — must stay symbolic. */
    if (SPECIAL_CONST_P(self)) return self;
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
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
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

