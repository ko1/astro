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
    /* Immediate values (true/false/nil/Integer/Symbol/Float) can't have
     * ivars; CRuby raises FrozenError ("can't modify frozen X").  Match
     * that semantic via a RuntimeError-class FrozenError. */
    if (SPECIAL_CONST_P(self) || FIXNUM_P(self) || FLONUM_P(self) || SYMBOL_P(self)) {
        VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
        const char *cn = "(special)";
        if (self == Qtrue) cn = "true";
        else if (self == Qfalse) cn = "false";
        else if (self == Qnil) cn = "nil";
        else if (FIXNUM_P(self)) cn = "Integer";
        else if (FLONUM_P(self)) cn = "Float";
        else if (SYMBOL_P(self)) cn = "Symbol";
        korb_raise(c, (struct korb_class *)eF,
                   "can't modify frozen %s", cn);
        return Qnil;
    }
    korb_ivar_set(self, name, argv[1]);
    return argv[1];
}

/* Class#class_variable_get / set / defined? / class_variables.
 * Receiver must be a Class/Module. */
static ID korb_name_to_id_(VALUE v) {
    if (SYMBOL_P(v)) return korb_sym2id(v);
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_STRING) {
        struct korb_string *s = (struct korb_string *)v;
        return korb_intern_n(s->ptr, s->len);
    }
    return 0;
}
/* Validate a class-variable name: must start with `@@` followed by at
 * least one identifier character.  Returns the ID on success, or
 * raises NameError / TypeError and returns 0. */
static ID korb_cvar_name_to_id_or_raise(CTX *c, VALUE v) {
    /* Coerce non-Symbol/String name via #to_str (CRuby semantics). */
    if (!SYMBOL_P(v) &&
        (SPECIAL_CONST_P(v) || BUILTIN_TYPE(v) != T_STRING) &&
        !SPECIAL_CONST_P(v)) {
        VALUE rt = korb_funcall(c, v, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_str")) });
        if (c->state == KORB_RAISE) return 0;
        if (RTEST(rt)) {
            v = korb_funcall(c, v, korb_intern("to_str"), 0, NULL);
            if (c->state == KORB_RAISE) return 0;
        }
    }
    const char *p; long n;
    if (SYMBOL_P(v)) {
        p = korb_id_name(korb_sym2id(v));
        n = (long)strlen(p);
    } else if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_STRING) {
        p = ((struct korb_string *)v)->ptr;
        n = ((struct korb_string *)v)->len;
    } else {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string",
                   "(arg)");
        return 0;
    }
    if (n < 3 || p[0] != '@' || p[1] != '@') {
        VALUE eN = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eN,
                   "`%.*s' is not allowed as a class variable name",
                   (int)n, p);
        return 0;
    }
    return korb_intern_n(p, n);
}
extern VALUE korb_cvar_names(struct korb_class *k);
VALUE mod_class_variable_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE)) {
        korb_raise(c, NULL, "class_variable_get: receiver must be Class/Module");
        return Qnil;
    }
    if (argc < 1) return Qnil;
    ID name = korb_cvar_name_to_id_or_raise(c, argv[0]);
    if (!name) return Qnil;
    /* Walk the receiver's class chain via the existing helper.  We
     * reuse korb_cvar_get which uses cref/current_class — set those
     * temporarily so the lookup roots at `self`. */
    struct korb_class *prev_class = c->current_class;
    struct korb_cref *prev_cref = c->cref;
    struct korb_cref tmp_cref = { .klass = (struct korb_class *)self, .prev = NULL };
    c->cref = &tmp_cref;
    c->current_class = (struct korb_class *)self;
    extern VALUE korb_cvar_get(CTX *c, ID name);
    VALUE v = korb_cvar_get(c, name);
    c->cref = prev_cref;
    c->current_class = prev_class;
    return v;
}

VALUE mod_class_variable_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE)) return Qnil;
    if (argc < 2) return Qnil;
    /* Frozen check before any side effects (CRuby semantics). */
    if (korb_obj_frozen_p(self)) {
        VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
        korb_raise(c, (struct korb_class *)eF, "can't modify frozen %s",
                   korb_id_name(korb_class_of_class(self)->name));
        return Qnil;
    }
    ID name = korb_cvar_name_to_id_or_raise(c, argv[0]);
    if (!name) return Qnil;
    extern void korb_cvar_set(CTX *c, ID name, VALUE val);
    struct korb_class *prev_class = c->current_class;
    struct korb_cref *prev_cref = c->cref;
    struct korb_cref tmp_cref = { .klass = (struct korb_class *)self, .prev = NULL };
    c->cref = &tmp_cref;
    c->current_class = (struct korb_class *)self;
    korb_cvar_set(c, name, argv[1]);
    c->cref = prev_cref;
    c->current_class = prev_class;
    return argv[1];
}

