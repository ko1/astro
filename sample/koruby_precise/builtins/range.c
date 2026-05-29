/* Range — moved from builtins.c. */

/* ---------- Range ---------- */
extern VALUE korb_range_new(VALUE b, VALUE e, bool excl);

static RESULT rng_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Range.new(begin, end[, exclude_end=false]) */
    if (argc < 2) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments to Range.new");
    }
    bool excl = (argc >= 3) && RTEST(argv[2]);
    /* Validate that begin <=> end is comparable.  If both are non-nil
     * and #<=> returns nil, raise ArgumentError.  #<=> raising
     * propagates (CRuby semantics). */
    if (!NIL_P(argv[0]) && !NIL_P(argv[1])) {
        VALUE arg2[1] = { argv[1] };
        VALUE cmp = UNWRAP(korb_funcall_r(c, argv[0], korb_intern("<=>"), 1, arg2));
        if (NIL_P(cmp)) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg, "bad value for range");
        }
    }
    return RESULT_OK(korb_range_new(argv[0], argv[1], excl));
}

/* Range#hash — same begin / end / exclude_end? must hash equal. */
static RESULT rng_hash(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    VALUE bh = UNWRAP(korb_funcall(c, r->begin, korb_intern("hash"), 0, NULL));
    VALUE eh = UNWRAP(korb_funcall(c, r->end,   korb_intern("hash"), 0, NULL));
    long bn = FIXNUM_P(bh) ? FIX2LONG(bh) : 0;
    long en = FIXNUM_P(eh) ? FIX2LONG(eh) : 0;
    long h = bn ^ (en * 0x9E3779B97F4A7C15L) ^ (r->exclude_end ? 0x5A : 0);
    return RESULT_OK(INT2FIX(h));
}

static RESULT rng_each(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* No block → Array stand-in (TODO: Enumerator once Fiber path is
     * GC-safe).  Array is close enough for most chain forms. */
    if (!korb_block_given(c)) {
        return korb_funcall(c, self, korb_intern("to_a"), 0, NULL);
    }
    struct korb_range *r = (struct korb_range *)self;
    /* Beginless range: TypeError (can't iterate starting from -Inf). */
    if (NIL_P(r->begin)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "can't iterate from beginless range");
    }
    /* Integer begin (incl Bignum): step by 1 until past end. */
    if (FIXNUM_P(r->begin) && (NIL_P(r->end) || FIXNUM_P(r->end))) {
        long b = FIX2LONG(r->begin);
        if (NIL_P(r->end)) {
            /* Endless: yield forever (CRuby raises after LONG_MAX but
             * effectively infinite — user must `break`). */
            for (long i = b; ; i++) {
                VALUE v = INT2FIX(i);
                SINK_RESULT(c, korb_yield(c, 1, &v));
                if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
            }
        } else {
            long e = FIX2LONG(r->end);
            long stop_excl = r->exclude_end ? e : e + 1;
            for (long i = b; i < stop_excl; i++) {
                VALUE v = INT2FIX(i);
                SINK_RESULT(c, korb_yield(c, 1, &v));
                if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
            }
        }
        return RESULT_OK(self);
    }
    /* Non-numeric ranges: walk via #succ until > end.  Begin must respond
     * to #succ; otherwise TypeError. */
    VALUE rt = UNWRAP(korb_funcall(c, r->begin, korb_intern("respond_to?"), 1,
                            (VALUE[]){ korb_id2sym(korb_intern("succ")) }));
    if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
    if (!RTEST(rt)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT, "can't iterate from %s",
                   SPECIAL_CONST_P(r->begin) ? "(special)"
                       : korb_id_name(korb_class_of_class(r->begin)->name));
    }
    VALUE cur = r->begin;
    while (true) {
        if (NIL_P(r->end)) {
            SINK_RESULT(c, korb_yield(c, 1, &cur));
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        } else {
            VALUE cmp = UNWRAP(korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end));
            if (!FIXNUM_P(cmp)) break;
            long cv = FIX2LONG(cmp);
            if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
            SINK_RESULT(c, korb_yield(c, 1, &cur));
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        }
        cur = UNWRAP(korb_funcall(c, cur, korb_intern("succ"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(self);
}

/* Compare two values using <=>.  Returns 0 if equal, negative if a<b,
 * positive if a>b.  Returns LONG_MAX if comparison failed (nil <=>). */
static long rng_cmp(CTX *c, VALUE a, VALUE b) {
    if (FIXNUM_P(a) && FIXNUM_P(b)) {
        return (long)((intptr_t)a - (intptr_t)b);
    }
    VALUE r = SINK_RESULT(c, korb_funcall(c, a, korb_intern("<=>"), 1, &b));
    if (c->state != KORB_NORMAL || !FIXNUM_P(r)) return LONG_MAX;
    return FIX2LONG(r);
}

static RESULT rng_min(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (korb_block_given(c)) {
        /* Block form: delegate to Enumerable#min by walking via each. */
        VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        return korb_funcall(c, arr, korb_intern("min"), argc, argv);
    }
    if (argc >= 1) {
        VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        return korb_funcall(c, arr, korb_intern("min"), argc, argv);
    }
    /* No block, no arg: min == begin (or nil if begin > end). */
    if (NIL_P(r->begin)) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "cannot get the minimum of beginless range");
    }
    if (NIL_P(r->end)) return RESULT_OK(r->begin);
    long cmp = rng_cmp(c, r->begin, r->end);
    if (cmp == LONG_MAX) return RESULT_OK(Qnil);
    if (cmp > 0) return RESULT_OK(Qnil);
    if (cmp == 0 && r->exclude_end) return RESULT_OK(Qnil);
    return RESULT_OK(r->begin);
}

