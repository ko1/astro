/* Proc / Method — moved from builtins.c. */

/* ---------- Proc ---------- */
extern VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv);

/* Proc#lambda? — true for ->{} / lambda{}, false for Proc.new / { } blocks. */
static VALUE proc_lambda_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return Qfalse;
    return KORB_BOOL(((struct korb_proc *)self)->is_lambda);
}

/* Proc#arity — count of required positional args (req + post).
 * Non-lambda procs: opt-only blocks return non-negative arity (req).
 * Any *rest makes it negative: -(req + 1).
 * Lambdas: opt also makes it negative. */
static VALUE proc_arity(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return INT2FIX(0);
    struct korb_proc *p = (struct korb_proc *)self;
    if (!p->body && p->params_cnt == 0 && p->rest_slot < 0) return INT2FIX(0);
    long required = (long)p->params_cnt - (long)p->opt_cnt + (long)p->post_cnt;
    if (required < 0) required = 0;
    bool has_rest = (p->rest_slot >= 0);
    bool has_opt = (p->opt_cnt > 0);
    if (has_rest) return INT2FIX(-(required + 1));
    if (p->is_lambda && has_opt) return INT2FIX(-(required + 1));
    return INT2FIX(required);
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
    } else if (p->is_lambda) {
        /* Lambda with rest: lower bound on required positional args. */
        int eff_argc = argc;
        if (p->kwh_save_slot >= 0 && argc > 0 &&
            !SPECIAL_CONST_P(argv[argc - 1]) &&
            BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
            eff_argc = argc - 1;
        }
        uint32_t required = (p->params_cnt > p->opt_cnt)
                             ? p->params_cnt - p->opt_cnt + p->post_cnt
                             : p->post_cnt;
        if ((uint32_t)eff_argc < required) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                     "wrong number of arguments (given %d, expected %u+)",
                     eff_argc, required);
            return Qnil;
        }
    }
    VALUE *prev_fp = c->fp;
    VALUE *prev_sp = c->sp;
    VALUE prev_self = c->self;
    /* Use the proc's own captured env directly so writes to closure
     * variables (`r = ...` inside the block) reach the outer scope.
     * korb_proc_snapshot_env_if_in_frame already detaches env to heap
     * when the enclosing method's frame goes away, so by the time
     * proc.call runs the env is safe to share.
     *
     * Slot-collision guard: if a method is currently active above env
     * (prev_fp lies inside env's slot range), the block's own slot
     * range [param_base, env_size) overlaps that method's locals.
     * Clone env to a fresh location above c->sp, run there, then write
     * back the closure-captured slots [0, param_base) so outer-scope
     * assignments survive. */
    VALUE *new_fp = p->env;
    VALUE *fresh_env = NULL;     /* non-null when we cloned */
    if (UNLIKELY(!new_fp)) {
        korb_raise(c, NULL, "proc with no env");
        return Qnil;
    }
    /* Slot collision guard: when prev_fp is inside the env's range
     * (a method's frame sits on top of env), the block body's nested
     * method calls would write into prev_fp's locals.  Clone env
     * to a fresh location above c->sp, then write back the closure-
     * captured slots [0, param_base) on return.  Don't clone when
     * prev_fp IS the env (the block is being called from within its
     * defining scope — closure writes propagate naturally) — that
     * case is also where nested-proc curry chains depend on direct
     * env aliasing for n-deep variable lookup to work.
     *
     * Additionally clone when env lies outside the current value-stack
     * range — this happens when a Proc captured outside a Fiber is
     * called from inside it: fp would jump out of the fiber's stack
     * and stack_end checks would misfire (false stack-overflow). */
    bool env_outside_stack = (new_fp < c->stack_base || new_fp >= c->stack_end);
    bool method_overlaps_env = (prev_fp && prev_fp != new_fp &&
                                prev_fp >= new_fp && prev_fp <= new_fp + p->env_size);
    /* Self-recursion: a proc/lambda is being called from inside its own
     * body (or a callee that hasn't returned yet).  prev_fp == new_fp
     * means the same env slots are about to be overwritten — including
     * the body's own locals, which must be per-call.  Clone so each
     * invocation gets a fresh local-vars region; closure-captured outer
     * vars still propagate via the writeback step below. */
    bool self_recursion = (prev_fp == new_fp && new_fp != NULL);
    if (method_overlaps_env || env_outside_stack || self_recursion) {
        fresh_env = c->sp;
        for (uint32_t i = 0; i < p->env_size; i++) fresh_env[i] = new_fp[i];
        c->sp = fresh_env + p->env_size;
        new_fp = fresh_env;
    }
    /* Kwargs peel: if block declares kwargs and last arg is a kwargs-
     * tagged Hash (FL_KWARGS — set by `m(**h)` / `m(k: v)`), stash it.
     * Plain positional Hash is NOT peeled (Ruby 3 separation). */
    VALUE peeled_kwh = Qundef;
    if (p->kwh_save_slot >= 0) {
        if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
            BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
            (RBASIC(argv[argc - 1])->flags & FL_KWARGS)) {
            peeled_kwh = argv[argc - 1];
            argc--;
        } else {
            peeled_kwh = korb_hash_new();
        }
    }
    /* If last positional is a kwargs-tagged empty Hash and callee has
     * no **kwargs, drop it. */
    if (p->kwh_save_slot < 0 && argc > 0 &&
        !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->flags & FL_KWARGS)) {
        struct korb_hash *h = (struct korb_hash *)argv[argc - 1];
        if (h->size == 0) argc--;
    }
    /* Copy params — Ruby block calling convention: when called with a
     * single Array argument and the block declares >1 param, the array
     * is auto-destructured into individual params (so blk.call([1,2])
     * with `|a, b|` binds a=1, b=2).  Procs (non-lambda) also try
     * to_ary on a non-Array sole arg. */
    uint32_t total_pos = p->params_cnt + p->post_cnt;
    if (argc == 1 && total_pos > 1 && p->rest_slot < 0 && !p->is_lambda) {
        VALUE arg0 = argv[0];
        VALUE arr = Qnil;
        if (!SPECIAL_CONST_P(arg0) && BUILTIN_TYPE(arg0) == T_ARRAY) {
            arr = arg0;
        } else if (!SPECIAL_CONST_P(arg0)) {
            VALUE rt = korb_funcall(c, arg0, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) });
            if (c->state != KORB_NORMAL) { c->state = KORB_NORMAL; c->state_value = Qnil; rt = Qfalse; }
            if (RTEST(rt)) {
                VALUE coerced = korb_funcall(c, arg0, korb_intern("to_ary"), 0, NULL);
                if (c->state != KORB_NORMAL) return Qnil;
                if (BUILTIN_TYPE(coerced) != T_ARRAY) {
                    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                    korb_raise(c, (struct korb_class *)eT,
                               "can't convert to Array (#to_ary gave non-Array)");
                    return Qnil;
                }
                arr = coerced;
            }
        }
        if (!NIL_P(arr) && BUILTIN_TYPE(arr) == T_ARRAY) {
            struct korb_array *a = (struct korb_array *)arr;
            argc = (int)a->len;
            argv = a->ptr;
            /* fall through to regular binding */
        }
    }
    {
        /* Bind positional params: req → post → (leftover spreads across
         * opt → *rest).  Posts have higher priority than opt/rest, so
         * subtract them off the back before deciding how to split the
         * "middle" between opt and rest. */
        uint32_t req_cnt = (p->params_cnt > p->opt_cnt) ? p->params_cnt - p->opt_cnt : 0;
        uint32_t post_cnt = p->post_cnt;
        uint32_t arg_cur = 0;  /* pointer into argv */
        uint32_t total_argc = (uint32_t)argc;
        /* 1) Fill required from front. */
        uint32_t req_fill = (total_argc < req_cnt) ? total_argc : req_cnt;
        for (uint32_t i = 0; i < req_fill; i++) {
            new_fp[p->param_base + i] = argv[arg_cur++];
        }
        for (uint32_t i = req_fill; i < req_cnt; i++) {
            new_fp[p->param_base + i] = Qnil;
        }
        /* 2) Reserve post slots — pulled later from argv tail. */
        uint32_t remaining = total_argc - arg_cur;  /* args left after req */
        uint32_t post_take = (remaining < post_cnt) ? remaining : post_cnt;
        uint32_t middle = remaining - post_take;    /* available to opt + *rest */
        /* 3) Optional: fill from the front of "middle". */
        for (uint32_t i = 0; i < p->opt_cnt; i++) {
            uint32_t opt_slot = p->param_base + req_cnt + i;
            if (i < middle) {
                new_fp[opt_slot] = argv[arg_cur++];
            } else {
                new_fp[opt_slot] = Qundef;  /* triggers default_init */
            }
        }
        if (middle > p->opt_cnt) middle -= p->opt_cnt; else middle = 0;
        /* 4) *rest: gather whatever is left in "middle" (after opt). */
        if (p->rest_slot >= 0) {
            VALUE rest = korb_ary_new();
            for (uint32_t i = 0; i < middle; i++) korb_ary_push(rest, argv[arg_cur++]);
            new_fp[p->rest_slot] = rest;
        } else {
            /* No rest — just skip the leftover middle args (they're dropped). */
            arg_cur += middle;
        }
        /* 5) Posts: absolute slots = param_base + params_cnt + (rest?1:0). */
        if (post_cnt > 0) {
            uint32_t post_base = p->param_base + p->params_cnt + (p->rest_slot >= 0 ? 1 : 0);
            for (uint32_t i = 0; i < post_take; i++) {
                new_fp[post_base + i] = argv[arg_cur++];
            }
            for (uint32_t i = post_take; i < post_cnt; i++) {
                new_fp[post_base + i] = Qnil;
            }
        }
    }
    /* Stash kwh into save slot. */
    if (p->kwh_save_slot >= 0 && !UNDEF_P(peeled_kwh)) {
        new_fp[p->kwh_save_slot] = peeled_kwh;
    }
    /* Bind &blk parameter — the caller's current_block (or nil) goes
     * into the slot the proc declared `&blk` on. */
    if (p->block_slot >= 0) {
        extern struct korb_proc *current_block;
        new_fp[p->block_slot] = current_block ? (VALUE)current_block : Qnil;
    }
    c->fp = new_fp;
    if (c->fp + p->env_size > c->sp) c->sp = c->fp + p->env_size;
    c->self = p->self;
    /* yield inside the proc body targets the enclosing method's block
     * captured at proc creation time. */
    extern struct korb_proc *current_block;
    extern struct korb_proc *running_block;
    struct korb_proc *prev_block = current_block;
    current_block = p->enclosing_block;
    struct korb_proc *prev_running = running_block;
    running_block = p;
    /* Restore the lexical class nesting captured at block-creation time
     * so constant lookups and `def` inside the body resolve in the
     * defining class scope, not the caller's. */
    struct korb_cref *prev_cref = c->cref;
    if (p->cref) c->cref = p->cref;
    VALUE r;