VALUE mod_class_variable_defined_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    ID name = korb_cvar_name_to_id_or_raise(c, argv[0]);
    if (!name) return Qfalse;
    if (SPECIAL_CONST_P(self) || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE)) return Qfalse;
    /* Walk super chain + transitive includes (CRuby semantics). */
    struct korb_class *root = (struct korb_class *)self;
    /* Open-coded recursive walk: use a small stack to avoid infinite
     * recursion on cyclic includes (defensive). */
    struct korb_class *stack[64];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        struct korb_class *cur = stack[--top];
        if (!cur) continue;
        for (uint32_t i = 0; i < cur->cvar_cnt; i++) {
            if (cur->cvars[i].name == name) return Qtrue;
        }
        if (cur->super && top < 63) stack[top++] = cur->super;
        for (uint32_t i = 0; i < cur->includes_cnt && top < 63; i++) {
            stack[top++] = cur->includes[i];
        }
    }
    return Qfalse;
}

/* Object#singleton_class — return the per-instance metaclass,
 * creating one if needed.  All subsequent define_method on it adds
 * a method visible only to this object. */
VALUE obj_singleton_class(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* CRuby: true / false / nil have no real singleton class; the
     * .singleton_class method returns the regular class (TrueClass /
     * FalseClass / NilClass).  Symbols and Integers raise TypeError
     * (immutable identity is shared across all instances). */
    if (NIL_P(self) || self == Qtrue || self == Qfalse) {
        return (VALUE)korb_class_of_class(self);
    }
    if (SPECIAL_CONST_P(self) && (FIXNUM_P(self) || SYMBOL_P(self) || KORB_IS_FLOAT(self))) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "can't define singleton on %s",
                   korb_id_name(korb_class_of_class(self)->name));
        return Qnil;
    }
    extern struct korb_class *korb_singleton_class_of_value(VALUE v);
    struct korb_class *meta = korb_singleton_class_of_value(self);
    if (!meta) {
        korb_raise(c, NULL, "no singleton class for %s",
                   korb_id_name(korb_class_of_class(self)->name));
        return Qnil;
    }
    return (VALUE)meta;
}

/* Class#allocate — create an instance without invoking initialize.
 * Used for serializers, Marshal.load, etc. */
VALUE class_allocate(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_CLASS) {
        korb_raise(c, NULL, "Class#allocate called on non-Class");
        return Qnil;
    }
    /* Singleton classes can't be instantiated. */
    if (((struct korb_class *)self)->basic.flags & FL_SINGLETON) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "can't create instance of singleton class");
        return Qnil;
    }
    return korb_object_new((struct korb_class *)self);
}

VALUE mod_class_variables(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE)) return korb_ary_new();
    return korb_cvar_names((struct korb_class *)self);
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
    struct korb_proc *blk = current_block;
    /* Symbol-proc / Method-proc shim handling — see obj_instance_exec
     * for the reasoning.  Without this, `obj.instance_eval(&:to_s)`
     * crashes inside korb_yield with a NULL body. */
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
        return Qnil;
    }
    VALUE prev_blk_self = blk->self;
    blk->self = self;
    /* instance_eval semantics: defs land on the receiver's singleton class.
     * Push that class at the head of the block's lexical cref chain. */
    extern struct korb_class *korb_singleton_class_of_value(VALUE v);
    struct korb_class *sing = korb_singleton_class_of_value(self);
    struct korb_cref *prev_blk_cref = blk->cref;
    struct korb_cref blk_new_cref = { .klass = sing, .prev = blk->cref };
    if (sing) blk->cref = &blk_new_cref;
    VALUE av0[1] = { self };
    VALUE r = korb_yield(c, 1, av0);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
    return r;
}

/* Object#instance_exec(*args) { |args| ... } — like instance_eval but
 * passes args to the block. */