static RESULT rng_max(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (korb_block_given(c)) {
        VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        return korb_funcall(c, arr, korb_intern("max"), argc, argv);
    }
    if (argc >= 1) {
        VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        return korb_funcall(c, arr, korb_intern("max"), argc, argv);
    }
    if (NIL_P(r->end)) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "cannot get the maximum of endless range");
    }
    if (NIL_P(r->begin)) {
        /* Beginless range with exclusive end: TypeError unless end is
         * Integer (CRuby's rule). */
        if (r->exclude_end) {
            if (FIXNUM_P(r->end) || (!SPECIAL_CONST_P(r->end) && BUILTIN_TYPE(r->end) == T_BIGNUM)) {
                /* Integer: max == end - 1 */
                VALUE one = INT2FIX(1);
                return korb_funcall(c, r->end, korb_intern("-"), 1, &one);
            }
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "cannot exclude non Integer end value");
        }
        return RESULT_OK(r->end);
    }
    long cmp = rng_cmp(c, r->begin, r->end);
    if (cmp == LONG_MAX) return RESULT_OK(Qnil);
    if (cmp > 0) return RESULT_OK(Qnil);
    if (cmp == 0 && r->exclude_end) return RESULT_OK(Qnil);
    if (!r->exclude_end) return RESULT_OK(r->end);
    /* Exclusive: max == end - 1 only for Integer end.  Else TypeError. */
    if (FIXNUM_P(r->end) || (!SPECIAL_CONST_P(r->end) && BUILTIN_TYPE(r->end) == T_BIGNUM)) {
        VALUE one = INT2FIX(1);
        return korb_funcall(c, r->end, korb_intern("-"), 1, &one);
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eT, "cannot exclude non Integer end value");
}

/* Range#begin — returns the begin field directly (nil for beginless).
 * Unlike #first, never raises.  CRuby semantics. */
static RESULT rng_begin(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    (void)argc; (void)sp;
    return RESULT_OK(((struct korb_range *)self)->begin);
}

/* Range#end / Range#last (no args) — returns the end field directly
 * (nil for endless).  #last with args raises for endless. */
static RESULT rng_end(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    (void)argc; (void)sp;
    return RESULT_OK(((struct korb_range *)self)->end);
}

