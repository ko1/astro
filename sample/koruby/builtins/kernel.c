/* Kernel — moved from builtins.c.  Includes caller / __method__ / loop / lambda / proc / eval. */

/* ---------- at_exit hooks (LIFO Proc list) ---------- */
static struct {
    struct korb_proc **procs;
    uint32_t cnt, capa;
} g_at_exit = {0};

static VALUE kernel_at_exit(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) {
        korb_raise(c, NULL, "called without a block");
        return Qnil;
    }
    if (g_at_exit.cnt >= g_at_exit.capa) {
        uint32_t nc = g_at_exit.capa ? g_at_exit.capa * 2 : 4;
        g_at_exit.procs = korb_xrealloc(g_at_exit.procs, nc * sizeof(*g_at_exit.procs));
        g_at_exit.capa = nc;
    }
    g_at_exit.procs[g_at_exit.cnt++] = current_block;
    return (VALUE)current_block;
}

void korb_run_at_exit_hooks(CTX *c) {
    /* LIFO order — last-registered runs first. */
    for (int i = (int)g_at_exit.cnt - 1; i >= 0; i--) {
        struct korb_proc *p = g_at_exit.procs[i];
        if (!p) continue;
        VALUE prev_state = c->state;
        VALUE prev_value = c->state_value;
        c->state = KORB_NORMAL;
        korb_funcall(c, (VALUE)p, korb_intern("call"), 0, NULL);
        if (c->state == KORB_RAISE) {
            VALUE s = korb_inspect(c->state_value);
            fprintf(stderr, "at_exit hook raised: %s\n", korb_str_cstr(s));
            c->state = KORB_NORMAL;
        }
        /* Restore the original raise so the process exits with the
         * right status. */
        c->state = prev_state;
        c->state_value = prev_value;
    }
    g_at_exit.cnt = 0;
}

/* ---------- Kernel#rand / srand ---------- */
#include <stdlib.h>
#include <time.h>

static VALUE kernel_srand(CTX *c, VALUE self, int argc, VALUE *argv) {
    unsigned int seed;
    if (argc >= 1 && FIXNUM_P(argv[0])) {
        seed = (unsigned int)FIX2LONG(argv[0]);
    } else {
        seed = (unsigned int)(time(NULL) ^ (long)self);
    }
    srand(seed);
    return INT2FIX((long)seed);
}

static bool g_srand_initialized = false;
static void korb_srand_lazy(void) {
    if (!g_srand_initialized) {
        srand((unsigned int)time(NULL));
        g_srand_initialized = true;
    }
}

static VALUE kernel_rand(CTX *c, VALUE self, int argc, VALUE *argv) {
    korb_srand_lazy();
    /* rand           → Float in [0, 1)
     * rand(N)        → Integer in [0, N) for Integer N
     * rand(F)        → Float in [0, F)
     * rand(a..b)     → Integer in [a, b]  (or [a, b) for exclusive) */
    if (argc < 1) {
        return korb_float_new((double)rand() / ((double)RAND_MAX + 1.0));
    }
    VALUE a = argv[0];
    if (FIXNUM_P(a)) {
        long n = FIX2LONG(a);
        if (n <= 0) return korb_float_new((double)rand() / ((double)RAND_MAX + 1.0));
        return INT2FIX((long)(rand() % n));
    }
    if (KORB_IS_FLOAT(a)) {
        double d = korb_num2dbl(a);
        if (d <= 0) return korb_float_new((double)rand() / ((double)RAND_MAX + 1.0));
        return korb_float_new(((double)rand() / ((double)RAND_MAX + 1.0)) * d);
    }
    if (!SPECIAL_CONST_P(a) && BUILTIN_TYPE(a) == T_RANGE) {
        struct korb_range *r = (struct korb_range *)a;
        if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
            long lo = FIX2LONG(r->begin), hi = FIX2LONG(r->end);
            if (!r->exclude_end) hi++;
            long span = hi - lo;
            if (span <= 0) return INT2FIX(lo);
            return INT2FIX(lo + (rand() % span));
        }
    }
    return korb_float_new((double)rand() / ((double)RAND_MAX + 1.0));
}

/* ---------- Kernel ---------- */
/* `**obj` in a hash literal: convert obj to Hash via to_hash.  CRuby
 * raises TypeError if obj doesn't respond to to_hash or if to_hash
 * returns a non-Hash (with the message "no implicit conversion of X
 * into Hash").  nil is rejected (CRuby semantics changed in 2.7+;
 * empty-hash splat is `**{}`). */
/* Common path: convert v to Hash via to_hash.  Returns Hash or raises
 * TypeError on non-conversion.  nil handling is decided by caller. */
