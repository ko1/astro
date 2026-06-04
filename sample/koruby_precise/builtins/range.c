/* Range — moved from builtins.c. */

/* ---------- Range ---------- */
extern VALUE korb_range_new(CTX *c, VALUE *sp, VALUE b, VALUE e, bool excl);

/* CRuby String#upto overshoot guard for succ-based Range iteration: stop once
 * the cursor's string length exceeds the end's.  Without it, ('A'..'z') /
 * (:A..:z) loop forever — String#succ carries 'Z'->'AA', which stays < 'z' by
 * <=> so the cmp test never terminates and the accumulator grows until it
 * SEGVs.  Returns the comparison length for String/Symbol, or -1 (no guard). */
static long range_succ_len(VALUE v) {
    if (SPECIAL_CONST_P(v)) {
        if (SYMBOL_P(v)) {
            const char *n = korb_id_name(korb_sym2id(v));
            return n ? (long)strlen(n) : -1;
        }
        return -1;
    }
    if (BUILTIN_TYPE(v) == T_STRING) return ((struct korb_string *)v)->len;
    return -1;
}

/* KORB_RNG_YIELD_FRAME — park a cross-yield root (an accumulator array or a
 * moving accumulator value) in a synthetic frame's last_line slot, made
 * current for the duration of a yield loop.  Structurally identical to
 * array.c's KORB_ARY_YIELD_FRAME: korb_yield runs the block body at the
 * block's own (lower) sp, shrinking the GC scan range [stack_base, c->sp_top),
 * so a root parked in an sp[] slot above that level gets collected under the
 * moving GC.  The frame chain is ALWAYS walked by visit_roots (last_line /
 * last_match are forwarded), so a root stashed there survives.  Caller MUST
 * restore c->current_frame = fr.prev on every exit path. */
#define KORB_RNG_YIELD_FRAME(c, fr, init_expr)                       \
    struct korb_frame fr = {                                         \
        .prev          = (c)->current_frame,                         \
        .self          = (c)->current_frame->self,                   \
        .fp            = (c)->current_frame->fp,                      \
        .cref          = (c)->current_frame->cref,                   \
        .current_class = (c)->current_frame->current_class,          \
        .current_file  = (c)->current_frame->current_file,           \
        .block         = (c)->current_frame->block,                  \
        .last_line     = Qnil,                                       \
        .last_match    = (c)->current_frame->last_match,             \
    };                                                               \
    fr.last_line = (init_expr);                                      \
    (c)->current_frame = &fr

static RESULT rng_class_new(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
        VALUE cmp = UNWRAP(korb_funcall_r(c, c->sp_top, argv[0], korb_intern("<=>"), 1, arg2));
        if (NIL_P(cmp)) {
            VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
            return korb_raise(c, (struct korb_class *)eArg, "bad value for range");
        }
    }
    return RESULT_OK(korb_range_new(c, c->sp_top, argv[0], argv[1], excl));
}

/* Range#hash — same begin / end / exclude_end? must hash equal. */
static RESULT rng_hash(CTX *c, int argc, VALUE *sp) {
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
    (void)argv;

    struct korb_range *r = (struct korb_range *)self;
    sp[0] = r->begin;                                          /* recv on the stack */
    VALUE bh = UNWRAP(korb_call(c, sp + 1, korb_intern("hash"), 0));
    r = (struct korb_range *)sp[-argc - 1];                    /* re-read: self moved across the call */
    sp[0] = r->end;
    VALUE eh = UNWRAP(korb_call(c, sp + 1, korb_intern("hash"), 0));
    long bn = FIXNUM_P(bh) ? FIX2LONG(bh) : 0;
    long en = FIXNUM_P(eh) ? FIX2LONG(eh) : 0;
    long h = bn ^ (en * 0x9E3779B97F4A7C15L) ^ (r->exclude_end ? 0x5A : 0);
    return RESULT_OK(INT2FIX(h));
}