static RESULT rng_first(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (argc < 1) {
        if (NIL_P(r->begin)) {
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR, "cannot get the first element of beginless range");
        }
        return RESULT_OK(r->begin);
    }
    /* Beginless range with argument: always RangeError. */
    if (NIL_P(r->begin)) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "cannot get the first element of beginless range");
    }
    /* Coerce non-Fixnum count via #to_int, raise TypeError if invalid. */
    VALUE nv = argv[0];
    if (NIL_P(nv)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT, "no implicit conversion from nil to integer");
    }
    if (!FIXNUM_P(nv)) {
        /* Float: CRuby truncates to integer. */
        if (FLONUM_P(nv) || (!SPECIAL_CONST_P(nv) && BUILTIN_TYPE(nv) == T_FLOAT)) {
            double d = korb_num2dbl(nv);
            nv = INT2FIX((long)d);
        } else if (!SPECIAL_CONST_P(nv)) {
            VALUE rt = UNWRAP(korb_funcall(c, nv, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (RTEST(rt)) {
                nv = UNWRAP(korb_funcall(c, nv, korb_intern("to_int"), 0, NULL));
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            }
        }
        if (!FIXNUM_P(nv)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion into Integer");
        }
    }
    /* Non-numeric begin (`('a'..'e').first(2)`): delegate to to_a then take. */
    if (!FIXNUM_P(r->begin)) {
        VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
        if (c->state != KORB_NORMAL || BUILTIN_TYPE(arr) != T_ARRAY) return RESULT_OK(Qnil);
        return korb_funcall(c, arr, korb_intern("first"), 1, &nv);
    }
    long n = FIX2LONG(nv);
    if (n < 0) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "negative array size");
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
        return RESULT_OK(Qnil);
    }
    if (n > avail) n = avail;
    VALUE a = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(b + i));
    return RESULT_OK(a);
}
static RESULT rng_last(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (argc < 1) {
        /* Plain end on an inclusive range; on an exclusive range CRuby
         * still returns the stored end (not end-1).  Match that. */
        if (NIL_P(r->end)) {
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR, "cannot get the last element of endless range");
        }
        return RESULT_OK(r->end);
    }
    /* Endless range with argument: RangeError. */
    if (NIL_P(r->end)) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "cannot get the last element of endless range");
    }
    VALUE nv = argv[0];
    if (NIL_P(nv)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT, "no implicit conversion from nil to integer");
    }
    if (!FIXNUM_P(nv)) {
        if (FLONUM_P(nv) || (!SPECIAL_CONST_P(nv) && BUILTIN_TYPE(nv) == T_FLOAT)) {
            nv = INT2FIX((long)korb_num2dbl(nv));
        } else if (!SPECIAL_CONST_P(nv)) {
            VALUE rt = UNWRAP(korb_funcall(c, nv, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (RTEST(rt)) {
                nv = UNWRAP(korb_funcall(c, nv, korb_intern("to_int"), 0, NULL));
                if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            }
        }
        if (!FIXNUM_P(nv)) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion into Integer");
        }
    }
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qnil);
    long n = FIX2LONG(nv);
    if (n < 0) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "negative array size");
    }
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    long avail = e - b + 1; if (avail < 0) avail = 0;
    if (n > avail) n = avail;
    long start = e - n + 1;
    VALUE a = korb_ary_new_capa(c, c->sp, n);
    for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(start + i));
    return RESULT_OK(a);
}
static RESULT rng_to_a(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        long n = e - b + 1; if (n < 0) n = 0;
        VALUE a = korb_ary_new_capa(c, c->sp, n);
        for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(b + i));
        return RESULT_OK(a);
    }
    /* Non-numeric: walk via #succ. */
    VALUE a = korb_ary_new(c, c->sp);
    if (NIL_P(r->begin) || NIL_P(r->end)) return RESULT_OK(a);
    VALUE cur = r->begin;
    while (true) {
        VALUE cmp = UNWRAP(korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end));
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
        korb_ary_push(a, cur);
        cur = UNWRAP(korb_funcall(c, cur, korb_intern("succ"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(a);
}


/* ---------- Range methods (extended) ---------- */
static RESULT rng_step(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    const struct korb_range *r = (const struct korb_range *)self;
    bool begin_num = FIXNUM_P(r->begin) || KORB_IS_FLOAT(r->begin);
    bool end_num = FIXNUM_P(r->end) || KORB_IS_FLOAT(r->end);
    if (!begin_num || !end_num) return RESULT_OK(Qnil);
    /* Float step OR float endpoints → walk in floating point so
     * 1..3 step 0.5 yields 1.0, 1.5, 2.0, 2.5, 3.0.  Otherwise
     * integer arithmetic. */
    bool flt_step = (argc >= 1 && KORB_IS_FLOAT(argv[0]))
                  || KORB_IS_FLOAT(r->begin) || KORB_IS_FLOAT(r->end);
    if (flt_step) {
        double step = (argc >= 1) ? korb_num2dbl(argv[0]) : 1.0;
        if (step == 0.0) return RESULT_OK(self);
        double b = korb_num2dbl(r->begin);
        double e = korb_num2dbl(r->end);
        bool has_block = korb_block_given(c);
        VALUE out = has_block ? Qnil : korb_ary_new(c, c->sp);
        for (double v = b; r->exclude_end ? (v < e) : (v <= e + 1e-12); v += step) {
            VALUE fv = korb_float_new(c, c->sp, v);
            if (has_block) {
                SINK_RESULT(c, korb_yield(c, 1, &fv));
                if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
            } else {
                korb_ary_push(out, fv);
            }
        }
        return RESULT_OK(has_block ? self : out);
    }
    long step = argc >= 1 && FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 1;
    if (step == 0) return RESULT_OK(self);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    if (!korb_block_given(c)) {
        VALUE a = korb_ary_new(c, c->sp);
        for (long i = b; i <= e; i += step) korb_ary_push(a, INT2FIX(i));
        return RESULT_OK(a);
    }
    for (long i = b; i <= e; i += step) {
        VALUE v = INT2FIX(i);
        SINK_RESULT(c, korb_yield(c, 1, &v));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(self);
}

/* Range#zip — pair each element with the corresponding element of
 * each given Array.  Missing slots get nil. */
static RESULT rng_zip(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Materialize self via to_a then delegate to Array#zip. */
    VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
    if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    return korb_funcall(c, arr, korb_intern("zip"), argc, argv);
}

/* Range#each_with_index — yields (value, index) pairs. */
static RESULT rng_each_with_index(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    long idx = 0;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        for (long i = b; i <= e; i++, idx++) {
            VALUE pair[2] = { INT2FIX(i), INT2FIX(idx) };
            SINK_RESULT(c, korb_yield(c, 2, pair));
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        }
        return RESULT_OK(self);
    }
    /* Non-numeric: walk via #succ */
    if (NIL_P(r->begin) || NIL_P(r->end)) return RESULT_OK(self);
    VALUE cur = r->begin;
    while (true) {
        VALUE cmp = UNWRAP(korb_funcall(c, cur, korb_intern("<=>"), 1, &r->end));
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
        VALUE pair[2] = { cur, INT2FIX(idx) };
        SINK_RESULT(c, korb_yield(c, 2, pair));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        cur = UNWRAP(korb_funcall(c, cur, korb_intern("succ"), 0, NULL));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        idx++;
    }
    return RESULT_OK(self);
}

static RESULT rng_size(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    bool b_numeric = FIXNUM_P(r->begin) ||
        (!SPECIAL_CONST_P(r->begin) &&
         (BUILTIN_TYPE(r->begin) == T_BIGNUM ||
          BUILTIN_TYPE(r->begin) == T_FLOAT)) ||
        FLONUM_P(r->begin);
    bool e_numeric = FIXNUM_P(r->end) ||
        (!SPECIAL_CONST_P(r->end) &&
         (BUILTIN_TYPE(r->end) == T_BIGNUM ||
          BUILTIN_TYPE(r->end) == T_FLOAT)) ||
        FLONUM_P(r->end);
    /* Endless range with numeric begin → Float::INFINITY. */
    if (NIL_P(r->end)) {
        if (NIL_P(r->begin)) return RESULT_OK(Qnil);
        if (!b_numeric) return RESULT_OK(Qnil);
        VALUE finf = korb_const_get(KORB_VM(c)->float_class, korb_intern("INFINITY"));
        return RESULT_OK(UNDEF_P(finf) ? Qnil : finf);
    }
    if (NIL_P(r->begin)) {
        if (!e_numeric) return RESULT_OK(Qnil);
        VALUE finf = korb_const_get(KORB_VM(c)->float_class, korb_intern("INFINITY"));
        return RESULT_OK(UNDEF_P(finf) ? Qnil : finf);
    }
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        long sz = e - b + 1; if (r->exclude_end) sz--;
        if (sz < 0) sz = 0;
        return RESULT_OK(INT2FIX(sz));
    }
    /* Float::INFINITY end → infinite (CRuby returns Float::INFINITY). */
    if (FIXNUM_P(r->begin) && (KORB_IS_FLOAT(r->end) || FLONUM_P(r->end))) {
        double e = korb_num2dbl(r->end);
        if (e > 1e18 || isinf(e)) {
            VALUE finf = korb_const_get(KORB_VM(c)->float_class, korb_intern("INFINITY"));
            return RESULT_OK(UNDEF_P(finf) ? Qnil : finf);
        }
    }
    /* Non-numeric (e.g. String range): CRuby returns nil for non-Numeric
     * ranges' size. */
    if (!b_numeric || !e_numeric) return RESULT_OK(Qnil);
    /* Numeric mixed: delegate to to_a length. */
    VALUE arr = UNWRAP(korb_funcall(c, self, korb_intern("to_a"), 0, NULL));
    if (c->state == KORB_NORMAL && BUILTIN_TYPE(arr) == T_ARRAY) {
        return RESULT_OK(INT2FIX(((struct korb_array *)arr)->len));
    }
    return RESULT_OK(Qnil);
}

static RESULT rng_include(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    const struct korb_range *r = (const struct korb_range *)self;
    /* Numeric range with FIXNUM endpoints: fast path. */
    if (FIXNUM_P(argv[0]) &&
        (FIXNUM_P(r->begin) || NIL_P(r->begin)) &&
        (FIXNUM_P(r->end)   || NIL_P(r->end))) {
        long v = FIX2LONG(argv[0]);
        bool ge_b = NIL_P(r->begin) || v >= FIX2LONG(r->begin);
        bool lt_e;
        if (NIL_P(r->end)) lt_e = true;
        else lt_e = r->exclude_end ? (v < FIX2LONG(r->end)) : (v <= FIX2LONG(r->end));
        return RESULT_OK(KORB_BOOL(ge_b && lt_e));
    }
    /* Non-numeric ranges (e.g. ("a".."z")): walk via #<=>.  We can't
     * just use cover? semantics here because String#<=> on bytes is
     * order-preserving but not a proper "discrete includes" relation
     * — but for the koruby subset (no full encoding awareness) the
     * comparison is sufficient. */
    VALUE arg = argv[0];
    VALUE end_copy = r->end;
    if (!NIL_P(r->begin)) {
        VALUE cmp = UNWRAP(korb_funcall(c, r->begin, korb_intern("<=>"), 1, &arg));
        if (!FIXNUM_P(cmp) || FIX2LONG(cmp) > 0) return RESULT_OK(Qfalse);
    }
    if (!NIL_P(r->end)) {
        VALUE cmp = UNWRAP(korb_funcall(c, arg, korb_intern("<=>"), 1, &end_copy));
        if (!FIXNUM_P(cmp)) return RESULT_OK(Qfalse);
        long cv = FIX2LONG(cmp);
        if (r->exclude_end ? (cv >= 0) : (cv > 0)) return RESULT_OK(Qfalse);
    }
    return RESULT_OK(Qtrue);
}

static RESULT rng_map(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        return korb_funcall(c, self, korb_intern("to_a"), 0, NULL);
    }
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(korb_ary_new(c, c->sp));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new(c, c->sp);
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = SINK_RESULT(c, korb_yield(c, 1, &v));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        korb_ary_push(out, m);
    }
    return RESULT_OK(out);
}

