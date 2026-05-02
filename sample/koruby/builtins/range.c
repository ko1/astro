/* Range — moved from builtins.c. */

/* ---------- Range ---------- */
static VALUE rng_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* No block → return Array stand-in (Enumerator placeholder).  An
     * Array is "Enumerable enough" for the common chains like
     * `(1..3).each.map { ... }` and `.each.to_a`. */
    if (!korb_block_given()) {
        return korb_funcall(c, self, korb_intern("to_a"), 0, NULL);
    }
    struct korb_range *r = (struct korb_range *)self;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        long stop_excl = r->exclude_end ? e : e + 1;
        for (long i = b; i < stop_excl; i++) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        return self;
    }
    /* Non-numeric ranges: walk via #succ until > end. */
    if (NIL_P(r->begin) || NIL_P(r->end)) return self;
    VALUE cur = r->begin;
    while (true) {
        VALUE cmp = korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end);
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
        korb_yield(c, 1, &cur);
        if (c->state != KORB_NORMAL) return Qnil;
        cur = korb_funcall(c, cur, korb_intern("succ"), 0, NULL);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE rng_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (argc < 1) return r->begin;
    if (!FIXNUM_P(argv[0]) || !FIXNUM_P(r->begin)) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg, "negative array size");
        return Qnil;
    }
    long b = FIX2LONG(r->begin);
    long avail;
    bool is_inf_float = false;
    if (FLONUM_P(r->end)) {
        is_inf_float = (korb_flonum_to_double(r->end) > 1e18);
    } else if (!SPECIAL_CONST_P(r->end) && BUILTIN_TYPE(r->end) == T_FLOAT) {
        is_inf_float = (((struct korb_float *)r->end)->value > 1e18);
    }
    if (NIL_P(r->end) || is_inf_float) {
        /* Endless range or `..Float::INFINITY` — supply n elements. */
        avail = n;
    } else if (FIXNUM_P(r->end)) {
        long e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        avail = e - b + 1; if (avail < 0) avail = 0;
    } else {
        return Qnil;
    }
    if (n > avail) n = avail;
    VALUE a = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(b + i));
    return a;
}
static VALUE rng_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (argc < 1) {
        /* Plain end on an inclusive range; on an exclusive range CRuby
         * still returns the stored end (not end-1).  Match that. */
        return r->end;
    }
    if (!FIXNUM_P(argv[0]) || !FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n < 0) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg, "negative array size");
        return Qnil;
    }
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    long avail = e - b + 1; if (avail < 0) avail = 0;
    if (n > avail) n = avail;
    long start = e - n + 1;
    VALUE a = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(start + i));
    return a;
}
static VALUE rng_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        long n = e - b + 1; if (n < 0) n = 0;
        VALUE a = korb_ary_new_capa(n);
        for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(b + i));
        return a;
    }
    /* Non-numeric: walk via #succ. */
    VALUE a = korb_ary_new();
    if (NIL_P(r->begin) || NIL_P(r->end)) return a;
    VALUE cur = r->begin;
    while (true) {
        VALUE cmp = korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end);
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
        korb_ary_push(a, cur);
        cur = korb_funcall(c, cur, korb_intern("succ"), 0, NULL);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return a;
}


/* ---------- Range methods (extended) ---------- */
static VALUE rng_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    /* Float step → walk in floating point so 1..3 step 0.5 yields
     * 1.0, 1.5, 2.0, 2.5, 3.0.  Otherwise integer arithmetic. */
    bool flt_step = (argc >= 1 && KORB_IS_FLOAT(argv[0]));
    if (flt_step) {
        double step = korb_num2dbl(argv[0]);
        if (step == 0.0) return self;
        double b = (double)FIX2LONG(r->begin);
        double e = (double)FIX2LONG(r->end);
        bool has_block = korb_block_given();
        VALUE out = has_block ? Qnil : korb_ary_new();
        for (double v = b; v <= e + 1e-12; v += step) {
            VALUE fv = korb_float_new(v);
            if (has_block) {
                korb_yield(c, 1, &fv);
                if (c->state != KORB_NORMAL) return Qnil;
            } else {
                korb_ary_push(out, fv);
            }
        }
        return has_block ? self : out;
    }
    long step = argc >= 1 && FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 1;
    if (step == 0) return self;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    if (!korb_block_given()) {
        VALUE a = korb_ary_new();
        for (long i = b; i <= e; i += step) korb_ary_push(a, INT2FIX(i));
        return a;
    }
    for (long i = b; i <= e; i += step) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