static RESULT rng_each(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* No block → Array stand-in (TODO: Enumerator once Fiber path is
     * GC-safe).  Array is close enough for most chain forms. */
    if (!korb_block_given(c)) {
        return korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL);
    }
    /* Beginless range: TypeError (can't iterate starting from -Inf). */
    if (NIL_P(((struct korb_range *)self)->begin)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT,
                   "can't iterate from beginless range");
    }
    /* Park the range receiver (fr.last_match) and, for non-numeric ranges,
     * the current element (fr.last_line) across the per-step korb_yield /
     * korb_funcall.  Those lower sp_top below sp[-argc-1] / relocate the
     * range, so the C-locals self/r/cur would go stale; the frame chain is
     * always walked by visit_roots so the parked slots stay forwarded.
     * Restore c->current_frame = fr.prev on every exit. */
    KORB_RNG_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];   /* range receiver (re-read fresh) */
    {
        struct korb_range *r = (struct korb_range *)fr.last_match;
        /* Integer begin (incl Bignum): step by 1 until past end. */
        if (FIXNUM_P(r->begin) && (NIL_P(r->end) || FIXNUM_P(r->end))) {
            long b = FIX2LONG(r->begin);
            bool endless = NIL_P(r->end);
            long stop_excl = 0;
            if (!endless) {
                long e = FIX2LONG(r->end);
                stop_excl = r->exclude_end ? e : e + 1;
            }
            for (long i = b; endless || i < stop_excl; i++) {
                VALUE v = INT2FIX(i);
                RESULT _y = korb_yield(c, c->sp_top, 1, &v);
                if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
            }
            VALUE result = fr.last_match;
            c->current_frame = fr.prev;
            return RESULT_OK(result);
        }
    }
    /* Non-numeric ranges: walk via #succ until > end.  Begin must respond
     * to #succ; otherwise TypeError.  Pre-intern all IDs / symbols up front:
     * korb_intern / korb_id2sym can fire GC (symbol-table growth), and if it
     * runs while a range-field read sits in a funcall argument temporary,
     * that temporary goes stale (C arg-eval order). */
    const ID id_respond = korb_intern("respond_to?");
    const ID id_succ    = korb_intern("succ");
    const ID id_cmp     = korb_intern("<=>");
    VALUE succ_sym      = korb_id2sym(id_succ);
    {
        struct korb_range *r = (struct korb_range *)fr.last_match;
        RESULT _rt = korb_funcall(c, c->sp_top, r->begin, id_respond, 1, &succ_sym);
        if (_rt.state != KORB_NORMAL) { c->current_frame = fr.prev; return _rt; }
        if (!RTEST(_rt.value)) {
            r = (struct korb_range *)fr.last_match;   /* re-read after funcall */
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            const char *bn = SPECIAL_CONST_P(r->begin) ? "(special)"
                               : korb_id_name(korb_class_of_class(r->begin)->name);
            c->current_frame = fr.prev;
            return korb_raise(c, (struct korb_class *)eT, "can't iterate from %s", bn);
        }
    }
    fr.last_line = ((struct korb_range *)fr.last_match)->begin;   /* cur */
    while (true) {
        struct korb_range *r = (struct korb_range *)fr.last_match;
        if (NIL_P(r->end)) {
            RESULT _y = korb_yield(c, c->sp_top, 1, &fr.last_line);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        } else {
            VALUE end = r->end;
            RESULT _cm = korb_funcall(c, c->sp_top, fr.last_line, id_cmp, 1, &end);
            if (_cm.state != KORB_NORMAL) { c->current_frame = fr.prev; return _cm; }
            if (!FIXNUM_P(_cm.value)) break;
            long cv = FIX2LONG(_cm.value);
            r = (struct korb_range *)fr.last_match;   /* re-read exclude_end */
            if (r->exclude_end ? (cv >= 0) : (cv > 0)) break;
            { long cl = range_succ_len(fr.last_line), el = range_succ_len(end);
              if (cl >= 0 && el >= 0 && cl > el) break; }
            RESULT _y = korb_yield(c, c->sp_top, 1, &fr.last_line);
            if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        }
        RESULT _sx = korb_funcall(c, c->sp_top, fr.last_line, id_succ, 0, NULL);
        if (_sx.state != KORB_NORMAL) { c->current_frame = fr.prev; return _sx; }
        fr.last_line = _sx.value;
    }
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

/* Compare two values using <=>.  Returns 0 if equal, negative if a<b,
 * positive if a>b.  Returns LONG_MAX if comparison failed (nil <=>).
 * On raise, sets *err and returns LONG_MAX. */
static long rng_cmp(CTX *c, VALUE a, VALUE b, RESULT *err) {
    if (FIXNUM_P(a) && FIXNUM_P(b)) {
        return (long)((intptr_t)a - (intptr_t)b);
    }
    RESULT _r = korb_funcall(c, c->sp_top, a, korb_intern("<=>"), 1, &b);
    if (_r.state != KORB_NORMAL) { *err = _r; return LONG_MAX; }
    VALUE r = _r.value;
    if (!FIXNUM_P(r)) return LONG_MAX;
    return FIX2LONG(r);
}