redo_proc:
    r = EVAL(c, p->body);
    /* `redo` inside a proc/lambda body — re-evaluate the body with
     * the same param bindings (CRuby semantics).  Without this, redo
     * leaks up and silently exits. */
    if (c->state == KORB_REDO) {
        c->state = KORB_NORMAL; c->state_value = Qnil;
        goto redo_proc;
    }
    c->cref = prev_cref;
    current_block = prev_block;
    running_block = prev_running;
    /* Snapshot any returned proc whose env points into our about-to-be-
     * popped frame. */
    korb_proc_snapshot_env_if_in_frame(r, new_fp, new_fp + p->env_size);
    if (c->state == KORB_RETURN || c->state == KORB_BREAK) {
        korb_proc_snapshot_env_if_in_frame(c->state_value, new_fp, new_fp + p->env_size);
    }
    /* If we used a cloned env, write back the closure-captured slots
     * (everything below the block's own param_base) so outer-scope
     * assignments survive. */
    if (fresh_env) {
        for (uint32_t i = 0; i < p->param_base; i++) {
            p->env[i] = fresh_env[i];
        }
    }
    /* Always restore fp/sp.  Without restoring sp on the no-clone
     * path, repeated proc.call from a hot loop creeps c->sp upward
     * (each call's slots stay committed) until stack overflow. */
    c->fp = prev_fp;
    c->sp = prev_sp;
    c->self = prev_self;
    /* Lambda: `return` inside the body targets the lambda itself, so we
     * consume it here and the caller sees the value as the call's result.
     * Plain Proc: `return` is non-local — let it propagate up to the
     * lexically-enclosing method, where it'll be consumed at that
     * method's prologue. */
    if (c->state == KORB_BREAK) {
        if (p->is_lambda && c->state_target_frame == NULL) {
            /* break inside a lambda's own body (no concrete target) —
             * consume as the lambda's return value.  When break carries
             * a concrete target_frame (set by an inner non-lambda proc
             * that found an &block-owner), let it propagate past us. */
            r = c->state_value;
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            c->state_target_frame = NULL;
        } else if (p->is_lambda) {
            /* lambda but break has a target above us — propagate. */
            r = c->state_value;
        } else {
            /* Non-lambda proc.call: distinguish "yield-style call from
             * within owning method" from "external .call".  Walk the
             * live frame chain looking for a method whose &block == p
             * — if found, this is a yield-style call (mid(&b) doing
             * b.call) and `break` escapes that method; set target_frame
             * so its prologue consumes the BREAK.  Otherwise the proc
             * was .called as a plain Proc with no owning method —
             * raise LocalJumpError. */
            struct korb_frame *owner = NULL;
            for (struct korb_frame *f = c->current_frame; f; f = f->prev) {
                if (f->block == p) { owner = f; break; }
            }
            if (owner) {
                c->state_target_frame = owner;
                r = c->state_value;
            } else {
                VALUE eL = korb_const_get(korb_vm->object_class, korb_intern("LocalJumpError"));
                c->state = KORB_NORMAL;
                c->state_value = Qnil;
                c->state_target_frame = NULL;
                korb_raise(c, (struct korb_class *)eL, "break from proc-closure");
                r = Qnil;
            }
        }
    } else if (c->state == KORB_RETURN && p->is_lambda &&
               c->state_target_frame == NULL) {
        /* Lambda swallows `return` only when the target is unset
         * (= the return originated from inside the lambda's own body).
         * A non-local return triggered by an inner non-lambda proc
         * whose enclosing method sits OUTSIDE this lambda has a
         * concrete target_frame and must propagate past us
         * (jruby/jruby#3143; ThroughDefineMethod return spec). */
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
        c->state_target_frame = NULL;
    } else if (c->state == KORB_NEXT) {
        /* `next` inside a proc/lambda body: consume it as the proc's
         * return value (CRuby: lambda { next 42 }.call == 42, and
         * proc { next 42 }.call also == 42).  Otherwise it leaks up
         * to the enclosing method and silently exits. */
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    } else if (c->state == KORB_THROW) {
        /* Throw escaping a proc/lambda — convert to UncaughtThrowError
         * raise.  catch() already cleared the state if its tag matched,
         * so reaching here means the throw is uncaught at this level.
         * Keep the tag on the exception's @tag ivar so re-thrown
         * conversions can carry it through outer rescue handlers. */
        VALUE eUTE = korb_const_get(korb_vm->object_class, korb_intern("UncaughtThrowError"));
        VALUE tag = Qnil, val = Qnil;
        if (!SPECIAL_CONST_P(c->state_value) && BUILTIN_TYPE(c->state_value) == T_ARRAY) {
            struct korb_array *pair = (struct korb_array *)c->state_value;
            if (pair->len >= 1) tag = pair->ptr[0];
            if (pair->len >= 2) val = pair->ptr[1];
        }
        VALUE tag_s = korb_inspect(tag);
        char buf[256];
        snprintf(buf, sizeof(buf), "uncaught throw %s", korb_str_cstr(tag_s));
        if (eUTE && !SPECIAL_CONST_P(eUTE) && BUILTIN_TYPE(eUTE) == T_CLASS) {
            korb_raise(c, (struct korb_class *)eUTE, "%s", buf);
        } else {
            korb_raise(c, NULL, "%s", buf);
        }
        /* Stash tag/value on the exception for catch to re-extract. */
        if (c->state == KORB_RAISE && !SPECIAL_CONST_P(c->state_value)) {
            korb_ivar_set(c->state_value, korb_intern("@__throw_tag__"), tag);
            korb_ivar_set(c->state_value, korb_intern("@__throw_value__"), val);
        }
    } else if (c->state == KORB_RETRY) {
        /* `retry` outside a rescue is a SyntaxError in CRuby (parse
         * time) or LocalJumpError at runtime if it escapes scope.
         * Convert to a SyntaxError-like raise so `rescue` can catch. */
        VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SyntaxError"));
        if (eSE && !SPECIAL_CONST_P(eSE) && BUILTIN_TYPE(eSE) == T_CLASS) {
            korb_raise(c, (struct korb_class *)eSE, "Invalid retry");
        } else {
            korb_raise(c, NULL, "Invalid retry");
        }
    }
    return r;
}