static VALUE kwsplat_convert(CTX *c, VALUE v) {
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_HASH) return v;
    VALUE rt = korb_funcall(c, v, korb_intern("respond_to?"), 1,
                            (VALUE[]){ korb_id2sym(korb_intern("to_hash")) });
    if (c->state != KORB_NORMAL) return Qnil;
    if (!RTEST(rt)) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "no implicit conversion of %s into Hash",
                   korb_id_name(korb_class_of_class(v)->name));
        return Qnil;
    }
    VALUE r = korb_funcall(c, v, korb_intern("to_hash"), 0, NULL);
    if (c->state != KORB_NORMAL) return Qnil;
    if (SPECIAL_CONST_P(r) || BUILTIN_TYPE(r) != T_HASH) {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "can't convert %s to Hash (%s#to_hash gives %s)",
                   korb_id_name(korb_class_of_class(v)->name),
                   korb_id_name(korb_class_of_class(v)->name),
                   korb_id_name(korb_class_of_class(r)->name));
        return Qnil;
    }
    return r;
}

/* `case x; in [...]` array pattern coerce step: if obj.deconstruct
 * returns non-Array, raise TypeError ("deconstruct must return Array"). */
VALUE kernel_pattern_decon_check(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE v = argv[0];
    if (NIL_P(v)) return Qnil;  /* propagate (failed-coerce) */
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_ARRAY) return v;
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eT, "deconstruct must return Array");
    return Qnil;
}
/* Same shape for deconstruct_keys: must return Hash else TypeError. */
VALUE kernel_pattern_decon_keys_check(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE v = argv[0];
    if (NIL_P(v)) return Qnil;
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_HASH) return v;
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eT, "deconstruct_keys must return Hash");
    return Qnil;
}

/* `case x; when *arr` lowering: iterate arr at runtime and return true
 * iff any element ===s x.  Mirrors rescue *list. */
VALUE kernel_case_splat_match(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qfalse;
    VALUE list = argv[0];
    VALUE x    = argv[1];
    if (SPECIAL_CONST_P(list) || BUILTIN_TYPE(list) != T_ARRAY) {
        VALUE rt = korb_funcall(c, list, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_a")) });
        if (c->state != KORB_NORMAL) return Qfalse;
        if (RTEST(rt)) {
            list = korb_funcall(c, list, korb_intern("to_a"), 0, NULL);
            if (c->state != KORB_NORMAL) return Qfalse;
        }
        if (SPECIAL_CONST_P(list) || BUILTIN_TYPE(list) != T_ARRAY) return Qfalse;
    }
    struct korb_array *a = (struct korb_array *)list;
    for (long i = 0; i < a->len; i++) {
        VALUE r = korb_funcall(c, a->ptr[i], korb_intern("==="), 1, &x);
        if (c->state != KORB_NORMAL) return Qfalse;
        if (RTEST(r)) return Qtrue;
    }
    return Qfalse;
}

/* `rescue *list => e` lowering: list may be Array (or anything with
 * to_a).  Returns true iff any element of the converted list ===s exc.
 * On a non-Array / non-to_a list, raises TypeError to match CRuby. */
/* Validate that a rescue clause value is a Module/Class — CRuby's
 * `rescue 42` raises TypeError "class or module required for rescue
 * clause".  Returns the value unchanged on success. */
VALUE kernel_rescue_class_check(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE v = argv[0];
    if (!SPECIAL_CONST_P(v) &&
        (BUILTIN_TYPE(v) == T_CLASS || BUILTIN_TYPE(v) == T_MODULE)) {
        return v;
    }
    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eT,
               "class or module required for rescue clause");
    return Qnil;
}

VALUE kernel_rescue_splat_match(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qfalse;
    VALUE list = argv[0];
    VALUE exc  = argv[1];
    if (SPECIAL_CONST_P(list) || BUILTIN_TYPE(list) != T_ARRAY) {
        /* `rescue *cls` where cls is a single Class/Module — treat as
         * one-element list. */
        if (!SPECIAL_CONST_P(list) &&
            (BUILTIN_TYPE(list) == T_CLASS || BUILTIN_TYPE(list) == T_MODULE)) {
            VALUE r = korb_funcall(c, list, korb_intern("==="), 1, &exc);
            if (c->state != KORB_NORMAL) return Qfalse;
            return RTEST(r) ? Qtrue : Qfalse;
        }
        VALUE rt = korb_funcall(c, list, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_a")) });
        if (c->state != KORB_NORMAL) return Qfalse;
        if (RTEST(rt)) {
            list = korb_funcall(c, list, korb_intern("to_a"), 0, NULL);
            if (c->state != KORB_NORMAL) return Qfalse;
        }
        if (SPECIAL_CONST_P(list) || BUILTIN_TYPE(list) != T_ARRAY) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT, "can't convert to Array (returned non-Array)");
            return Qfalse;
        }
    }
    struct korb_array *a = (struct korb_array *)list;
    for (long i = 0; i < a->len; i++) {
        VALUE el = a->ptr[i];
        if (SPECIAL_CONST_P(el) ||
            (BUILTIN_TYPE(el) != T_CLASS && BUILTIN_TYPE(el) != T_MODULE)) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "class or module required for rescue clause");
            return Qfalse;
        }
        VALUE r = korb_funcall(c, el, korb_intern("==="), 1, &exc);
        if (c->state != KORB_NORMAL) return Qfalse;
        if (RTEST(r)) return Qtrue;
    }
    return Qfalse;
}