static RESULT rng_min(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (korb_block_given(c)) {
        /* Block form: delegate to Enumerable#min by walking via each. */
        VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
        return korb_funcall(c, c->sp_top, arr, korb_intern("min"), argc, argv);
    }
    if (argc >= 1) {
        VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
        return korb_funcall(c, c->sp_top, arr, korb_intern("min"), argc, argv);
    }
    /* No block, no arg: min == begin (or nil if begin > end). */
    if (NIL_P(r->begin)) {
        VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
        return korb_raise(c, (struct korb_class *)eR, "cannot get the minimum of beginless range");
    }
    if (NIL_P(r->end)) return RESULT_OK(r->begin);
    RESULT _err = RESULT_OK(Qnil);
    long cmp = rng_cmp(c, r->begin, r->end, &_err);
    if (_err.state != KORB_NORMAL) return _err;
    r = (struct korb_range *)sp[-argc - 1];   /* range moved across rng_cmp's funcall */
    if (cmp == LONG_MAX) return RESULT_OK(Qnil);
    if (cmp > 0) return RESULT_OK(Qnil);
    if (cmp == 0 && r->exclude_end) return RESULT_OK(Qnil);
    return RESULT_OK(r->begin);
}

static RESULT rng_max(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (korb_block_given(c)) {
        VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
        return korb_funcall(c, c->sp_top, arr, korb_intern("max"), argc, argv);
    }
    if (argc >= 1) {
        VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
        return korb_funcall(c, c->sp_top, arr, korb_intern("max"), argc, argv);
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
                return korb_funcall(c, c->sp_top, r->end, korb_intern("-"), 1, &one);
            }
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "cannot exclude non Integer end value");
        }
        return RESULT_OK(r->end);
    }
    RESULT _err2 = RESULT_OK(Qnil);
    long cmp = rng_cmp(c, r->begin, r->end, &_err2);
    if (_err2.state != KORB_NORMAL) return _err2;
    r = (struct korb_range *)sp[-argc - 1];   /* range moved across rng_cmp's funcall */
    if (cmp == LONG_MAX) return RESULT_OK(Qnil);
    if (cmp > 0) return RESULT_OK(Qnil);
    if (cmp == 0 && r->exclude_end) return RESULT_OK(Qnil);
    if (!r->exclude_end) return RESULT_OK(r->end);
    /* Exclusive: max == end - 1 for Integer end. */
    if (FIXNUM_P(r->end) || (!SPECIAL_CONST_P(r->end) && BUILTIN_TYPE(r->end) == T_BIGNUM)) {
        VALUE one = INT2FIX(1);
        return korb_funcall(c, c->sp_top, r->end, korb_intern("-"), 1, &one);
    }
    /* Float exclusive end: CRuby raises TypeError. */
    if (FLONUM_P(r->end) || (!SPECIAL_CONST_P(r->end) && BUILTIN_TYPE(r->end) == T_FLOAT)) {
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
        return korb_raise(c, (struct korb_class *)eT, "cannot exclude non Integer end value");
    }
    /* Non-numeric end (String etc.): fall back to to_a.max which uses
     * succ-based iteration. */
    VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
    return korb_funcall(c, c->sp_top, arr, korb_intern("max"), 0, NULL);
}

/* Range#begin — returns the begin field directly (nil for beginless).
 * Unlike #first, never raises.  CRuby semantics. */
static RESULT rng_begin(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    (void)argc; (void)sp;
    return RESULT_OK(((struct korb_range *)self)->begin);
}

/* Range#end / Range#last (no args) — returns the end field directly
 * (nil for endless).  #last with args raises for endless. */
static RESULT rng_end(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    (void)argc; (void)sp;
    return RESULT_OK(((struct korb_range *)self)->end);
}

