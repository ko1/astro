/* Object reflection — moved from builtins.c.
 * send / instance_variable_* / method / dup / clone / instance_variables / tap / then / itself. */

/* ---------- Object reflection ---------- */

/* Global send cache — small (klass, name) → method_cache table.
 * obj_send is shared across all `obj.send(name, args)` AST sites and
 * has no per-callsite mc, so we'd otherwise do a full method-table walk
 * on every call.  optcarrot's CPU.run does send(*DISPATCH[opcode]) for
 * every instruction, so this table is hot. */
#define KORB_SEND_CACHE_SIZE 512
struct korb_send_cache_entry {
    state_serial_t serial;
    struct korb_class *klass;
    ID name;
    struct method_cache mc;
};
static struct korb_send_cache_entry korb_send_cache[KORB_SEND_CACHE_SIZE];

extern void korb_method_cache_fill(struct method_cache *mc, struct korb_class *klass, struct korb_method *m);
extern struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name);

/* Shared implementation for send / __send__ / public_send.
 * `enforce_public` ⇒ refuse to call private/protected methods (the
 * public_send semantics).  send / __send__ ignore visibility. */
static VALUE obj_send_impl(CTX *c, VALUE self, int argc, VALUE *argv, bool enforce_public) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    /* Forward the block that was passed to send itself: `obj.send(:foo) { ... }`
     * should run with the block visible to foo's `yield`. */
    extern struct korb_proc *current_block;
    struct korb_proc *blk = current_block;
    if (enforce_public) {
        struct korb_class *klass = korb_class_of_class(self);
        struct korb_method *m = korb_class_find_method(klass, name);
        if (m && m->visibility != KORB_VIS_PUBLIC) {
            VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
            korb_raise(c, (struct korb_class *)eNo,
                     "private method '%s' called for %s",
                     korb_id_name(name), korb_id_name(klass->name));
            return Qnil;
        }
    }
    /* argv+1 is &c->fp[arg_index+1] — points into the caller's frame.
     * Translate to arg_index for the prologue path. */
    if (LIKELY(argv >= c->fp && argv < c->stack_end)) {
        struct korb_class *klass = korb_class_of_class(self);
        uint32_t slot = (uint32_t)(((uintptr_t)klass ^ (uintptr_t)name * 0x9E3779B97F4A7C15ULL) % KORB_SEND_CACHE_SIZE);
        struct korb_send_cache_entry *e = &korb_send_cache[slot];
        if (UNLIKELY(e->serial != korb_g_method_serial || e->klass != klass || e->name != name)) {
            struct korb_method *m = korb_class_find_method(klass, name);
            if (!m) return korb_funcall(c, self, name, argc - 1, argv + 1);
            korb_method_cache_fill(&e->mc, klass, m);
            e->klass = klass;
            e->name = name;
            e->serial = korb_g_method_serial;
        }
        uint32_t arg_index = (uint32_t)((argv + 1) - c->fp);
        return e->mc.prologue(c, NULL, self, (uint32_t)(argc - 1), arg_index, blk, &e->mc);
    }
    return korb_funcall(c, self, name, argc - 1, argv + 1);
}

static VALUE obj_send(CTX *c, VALUE self, int argc, VALUE *argv) {
    return obj_send_impl(c, self, argc, argv, false);
}

static VALUE obj_public_send(CTX *c, VALUE self, int argc, VALUE *argv) {
    return obj_send_impl(c, self, argc, argv, true);
}

static VALUE obj_instance_variable_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    return korb_ivar_get(self, name);
}

static VALUE obj_instance_variable_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    korb_ivar_set(self, name, argv[1]);
    return argv[1];
}

static VALUE obj_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    /* Build a Method object: a small heap struct with receiver + name. */
    struct korb_method_obj *m = korb_xmalloc(sizeof(*m));
    m->basic.flags = T_DATA;
    m->basic.klass = korb_vm->method_class
                       ? (VALUE)korb_vm->method_class
                       : (VALUE)korb_vm->object_class;
    m->receiver = self;
    m->name = name;
    return (VALUE)m;
}

/* Object#instance_eval { ... } / instance_eval(string) —
 * evaluate the block (or parsed string) with self = receiver. */
extern VALUE korb_eval_string_in_self(CTX *c, const char *src, size_t len,
                                       const char *filename, VALUE recv);
static VALUE obj_instance_eval(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        struct korb_string *s = (struct korb_string *)argv[0];
        return korb_eval_string_in_self(c, s->ptr, (size_t)s->len, "(instance_eval)", self);
    }
    if (!current_block) return Qnil;
    VALUE prev_blk_self = current_block->self;
    current_block->self = self;
    VALUE av0[1] = { self };
    VALUE r = korb_yield(c, 1, av0);
    current_block->self = prev_blk_self;
    return r;
}

/* Object#instance_exec(*args) { |args| ... } — like instance_eval but
 * passes args to the block. */
static VALUE obj_instance_exec(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) return Qnil;
    VALUE prev_blk_self = current_block->self;
    current_block->self = self;
    VALUE r = korb_yield(c, (uint32_t)argc, argv);
    current_block->self = prev_blk_self;
    return r;
}

