/* Kernel — moved from builtins.c.  Includes caller / __method__ / loop / lambda / proc / eval. */

/* ---------- Kernel ---------- */
static VALUE kernel_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) korb_p(argv[i]);
    if (argc == 0) return Qnil;
    if (argc == 1) return argv[0];
    return korb_ary_new_from_values(argc, argv);
}

/* Pick the FILE * for IO-method writes.  When the receiver is a special
 * object marked as $stderr, write to stderr; otherwise stdout. */
static VALUE g_stderr_obj = 0;  /* set during init; treated as a marker */
static FILE *io_stream(VALUE self) {
    return (g_stderr_obj && self == g_stderr_obj) ? stderr : stdout;
}

static VALUE kernel_puts(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *out = io_stream(self);
    if (argc == 0) { fputc('\n', out); return Qnil; }
    for (int i = 0; i < argc; i++) {
        VALUE v = argv[i];
        if (BUILTIN_TYPE(v) == T_ARRAY) {
            struct korb_array *a = (struct korb_array *)v;
            for (long j = 0; j < a->len; j++) {
                VALUE s = korb_to_s_dispatch(c, a->ptr[j]);
                fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, out);
                fputc('\n', out);
            }
        } else {
            VALUE s = korb_to_s_dispatch(c, v);
            struct korb_string *str = (struct korb_string *)s;
            fwrite(str->ptr, 1, str->len, out);
            if (str->len == 0 || str->ptr[str->len-1] != '\n') fputc('\n', out);
        }
    }
    return Qnil;
}

static VALUE kernel_print(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *out = io_stream(self);
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, out);
    }
    return Qnil;
}

static VALUE kernel_raise(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) {
        korb_raise(c, NULL, "unhandled exception");
    } else if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        korb_raise(c, NULL, "%s", korb_str_cstr(argv[0]));
    } else if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_CLASS) {
        /* raise Klass, msg */
        const char *msg = "(unspecified)";
        if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
            msg = korb_str_cstr(argv[1]);
        }
        VALUE e = korb_exc_new((struct korb_class *)argv[0], msg);
        c->state = KORB_RAISE;
        c->state_value = e;
    } else {
        /* assume argv[0] is already an exception instance */
        c->state = KORB_RAISE;
        c->state_value = argv[0];
    }
    return Qnil;
}

static VALUE kernel_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_inspect(self);
}

static VALUE kernel_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_to_s(self);
}

static VALUE kernel_class(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_class_of(self);
}

static VALUE kernel_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_eq(self, argv[0]));
}

static VALUE kernel_neq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(!korb_eq(self, argv[0]));
}

static VALUE kernel_not(CTX *c, VALUE self, int argc, VALUE *argv) {
    return RTEST(self) ? Qfalse : Qtrue;
}

static VALUE kernel_nil_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(NIL_P(self));
}

static VALUE kernel_object_id(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)self / 8);
}

static VALUE kernel_freeze(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!SPECIAL_CONST_P(self)) RBASIC(self)->flags |= FL_FROZEN;
    return self;
}

static VALUE kernel_frozen_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* immediates and Symbol/String literals are always frozen — we
     * only track heap objects via the FL_FROZEN flag. */
    if (SPECIAL_CONST_P(self)) return Qtrue;
    return KORB_BOOL(RBASIC(self)->flags & FL_FROZEN);
}

static VALUE kernel_respond_to_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    struct korb_class *klass = korb_class_of_class(self);
    if (korb_class_find_method(klass, name) != NULL) return Qtrue;
    /* Defer to user-defined respond_to_missing?, but only if the class
     * actually overrode it (the default Object#respond_to_missing?
     * returns false and we just answered false anyway). */
    struct korb_method *rtm = korb_class_find_method(klass, korb_intern("respond_to_missing?"));
    if (rtm) {
        VALUE args[2] = { korb_id2sym(name), (argc >= 2 ? argv[1] : Qfalse) };
        VALUE r = korb_funcall(c, self, korb_intern("respond_to_missing?"), 2, args);
        return RTEST(r) ? Qtrue : Qfalse;
    }
    return Qfalse;
}

static VALUE kernel_is_a_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return Qfalse;
    struct korb_class *target = (struct korb_class *)argv[0];
    for (struct korb_class *k = korb_class_of_class(self); k; k = k->super) {
        if (k == target) return Qtrue;
        for (uint32_t i = 0; i < k->includes_cnt; i++) {
            if (k->includes[i] == target) return Qtrue;
        }
    }
    return Qfalse;
}

static VALUE kernel_block_given(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Inspect the closest enclosing AST method frame's block.  cfuncs
     * (like this one) don't push a frame, so current_frame already
     * points to the caller's AST frame. */
    if (c->current_frame) return KORB_BOOL(c->current_frame->block != NULL);
    return KORB_BOOL(korb_block_given());
}

