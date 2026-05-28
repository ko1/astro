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

/* Resolve an attr_*'s name argument: accept Symbol/String, fall back
 * to #to_str.  Returns 0 (and raises) on TypeError / invalid name.
 * Sets *out_id on success. */
static bool attr_resolve_name(CTX *c, VALUE arg, ID *out_id, const char *meth) {
    VALUE v = arg;
    if (!SYMBOL_P(v) && (SPECIAL_CONST_P(v) || BUILTIN_TYPE(v) != T_STRING)) {
        if (!SPECIAL_CONST_P(v)) {
            VALUE rt = korb_funcall(c, v, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return false;
            if (RTEST(rt)) {
                v = korb_funcall(c, v, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return false;
            }
        }
    }
    ID name;
    if (SYMBOL_P(v)) name = korb_sym2id(v);
    else if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_STRING) {
        name = korb_intern_n(((struct korb_string *)v)->ptr,
                             ((struct korb_string *)v)->len);
    } else {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string",
                   SPECIAL_CONST_P(arg) ? "(special)"
                       : korb_id_name(korb_class_of_class(arg)->name));
        return false;
    }
    const char *base = korb_id_name(name);
    if (!base || (!((base[0] >= 'a' && base[0] <= 'z') ||
                    (base[0] >= 'A' && base[0] <= 'Z') || base[0] == '_'))) {
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "invalid attribute name '%s'", base ? base : "");
        return false;
    }
    *out_id = name;
    (void)meth;
    return true;
}

/* Frozen check: receiver class/module must not be frozen.  Returns
 * false (and raises FrozenError) on failure. */
static bool attr_check_frozen(CTX *c, VALUE self) {
    if (korb_obj_frozen_p(self)) {
        VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
        struct korb_class *k = (struct korb_class *)self;
        const char *cn = (k->name != 0) ? korb_id_name(k->name) : "(anon)";
        korb_raise(c, (struct korb_class *)eF,
                   "can't modify frozen %s: %s",
                   BUILTIN_TYPE(self) == T_MODULE ? "Module" : "Class", cn);
        return false;
    }
    return true;
}
static VALUE module_attr_reader(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_reader: not on a class/module");
        return Qnil;
    }
    if (!attr_check_frozen(c, self)) return Qnil;
    struct korb_class *klass = (struct korb_class *)self;
    VALUE result = korb_ary_new(c, c->sp);
    for (int i = 0; i < argc; i++) {
        ID name;
        if (!attr_resolve_name(c, argv[i], &name, "attr_reader")) return Qnil;
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        NODE *body = ALLOC_node_ivar_get(iv_id);
        korb_class_add_method_ast(klass, name, body, 0, 0);
        korb_ary_push(result, korb_id2sym(name));
    }
    return result;
}

static VALUE module_attr_writer(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_writer: not on a class/module");
        return Qnil;
    }
    if (!attr_check_frozen(c, self)) return Qnil;
    struct korb_class *klass = (struct korb_class *)self;
    VALUE result = korb_ary_new(c, c->sp);
    for (int i = 0; i < argc; i++) {
        ID name;
        if (!attr_resolve_name(c, argv[i], &name, "attr_writer")) return Qnil;
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *sn = korb_xmalloc_atomic(bl + 2);
        memcpy(sn, base, bl); sn[bl] = '='; sn[bl + 1] = 0;
        ID setter_id = korb_intern(sn);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        NODE *body = ALLOC_node_ivar_set(iv_id, ALLOC_node_lvar_get(0));
        korb_class_add_method_ast(klass, setter_id, body, 1, 1);
        korb_ary_push(result, korb_id2sym(setter_id));
    }
    return result;
}

static VALUE module_attr_accessor(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r1 = module_attr_reader(c, self, argc, argv);
    if (c->state != KORB_NORMAL) return Qnil;
    VALUE r2 = module_attr_writer(c, self, argc, argv);
    if (c->state != KORB_NORMAL) return Qnil;
    /* Interleave readers and writers like CRuby: [a, a=, b, b=]. */
    VALUE result = korb_ary_new(c, c->sp);
    if (BUILTIN_TYPE(r1) == T_ARRAY && BUILTIN_TYPE(r2) == T_ARRAY) {
        struct korb_array *a1 = (struct korb_array *)r1;
        struct korb_array *a2 = (struct korb_array *)r2;
        long n = a1->len;
        if (a2->len < n) n = a2->len;
        for (long i = 0; i < n; i++) {
            korb_ary_push(result, a1->ptr[i]);
            korb_ary_push(result, a2->ptr[i]);
        }
    }
    return result;
}