static VALUE obj_instance_exec(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) return Qnil;
    struct korb_proc *blk = current_block;
    /* &m where m is a Method or Symbol creates a "shim" proc whose body
     * is NULL and whose self points at the underlying Method/Symbol.
     * Re-binding the self to the instance_exec receiver would lose that
     * pointer (and dispatch into a NULL body); instead, dispatch to the
     * shim's call semantics directly with the new self as the receiver. */
    if (blk->body == NULL) {
        if (SYMBOL_P(blk->self)) {
            ID name = korb_sym2id(blk->self);
            return korb_funcall(c, self, name, (uint32_t)argc, argv);
        }
        if (!SPECIAL_CONST_P(blk->self) &&
            BUILTIN_TYPE(blk->self) == T_DATA &&
            ((struct RBasic *)blk->self)->klass == (VALUE)korb_vm->method_class) {
            struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
            return korb_funcall(c, self, mo->name, (uint32_t)argc, argv);
        }
        return Qnil;
    }
    VALUE prev_blk_self = blk->self;
    blk->self = self;
    extern struct korb_class *korb_singleton_class_of_value(VALUE v);
    struct korb_class *sing = korb_singleton_class_of_value(self);
    struct korb_cref *prev_blk_cref = blk->cref;
    struct korb_cref blk_new_cref = { .klass = sing, .prev = blk->cref };
    if (sing) blk->cref = &blk_new_cref;
    VALUE r = korb_yield(c, (uint32_t)argc, argv);
    blk->cref = prev_blk_cref;
    blk->self = prev_blk_self;
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

/* Method#unbind — return an UnboundMethod (a Method-shaped record
 * whose receiver is the class instead of an instance). */