/* ---------- catch / throw ----------
 * `catch(tag) { ... }` runs the block; if `throw(tag, val)` fires
 * inside, unwinding stops here and `val` becomes the return value.
 * Mismatched tag → propagates further up.
 *
 * Implementation: throw sets c->state = KORB_THROW and parks
 * [tag, val] in c->state_value as a 2-element Array.  catch yields,
 * then if state==THROW with a matching tag clears state and returns
 * val; otherwise re-propagates.  No setjmp/longjmp — the existing
 * EVAL_ARG / korb_yield bail-on-non-NORMAL machinery does the work. */
static VALUE kernel_throw(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        korb_raise(c, NULL, "throw: tag required");
        return Qnil;
    }
    VALUE tag = argv[0];
    VALUE val = argc >= 2 ? argv[1] : Qnil;
    VALUE pair = korb_ary_new_capa(2);
    korb_ary_push(pair, tag);
    korb_ary_push(pair, val);
    c->state = KORB_THROW;
    c->state_value = pair;
    return Qnil;
}

static VALUE kernel_catch(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* `catch` invocations may use either an explicit tag (`catch(:t) {}`)
     * or no tag (`catch {}` — the block param is the implicit tag).
     * For the no-tag form we synthesize a fresh Object as the tag. */
    VALUE tag = (argc >= 1) ? argv[0] : korb_object_new(korb_vm->object_class);
    VALUE block_arg[1] = { tag };
    VALUE r = korb_yield(c, 1, block_arg);
    if (c->state == KORB_THROW && !SPECIAL_CONST_P(c->state_value) &&
        BUILTIN_TYPE(c->state_value) == T_ARRAY) {
        struct korb_array *pair = (struct korb_array *)c->state_value;
        if (pair->len == 2 && korb_eq(pair->ptr[0], tag)) {
            VALUE v = pair->ptr[1];
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            return v;
        }
    }
    return r;
}

static VALUE kernel_dir(CTX *c, VALUE self, int argc, VALUE *argv) {
    const char *cur = c->current_file ? c->current_file : ".";
    return korb_str_new_cstr(korb_dirname(cur));
}

static VALUE kernel_file(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(c->current_file ? c->current_file : "(eval)");
}

static VALUE kernel_require_relative(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "require_relative: expected 1 String");
        return Qnil;
    }
    const char *name = korb_str_cstr(argv[0]);
    char *resolved = korb_resolve_relative(c->current_file, name);
    if (!resolved) {
        korb_raise(c, NULL, "cannot load such file -- %s", name);
        return Qnil;
    }
    return korb_load_file(c, resolved);
}

static VALUE kernel_require(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "require: expected 1 String");
        return Qnil;
    }
    const char *name = korb_str_cstr(argv[0]);
    /* Bare path: try as is, then as .rb in cwd */
    if (korb_file_exists(name)) return korb_load_file(c, name);
    long nl = strlen(name);
    bool has_rb = nl >= 3 && strcmp(name + nl - 3, ".rb") == 0;
    if (!has_rb) {
        char *with = korb_xmalloc_atomic(nl + 4);
        sprintf(with, "%s.rb", name);
        if (korb_file_exists(with)) return korb_load_file(c, with);
    }
    /* Stub: pretend stdlib gems aren't available, return false */
    if (strcmp(name, "stackprof") == 0) return Qfalse;
    if (strcmp(name, "fiddle") == 0) return Qfalse;
    if (strcmp(name, "rbconfig") == 0) return Qfalse;
    if (strcmp(name, "ffi") == 0) return Qfalse;
    /* unknown — don't raise, just return false (CRuby would raise but be lenient) */
    return Qfalse;
}

static VALUE kernel_load(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    return korb_load_file(c, korb_str_cstr(argv[0]));
}

static VALUE kernel_abort(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        fprintf(stderr, "%s\n", korb_str_cstr(argv[0]));
    }
    exit(1);
}

static VALUE kernel_exit(CTX *c, VALUE self, int argc, VALUE *argv) {
    int code = 0;
    if (argc >= 1 && FIXNUM_P(argv[0])) code = (int)FIX2LONG(argv[0]);
    else if (argc >= 1 && argv[0] == Qfalse) code = 1;
    exit(code);
}

static VALUE kernel_integer(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) { korb_raise(c, NULL, "Integer() needs argument"); return Qnil; }
    if (FIXNUM_P(argv[0])) return argv[0];
    if (BUILTIN_TYPE(argv[0]) == T_BIGNUM) return argv[0];
    if (KORB_IS_FLOAT(argv[0])) {
        return INT2FIX((long)korb_num2dbl(argv[0]));
    }
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        const char *s = korb_str_cstr(argv[0]);
        char *end;
        int base = argc >= 2 && FIXNUM_P(argv[1]) ? (int)FIX2LONG(argv[1]) : 10;
        long v = strtol(s, &end, base);
        if (end == s) {
            korb_raise(c, NULL, "invalid value for Integer(): %s", s);
            return Qnil;
        }
        return INT2FIX(v);
    }
    korb_raise(c, NULL, "can't convert to Integer");
    return Qnil;
}