static VALUE module_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    
    /* Top-level `include M` forwards to Object — that's how a file's
     * `include ConstantSpecs::ModuleA` (no enclosing class/module)
     * makes M's constants reachable as toplevel constants. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_OBJECT &&
            self == korb_vm->main_obj) {
            self = (VALUE)korb_vm->object_class;
        } else {
            return self;
        }
    }
    /* Simplified include: copy module's methods/constants into the class.
     * Real Ruby inserts the module into the ancestor chain; we flatten. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
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
            korb_funcall(c, argv[i], korb_intern("included"), 1, &klass_v);
            if (c->state == KORB_RAISE) return Qnil;
        }
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
    
    struct korb_proc *p;
    /* UnboundMethod / Method whose receiver is a class — install the
     * underlying method directly into self by alias.  This preserves
     * the original method's body / arity / locals_cnt instead of
     * routing through a Proc shim that would re-dispatch through the
     * class itself (wrong receiver). */
    if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) &&
        BUILTIN_TYPE(argv[1]) == T_DATA &&
        ((struct RBasic *)argv[1])->klass == (VALUE)korb_vm->method_class) {
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
                return korb_id2sym(name);
            }
        }
        /* Bound Method (receiver is an instance): fall back to the
         * Proc-shim path so `m.receiver.send(m.name, *args)` runs. */
        VALUE pr = korb_funcall(c, argv[1], korb_intern("to_proc"), 0, NULL);
        if (BUILTIN_TYPE(pr) != T_PROC) return Qnil;
        p = (struct korb_proc *)pr;
    } else if (argc >= 2 && !SPECIAL_CONST_P(argv[1]) && BUILTIN_TYPE(argv[1]) == T_PROC) {
        p = (struct korb_proc *)argv[1];
    } else if (c->current_block) {
        p = c->current_block;
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
    /* Uninitialized class (`Class.allocate`) has no super yet — CRuby
     * raises TypeError on #superclass.  Detect via our sentinel name. */
    if (k->super == NULL && k->name == korb_intern("(uninitialized)")) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "uninitialized class");
        return Qnil;
    }
    return k->super ? (VALUE)k->super : Qnil;
}

/* Module#instance_methods([include_inherited=true]) — sym list. */
static VALUE module_instance_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        return korb_ary_new(c, c->sp);
    }
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *root = (struct korb_class *)self;
    VALUE r = korb_ary_new(c, c->sp);
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
/* Helper: collect methods of `vis` visibility from the receiver's class
 * chain.  vis = -1 means "all public + protected" (default for #methods).
 * vis = KORB_VIS_PUBLIC / PRIVATE / PROTECTED selects exactly that set. */
static VALUE methods_with_visibility(VALUE self, int vis, bool include_inherited) {
    CTX *c = korb_vm->current_ctx;
    struct korb_class *k = korb_class_of_class(self);
    VALUE r = korb_ary_new(c, c->sp);
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
static VALUE obj_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return methods_with_visibility(self, -1, include_inherited);
}
static VALUE obj_public_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return methods_with_visibility(self, KORB_VIS_PUBLIC, include_inherited);
}
static VALUE obj_private_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return methods_with_visibility(self, KORB_VIS_PRIVATE, include_inherited);
}
static VALUE obj_protected_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    return methods_with_visibility(self, KORB_VIS_PROTECTED, include_inherited);
}

/* Object#singleton_methods — methods defined directly on this object's
 * singleton class (not inherited from regular class). */
static VALUE obj_singleton_methods(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_ary_new(c, c->sp);
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

/* Module#method_defined?(name [, inherit=true]) — true for public/
 * protected.  When inherit is false, only the receiver's own method
 * table (and its prepends/includes) is consulted, not super classes. */
static VALUE module_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
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
    if (!m) return Qfalse;
    return KORB_BOOL(m->visibility != KORB_VIS_PRIVATE);
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
static VALUE module_public_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PUBLIC);
}

static VALUE module_private_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PRIVATE);
}

static VALUE module_protected_method_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    bool inherit = (argc < 2) || RTEST(argv[1]);
    struct korb_method *m = find_method_with_inherit((struct korb_class *)self, name, inherit);
    return KORB_BOOL(m && m->visibility == KORB_VIS_PROTECTED);
}