static RESULT rng_first(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (RTEST(rt)) {
                nv = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("to_int"), 0, NULL));
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
        VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
        if (BUILTIN_TYPE(arr) != T_ARRAY) return RESULT_OK(Qnil);
        return korb_funcall(c, c->sp_top, arr, korb_intern("first"), 1, &nv);
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
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], INT2FIX(b + i));
    return RESULT_OK(sp[0]);
}
static RESULT rng_last(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
            VALUE rt = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
            if (RTEST(rt)) {
                nv = UNWRAP(korb_funcall(c, c->sp_top, nv, korb_intern("to_int"), 0, NULL));
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
    sp[0] = korb_ary_new_capa(c, sp + 1, n);
    for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], INT2FIX(start + i));
    return RESULT_OK(sp[0]);
}
static RESULT rng_to_a(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        long n = e - b + 1; if (n < 0) n = 0;
        sp[0] = korb_ary_new_capa(c, sp + 1, n);
        for (long i = 0; i < n; i++) korb_ary_push(c, sp + 1, sp[0], INT2FIX(b + i));
        return RESULT_OK(sp[0]);
    }
    /* Non-numeric: walk via #succ.  Result array parked at sp[0], cursor at
     * sp[1] so both survive the funcall GC points; staging starts at sp+2.
     * The range is now arena (moving) — re-read it from sp[-argc-1] after each
     * GC and copy ->end into a local (don't pass &r->end of a moving obj).
     * Pre-intern IDs so no symbol-table GC strands the freshly-read end. */
    const ID id_cmp = korb_intern("<=>");
    const ID id_succ = korb_intern("succ");
    sp[0] = korb_ary_new(c, sp + 1);
    {
        struct korb_range *r2 = (struct korb_range *)sp[-argc - 1];
        if (NIL_P(r2->begin) || NIL_P(r2->end)) return RESULT_OK(sp[0]);
        sp[1] = r2->begin;
    }
    while (true) {
        struct korb_range *r2 = (struct korb_range *)sp[-argc - 1];
        VALUE end_v = r2->end;
        bool excl = r2->exclude_end;
        VALUE cmp = UNWRAP(korb_funcall(c, c->sp_top, sp[1], id_cmp, 1, &end_v));
        if (!FIXNUM_P(cmp)) break;
        long cv = FIX2LONG(cmp);
        if (excl ? (cv >= 0) : (cv > 0)) break;
        { long cl = range_succ_len(sp[1]), el = range_succ_len(end_v);
          if (cl >= 0 && el >= 0 && cl > el) break; }
        korb_ary_push(c, sp + 2, sp[0], sp[1]);
        sp[1] = UNWRAP(korb_funcall(c, c->sp_top, sp[1], id_succ, 0, NULL));
    }
    return RESULT_OK(sp[0]);
}


/* ---------- Range methods (extended) ---------- */
static RESULT rng_step(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
        bool fexcl = r->exclude_end;   /* capture: r (moving) goes stale in the loop */
        bool has_block = korb_block_given(c);
        sp[0] = has_block ? Qnil : korb_ary_new(c, sp + 1);
        for (double v = b; fexcl ? (v < e) : (v <= e + 1e-12); v += step) {
            VALUE fv = korb_float_new(c, sp + 1, v);
            if (has_block) {
                CHECK(korb_yield(c, c->sp_top, 1, &fv));
            } else {
                korb_ary_push(c, sp + 1, sp[0], fv);
            }
        }
        return RESULT_OK(has_block ? sp[-argc - 1] : sp[0]);
    }
    long step = argc >= 1 && FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 1;
    if (step == 0) return RESULT_OK(self);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) {
        if (step > 0) e--;
        else if (step < 0) e++;
    }
    if (!korb_block_given(c)) {
        sp[0] = korb_ary_new(c, sp + 1);
        if (step > 0) {
            for (long i = b; i <= e; i += step) korb_ary_push(c, sp + 1, sp[0], INT2FIX(i));
        } else {
            for (long i = b; i >= e; i += step) korb_ary_push(c, sp + 1, sp[0], INT2FIX(i));
        }
        return RESULT_OK(sp[0]);
    }
    if (step > 0) {
        for (long i = b; i <= e; i += step) {
            VALUE v = INT2FIX(i);
            CHECK(korb_yield(c, c->sp_top, 1, &v));
        }
    } else {
        for (long i = b; i >= e; i += step) {
            VALUE v = INT2FIX(i);
            CHECK(korb_yield(c, c->sp_top, 1, &v));
        }
    }
    return RESULT_OK(sp[-argc - 1]);   /* self moved across the yields */
}

/* Range#zip — pair each element with the corresponding element of
 * each given Array.  Missing slots get nil. */
static RESULT rng_zip(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Materialize self via to_a then delegate to Array#zip. */
    VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
    return korb_funcall(c, c->sp_top, arr, korb_intern("zip"), argc, argv);
}