static VALUE method_unbind(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *src = (struct korb_method_obj *)self;
    struct korb_method_obj *m = korb_xmalloc(sizeof(*m));
    m->basic.flags = T_DATA;
    m->basic.klass = (VALUE)korb_vm->method_class;
    /* Drop the bound receiver — bind() will set it. */
    m->receiver = (VALUE)korb_class_of_class(src->receiver);
    m->name = src->name;
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
     * and dispatch as `m.receiver.send(m.name, *args)`.  Mirror the
     * underlying method's arity so Proc#arity / Proc#curry work. */
    struct korb_method_obj *mo = (struct korb_method_obj *)self;
    struct korb_method *km = korb_class_find_method(korb_class_of_class(mo->receiver),
                                                    mo->name);
    uint32_t pcnt = 1;
    int32_t  rest = -1;
    if (km && km->type == KORB_METHOD_AST) {
        pcnt = km->u.ast.total_params_cnt;
        rest = km->u.ast.rest_slot >= 0 ? 0 : -1;
    } else if (km && km->type == KORB_METHOD_CFUNC) {
        pcnt = (km->u.cfunc.argc < 0) ? 0 : (uint32_t)km->u.cfunc.argc;
        rest = (km->u.cfunc.argc < 0) ? 0 : -1;
    }
    struct korb_proc *p = korb_xcalloc(1, sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = (VALUE)korb_vm->proc_class;
    p->body = NULL;
    p->env = NULL;
    p->env_size = 0;
    p->params_cnt = pcnt;
    p->param_base = 0;
    p->rest_slot = rest;
    p->self = self;            /* the Method object */
    p->is_lambda = true;       /* methods have strict arity, like lambdas */
    return (VALUE)p;
}

static VALUE method_arity(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    struct korb_method *km = korb_class_find_method(korb_class_of_class(m->receiver), m->name);
    if (!km) return INT2FIX(0);
    if (km->type == KORB_METHOD_AST) {
        long req = (long)km->u.ast.required_params_cnt;
        long total = (long)km->u.ast.total_params_cnt;
        long post = (long)km->u.ast.post_params_cnt;
        bool has_rest = km->u.ast.rest_slot >= 0;
        /* opt count = total - req - post - (rest counted as 1) */
        long opt = total - req - post - (has_rest ? 1 : 0);
        bool has_opt = opt > 0;
        bool has_kwh = km->u.ast.kwh_save_slot >= 0;
        long req_total = req + post;  /* required = pre + post */
        if (has_rest || has_opt || has_kwh) return INT2FIX(-(req_total + 1));
        return INT2FIX(req_total);
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
        /* parse.c counts required_params_cnt as the *pre-rest* required
         * params (post params are tracked separately in post_params_cnt).
         * total_params_cnt = pre_req + opt + (1 if rest) + post — kwh and
         * block are NOT counted into total (they live at their own
         * dedicated slots). */
        long pre_req = (long)km->u.ast.required_params_cnt;
        long total   = (long)km->u.ast.total_params_cnt;
        long post    = (long)km->u.ast.post_params_cnt;
        long locals_cnt = (long)km->u.ast.locals_cnt;
        bool has_rest  = km->u.ast.rest_slot >= 0;
        bool has_block = km->u.ast.block_slot >= 0;
        bool has_kwh   = km->u.ast.kwh_save_slot >= 0;
        long opt_cnt = total - pre_req - post - (has_rest ? 1 : 0);
        if (opt_cnt < 0) opt_cnt = 0;
        ID *names = km->u.ast.local_names;
        /* Helper: append [kind] or [kind, name] depending on whether the
         * slot has a recoverable name in local_names.  CRuby returns the
         * tagged-name form for ordinary AST defs and the bare-kind form
         * for synthesized stubs (attr_writer, etc); we can't distinguish
         * those, so always include a name when one exists. */
        #define PUSH_PARAM(kind_str, slot)                                  \
            do {                                                              \
                VALUE _pair = korb_ary_new_capa(2);                            \
                korb_ary_push(_pair, korb_id2sym(korb_intern((kind_str))));   \
                if (names && (slot) >= 0 && (slot) < locals_cnt &&            \
                    names[(slot)] != 0) {                                     \
                    const char *_n = korb_id_name(names[(slot)]);             \
                    if (_n && _n[0] != 0 && !(_n[0] == '_' && _n[1] == 0)) {  \
                        korb_ary_push(_pair, korb_id2sym(names[(slot)]));     \
                    }                                                          \
                }                                                              \
                korb_ary_push(r, _pair);                                       \
            } while (0)
        long slot = 0;
        for (long i = 0; i < pre_req; i++) { PUSH_PARAM("req", slot); slot++; }
        for (long i = 0; i < opt_cnt; i++) { PUSH_PARAM("opt", slot); slot++; }
        if (has_rest) { PUSH_PARAM("rest", km->u.ast.rest_slot); slot++; }
        for (long i = 0; i < post; i++) { PUSH_PARAM("req", slot); slot++; }
        if (has_kwh)   { PUSH_PARAM("keyrest", km->u.ast.kwh_save_slot); }
        if (has_block) { PUSH_PARAM("block",   km->u.ast.block_slot); }
        #undef PUSH_PARAM
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

/* Method#source_location — [file, line] of the method's body node,
 * or nil for cfunc / synthesized methods. */
static VALUE method_source_location(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_method_obj *m = (struct korb_method_obj *)self;
    struct korb_class *k;
    if (!SPECIAL_CONST_P(m->receiver) &&
        (BUILTIN_TYPE(m->receiver) == T_CLASS || BUILTIN_TYPE(m->receiver) == T_MODULE)) {
        k = (struct korb_class *)m->receiver;
    } else {
        k = korb_class_of_class(m->receiver);
    }
    struct korb_method *km = korb_class_find_method(k, m->name);
    if (!km || km->type != KORB_METHOD_AST || !km->u.ast.body) return Qnil;
    struct Node *body = km->u.ast.body;
    VALUE r = korb_ary_new_capa(2);
    korb_ary_push(r, body->head.source_file ? korb_str_new_cstr(body->head.source_file) : Qnil);
    korb_ary_push(r, INT2FIX(body->head.line));
    return r;
}

/* Proc#source_location — same shape, drawn from blk->body. */
VALUE proc_source_location(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return Qnil;
    struct korb_proc *p = (struct korb_proc *)self;
    if (!p->body) return Qnil;
    VALUE r = korb_ary_new_capa(2);
    korb_ary_push(r, p->body->head.source_file ? korb_str_new_cstr(p->body->head.source_file) : Qnil);
    korb_ary_push(r, INT2FIX(p->body->head.line));
    return r;
}

/* Proc#parameters — derive from korb_proc fields.  Method-proc shims
 * (body == NULL, self is a Method object) defer to the underlying
 * method's #parameters so things like
 *   "".method(:gsub).to_proc.parameters
 * report [[:rest]] from the cfunc rather than [].  */
VALUE proc_parameters(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return korb_ary_new();
    struct korb_proc *p = (struct korb_proc *)self;
    if (p->body == NULL && !SPECIAL_CONST_P(p->self) &&
        BUILTIN_TYPE(p->self) == T_DATA &&
        ((struct RBasic *)p->self)->klass == (VALUE)korb_vm->method_class) {
        return korb_funcall(c, p->self, korb_intern("parameters"), 0, NULL);
    }
    VALUE r = korb_ary_new();
    ID *names = p->body ? korb_body_local_names(p->body) : NULL;
    /* names[] is indexed from prism's locals.ids[] (lvar 0 = first local).
     * Proc params live at fp[param_base + i] = fp[slot_base + i]; the name
     * for param i is names[i] (locals[0] is the first lvar in the proc's
     * own frame).  For rest/kwrest/blk slots the registered slot is
     * absolute (fp index), so we recover the locals index by subtracting
     * param_base. */
    uint32_t req_cnt = (p->params_cnt > p->opt_cnt) ? p->params_cnt - p->opt_cnt : 0;
    for (uint32_t i = 0; i < p->params_cnt; i++) {
        VALUE pair = korb_ary_new_capa(2);
        const char *kind;
        if (i < req_cnt) {
            kind = p->is_lambda ? "req" : "opt";
        } else {
            kind = "opt";
        }
        korb_ary_push(pair, korb_id2sym(korb_intern(kind)));
        if (names) {
            ID nm = names[i];
            const char *n = nm ? korb_id_name(nm) : "";
            if (n && n[0] != 0) {
                korb_ary_push(pair, korb_id2sym(nm));
            }
        }
        korb_ary_push(r, pair);
    }
    if (p->rest_slot >= 0 && !p->implicit_rest) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, korb_id2sym(korb_intern("rest")));
        bool added_name = false;
        if (names) {
            long li = (long)p->rest_slot - (long)p->param_base;
            if (li >= 0) {
                ID nm = names[li];
                const char *n = nm ? korb_id_name(nm) : "";
                if (n && n[0] != 0) {
                    korb_ary_push(pair, korb_id2sym(nm));
                    added_name = true;
                }
            }
        }
        /* Anonymous splat `*` — CRuby returns the literal name `:*`. */
        if (!added_name) {
            korb_ary_push(pair, korb_id2sym(korb_intern("*")));
        }
        korb_ary_push(r, pair);
    }
    /* Post params (after rest). */
    for (uint32_t i = 0; i < p->post_cnt; i++) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, korb_id2sym(korb_intern("req")));
        long abs = (long)p->param_base + (long)p->params_cnt + (p->rest_slot >= 0 ? 1 : 0) + (long)i;
        long li = abs - (long)p->param_base;
        if (names && li >= 0) {
            ID nm = names[li];
            const char *n = nm ? korb_id_name(nm) : "";
            if (n && n[0] != 0) {
                korb_ary_push(pair, korb_id2sym(nm));
            }
        }
        korb_ary_push(r, pair);
    }
    if (p->kwh_save_slot >= 0) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, korb_id2sym(korb_intern("keyrest")));
        bool added_name = false;
        if (names) {
            long li = (long)p->kwh_save_slot - (long)p->param_base;
            if (li >= 0) {
                ID nm = names[li];
                const char *n = nm ? korb_id_name(nm) : "";
                if (n && n[0] != 0) {
                    korb_ary_push(pair, korb_id2sym(nm));
                    added_name = true;
                }
            }
        }
        if (!added_name) {
            korb_ary_push(pair, korb_id2sym(korb_intern("**")));
        }
        korb_ary_push(r, pair);
    }
    /* Block parameter `&blk`: param appears at the end of the list. */
    if (p->block_slot >= 0) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, korb_id2sym(korb_intern("block")));
        bool added_name = false;
        if (names) {
            long li = (long)p->block_slot - (long)p->param_base;
            if (li >= 0) {
                ID nm = names[li];
                const char *n = nm ? korb_id_name(nm) : "";
                if (n && n[0] != 0) {
                    korb_ary_push(pair, korb_id2sym(nm));
                    added_name = true;
                }
            }
        }
        if (!added_name) {
            korb_ary_push(pair, korb_id2sym(korb_intern("&")));
        }
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
    /* CRuby: argument must be a Class or Module; otherwise TypeError. */
    if (SPECIAL_CONST_P(argv[0]) ||
        (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE)) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "class or module required");
        return Qnil;
    }
    return KORB_BOOL((VALUE)korb_class_of_class(self) == argv[0]);
}