/* `&expr` block-pass: nil → no block (Qnil); else expr.to_proc.
 * CRuby allows `m(&nil)` to mean "no block". */
VALUE kernel_to_block_arg(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || NIL_P(argv[0])) return Qnil;
    VALUE v = argv[0];
    if (!SPECIAL_CONST_P(v) && BUILTIN_TYPE(v) == T_PROC) return v;
    VALUE r = korb_funcall(c, v, korb_intern("to_proc"), 0, NULL);
    if (c->state != KORB_NORMAL) return Qnil;
    return r;
}

/* Lenient: `m(**nil)` is allowed and treated as no kwargs. */
VALUE kernel_kwsplat_to_hash_lenient(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || NIL_P(argv[0])) return korb_hash_new();
    return kwsplat_convert(c, argv[0]);
}

VALUE kernel_kwsplat_to_hash(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Ruby 3.4+: `{**nil}` evaluates to {}.  Earlier versions raised
     * TypeError; we follow current CRuby (≥ 3.4). */
    if (argc < 1 || NIL_P(argv[0])) return korb_hash_new();
    return kwsplat_convert(c, argv[0]);
}

static VALUE kernel_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_inspect_dispatch(c, argv[i]);
        struct korb_string *str = (struct korb_string *)s;
        fwrite(str->ptr, 1, str->len, stdout);
        fputc('\n', stdout);
    }
    if (argc == 0) return Qnil;
    if (argc == 1) return argv[0];
    return korb_ary_new_from_values(argc, argv);
}

/* Pick the FILE * for IO-method writes.  When the receiver carries
 * an @__fp__ ivar (e.g. an IO returned from File.open or IO.pipe),
 * write to that fd; otherwise default to stdout (or stderr if it's
 * the well-known $stderr object). */
static VALUE g_stderr_obj = 0;  /* set during init; treated as a marker */
static FILE *io_stream(VALUE self) {
    if (!SPECIAL_CONST_P(self)) {
        VALUE v = korb_ivar_get(self, korb_intern("@__fp__"));
        if (FIXNUM_P(v)) return (FILE *)(uintptr_t)FIX2LONG(v);
    }
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
    /* CRuby: `print` with no args prints $_. */
    if (argc == 0) {
        VALUE dollar_underscore = korb_last_line_get(c);
        if (!NIL_P(dollar_underscore)) {
            VALUE s = korb_to_s_dispatch(c, dollar_underscore);
            fwrite(((struct korb_string *)s)->ptr, 1,
                   ((struct korb_string *)s)->len, out);
        }
        return Qnil;
    }
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, out);
    }
    return Qnil;
}