/* Module#instance_method(name) — returns an UnboundMethod, represented
 * as a Method object whose receiver is the class itself. */
static VALUE module_instance_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                              ((struct korb_string *)argv[0])->len);
    else return Qnil;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    struct korb_method *km = korb_class_find_method((struct korb_class *)self, name);
    if (!km) {
        korb_raise(c, NULL, "undefined method '%s' for %s",
                   korb_id_name(name), korb_id_name(((struct korb_class *)self)->name));
        return Qnil;
    }
    struct korb_method_obj *m = korb_xmalloc(sizeof(*m));
    m->basic.flags = T_DATA;
    m->basic.klass = (VALUE)korb_vm->method_class;
    m->receiver = self;   /* class as "receiver" — unbound */
    m->name = name;
    return (VALUE)m;
}

/* UnboundMethod#bind(obj) — return a Method bound to obj. */
static VALUE method_bind(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    struct korb_method_obj *src = (struct korb_method_obj *)self;
    struct korb_method_obj *m = korb_xmalloc(sizeof(*m));
    m->basic.flags = T_DATA;
    m->basic.klass = (VALUE)korb_vm->method_class;
    m->receiver = argv[0];
    m->name = src->name;
    return (VALUE)m;
}

static VALUE method_call(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Method#call / Method#[] — dispatch to receiver.name(*args) */
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    return korb_funcall(c, m->receiver, m->name, argc, argv);
}

static VALUE method_to_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Method#to_proc — return a shim Proc whose body == NULL and whose
     * self is the Method object.  korb_yield_slow / proc_call detect this
     * and dispatch as `m.receiver.send(m.name, *args)`. */
    struct korb_proc *p = korb_xcalloc(1, sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = (VALUE)korb_vm->proc_class;
    p->body = NULL;
    p->env = NULL;
    p->env_size = 0;
    p->params_cnt = 1;
    p->param_base = 0;
    p->self = self;            /* the Method object */
    p->is_lambda = false;
    return (VALUE)p;
}

static VALUE method_arity(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    struct korb_method *km = korb_class_find_method(korb_class_of_class(m->receiver), m->name);
    if (!km) return INT2FIX(0);
    if (km->type == KORB_METHOD_AST) {
        long req = (long)km->u.ast.required_params_cnt;
        long total = (long)km->u.ast.total_params_cnt;
        if (km->u.ast.rest_slot >= 0 || total > req) return INT2FIX(-(req + 1));
        return INT2FIX(req);
    }
    if (km->type == KORB_METHOD_CFUNC && km->u.cfunc.argc < 0) return INT2FIX(-1);
    return INT2FIX(km->type == KORB_METHOD_CFUNC ? km->u.cfunc.argc : 0);
}

static VALUE method_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    return korb_id2sym(m->name);
}

static VALUE method_receiver(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    return m->receiver;
}

/* Method#parameters / Proc#parameters — return [[kind, name], ...].
 * koruby doesn't preserve the per-param Symbol name on korb_method
 * (only the locals_cnt and kind counts), so we emit single-element
 * arrays [:req] / [:opt] / etc.  CRuby accepts that form for
 * anonymous parameters. */
static VALUE method_params_for_method(struct korb_method *km) {
    VALUE r = korb_ary_new();
    if (!km) return r;
    if (km->type == KORB_METHOD_CFUNC) {
        int n = km->u.cfunc.argc;
        if (n < 0) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("rest")));
            korb_ary_push(r, pair);
        } else {
            for (int i = 0; i < n; i++) {
                VALUE pair = korb_ary_new_capa(1);
                korb_ary_push(pair, korb_id2sym(korb_intern("req")));
                korb_ary_push(r, pair);
            }
        }
        return r;
    }
    if (km->type == KORB_METHOD_AST) {
        long req   = (long)km->u.ast.required_params_cnt;
        long total = (long)km->u.ast.total_params_cnt;
        long post  = (long)km->u.ast.post_params_cnt;
        bool has_rest = km->u.ast.rest_slot >= 0;
        bool has_block = km->u.ast.block_slot >= 0;
        bool has_kwh = km->u.ast.kwh_save_slot >= 0;
        long opt_cnt = total - req - post - (has_rest ? 1 : 0) - (has_kwh ? 1 : 0);
        if (opt_cnt < 0) opt_cnt = 0;
        long pre_req = req - post;
        if (pre_req < 0) pre_req = 0;
        for (long i = 0; i < pre_req; i++) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("req")));
            korb_ary_push(r, pair);
        }
        for (long i = 0; i < opt_cnt; i++) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("opt")));
            korb_ary_push(r, pair);
        }
        if (has_rest) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("rest")));
            korb_ary_push(r, pair);
        }
        for (long i = 0; i < post; i++) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("req")));
            korb_ary_push(r, pair);
        }
        if (has_kwh) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("keyrest")));
            korb_ary_push(r, pair);
        }
        if (has_block) {
            VALUE pair = korb_ary_new_capa(1);
            korb_ary_push(pair, korb_id2sym(korb_intern("block")));
            korb_ary_push(r, pair);
        }
        return r;
    }
    return r;
}