static RESULT rng_select(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(korb_ary_new(c, c->sp));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new(c, c->sp);
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = SINK_RESULT(c, korb_yield(c, 1, &v));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        if (RTEST(m)) korb_ary_push(out, v);
    }
    return RESULT_OK(out);
}

static RESULT rng_all_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qtrue);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = SINK_RESULT(c, korb_yield(c, 1, &v));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        if (!RTEST(m)) return RESULT_OK(Qfalse);
    }
    return RESULT_OK(Qtrue);
}

static RESULT rng_any_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qfalse);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = SINK_RESULT(c, korb_yield(c, 1, &v));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        if (RTEST(m)) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT rng_count(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    /* Beginless or endless ranges have infinite element count when no
     * argument or block is given to filter (CRuby semantics).  Float
     * infinity is the conventional carrier for "infinite". */
    if (argc == 0 && !korb_block_given(c) &&
        (NIL_P(r->begin) || NIL_P(r->end))) {
        return RESULT_OK(korb_float_new(c, c->sp, 1.0/0.0));
    }
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(INT2FIX(0));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    long n = e - b + 1;
    if (r->exclude_end) n--;
    if (n < 0) n = 0;
    return RESULT_OK(INT2FIX(n));
}

static RESULT rng_reduce(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qnil);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    /* Symbol-arg form: reduce(:+) or reduce(init, :+). */
    ID op = 0;
    int sym_idx = -1;
    if (argc >= 1 && SYMBOL_P(argv[argc - 1]) && !korb_block_given(c)) {
        op = korb_sym2id(argv[argc - 1]);
        sym_idx = argc - 1;
    }
    VALUE acc;
    long start;
    if (op != 0) {
        if (sym_idx == 0) {            /* (:+) */
            if (b > e) return RESULT_OK(Qnil);
            acc = INT2FIX(b);
            start = b + 1;
        } else {                        /* (init, :+) */
            acc = argv[0];
            start = b;
        }
        for (long i = start; i <= e; i++) {
            VALUE other = INT2FIX(i);
            acc = UNWRAP(korb_funcall(c, acc, op, 1, &other));
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        }
        return RESULT_OK(acc);
    }
    acc = argc > 0 ? argv[0] : INT2FIX(b++);
    for (long i = b; i <= e; i++) {
        VALUE args[2] = { acc, INT2FIX(i) };
        acc = SINK_RESULT(c, korb_yield(c, 2, args));
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(acc);
}


/* ---------- Range#exclude_end? ---------- */
static RESULT rng_exclude_end_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_RANGE) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(((struct korb_range *)self)->exclude_end));
}