static VALUE kernel_float(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (KORB_IS_FLOAT(argv[0])) return argv[0];
    if (FIXNUM_P(argv[0])) return korb_float_new((double)FIX2LONG(argv[0]));
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        return korb_float_new(strtod(korb_str_cstr(argv[0]), NULL));
    }
    return Qnil;
}

static VALUE kernel_string(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_str_new("", 0);
    return korb_to_s(argv[0]);
}

static VALUE kernel_array(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return korb_ary_new();
    VALUE v = argv[0];
    if (NIL_P(v)) return korb_ary_new();
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_ARRAY) return v;
    /* Range / Hash / anything responding to to_a: delegate.  Only
     * wrap in a 1-element Array when the value doesn't.  CRuby uses
     * to_ary first then falls back to to_a; for koruby's coverage
     * to_a is enough. */
    if (!SPECIAL_CONST_P(v) && (BUILTIN_TYPE(v) == T_RANGE || BUILTIN_TYPE(v) == T_HASH)) {
        return korb_funcall(c, v, korb_intern("to_a"), 0, NULL);
    }
    VALUE r = korb_ary_new_capa(1);
    korb_ary_push(r, v);
    return r;
}


/* ---------- Kernel#caller / __method__ / eval (stub) / loop ---------- */
static VALUE kernel_caller(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Walk current_frame chain.  HEAD doesn't track caller_node so we
     * just return method names in the form "in `name'". */
    VALUE arr = korb_ary_new();
    struct korb_frame *f = c->current_frame ? c->current_frame->prev : NULL;
    while (f) {
        const char *name = (f->method && f->method->name)
                             ? korb_id_name(f->method->name) : "<unknown>";
        char buf[128];
        snprintf(buf, sizeof(buf), "(eval):in `%s'", name);
        korb_ary_push(arr, korb_str_new_cstr(buf));
        f = f->prev;
    }
    return arr;
}
static VALUE kernel_method_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* cfunc prologue (prologue_cfunc_inl) doesn't push a frame, so
     * c->current_frame is the *enclosing* AST method's frame — exactly
     * what __method__ should report. */
    struct korb_frame *f = c->current_frame;
    if (!f || !f->method) return Qnil;
    return korb_id2sym(f->method->name);
}
extern VALUE korb_eval_string(CTX *c, const char *src, size_t len, const char *filename);
static VALUE kernel_eval_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* eval(string [, binding [, filename [, line]]]).
     * Binding-aware semantics aren't supported (no real binding); the
     * string is parsed + evaluated at the top level — which means it
     * sees globals / constants but not the caller's local variables.
     * Enough for tests that just `eval "1 + 2"`. */
    if (argc < 1) return Qnil;
    if (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "eval: argument must be a String");
        return Qnil;
    }
    struct korb_string *s = (struct korb_string *)argv[0];
    return korb_eval_string(c, s->ptr, (size_t)s->len, "(eval)");
}
static VALUE kernel_loop(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* loop { ... } — call block forever, swallow StopIteration. */
    extern struct korb_proc *current_block;
    if (!current_block) {
        korb_raise(c, NULL, "no block given (loop)");
        return Qnil;
    }
    while (c->state == KORB_NORMAL) {
        korb_yield(c, 0, NULL);
        if (c->state == KORB_BREAK) {
            VALUE r = c->state_value;
            c->state = KORB_NORMAL; c->state_value = Qnil;
            return r;
        }
        if (c->state == KORB_RAISE) {
            /* StopIteration → swallow.  Anything else propagates. */
            VALUE exc = c->state_value;
            if (!SPECIAL_CONST_P(exc)) {
                struct korb_class *k = (struct korb_class *)((struct RBasic *)exc)->klass;
                if (k && k->name == korb_intern("StopIteration")) {
                    c->state = KORB_NORMAL; c->state_value = Qnil;
                    return Qnil;
                }
            }
            return Qnil;
        }
    }
    return Qnil;
}
static VALUE kernel_lambda(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) {
        korb_raise(c, NULL, "tried to create Proc object without a block");
        return Qnil;
    }
    /* Mark as lambda so Proc#call's `return` becomes local. */
    current_block->is_lambda = true;
    return (VALUE)current_block;
}
static VALUE kernel_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) {
        korb_raise(c, NULL, "tried to create Proc object without a block");
        return Qnil;
    }
    return (VALUE)current_block;
}