/* Module#private_instance_methods / public_instance_methods /
 * protected_instance_methods — visibility-filtered list.  Only own
 * methods (no inherited) since the existing instance_methods walks
 * super for inherited. */
static VALUE module_methods_by_vis(CTX *c, VALUE self, int argc, VALUE *argv,
                                     enum korb_visibility vis) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return korb_ary_new(c, c->sp);
    bool include_inherited = (argc < 1) || RTEST(argv[0]);
    struct korb_class *k = (struct korb_class *)self;
    VALUE r = korb_ary_new(c, c->sp);
    bool first = true;
    while (k) {
        for (uint32_t b = 0; b < k->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = k->methods.buckets[b]; e; e = e->next) {
                /* When inherit=false, only methods defined DIRECTLY on
                 * this class count — drop entries that came from an
                 * included module (include_depth > 0). */
                if (!include_inherited && first && e->include_depth > 0) continue;
                if (e->method && e->method->visibility == vis) korb_ary_push(r, korb_id2sym(e->name));
            }
        }
        first = false;
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
        return korb_ary_new(c, c->sp);
    }
    VALUE r = korb_ary_new(c, c->sp);
    for (struct korb_const_entry *e = ((struct korb_class *)self)->constants; e; e = e->next) {
        korb_ary_push(r, korb_id2sym(e->name));
    }
    return r;
}

/* Module#class_eval(string) / Module#class_eval { ... } — evaluate
 * the source string or block with self = the module. */
static VALUE module_class_eval(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    
    /* String-form: eval source in the module's context. */
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) &&
        BUILTIN_TYPE(argv[0]) == T_STRING) {
        extern NODE *koruby_parse(const char *src, size_t len, const char *filename);
        struct korb_string *s = (struct korb_string *)argv[0];
        struct korb_class *klass = (struct korb_class *)self;
        NODE *ast = koruby_parse(s->ptr, (size_t)s->len, "(eval)");
        if (!ast) return Qnil;
        VALUE *prev_fp = c->current_frame->fp;
        VALUE prev_self = c->current_frame->self;
        struct korb_class *prev_class = c->current_frame->current_class;
        struct korb_cref *prev_cref = c->current_frame->cref;
        c->current_frame->fp = c->sp + 1;
        c->current_frame->self = self;
        c->current_frame->current_class = klass;
        struct korb_cref top = { .klass = klass, .prev = NULL };
        c->current_frame->cref = &top;
        OPTIMIZE(ast);
        VALUE r = EVAL(c, ast, c->current_frame->fp);
        c->current_frame->fp = prev_fp;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        return r;
    }
    if (!c->current_block) return self;
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
            ((struct RBasic *)blk->self)->klass == (VALUE)korb_vm->method_class) {
            struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
            return korb_funcall(c, self, mo->name, 0, NULL);
        }
        return self;
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
    VALUE r = korb_yield(c, 1, av0);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
    c->current_frame->self = prev_self;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    if (c->state == KORB_BREAK) { c->state = KORB_NORMAL; c->state_value = Qnil; }
    return r;
}

/* Module#class_exec(*args) { |*args| ... } — like class_eval but
 * passes args to the block.  module_exec is just an alias. */