static VALUE method_parameters(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    struct korb_class *k;
    if (!SPECIAL_CONST_P(m->receiver) &&
        (BUILTIN_TYPE(m->receiver) == T_CLASS || BUILTIN_TYPE(m->receiver) == T_MODULE)) {
        k = (struct korb_class *)m->receiver;
    } else {
        k = korb_class_of_class(m->receiver);
    }
    return method_params_for_method(korb_class_find_method(k, m->name));
}

/* Proc#parameters — derive from korb_proc fields. */
VALUE proc_parameters(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return korb_ary_new();
    struct korb_proc *p = (struct korb_proc *)self;
    VALUE r = korb_ary_new();
    /* Required params: params_cnt at param_base. */
    /* Lambda is strict; proc is lenient.  Same shape either way for
     * the parameter list — opts/post are not tracked separately on
     * korb_proc so emit `req` for each. */
    for (uint32_t i = 0; i < p->params_cnt; i++) {
        VALUE pair = korb_ary_new_capa(1);
        korb_ary_push(pair, korb_id2sym(korb_intern(p->is_lambda ? "req" : "opt")));
        korb_ary_push(r, pair);
    }
    if (p->rest_slot >= 0) {
        VALUE pair = korb_ary_new_capa(1);
        korb_ary_push(pair, korb_id2sym(korb_intern("rest")));
        korb_ary_push(r, pair);
    }
    if (p->kwh_save_slot >= 0) {
        VALUE pair = korb_ary_new_capa(1);
        korb_ary_push(pair, korb_id2sym(korb_intern("keyrest")));
        korb_ary_push(r, pair);
    }
    return r;
}

static VALUE method_owner(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    /* If the "receiver" is itself a class/module (UnboundMethod from
     * instance_method), search it as the lookup root rather than its
     * metaclass. */
    struct korb_class *root;
    if (!SPECIAL_CONST_P(m->receiver) &&
        (BUILTIN_TYPE(m->receiver) == T_CLASS || BUILTIN_TYPE(m->receiver) == T_MODULE)) {
        root = (struct korb_class *)m->receiver;
    } else {
        root = korb_class_of_class(m->receiver);
    }
    struct korb_method *km = korb_class_find_method(root, m->name);
    if (km && km->defining_class) return (VALUE)km->defining_class;
    return (VALUE)root;
}

static VALUE obj_instance_of_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    return KORB_BOOL((VALUE)korb_class_of_class(self) == argv[0]);
}

static VALUE obj_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* default === is == */
    return KORB_BOOL(korb_eq(self, argv[0]));
}


/* ---------- Object#tap / #then / #itself ---------- */
VALUE obj_tap(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (current_block) {
        VALUE av[1] = { self };
        korb_yield(c, 1, av);
    }
    return self;
}
VALUE obj_then(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (current_block) {
        VALUE av[1] = { self };
        return korb_yield(c, 1, av);
    }
    return self;
}
VALUE obj_itself(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }


/* ---------- Object#dup / clone / instance_variables ---------- */
static VALUE obj_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self)) return self;
    enum korb_type t = BUILTIN_TYPE(self);
    if (t == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)self;
        struct korb_class *k = (struct korb_class *)o->basic.klass;
        VALUE r = korb_object_new(k);
        struct korb_object *no = (struct korb_object *)r;
        for (uint32_t i = 0; i < o->ivar_cnt && i < no->ivar_capa; i++) {
            no->ivars[i] = o->ivars[i];
        }
        if (no->ivar_cnt < o->ivar_cnt) no->ivar_cnt = o->ivar_cnt;
        return r;
    }
    if (t == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)self;
        VALUE r = korb_ary_new_capa(a->len);
        for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
        return r;
    }
    if (t == T_STRING) {
        return korb_str_new(korb_str_cstr(self), korb_str_len(self));
    }
    if (t == T_HASH) {
        VALUE r = korb_hash_new();
        struct korb_hash *h = (struct korb_hash *)self;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            korb_hash_aset(r, e->key, e->value);
        }
        return r;
    }
    return self;
}
static VALUE obj_instance_variables(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_OBJECT) return arr;
    struct korb_object *o = (struct korb_object *)self;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    /* Only report ivars that have been set (i.e. slot has a non-Qundef
     * value).  ivar_names[i] is the name; o->ivars[i] is the value. */
    for (uint32_t i = 0; i < k->ivar_count && i < o->ivar_cnt; i++) {
        if (UNDEF_P(o->ivars[i])) continue;
        const char *base = korb_id_name(k->ivar_names[i]);
        /* The stored ID may already include the leading `@`; if not, prefix. */
        if (base && base[0] == '@') {
            korb_ary_push(arr, korb_id2sym(k->ivar_names[i]));
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "@%s", base ? base : "");
            korb_ary_push(arr, korb_id2sym(korb_intern(buf)));
        }
    }
    return arr;
}

static VALUE obj_ivar_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_OBJECT) return Qfalse;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING)
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    else return Qfalse;
    VALUE v = korb_ivar_get(self, name);
    return KORB_BOOL(!UNDEF_P(v) && !NIL_P(v));
}