/* Range#each_with_index — yields (value, index) pairs. */
static RESULT rng_each_with_index(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    long idx = 0;
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        for (long i = b; i <= e; i++, idx++) {
            VALUE pair[2] = { INT2FIX(i), INT2FIX(idx) };
            CHECK(korb_yield(c, c->sp_top, 2, pair));
        }
        return RESULT_OK(sp[-argc - 1]);   /* self moved across the yields */
    }
    /* Non-numeric: walk via #succ.  Park range (fr.last_match) + cursor
     * (fr.last_line) across the per-step funcall/yield; the range is now
     * arena (moving) so re-read it and copy ->end into a local. */
    {
        struct korb_range *r2 = (struct korb_range *)sp[-argc - 1];
        if (NIL_P(r2->begin) || NIL_P(r2->end)) return RESULT_OK(sp[-argc - 1]);
    }
    const ID id_cmp = korb_intern("<=>");
    const ID id_succ = korb_intern("succ");
    KORB_RNG_YIELD_FRAME(c, fr, Qnil);
    fr.last_match = sp[-argc - 1];
    fr.last_line = ((struct korb_range *)fr.last_match)->begin;
    while (true) {
        struct korb_range *r2 = (struct korb_range *)fr.last_match;
        VALUE end_v = r2->end;
        bool excl = r2->exclude_end;
        RESULT _cm = korb_funcall(c, c->sp_top, fr.last_line, id_cmp, 1, &end_v);
        if (_cm.state != KORB_NORMAL) { c->current_frame = fr.prev; return _cm; }
        if (!FIXNUM_P(_cm.value)) break;
        long cv = FIX2LONG(_cm.value);
        if (excl ? (cv >= 0) : (cv > 0)) break;
        VALUE pair[2] = { fr.last_line, INT2FIX(idx) };
        RESULT _y = korb_yield(c, c->sp_top, 2, pair);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        RESULT _sx = korb_funcall(c, c->sp_top, fr.last_line, id_succ, 0, NULL);
        if (_sx.state != KORB_NORMAL) { c->current_frame = fr.prev; return _sx; }
        fr.last_line = _sx.value;
        idx++;
    }
    VALUE result = fr.last_match;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT rng_size(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
    /* Non-numeric ranges: CRuby returns nil for String / Symbol
     * (which have #succ) but raises TypeError "can't iterate from X"
     * for arbitrary objects without #succ. */
    if (!b_numeric || !e_numeric) {
        struct korb_class *bk = korb_class_of_class(r->begin);
        bool has_succ = (bk && korb_class_find_method(bk, korb_intern("succ")));
        if (!has_succ) {
            return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")),
                              "can't iterate from %s",
                              bk ? korb_id_name(bk->name) : "(special)");
        }
        return RESULT_OK(Qnil);
    }
    /* Numeric mixed: delegate to to_a length. */
    VALUE arr = UNWRAP(korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL));
    if (BUILTIN_TYPE(arr) == T_ARRAY) {
        return RESULT_OK(INT2FIX(((struct korb_array *)arr)->len));
    }
    return RESULT_OK(Qnil);
}

static RESULT rng_include(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
    /* Range is arena (moving): re-read it from sp[-argc-1] after each funcall
     * and copy begin/end into locals (don't pass &r->field of a moving obj).
     * argv[0] (the include? arg) is read fresh from its scanned slot. */
    const ID id_cmp = korb_intern("<=>");
    {
        struct korb_range *r2 = (struct korb_range *)sp[-argc - 1];
        if (!NIL_P(r2->begin)) {
            VALUE begin_v = r2->begin;
            VALUE cmp = UNWRAP(korb_funcall(c, c->sp_top, begin_v, id_cmp, 1, &argv[0]));
            if (!FIXNUM_P(cmp) || FIX2LONG(cmp) > 0) return RESULT_OK(Qfalse);
        }
    }
    {
        struct korb_range *r2 = (struct korb_range *)sp[-argc - 1];
        if (!NIL_P(r2->end)) {
            VALUE end_v = r2->end;
            bool excl = r2->exclude_end;
            VALUE cmp = UNWRAP(korb_funcall(c, c->sp_top, argv[0], id_cmp, 1, &end_v));
            if (!FIXNUM_P(cmp)) return RESULT_OK(Qfalse);
            long cv = FIX2LONG(cmp);
            if (excl ? (cv >= 0) : (cv > 0)) return RESULT_OK(Qfalse);
        }
    }
    return RESULT_OK(Qtrue);
}