static VALUE module_class_exec(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    
    if (!c->current_block) return self;
    struct korb_proc *blk = c->current_block;
    if (blk->body == NULL) {
        if (SYMBOL_P(blk->self)) {
            return korb_funcall(c, self, korb_sym2id(blk->self), (uint32_t)argc, argv);
        }
        if (!SPECIAL_CONST_P(blk->self) &&
            BUILTIN_TYPE(blk->self) == T_DATA &&
            ((struct RBasic *)blk->self)->klass == (VALUE)korb_vm->method_class) {
            struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
            return korb_funcall(c, self, mo->name, (uint32_t)argc, argv);
        }
        return self;
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
    VALUE r = korb_yield(c, (uint32_t)argc, argv);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
    c->current_frame->self = prev_self;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    if (c->state == KORB_BREAK) { c->state = KORB_NORMAL; c->state_value = Qnil; }
    return r;
}

/* Module#< — true if self is a subclass/submodule of other. */
extern bool korb_module_has_ancestor(struct korb_class *, struct korb_class *);
static VALUE module_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(argv[0]) ||
        (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "compared with non class/module");
        return Qnil;
    }
    if (self == argv[0]) return Qfalse;  /* CRuby: same → false for `<` */
    struct korb_class *target = (struct korb_class *)argv[0];
    /* Use transitive include walk. */
    if (korb_module_has_ancestor((struct korb_class *)self, target)) return Qtrue;
    if (korb_module_has_ancestor(target, (struct korb_class *)self)) return Qfalse;
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
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(argv[0]) ||
        (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "compared with non class/module");
        return Qnil;
    }
    if (self == argv[0]) return Qfalse;
    VALUE swap[1] = {self};
    return module_lt(c, argv[0], 1, swap);
}
static VALUE module_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (self == argv[0]) return Qtrue;
    return module_gt(c, self, argc, argv);
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
static VALUE class_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Class === obj  ⇔ obj.is_a?(self).  Walks super chain + transitive
     * includes (Module includes Module).  `M === obj.extend(M)` and
     * `Basic === Child.new` (Child includes Super, Super includes Basic)
     * both return true. */
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    struct korb_class *target = (struct korb_class *)self;
    return KORB_BOOL(korb_module_has_ancestor(korb_class_of_class(argv[0]), target));
}


/* ---------- Class.new etc ---------- */
static VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) {
        korb_raise(c, NULL, "Class.new called on non-class");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    /* Singleton classes can't be instantiated — CRuby raises TypeError. */
    if (klass->basic.head.flags & FL_SINGLETON) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "can't create instance of singleton class");
        return Qnil;
    }
    /* Reject .new on an uninitialized class (the result of Class.allocate
     * before its super has been wired up).  Without this, dispatching to
     * .allocate or .initialize on a half-built class crashes deep in
     * method lookup.  CRuby raises TypeError here. */
    if (klass->name == korb_intern("(uninitialized)") && klass->super == NULL) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "can't create instance of uninitialized class");
        return Qnil;
    }
    /* Class.new(superclass = Object) — create an anonymous subclass. */
    if (klass == korb_vm->class_class) {
        struct korb_class *super = korb_vm->object_class;
        if (argc >= 1) {
            /* Reject non-Class superclass with TypeError. */
            if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_CLASS) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "superclass must be an instance of Class (given an instance of %s)",
                           korb_id_name(korb_class_of_class(argv[0])->name));
                return Qnil;
            }
            super = (struct korb_class *)argv[0];
            /* Reject metaclass (FL_SINGLETON) — CRuby raises TypeError
             * "can't make subclass of singleton class". */
            if (super->basic.head.flags & FL_SINGLETON) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "can't make subclass of singleton class");
                return Qnil;
            }
            /* Reject an uninitialized superclass — same TypeError as
             * `klass.new` on it.  Uninitialized classes have no super
             * field set up yet, so subclassing them would inherit a
             * broken chain. */
            if (super->super == NULL &&
                super->name == korb_intern("(uninitialized)")) {
                VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                korb_raise(c, (struct korb_class *)eT,
                           "can't inherit uninitialized class");
                return Qnil;
            }
        }
        struct korb_class *nk = korb_class_new(c, c->sp, korb_intern("(anon)"),
                                               super, super->instance_type);
        /* Fire `inherited` on the parent — same as `class C < P; end`. */
        {
            struct korb_class *super_meta = korb_class_of_class((VALUE)super);
            if (super_meta && korb_class_find_method(super_meta, korb_intern("inherited"))) {
                VALUE child_v = (VALUE)nk;
                korb_funcall(c, (VALUE)super, korb_intern("inherited"), 1, &child_v);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        
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
            korb_yield(c, 1, av0);
            c->current_block->cref = prev_blk_cref;
            c->current_block->self = prev_blk_self;
            c->current_frame->self = prev_self;
            c->current_frame->current_class = prev_class;
            c->current_frame->cref = prev_cref;
            if (c->state == KORB_BREAK) {
                c->state = KORB_NORMAL; c->state_value = Qnil;
            }
        }
        return (VALUE)nk;
    }
    /* Park klass + obj across alloc-can-GC: korb_object_new can move
     * klass, and find_method then derefs stale klass → SEGV under PURGE
     * (or garbage under STRESS).  Use ARO_ROOT_SCOPE so both survive. */
    
    VALUE obj;
    ARO_ROOT_SCOPE_START(c, rs, 2) {
        rs[0] = (VALUE)klass;
        rs[1] = korb_object_new(c, c->sp, (struct korb_class *)rs[0]);
        struct korb_method *m = korb_class_find_method((struct korb_class *)rs[0], id_initialize);
        VALUE *fp_lo = c->current_frame->fp;
        VALUE *fp_hi = c->sp;
        if (m) {
            VALUE blk = c->current_block ? (VALUE)c->current_block : Qnil;
            korb_funcall_with_block(c, rs[1], id_initialize, argc, argv, blk);
        }
        obj = rs[1];
        (void)fp_lo; (void)fp_hi;
    } ARO_ROOT_SCOPE_END(c, rs);
    VALUE *fp_lo = c->current_frame->fp;
    VALUE *fp_hi = c->sp;
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
    return obj;
}

