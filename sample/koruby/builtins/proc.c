/* Proc / Method — moved from builtins.c. */

/* ---------- Proc ---------- */
extern VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv);

/* Proc#lambda? — true for ->{} / lambda{}, false for Proc.new / { } blocks. */
static VALUE proc_lambda_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return Qfalse;
    return KORB_BOOL(((struct korb_proc *)self)->is_lambda);
}

/* Proc#arity — params_cnt; -(required+1) for blocks with *rest. */
static VALUE proc_arity(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return INT2FIX(0);
    struct korb_proc *p = (struct korb_proc *)self;
    if (!p->body) return INT2FIX(0);
    if (p->rest_slot >= 0) return INT2FIX(-((long)p->params_cnt + 1));
    return INT2FIX((long)p->params_cnt);
}

/* Proc#== — same Proc identity. */
static VALUE proc_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(self == argv[0]);
}

/* Proc.new — captures the current block as a Proc. */
static VALUE proc_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) {
        korb_raise(c, NULL, "tried to create Proc object without a block");
        return Qnil;
    }
    return (VALUE)current_block;
}

VALUE proc_call(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Proc#call is the "escape" path: the proc may be invoked long after
     * its enclosing scope is gone, so we cannot share its env with that
     * scope's stack slots.  Push a fresh frame on top of the current sp,
     * snapshot the captured env into it, then evaluate the body. */
    if (BUILTIN_TYPE(self) != T_PROC) return Qnil;
    struct korb_proc *p = (struct korb_proc *)self;
    /* Symbol-proc shim: created by Symbol#to_proc; dispatch as
     * `argv[0].send(symbol, *rest)`. */
    if (p->body == NULL && SYMBOL_P(p->self)) {
        if (argc < 1) {
            korb_raise(c, NULL, "no receiver for symbol proc");
            return Qnil;
        }
        ID name = korb_sym2id(p->self);
        return korb_funcall(c, argv[0], name, argc - 1, argv + 1);
    }
    /* Method-proc shim: created by Method#to_proc; dispatch as
     * `m.receiver.send(m.name, *args)`. */
    if (p->body == NULL && !SPECIAL_CONST_P(p->self) &&
        BUILTIN_TYPE(p->self) == T_DATA &&
        ((struct RBasic *)p->self)->klass == (VALUE)korb_vm->method_class) {
        struct korb_method_obj *m = (struct korb_method_obj *)p->self;
        return korb_funcall(c, m->receiver, m->name, argc, argv);
    }
    /* Lambda is strict: argc must match params_cnt (or be in
     * required..total range when rest/optional are present).
     * Exception: when the lambda accepts kwargs (kwh_save_slot >= 0),
     * an extra trailing Hash is consumed as kwargs and doesn't count
     * toward the positional arity check. */
    if (p->is_lambda && p->rest_slot < 0) {
        int eff_argc = argc;
        if (p->kwh_save_slot >= 0 && argc > 0 &&
            !SPECIAL_CONST_P(argv[argc - 1]) &&
            BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
            eff_argc = argc - 1;
        }
        if ((uint32_t)eff_argc != p->params_cnt) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                     "wrong number of arguments (given %d, expected %u)",
                     eff_argc, p->params_cnt);
            return Qnil;
        }
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    VALUE *new_fp = c->sp + 1;
    if (new_fp + p->env_size + 16 >= c->stack_end) {
        korb_raise(c, NULL, "stack overflow (proc call)");
        return Qnil;
    }
    /* Snapshot env */
    for (uint32_t i = 0; i < p->env_size; i++) new_fp[i] = p->env[i];
    /* Kwargs peel: if block declares kwargs and last arg is a Hash,
     * stash it; otherwise default to {}. */
    VALUE peeled_kwh = Qundef;
    if (p->kwh_save_slot >= 0) {
        if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
            BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
            peeled_kwh = argv[argc - 1];
            argc--;
        } else {
            peeled_kwh = korb_hash_new();
        }
    }
    /* Copy params — Ruby block calling convention: when called with a
     * single Array argument and the block declares >1 param, the array
     * is auto-destructured into individual params (so blk.call([1,2])
     * with `|a, b|` binds a=1, b=2). */
    if (argc == 1 && p->params_cnt > 1 && p->rest_slot < 0 &&
        !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        for (uint32_t i = 0; i < p->params_cnt; i++) {
            new_fp[p->param_base + i] = (i < (uint32_t)a->len) ? a->ptr[i] : Qnil;
        }
    } else {
        /* Required params first. */
        for (int i = 0; i < argc && (uint32_t)i < p->params_cnt; i++) {
            new_fp[p->param_base + i] = argv[i];
        }
        for (uint32_t i = (uint32_t)(argc < (int)p->params_cnt ? argc : (int)p->params_cnt);
             i < p->params_cnt; i++) {
            new_fp[p->param_base + i] = Qnil;
        }
        /* *rest: gather remaining args into rest_slot. */
        if (p->rest_slot >= 0) {
            VALUE rest = korb_ary_new();
            for (int i = (int)p->params_cnt; i < argc; i++) korb_ary_push(rest, argv[i]);
            new_fp[p->rest_slot] = rest;
        }
    }
    /* Stash kwh into save slot. */
    if (p->kwh_save_slot >= 0 && !UNDEF_P(peeled_kwh)) {
        new_fp[p->kwh_save_slot] = peeled_kwh;
    }
    c->fp = new_fp;
    if (c->fp + p->env_size > c->sp) c->sp = c->fp + p->env_size;
    c->self = p->self;
    /* yield inside the proc body targets the enclosing method's block
     * captured at proc creation time. */
    extern struct korb_proc *current_block;
    struct korb_proc *prev_block = current_block;
    current_block = p->enclosing_block;
    VALUE r = EVAL(c, p->body);
    current_block = prev_block;
    /* Snapshot any returned proc whose env points into our about-to-be-
     * popped frame. */
    korb_proc_snapshot_env_if_in_frame(r, new_fp, new_fp + p->env_size);
    if (c->state == KORB_RETURN || c->state == KORB_BREAK) {
        korb_proc_snapshot_env_if_in_frame(c->state_value, new_fp, new_fp + p->env_size);
    }
    c->fp = prev_fp;
    c->self = prev_self;
    /* Lambda: `return` inside the body targets the lambda itself, so we
     * consume it here and the caller sees the value as the call's result.
     * Plain Proc: `return` is non-local — let it propagate up to the
     * lexically-enclosing method, where it'll be consumed at that
     * method's prologue. */
    if (c->state == KORB_BREAK) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    } else if (c->state == KORB_RETURN && p->is_lambda) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