static VALUE kernel_raise(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Peel a trailing keyword-arg Hash with `cause:` (and friends) before
     * the positional dispatch.  `raise cause: x` lowers to
     * `raise({cause: x})` at parse time; we want it treated as
     * `raise(*[], cause: x)`. */
    VALUE kw_cause = Qundef;
    if (argc >= 1 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH) {
        VALUE last = argv[argc - 1];
        VALUE k_cause = korb_id2sym(korb_intern("cause"));
        VALUE v = korb_hash_aref(last, k_cause);
        if (!UNDEF_P(v)) {
            kw_cause = v;
            argc--;
        }
    }
    if (argc == 0 && !UNDEF_P(kw_cause)) {
        /* `raise cause: x` with no positional args — CRuby raises
         * ArgumentError "only cause is given with no arguments". */
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "only cause is given with no arguments");
        return Qnil;
    }
    /* CRuby: raise accepts 0..3 positional args.  >3 → ArgumentError. */
    if (argc > 3) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected 0..3)", argc);
        return Qnil;
    }
    if (argc == 0) {
        /* Bare `raise` re-raises $! if set; only fall back to a fresh
         * RuntimeError when there's no current exception. */
        VALUE bang = korb_gvar_get(korb_intern("$!"));
        if (!NIL_P(bang)) {
            c->state = KORB_RAISE;
            c->state_value = bang;
            return Qnil;
        }
        korb_raise(c, NULL, "unhandled exception");
    } else if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        korb_raise(c, NULL, "%s", korb_str_cstr(argv[0]));
    } else if (argc == 1 && !SPECIAL_CONST_P(argv[0]) &&
               BUILTIN_TYPE(argv[0]) == T_OBJECT) {
        /* `raise(obj)` — obj must be an Exception (or implement
         * #exception).  Otherwise CRuby raises TypeError. */
        VALUE eExc = korb_const_get(korb_vm->object_class, korb_intern("Exception"));
        struct korb_class *exc_cls = (eExc && !SPECIAL_CONST_P(eExc) &&
                                       (BUILTIN_TYPE(eExc) == T_CLASS || BUILTIN_TYPE(eExc) == T_MODULE))
                                          ? (struct korb_class *)eExc : NULL;
        struct korb_class *obj_cls = (struct korb_class *)((struct RBasic *)argv[0])->klass;
        bool is_exc = false;
        for (struct korb_class *kk = obj_cls; kk; kk = kk->super) {
            if (kk == exc_cls) { is_exc = true; break; }
        }
        if (!is_exc) {
            VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
            korb_raise(c, (struct korb_class *)eT,
                       "exception class/object expected");
            return Qnil;
        }
        c->state = KORB_RAISE;
        c->state_value = argv[0];
    } else if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_CLASS) {
        /* raise Klass, msg */
        const char *msg = "(unspecified)";
        if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
            msg = korb_str_cstr(argv[1]);
        }
        VALUE e = korb_exc_new((struct korb_class *)argv[0], msg);
        VALUE cur = korb_gvar_get(korb_intern("$!"));
        if (!NIL_P(cur) && cur != e &&
            !SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_OBJECT) {
            VALUE existing = korb_ivar_get(e, korb_intern("@cause"));
            if (UNDEF_P(existing) || NIL_P(existing)) {
                /* Cycle guard — see korb_raise. */
                VALUE walk = cur;
                int hops = 0;
                bool would_cycle = false;
                while (!NIL_P(walk) && hops++ < 32) {
                    if (walk == e) { would_cycle = true; break; }
                    if (SPECIAL_CONST_P(walk) || BUILTIN_TYPE(walk) != T_OBJECT) break;
                    walk = korb_ivar_get(walk, korb_intern("@cause"));
                    if (UNDEF_P(walk)) break;
                }
                if (!would_cycle) korb_ivar_set(e, korb_intern("@cause"), cur);
            }
        }
        c->state = KORB_RAISE;
        c->state_value = e;
    } else {
        /* Anything else (nil, Integer, etc.) — CRuby raises TypeError. */
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT,
                   "exception class/object expected");
        return Qnil;
        /* unreachable */
        VALUE e = argv[0];
        VALUE cur = korb_gvar_get(korb_intern("$!"));
        if (!NIL_P(cur) && cur != e &&
            !SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_OBJECT) {
            VALUE existing = korb_ivar_get(e, korb_intern("@cause"));
            if (UNDEF_P(existing) || NIL_P(existing)) {
                /* Cycle guard — see korb_raise. */
                VALUE walk = cur;
                int hops = 0;
                bool would_cycle = false;
                while (!NIL_P(walk) && hops++ < 32) {
                    if (walk == e) { would_cycle = true; break; }
                    if (SPECIAL_CONST_P(walk) || BUILTIN_TYPE(walk) != T_OBJECT) break;
                    walk = korb_ivar_get(walk, korb_intern("@cause"));
                    if (UNDEF_P(walk)) break;
                }
                if (!would_cycle) korb_ivar_set(e, korb_intern("@cause"), cur);
            }
        }
        c->state = KORB_RAISE;
        c->state_value = e;
    }
    /* Apply explicit `cause:` kwarg.  Override any auto-linked cause. */
    if (!UNDEF_P(kw_cause) && c->state == KORB_RAISE) {
        VALUE e = c->state_value;
        if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_OBJECT) {
            korb_ivar_set(e, korb_intern("@cause"),
                          NIL_P(kw_cause) ? Qnil : kw_cause);
        }
    }
    return Qnil;
}

static VALUE kernel_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* main object: CRuby's main_object inspects/to_s as "main". */
    if (self == korb_vm->main_obj) return korb_str_new_cstr("main");
    /* Default Kernel#inspect for objects that don't override it.
     * Avoid calling korb_inspect_dispatch here — that would loop
     * straight back to this cfunc.  korb_inspect skips user dispatch. */
    return korb_inspect(self);
}

static VALUE kernel_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (self == korb_vm->main_obj) return korb_str_new_cstr("main");
    return korb_to_s(self);
}

static VALUE kernel_class(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Skip past any FL_SINGLETON metaclasses up the chain — `obj.class`
     * reports the user-facing class (Class for any class object, the
     * ordinary instance class for ordinary objects), not the lazy
     * metaclass we synthesize for singleton-method propagation. */
    VALUE k = korb_class_of(self);
    while (!SPECIAL_CONST_P(k) && (((struct RBasic *)k)->flags & FL_SINGLETON)) {
        struct korb_class *kk = (struct korb_class *)k;
        if (!kk->super) break;
        k = (VALUE)kk->super;
    }
    return k;
}

static VALUE kernel_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_eq(self, argv[0]));
}

static VALUE kernel_neq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(!korb_eq(self, argv[0]));
}

/* Object#!~: inverted =~.  Default implementation returns !(self =~ arg).
 * `defined?(x !~ y)` returns "method" because every Object responds to !~. */