static VALUE class_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* CRuby: anonymous Class/Module returns nil; named ones return the
     * registered name string. */
    struct korb_class *k = (struct korb_class *)self;
    if (!k->name || k->name == korb_intern("(anon)")) return Qnil;
    return korb_str_new_cstr(c, c->sp, korb_id_name(k->name));
}

/* (Array#hash folded into builtins/array.c) */

/* ---------- Class#ancestors / Module#prepend ---------- */

/* Append `m` and its transitive included modules to `arr`, dedup'd. */
static void ancestors_push_module(VALUE arr, struct korb_class *m) {
    if (!m) return;
    struct korb_array *a = (struct korb_array *)arr;
    for (long j = 0; j < a->len; j++) {
        if (a->ptr[j] == (VALUE)m) return;
    }
    korb_ary_push(arr, (VALUE)m);
    /* Recurse into m's own includes (latest-include first to match
     * CRuby's "module that is included later sits earlier in
     * ancestors"). */
    for (int32_t i = (int32_t)m->includes_cnt - 1; i >= 0; i--) {
        ancestors_push_module(arr, m->includes[i]);
    }
}

static VALUE class_ancestors(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new(c, c->sp);
    if (SPECIAL_CONST_P(self)) return arr;
    struct korb_class *k = (struct korb_class *)self;
    while (k) {
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            ancestors_push_module(arr, k->prepends[i]);
        }
        ancestors_push_module(arr, k);
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            ancestors_push_module(arr, k->includes[i]);
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
            meta = korb_class_new(c, c->sp, korb_intern("(singleton)"), cur, cur ? cur->instance_type : T_OBJECT);
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
    /* Fire `extended` hook on each module (defined as a class method
     * on the module).  CRuby calls hooks after the inclusion takes
     * effect, mirroring how `included` is invoked. */
    for (int i = 0; i < argc; i++) {
        if (SPECIAL_CONST_P(argv[i])) continue;
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        struct korb_class *meta = korb_class_of_class(argv[i]);
        if (meta && korb_class_find_method(meta, korb_intern("extended"))) {
            VALUE obj_v = self;
            korb_funcall(c, argv[i], korb_intern("extended"), 1, &obj_v);
            if (c->state == KORB_RAISE) return Qnil;
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
/* Module#remove_const(:NAME) — remove a constant from the module.
 * Returns the previous value, or raises NameError if missing. */
static VALUE module_remove_const(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 1) return Qnil;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              (BUILTIN_TYPE(argv[0]) == T_STRING ?
               korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len) : 0);
    if (!name) return Qnil;
    struct korb_class *k = (struct korb_class *)self;
    /* Walk the linked list of constants; unlink and return value. */
    extern bool korb_const_remove(struct korb_class *k, ID name, VALUE *out);
    VALUE prev = Qnil;
    if (!korb_const_remove(k, name, &prev)) {
        VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eName,
                   "constant %s::%s not defined",
                   korb_id_name(k->name), korb_id_name(name));
        return Qnil;
    }
    return prev;
}

/* Module#remove_class_variable(:@@name) — remove a cvar from the
 * module's own table.  Returns the previous value or raises NameError. */
static VALUE module_remove_class_variable(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 1) return Qnil;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) :
              (BUILTIN_TYPE(argv[0]) == T_STRING ?
               korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len) : 0);
    if (!name) return Qnil;
    struct korb_class *k = (struct korb_class *)self;
    for (uint32_t i = 0; i < k->cvar_cnt; i++) {
        if (k->cvars[i].name == name) {
            VALUE prev = k->cvars[i].value;
            for (uint32_t j = i + 1; j < k->cvar_cnt; j++) k->cvars[j-1] = k->cvars[j];
            k->cvar_cnt--;
            return prev;
        }
    }
    VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
    korb_raise(c, (struct korb_class *)eName,
               "class variable %s not defined for %s",
               korb_id_name(name), korb_id_name(k->name));
    return Qnil;
}

