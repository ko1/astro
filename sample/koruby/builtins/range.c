/* Range — moved from builtins.c. */

/* ---------- Range ---------- */
static VALUE rng_each(CTX *c, VALUE self, int argc, VALUE *argv) {
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
    long step = argc >= 1 && FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 1;
    if (step == 0) return self;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    /* Block-less form returns an Array of stepped values — koruby's
     * Enumerator stand-in (matches Integer#step's behavior). */
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
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qfalse;
    long v = FIX2LONG(argv[0]);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) return KORB_BOOL(v >= b && v < e);
    return KORB_BOOL(v >= b && v <= e);
}

static VALUE rng_map(CTX *c, VALUE self, int argc, VALUE *argv) {
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
    VALUE acc = argc > 0 ? argv[0] : INT2FIX(b++);
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