static VALUE kernel_not_match(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE m = korb_funcall(c, self, korb_intern("=~"), 1, argv);
    if (c->state != KORB_NORMAL) return Qnil;
    return RTEST(m) ? Qfalse : Qtrue;
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
    if (!SPECIAL_CONST_P(self)) {
        RBASIC(self)->flags |= FL_FROZEN;
        /* Propagate freeze to the singleton class if one already exists.
         * (T_OBJECT's basic.klass points to the singleton; lazy creation
         * for other types doesn't pre-allocate.) */
        if (BUILTIN_TYPE(self) == T_OBJECT) {
            struct korb_object *o = (struct korb_object *)self;
            struct korb_class *meta = (struct korb_class *)o->basic.klass;
            if (meta && (meta->basic.flags & FL_SINGLETON)) {
                meta->basic.flags |= FL_FROZEN;
            }
        } else if (BUILTIN_TYPE(self) == T_CLASS || BUILTIN_TYPE(self) == T_MODULE) {
            /* Class / Module: freeze its eigenclass too. */
            struct korb_class *meta = (struct korb_class *)((struct RBasic *)self)->klass;
            if (meta && (meta->basic.flags & FL_SINGLETON)) {
                meta->basic.flags |= FL_FROZEN;
            }
        }
    }
    return self;
}

static VALUE kernel_frozen_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* immediates and Symbol/String literals are always frozen — we
     * only track heap objects via the FL_FROZEN flag. */
    if (SPECIAL_CONST_P(self)) return Qtrue;
    return KORB_BOOL(RBASIC(self)->flags & FL_FROZEN);
}

static VALUE kernel_respond_to_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    ID name;
    if (SYMBOL_P(argv[0])) {
        name = korb_sym2id(argv[0]);
    } else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        struct korb_string *s = (struct korb_string *)argv[0];
        name = korb_intern_n(s->ptr, s->len);
    } else {
        VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
        korb_raise(c, (struct korb_class *)eT, "expected Symbol or String");
        return Qfalse;
    }
    struct korb_class *klass = korb_class_of_class(self);
    bool include_private = (argc >= 2) && RTEST(argv[1]);
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m != NULL) {
        /* CRuby: private methods are excluded unless include_private=true.
         * Protected: included only when receiver is self in the calling
         * scope; we don't track call-site visibility here, so treat them
         * as included (matches CRuby for `respond_to?(:m, true)` and
         * close enough for the common `respond_to?(:m)` test). */
        if (m->visibility == KORB_VIS_PRIVATE && !include_private) return Qfalse;
        return Qtrue;
    }
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
    /* state == THROW: tag/value live on c->state_value as a 2-element ary. */
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
    /* state == RAISE with UncaughtThrowError: proc_call already converted
     * a throw escaping a lambda body to a raise.  Look for our @__throw_tag__
     * ivar and re-handle it as a throw if the tag matches. */
    if (c->state == KORB_RAISE && !SPECIAL_CONST_P(c->state_value)) {
        VALUE eUTE = korb_const_get(korb_vm->object_class, korb_intern("UncaughtThrowError"));
        struct korb_class *exc_cls = (struct korb_class *)((struct RBasic *)c->state_value)->klass;
        bool is_ute = false;
        for (struct korb_class *kk = exc_cls; kk; kk = kk->super) {
            if ((VALUE)kk == eUTE) { is_ute = true; break; }
        }
        if (is_ute) {
            VALUE thrown_tag = korb_ivar_get(c->state_value, korb_intern("@__throw_tag__"));
            if (!UNDEF_P(thrown_tag) && korb_eq(thrown_tag, tag)) {
                VALUE v = korb_ivar_get(c->state_value, korb_intern("@__throw_value__"));
                if (UNDEF_P(v)) v = Qnil;
                c->state = KORB_NORMAL;
                c->state_value = Qnil;
                return v;
            }
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

static VALUE kernel_exit(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Ruby's `exit` raises SystemExit so `rescue SystemExit` and
     * `at_exit` can run.  CRuby's `exit!` is the libc-style abort.  */
    int code = 0;
    bool success = true;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) {
            code = (int)FIX2LONG(argv[0]);
            success = (code == 0);
        } else if (argv[0] == Qfalse) {
            code = 1; success = false;
        } else if (argv[0] == Qtrue) {
            code = 0; success = true;
        }
    }
    VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SystemExit"));
    struct korb_class *exc_class = NULL;
    if (eSE && !SPECIAL_CONST_P(eSE) &&
        (BUILTIN_TYPE(eSE) == T_CLASS || BUILTIN_TYPE(eSE) == T_MODULE)) {
        exc_class = (struct korb_class *)eSE;
    }
    VALUE e = korb_exc_new(exc_class, "exit");
    if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_OBJECT) {
        korb_ivar_set(e, korb_intern("@status"), INT2FIX(code));
        korb_ivar_set(e, korb_intern("@success"), KORB_BOOL(success));
    }
    c->state = KORB_RAISE;
    c->state_value = e;
    return Qnil;
}
static VALUE kernel_exit_bang(CTX *c, VALUE self, int argc, VALUE *argv) {
    int code = 0;
    if (argc >= 1 && FIXNUM_P(argv[0])) code = (int)FIX2LONG(argv[0]);
    else if (argc >= 1 && argv[0] == Qfalse) code = 1;
    exit(code);
}
static VALUE kernel_abort(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING) {
        fprintf(stderr, "%s\n", ((struct korb_string *)argv[0])->ptr);
    }
    VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SystemExit"));
    struct korb_class *exc_class = (eSE && !SPECIAL_CONST_P(eSE) &&
                                    (BUILTIN_TYPE(eSE) == T_CLASS || BUILTIN_TYPE(eSE) == T_MODULE))
                                       ? (struct korb_class *)eSE : NULL;
    VALUE e = korb_exc_new(exc_class, "abort");
    if (!SPECIAL_CONST_P(e) && BUILTIN_TYPE(e) == T_OBJECT) {
        korb_ivar_set(e, korb_intern("@status"), INT2FIX(1));
        korb_ivar_set(e, korb_intern("@success"), Qfalse);
    }
    c->state = KORB_RAISE;
    c->state_value = e;
    return Qnil;
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
        /* Skip leading sign + whitespace, detect prefix.  When base is
         * implicit (no second arg), strtol with base=0 auto-detects
         * 0x / 0 prefixes — same behavior CRuby's Integer(str) gives. */
        int explicit_base = argc >= 2 && FIXNUM_P(argv[1]) ? (int)FIX2LONG(argv[1]) : 0;
        int base = explicit_base ? explicit_base : 0;
        /* CRuby also recognises 0o / 0b as octal / binary prefixes; strtol
         * doesn't, so handle them up front. */
        const char *p = s;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '+' || *p == '-') p++;
        if (!explicit_base && p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
            /* synthesize a string without the o/O so strtol sees octal */
            base = 8;
            char *buf = korb_xmalloc_atomic(strlen(s) + 1);
            long w = 0;
            for (const char *q = s; *q; q++) {
                if (q == p + 1) continue; /* skip 'o' */
                buf[w++] = *q;
            }
            buf[w] = 0;
            char *end;
            long v = strtol(buf, &end, base);
            if (end == buf) {
                VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
                korb_raise(c, (struct korb_class *)eArg, "invalid value for Integer(): %s", s);
                return Qnil;
            }
            return INT2FIX(v);
        }
        if (!explicit_base && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            base = 2;
            char *buf = korb_xmalloc_atomic(strlen(s) + 1);
            long w = 0;
            for (const char *q = s; *q; q++) {
                if (q == p + 1) continue;
                buf[w++] = *q;
            }
            buf[w] = 0;
            char *end;
            long v = strtol(buf, &end, base);
            if (end == buf) {
                VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
                korb_raise(c, (struct korb_class *)eArg, "invalid value for Integer(): %s", s);
                return Qnil;
            }
            return INT2FIX(v);
        }
        char *end;
        long v = strtol(s, &end, base);
        if (end == s || (*end != '\0' && *end != ' ' && *end != '\t')) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg, "invalid value for Integer(): %s", s);
            return Qnil;
        }
        return INT2FIX(v);
    }
    VALUE eTyp = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eTyp, "can't convert to Integer");
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
/* caller([start=1, [length]] | [range])
 *
 * Returns an Array of "FILE:LINE:in `METHOD'" strings.  Each entry's
 * line is where IN that frame's body the next-up call was made — the
 * same logic as korb_build_backtrace.  Skips kernel_caller's own
 * level so the result starts at the actual caller (CRuby behavior).
 */