/* Range#zip — pair each element with the corresponding element of
 * each given Array.  Missing slots get nil. */
static VALUE rng_zip(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Materialize self via to_a then delegate to Array#zip. */
    VALUE arr = korb_funcall(c, self, korb_intern("to_a"), 0, NULL);
    if (c->state != KORB_NORMAL) return Qnil;
    return korb_funcall(c, arr, korb_intern("zip"), argc, argv);
}

/* Range#each_with_index — yields (value, index) pairs. */
static VALUE rng_each_with_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    long idx = 0;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        for (long i = b; i <= e; i++, idx++) {
            VALUE pair[2] = { INT2FIX(i), INT2FIX(idx) };
            korb_yield(c, 2, pair);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        return self;
    }
    /* Non-numeric: walk via #succ */
    if (NIL_P(r->begin) || NIL_P(r->end)) return self;
    VALUE cur = r->begin;
    while (true) {
        VALUE cmp = korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end);
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
        VALUE pair[2] = { cur, INT2FIX(idx) };
        korb_yield(c, 2, pair);
        if (c->state != KORB_NORMAL) return Qnil;
        cur = korb_funcall(c, cur, korb_intern("succ"), 0, NULL);
        if (c->state != KORB_NORMAL) return Qnil;
        idx++;
    }
    return self;
}

static VALUE rng_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    long sz = e - b + 1; if (r->exclude_end) sz--;
    if (sz < 0) sz = 0;
    return INT2FIX(sz);
}

static VALUE rng_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qfalse;
    const struct korb_range *r = (const struct korb_range *)self;
    if (!FIXNUM_P(r->begin) && !NIL_P(r->begin)) return Qfalse;
    if (!FIXNUM_P(r->end)   && !NIL_P(r->end))   return Qfalse;
    long v = FIX2LONG(argv[0]);
    /* Endless / beginless ranges: missing end is +infinity, missing
     * begin is -infinity.  Inclusive on the present side only. */
    bool ge_b = NIL_P(r->begin) || v >= FIX2LONG(r->begin);
    bool lt_e;
    if (NIL_P(r->end)) lt_e = true;
    else lt_e = r->exclude_end ? (v < FIX2LONG(r->end)) : (v <= FIX2LONG(r->end));
    return KORB_BOOL(ge_b && lt_e);
}

static VALUE rng_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!korb_block_given()) {
        return korb_funcall(c, self, korb_intern("to_a"), 0, NULL);
    }
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return korb_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new();
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(out, m);
    }
    return out;
}

static VALUE rng_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return korb_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new();
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(out, v);
    }
    return out;
}

static VALUE rng_all_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qtrue;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (!RTEST(m)) return Qfalse;
    }
    return Qtrue;
}

static VALUE rng_any_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qfalse;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) return Qtrue;
    }
    return Qfalse;
}

static VALUE rng_count(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return INT2FIX(0);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    long n = e - b + 1;
    if (r->exclude_end) n--;
    if (n < 0) n = 0;
    return INT2FIX(n);
}

static VALUE rng_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    /* Symbol-arg form: reduce(:+) or reduce(init, :+). */
    ID op = 0;
    int sym_idx = -1;
    if (argc >= 1 && SYMBOL_P(argv[argc - 1]) && !korb_block_given()) {
        op = korb_sym2id(argv[argc - 1]);
        sym_idx = argc - 1;
    }
    VALUE acc;
    long start;
    if (op != 0) {
        if (sym_idx == 0) {            /* (:+) */
            if (b > e) return Qnil;
            acc = INT2FIX(b);
            start = b + 1;
        } else {                        /* (init, :+) */
            acc = argv[0];
            start = b;
        }
        for (long i = start; i <= e; i++) {
            VALUE other = INT2FIX(i);
            acc = korb_funcall(c, acc, op, 1, &other);
            if (c->state != KORB_NORMAL) return Qnil;
        }
        return acc;
    }
    acc = argc > 0 ? argv[0] : INT2FIX(b++);
    for (long i = b; i <= e; i++) {
        VALUE args[2] = { acc, INT2FIX(i) };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}


/* ---------- Range#exclude_end? ---------- */
static VALUE rng_exclude_end_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_RANGE) return Qfalse;
    return KORB_BOOL(((struct korb_range *)self)->exclude_end);
}