static VALUE obj_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* default === is "object_id-or-==": same identity (a == b VALUE)
     * passes immediately, otherwise dispatch through user-defined #==.
     * This matches CRuby's Kernel#=== behavior — even if a user
     * overrides #== / #equal? to return false, identical instances
     * still match. */
    if (self == argv[0]) return Qtrue;
    return korb_funcall(c, self, korb_intern("=="), 1, argv);
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
static VALUE obj_dup_impl(CTX *c, VALUE self, bool preserve_frozen);

static VALUE obj_dup_impl_freeze(CTX *c, VALUE self, bool preserve_frozen, int freeze_arg);
static VALUE obj_clone(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* clone(freeze: nil/true/false) — `freeze: true` (default) preserves
     * the source's frozen-ness; `false` produces an unfrozen copy even if
     * the source was frozen; `nil` (only Ruby 3.0+) is the same as default
     * for koruby's purposes. */
    int freeze_arg = -1; /* -1 = default (preserve) */
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_HASH) {
        VALUE fk = korb_id2sym(korb_intern("freeze"));
        struct korb_hash *h = (struct korb_hash *)argv[0];
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (korb_eql(e->key, fk)) {
                if (e->value == Qfalse) freeze_arg = 0;
                else if (e->value == Qtrue) freeze_arg = 1;
                break;
            }
        }
    }
    return obj_dup_impl_freeze(c, self, true, freeze_arg);
}
static VALUE obj_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    return obj_dup_impl_freeze(c, self, false, -1);
}
static VALUE obj_dup_impl(CTX *c, VALUE self, bool preserve_frozen) {
    return obj_dup_impl_freeze(c, self, preserve_frozen, -1);
}
static VALUE obj_dup_impl_freeze(CTX *c, VALUE self, bool preserve_frozen, int freeze_arg) {
    if (SPECIAL_CONST_P(self)) return self;
    enum korb_type t = BUILTIN_TYPE(self);
    VALUE r = self;
    if (t == T_OBJECT) {
        struct korb_object *o = (struct korb_object *)self;
        struct korb_class *k = (struct korb_class *)o->basic.klass;
        /* dup (preserve_frozen=false) drops the singleton class — CRuby
         * dup creates a new object with the "real" class, ignoring any
         * methods/constants added on the original's singleton class.
         * clone (preserve_frozen=true) keeps the singleton class. */
        if (!preserve_frozen && k && k->name == korb_intern("(singleton)")) {
            while (k && k->name == korb_intern("(singleton)")) k = k->super;
        }
        r = korb_object_new(k);
        struct korb_object *no = (struct korb_object *)r;
        for (uint32_t i = 0; i < o->ivar_cnt && i < no->ivar_capa; i++) {
            no->ivars[i] = o->ivars[i];
        }
        if (no->ivar_cnt < o->ivar_cnt) no->ivar_cnt = o->ivar_cnt;
    } else if (t == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)self;
        r = korb_ary_new_capa(a->len);
        for (long i = 0; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    } else if (t == T_STRING) {
        r = korb_str_new(korb_str_cstr(self), korb_str_len(self));
    } else if (t == T_HASH) {
        r = korb_hash_new();
        struct korb_hash *h = (struct korb_hash *)self;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            korb_hash_aset(r, e->key, e->value);
        }
    } else if (t == T_CLASS || t == T_MODULE) {
        /* Module/Class#dup: shallow copy into a new anonymous
         * module/class.  Methods, constants, class_ivars, and class
         * variables are independent from the source after dup. */
        struct korb_class *src = (struct korb_class *)self;
        struct korb_class *nk;
        if (t == T_CLASS) {
            nk = korb_class_new(0, src->super, src->instance_type);
        } else {
            nk = korb_module_new(0);
        }
        /* Copy methods (shallow — share method body / ast nodes). */
        for (uint32_t b = 0; b < src->methods.bucket_cnt; b++) {
            for (struct korb_method_table_entry *e = src->methods.buckets[b]; e; e = e->next) {
                if (e->method) korb_class_alias_method(nk, e->name, e->method);
            }
        }
        /* Copy class variables. */
        for (uint32_t i = 0; i < src->cvar_cnt; i++) {
            if (nk->cvar_cnt >= nk->cvar_capa) {
                uint32_t nc = nk->cvar_capa ? nk->cvar_capa * 2 : 4;
                nk->cvars = korb_xrealloc(nk->cvars, nc * sizeof(*nk->cvars));
                nk->cvar_capa = nc;
            }
            nk->cvars[nk->cvar_cnt].name = src->cvars[i].name;
            nk->cvars[nk->cvar_cnt].value = src->cvars[i].value;
            nk->cvar_cnt++;
        }
        /* Copy class_ivars (e.g. `class C; @x = 1; end`). */
        for (uint32_t i = 0; i < src->class_ivar_cnt; i++) {
            if (nk->class_ivar_cnt >= nk->class_ivar_capa) {
                uint32_t nc = nk->class_ivar_capa ? nk->class_ivar_capa * 2 : 4;
                nk->class_ivars = korb_xrealloc(nk->class_ivars,
                                                 nc * sizeof(*nk->class_ivars));
                nk->class_ivar_capa = nc;
            }
            nk->class_ivars[nk->class_ivar_cnt].name = src->class_ivars[i].name;
            nk->class_ivars[nk->class_ivar_cnt].value = src->class_ivars[i].value;
            nk->class_ivar_cnt++;
        }
        /* Copy constants. */
        for (struct korb_const_entry *e = src->constants; e; e = e->next) {
            korb_const_set(nk, e->name, e->value);
        }
        /* Copy includes (shallow — share Module instances). */
        for (uint32_t i = 0; i < src->includes_cnt; i++) {
            korb_module_include(nk, src->includes[i]);
        }
        r = (VALUE)nk;
    }
    /* freeze_arg: -1 = default (preserve from source), 0 = force unfrozen,
     * 1 = force frozen.  preserve_frozen is the dup-vs-clone selector. */
    if (r != self && !SPECIAL_CONST_P(r)) {
        if (freeze_arg == 1) {
            ((struct RBasic *)r)->flags |= FL_FROZEN;
        } else if (freeze_arg == 0) {
            ((struct RBasic *)r)->flags &= ~FL_FROZEN;
        } else if (preserve_frozen && korb_obj_frozen_p(self)) {
            ((struct RBasic *)r)->flags |= FL_FROZEN;
        }
    }
    /* CRuby protocol: after the shallow copy, dispatch initialize_copy
     * (or initialize_clone for clone) on the new instance with the
     * original as the argument so user code can deep-copy / install
     * extra state.  Only fire when the user actually defined the hook
     * (the default Object#initialize_copy is a no-op). */
    if (r != self && !SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_OBJECT) {
        struct korb_class *k = korb_class_of_class(r);
        ID hook = preserve_frozen ? korb_intern("initialize_clone")
                                   : korb_intern("initialize_copy");
        if (k && korb_class_find_method(k, hook)) {
            VALUE args[1] = { self };
            korb_funcall(c, r, hook, 1, args);
            if (c->state == KORB_RAISE) return Qnil;
        }
    }
    return r;
}
static VALUE obj_instance_variables(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    if (SPECIAL_CONST_P(self)) return arr;
    /* Class / Module: their own ivars (e.g. `class C; @x = 1; end` →
     * C.instance_variables == [:@x]).  Stored on the class itself in
     * class_ivars[]. */
    if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
        struct korb_class *k = (struct korb_class *)self;
        for (uint32_t i = 0; i < k->class_ivar_cnt; i++) {
            const char *base = korb_id_name(k->class_ivars[i].name);
            if (base && base[0] == '@') {
                korb_ary_push(arr, korb_id2sym(k->class_ivars[i].name));
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "@%s", base ? base : "");
                korb_ary_push(arr, korb_id2sym(korb_intern(buf)));
            }
        }
        return arr;
    }
    if (BUILTIN_TYPE(self) != T_OBJECT) return arr;
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
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        name = korb_intern_n(((struct korb_string *)argv[0])->ptr,
                             ((struct korb_string *)argv[0])->len);
    } else {
        /* CRuby: arg must be Symbol/String or respond to #to_str.
         * Otherwise TypeError. */
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        const char *cn = SPECIAL_CONST_P(argv[0]) ? "(special)"
                              : korb_id_name(korb_class_of_class(argv[0])->name);
        korb_raise(c, (struct korb_class *)eT,
                   "%s is not a symbol nor a string", cn);
        return Qnil;
    }
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_OBJECT) return Qfalse;
    /* CRuby: ivar is "defined" once it has been set, even to nil.
     * korb_ivar_get returns Qnil for both unset and set-to-nil — use
     * korb_ivar_defined directly so we don't lose that distinction. */
    return KORB_BOOL(korb_ivar_defined(self, name));
}