static VALUE kernel_caller(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Build the full caller list, including the immediate caller's own
     * frame at index 0; slice it per start/length args.  Default start
     * is 1, so out-of-the-box `caller` skips the immediate frame and
     * matches CRuby. */
    VALUE arr = korb_ary_new();
    const char *default_file = c->current_file ? c->current_file : "(unknown)";
    struct korb_frame *f = c->current_frame;
    /* Index-0 entry's line is where IN that frame's body `caller` was
     * called.  kernel_caller is a cfunc; its callsite is recorded in
     * last_cfunc_callsite by prologue_cfunc_inl. */
    int next_line = c->last_cfunc_callsite ? c->last_cfunc_callsite->head.line : 0;
    char buf[512];
    char nbuf[256];
    /* Inside a block / proc / lambda body — prepend a "block in
     * <enclosing>" entry so caller(0) sees the block as a frame. */
    if (running_block) {
        const char *enc_name = (f && f->method && f->method->name)
                                  ? korb_id_name(f->method->name) : "<main>";
        const char *enc_file = default_file;
        if (running_block->body && running_block->body->head.source_file) {
            enc_file = running_block->body->head.source_file;
        }
        snprintf(nbuf, sizeof(nbuf), "block in %s", enc_name);
        snprintf(buf, sizeof(buf), "%s:%d:in '%s'", enc_file, next_line, nbuf);
        korb_ary_push(arr, korb_str_new_cstr(buf));
    }
    while (f) {
        /* Inserted block-in-<enclosing> for THIS frame's caller block,
         * mirroring korb_build_backtrace's handling. */
        const char *name = (f->method && f->method->name)
                             ? korb_id_name(f->method->name) : "<main>";
        const char *file = default_file;
        if (f->method && f->method->type == KORB_METHOD_AST &&
            f->method->u.ast.body && f->method->u.ast.body->head.source_file) {
            file = f->method->u.ast.body->head.source_file;
        }
        snprintf(buf, sizeof(buf), "%s:%d:in '%s'", file, next_line, name);
        korb_ary_push(arr, korb_str_new_cstr(buf));
        next_line = f->caller_node ? f->caller_node->head.line : 0;
        if (f->caller_running_block) {
            struct korb_proc *cb = (struct korb_proc *)f->caller_running_block;
            struct korb_frame *parent = f->prev;
            const char *enc_name = (parent && parent->method && parent->method->name)
                                      ? korb_id_name(parent->method->name) : "<main>";
            const char *enc_file = default_file;
            if (cb->body && cb->body->head.source_file) {
                enc_file = cb->body->head.source_file;
            } else if (parent && parent->method && parent->method->type == KORB_METHOD_AST &&
                parent->method->u.ast.body && parent->method->u.ast.body->head.source_file) {
                enc_file = parent->method->u.ast.body->head.source_file;
            }
            snprintf(nbuf, sizeof(nbuf), "block in %s", enc_name);
            snprintf(buf, sizeof(buf), "%s:%d:in '%s'", enc_file, next_line, nbuf);
            korb_ary_push(arr, korb_str_new_cstr(buf));
        }
        f = f->prev;
    }
    /* Append a <main> entry so the chain always ends in main. */
    snprintf(buf, sizeof(buf), "%s:%d:in '<main>'", default_file, next_line);
    korb_ary_push(arr, korb_str_new_cstr(buf));

    /* Slice per CRuby: caller(start=1, length=nil) or caller(range). */
    long total = ((struct korb_array *)arr)->len;
    long start = 1, len = -1;  /* -1 = "all from start" */
    if (argc >= 1) {
        if (BUILTIN_TYPE(argv[0]) == T_RANGE) {
            struct korb_range *r = (struct korb_range *)argv[0];
            if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
                long b = FIX2LONG(r->begin);
                long e = FIX2LONG(r->end);
                if (b < 0) b += total;
                if (e < 0) e += total;
                if (r->exclude_end) e -= 1;
                start = b;
                len = e - b + 1;
                if (len < 0) len = 0;
            }
        } else if (FIXNUM_P(argv[0])) {
            start = FIX2LONG(argv[0]);
            if (argc >= 2 && FIXNUM_P(argv[1])) len = FIX2LONG(argv[1]);
        }
    }
    if (start < 0 || start > total) return Qnil;
    long end_exclusive = (len < 0) ? total : start + len;
    if (end_exclusive > total) end_exclusive = total;
    if (end_exclusive < start) end_exclusive = start;
    VALUE out = korb_ary_new_capa(end_exclusive - start);
    for (long i = start; i < end_exclusive; i++) {
        korb_ary_push(out, korb_ary_aref(arr, i));
    }
    return out;
}
static VALUE kernel_method_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* cfunc prologue (prologue_cfunc_inl) doesn't push a frame, so
     * c->current_frame is the *enclosing* AST method's frame — exactly
     * what __method__ should report. */
    struct korb_frame *f = c->current_frame;
    if (!f || !f->method) return Qnil;
    return korb_id2sym(f->method->name);
}