static RESULT rng_map(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!korb_block_given(c)) {
        return korb_funcall(c, c->sp_top, self, korb_intern("to_a"), 0, NULL);
    }
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(korb_ary_new(c, c->sp_top));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    /* Park the result array in a synthetic frame across the per-element
     * korb_yield (the block body may allocate and move the result under a
     * moving GC).  Source elements are fixnums derived from `i`, so they
     * need no re-rooting.  See KORB_RNG_YIELD_FRAME / array.c ary_map. */
    KORB_RNG_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        RESULT _y = korb_yield(c, c->sp_top, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        korb_ary_push(c, c->sp_top, fr.last_line, _y.value);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT rng_select(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(korb_ary_new(c, c->sp_top));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    /* Park the result array in a synthetic frame across the per-element
     * korb_yield (see rng_map / array.c ary_select).  Selected elements are
     * fixnums derived from `i`. */
    KORB_RNG_YIELD_FRAME(c, fr, korb_ary_new(c, c->sp_top));
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        RESULT _y = korb_yield(c, c->sp_top, 1, &v);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        if (RTEST(_y.value)) korb_ary_push(c, c->sp_top, fr.last_line, v);
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}

static RESULT rng_all_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qtrue);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = UNWRAP(korb_yield(c, c->sp_top, 1, &v));
        if (!RTEST(m)) return RESULT_OK(Qfalse);
    }
    return RESULT_OK(Qtrue);
}

static RESULT rng_any_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(Qfalse);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = UNWRAP(korb_yield(c, c->sp_top, 1, &v));
        if (RTEST(m)) return RESULT_OK(Qtrue);
    }
    return RESULT_OK(Qfalse);
}

static RESULT rng_count(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    struct korb_range *r = (struct korb_range *)self;
    /* Beginless or endless ranges have infinite element count when no
     * argument or block is given to filter (CRuby semantics).  Float
     * infinity is the conventional carrier for "infinite". */
    if (argc == 0 && !korb_block_given(c) &&
        (NIL_P(r->begin) || NIL_P(r->end))) {
        return RESULT_OK(korb_float_new(c, c->sp_top, 1.0/0.0));
    }
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return RESULT_OK(INT2FIX(0));
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    long n = e - b + 1;
    if (r->exclude_end) n--;
    if (n < 0) n = 0;
    return RESULT_OK(INT2FIX(n));
}

static RESULT rng_reduce(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
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
    /* `acc` can be a moving handle (e.g. a String built by :+ or by the
     * block).  Park it in a synthetic frame across the per-element
     * funcall / korb_yield so a moving GC can't collect it.  Elements are
     * fixnums derived from `i`. */
    long start;
    if (op != 0) {
        VALUE acc0;
        if (sym_idx == 0) {            /* (:+) */
            if (b > e) return RESULT_OK(Qnil);
            acc0 = INT2FIX(b);
            start = b + 1;
        } else {                        /* (init, :+) */
            acc0 = argv[0];
            start = b;
        }
        KORB_RNG_YIELD_FRAME(c, fr, acc0);
        for (long i = start; i <= e; i++) {
            VALUE other = INT2FIX(i);
            RESULT _r = korb_funcall(c, c->sp_top, fr.last_line, op, 1, &other);
            if (_r.state != KORB_NORMAL) { c->current_frame = fr.prev; return _r; }
            fr.last_line = _r.value;
        }
        VALUE result = fr.last_line;
        c->current_frame = fr.prev;
        return RESULT_OK(result);
    }
    KORB_RNG_YIELD_FRAME(c, fr, argc > 0 ? argv[0] : INT2FIX(b++));
    for (long i = b; i <= e; i++) {
        VALUE args[2] = { fr.last_line, INT2FIX(i) };
        RESULT _y = korb_yield(c, c->sp_top, 2, args);
        if (_y.state != KORB_NORMAL) { c->current_frame = fr.prev; return _y; }
        fr.last_line = _y.value;
    }
    VALUE result = fr.last_line;
    c->current_frame = fr.prev;
    return RESULT_OK(result);
}


/* ---------- Range#exclude_end? ---------- */
static RESULT rng_exclude_end_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (SPECIAL_CONST_P(self) || BUILTIN_TYPE(self) != T_RANGE) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(((struct korb_range *)self)->exclude_end));
}