/* Module#private_class_method(:foo, ...) — mark singleton method as
 * private.  Stub-level: just set visibility flag.  We keep things
 * simple — return self regardless. */
static VALUE class_visibility_set(CTX *c, VALUE self, int argc, VALUE *argv,
                                  enum korb_visibility v) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    extern struct korb_class *korb_singleton_class_of(struct korb_class *);
    struct korb_class *meta = korb_singleton_class_of((struct korb_class *)self);
    /* Single-Array form: `private_class_method([:foo, :bar])` (Ruby 3.x). */
    if (argc == 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        argv = a->ptr;
        argc = (int)a->len;
    }
    for (int i = 0; i < argc; i++) {
        VALUE arg = argv[i];
        if (!SYMBOL_P(arg) &&
            (SPECIAL_CONST_P(arg) || BUILTIN_TYPE(arg) != T_STRING) &&
            !SPECIAL_CONST_P(arg)) {
            VALUE rt = korb_funcall(c, arg, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
            if (c->state == KORB_RAISE) return Qnil;
            if (RTEST(rt)) {
                arg = korb_funcall(c, arg, korb_intern("to_str"), 0, NULL);
                if (c->state == KORB_RAISE) return Qnil;
            }
        }
        ID name = 0;
        if (SYMBOL_P(arg)) name = korb_sym2id(arg);
        else if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_STRING)
            name = korb_intern_n(((struct korb_string *)arg)->ptr,
                                 ((struct korb_string *)arg)->len);
        if (!name) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "%s is not a symbol nor a string",
                       SPECIAL_CONST_P(argv[i]) ? "(special)"
                           : korb_id_name(korb_class_of_class(argv[i])->name));
            return Qnil;
        }
        struct korb_method *m = meta ? korb_class_find_method(meta, name) : NULL;
        if (!m) {
            VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
            const char *cn = (((struct korb_class *)self)->name)
                              ? korb_id_name(((struct korb_class *)self)->name) : "(anon)";
            korb_raise(c, (struct korb_class *)eN,
                       "undefined method '%s' for class '%s'",
                       korb_id_name(name), cn);
            return Qnil;
        }
        m->visibility = v;
    }
    return self;
}
static VALUE module_private_class_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    return class_visibility_set(c, self, argc, argv, KORB_VIS_PRIVATE);
}

static VALUE module_public_class_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    return class_visibility_set(c, self, argc, argv, KORB_VIS_PUBLIC);
}

/* Module#private_constant / public_constant — visibility on constants. */
static VALUE module_private_constant(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
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
            VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
            korb_raise(c, (struct korb_class *)eN,
                       "constant %s::%s not defined",
                       k->name ? korb_id_name(k->name) : "?", korb_id_name(name));
            return Qnil;
        }
    }
    return self;
}
static VALUE module_public_constant(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
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
    return self;
}

/* Module.nesting — array of the lexically enclosing class/module
 * stack, innermost first.  Just walks c->current_frame->cref which already mirrors
 * the source's `module M; class C; ...; end; end` nesting. */
static VALUE module_class_nesting(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new(c, c->sp);
    for (struct korb_cref *cur = c->current_frame->cref; cur; cur = cur->prev) {
        if (cur->klass && cur->klass != korb_vm->object_class) {
            korb_ary_push(arr, (VALUE)cur->klass);
        }
    }
    return arr;
}

static VALUE module_new_class_func(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Module.new — create an anonymous module.  If a block is given,
     * evaluate it with self = the new module (lets `include`/method defs
     * land on the new module). */
    struct korb_class *m = korb_module_new(c, c->sp, korb_intern("(anon)"));
    
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
        korb_yield(c, 1, argv0);
        c->current_block->cref = prev_blk_cref;
        c->current_block->self = prev_blk_self;
        c->current_frame->self = prev_self;
        c->current_frame->current_class = prev_class;
        c->current_frame->cref = prev_cref;
        if (c->state == KORB_BREAK) {
            c->state = KORB_NORMAL; c->state_value = Qnil;
        }
    }
    return (VALUE)m;
}