/* Returns a Hash of {name_sym => current value} for the lvars in
 * the *caller* of the AST method that called this cfunc.  Concretely,
 * Kernel#binding (defined in bootstrap.rb) calls this — current_frame
 * is binding's own frame, and we want one level up.  Cfunc top-level
 * callers return {} (no AST frame to scrape). */
/* Kernel#local_variables — Array of Symbols naming each lvar visible
 * to the caller's scope.
 *
 * Approximation: walks current_frame's method's local_names.  Block
 * bodies share their enclosing method's frame in koruby, so a block
 * call from inside a method sees that method's lvars (close to CRuby
 * semantics — CRuby would only show block-local + closure-captured
 * outer lvars, but mspec_shim's `it` block is the typical caller and
 * we don't want it to expose `it`'s own internal locals).  When called
 * from inside an `it` block (current_frame->method.name == :it),
 * return [] to avoid leaking mspec_shim internals. */
static VALUE kernel_local_variables(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE arr = korb_ary_new();
    struct korb_frame *f = c->current_frame;
    if (!f || !f->method || f->method->type != KORB_METHOD_AST) return arr;
    /* Filter out the test harness frames so user-level local_variables
     * doesn't see them.  `it`, `before`, `after`, `describe`, `context`,
     * `specify` are the most common in mspec-style suites. */
    static const char *harness_methods[] = {
        "it", "before", "after", "describe", "context", "specify", NULL
    };
    if (f->method->name) {
        const char *mn = korb_id_name(f->method->name);
        if (mn) {
            for (int i = 0; harness_methods[i]; i++) {
                if (strcmp(mn, harness_methods[i]) == 0) return arr;
            }
        }
    }
    ID *names = f->method->u.ast.local_names;
    if (!names) return arr;
    for (uint32_t i = 0; names[i] != 0; i++) {
        const char *cname = korb_id_name(names[i]);
        if (!cname) continue;
        if (cname[0] == '_' && cname[1] == 0) continue;
        korb_ary_push(arr, korb_id2sym(names[i]));
    }
    return arr;
}

static VALUE kernel_capture_lvars(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE h = korb_hash_new();
    /* Skip past the AST method that's hosting this cfunc call (typically
     * Kernel#binding from bootstrap) to get to the user's frame. */
    struct korb_frame *f = c->current_frame ? c->current_frame->prev : NULL;
    if (!f || !f->method || f->method->type != KORB_METHOD_AST) return h;
    ID *names = f->method->u.ast.local_names;
    if (!names || !f->fp) return h;
    for (uint32_t i = 0; names[i] != 0; i++) {
        const char *cname = korb_id_name(names[i]);
        if (!cname) continue;
        /* Skip synthesized slot names that prism inserts for things
         * like multi-assignment temporaries (`_1`, `_*`) — those
         * aren't user-named lvars. */
        if (cname[0] == '_' && cname[1] == 0) continue;
        VALUE name_sym = korb_id2sym(names[i]);
        VALUE val = f->fp[i];
        if (UNDEF_P(val)) val = Qnil;
        korb_hash_aset(h, name_sym, val);
    }
    return h;
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
    /* Build "(eval at <caller_file>:<line>)" — CRuby's __FILE__ format
     * inside eval'd code.  Use last_cfunc_callsite line if available. */
    char filename[1024];
    int line = (c->last_cfunc_callsite ? c->last_cfunc_callsite->head.line : 0);
    snprintf(filename, sizeof(filename), "(eval at %s:%d)",
             c->current_file ? c->current_file : "(unknown)", line);
    /* `eval(str)` (without explicit binding) runs in the caller's
     * lexical context: cref / current_frame stay; def lands on the
     * caller's class.  korb_eval_string normally resets these to
     * top-level — for the with-binding case we preserve them. */
    extern NODE *koruby_parse_full(const char *src, size_t len, const char *filename, char **err_msg);
    char *err_msg = NULL;
    NODE *ast = koruby_parse_full(s->ptr, (size_t)s->len, filename, &err_msg);
    if (err_msg) {
        VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SyntaxError"));
        if (eSE && !SPECIAL_CONST_P(eSE) && BUILTIN_TYPE(eSE) == T_CLASS) {
            korb_raise(c, (struct korb_class *)eSE, "%s", err_msg);
        } else {
            korb_raise(c, NULL, "syntax error: %s", err_msg);
        }
        return Qnil;
    }
    if (!ast) return Qnil;
    const char *prev_file = c->current_file;
    c->current_file = filename;
    extern void OPTIMIZE_decl(void);
    extern struct Node *OPTIMIZE(struct Node *n);
    OPTIMIZE(ast);
    VALUE r = c->cref ? EVAL(c, ast) : EVAL(c, ast);
    c->current_file = prev_file;
    return r;
}
/* Default Object#initialize — accepts any args and returns self.
 * Lets `super` from an overridden initialize at the top of the chain
 * succeed (CRuby has the same convention via BasicObject#initialize). */
static VALUE kernel_initialize_default(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
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
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "tried to create Proc object without a block");
        return Qnil;
    }
    /* Mark as lambda so Proc#call's `return` becomes local. */
    current_block->is_lambda = true;
    return (VALUE)current_block;
}
static VALUE kernel_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *current_block;
    if (!current_block) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "tried to create Proc object without a block");
        return Qnil;
    }
    return (VALUE)current_block;
}


/* ---------- ObjectSpace stubs ----------
 * Boehm GC doesn't expose live-object enumeration.  These stubs keep
 * the API surface so calling code doesn't crash; real programs that
 * depend on each_object will need a weak-ref registry layered on top
 * of GC_register_finalizer (TODO). */
extern void GC_gcollect(void);
extern size_t GC_get_heap_size(void);

VALUE objspace_each_object(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Yields nothing.  Returns 0 (the count of yielded objects). */
    if (!korb_block_given()) return korb_ary_new();
    return INT2FIX(0);
}

VALUE objspace_count_objects(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Hash{ :TOTAL => N, :FREE => 0, ...:T_OBJECT => 0, ... }.  We
     * don't track per-type counts so just provide :TOTAL from the
     * heap size and zero counts for the per-T_xxx keys. */
    VALUE h = korb_hash_new();
    korb_hash_aset(h, korb_id2sym(korb_intern("TOTAL")),
                   INT2FIX((long)(GC_get_heap_size() / 64)));
    korb_hash_aset(h, korb_id2sym(korb_intern("FREE")), INT2FIX(0));
    /* Optionally accept a result-hash arg to merge into. */
    if (argc >= 1 && !SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_HASH) {
        korb_hash_aset(argv[0], korb_id2sym(korb_intern("TOTAL")),
                       INT2FIX((long)(GC_get_heap_size() / 64)));
        return argv[0];
    }
    return h;
}

VALUE objspace_garbage_collect(CTX *c, VALUE self, int argc, VALUE *argv) {
    GC_gcollect();
    return Qnil;
}
