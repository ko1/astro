/* Float — moved from builtins.c. */

/* ---------- Float ---------- */
static VALUE flt_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(korb_num2dbl(self) + korb_num2dbl(argv[0]));
}
static VALUE flt_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(korb_num2dbl(self) - korb_num2dbl(argv[0]));
}
static VALUE flt_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(korb_num2dbl(self) * korb_num2dbl(argv[0]));
}
static VALUE flt_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(korb_num2dbl(self) / korb_num2dbl(argv[0]));
}
/* Format a double using the shortest %.<p>g that round-trips back
 * to the same bit pattern.  This matches CRuby's `3.14.to_s == "3.14"`
 * (not "3.1400000000000001") while still being unambiguous.  Prefers
 * fixed-point over scientific when both round-trip and the magnitude
 * is reasonable (CRuby uses a similar threshold). */
extern void korb_double_to_str(double d, char *out, size_t out_cap);
static void korb_float_to_shortest(double d, char *out, size_t out_cap) {
    korb_double_to_str(d, out, out_cap);
}

/* Float#step(limit, step) [{ |x| ... }] — yield self, self+step, ...
 * up to (and including) limit.  Mirrors Numeric#step. */
static VALUE flt_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    double start = korb_num2dbl(self);
    double limit = korb_num2dbl(argv[0]);
    double step  = (argc >= 2) ? korb_num2dbl(argv[1]) : 1.0;
    bool has_block = korb_block_given();
    VALUE out = has_block ? Qnil : korb_ary_new();
    if (step == 0.0) return self;
    if (step > 0.0) {
        for (double v = start; v <= limit + 1e-12; v += step) {
            VALUE fv = korb_float_new(v);
            if (has_block) {
                korb_yield(c, 1, &fv);
                if (c->state != KORB_NORMAL) return Qnil;
            } else {
                korb_ary_push(out, fv);
            }
        }
    } else {
        for (double v = start; v >= limit - 1e-12; v += step) {
            VALUE fv = korb_float_new(v);
            if (has_block) {
                korb_yield(c, 1, &fv);
                if (c->state != KORB_NORMAL) return Qnil;
            } else {
                korb_ary_push(out, fv);
            }
        }
    }
    return has_block ? self : out;
}

static VALUE flt_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    double d = korb_num2dbl(self);
    /* Ruby uses fixed names for special values, not C's "inf" / "nan". */
    if (isnan(d)) return korb_str_new_cstr("NaN");
    if (isinf(d)) return korb_str_new_cstr(d < 0 ? "-Infinity" : "Infinity");
    char b[64];
    korb_float_to_shortest(d, b, sizeof(b));
    /* Ruby's Float#to_s appends ".0" for whole-number Floats so the
     * type is unambiguous: `1.0.to_s == "1.0"` (not "1"). */
    bool has_dot_or_e = false;
    for (char *p = b; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') { has_dot_or_e = true; break; }
    }
    if (!has_dot_or_e) {
        size_t l = strlen(b);
        if (l + 2 < sizeof(b)) { b[l] = '.'; b[l+1] = '0'; b[l+2] = 0; }
    }
    return korb_str_new_cstr(b);
}


/* Numeric#coerce — Float variant.  Integer/Bignum/Float other all
 * promoted to a Float pair. */
static VALUE flt_coerce(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments");
        return Qnil;
    }
    VALUE other = argv[0];
    VALUE pair = korb_ary_new_capa(2);
    if (FIXNUM_P(other)) {
        korb_ary_push(pair, korb_float_new((double)FIX2LONG(other)));
        korb_ary_push(pair, self);
        return pair;
    }
    if (KORB_IS_FLOAT(other)) {
        korb_ary_push(pair, other);
        korb_ary_push(pair, self);
        return pair;
    }
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM) {
        korb_ary_push(pair, korb_float_new(korb_num2dbl(other)));
        korb_ary_push(pair, self);
        return pair;
    }
    VALUE eTyp = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eTyp, "%s can't be coerced into Float",
             korb_id_name(korb_class_of_class(other)->name));
    return Qnil;
}

static VALUE flt_abs2(CTX *c, VALUE self, int argc, VALUE *argv) {
    double v = korb_num2dbl(self);
    return korb_float_new(v * v);
}

/* ---------- Float methods (extended) ---------- */
static VALUE flt_floor(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* largest integer <= v.  C's floor() handles negatives correctly. */
    return INT2FIX((long)floor(korb_num2dbl(self)));
}
static VALUE flt_ceil(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)ceil(korb_num2dbl(self)));
}
/* Float#eql? — type-strict.  `1.0.eql?(1) == false` in CRuby; the
 * default Object#eql? falls through to ==, which coerces, so we need
 * a bespoke version here. */
static VALUE flt_eql(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    VALUE other = argv[0];
    if (FLONUM_P(self) && FLONUM_P(other)) return KORB_BOOL(korb_num2dbl(self) == korb_num2dbl(other));
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT &&
        (!SPECIAL_CONST_P(self) ? BUILTIN_TYPE(self) == T_FLOAT : FLONUM_P(self)))
        return KORB_BOOL(korb_num2dbl(self) == korb_num2dbl(other));
    return Qfalse;
}

static VALUE flt_round(CTX *c, VALUE self, int argc, VALUE *argv) {
    double v = korb_num2dbl(self);
    /* No-arg / arg==0 → round to integer, return Integer. */
    if (argc < 1 || (FIXNUM_P(argv[0]) && FIX2LONG(argv[0]) == 0)) {
        return INT2FIX((long)round(v));
    }
    if (!FIXNUM_P(argv[0])) return INT2FIX((long)round(v));
    long n = FIX2LONG(argv[0]);
    if (n > 0) {
        /* Round to n decimals, return Float. */
        double scale = pow(10.0, (double)n);
        return korb_float_new(round(v * scale) / scale);
    }
    /* Negative precision → round to nearest 10^|n|, return Float. */
    double scale = pow(10.0, (double)(-n));
    return korb_float_new(round(v / scale) * scale);
}
static VALUE flt_truncate(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* truncate toward zero — same as to_i for Float. */
    return INT2FIX((long)korb_num2dbl(self));
}

static VALUE flt_pow(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return self;
    double a = korb_num2dbl(self);
    double b = korb_num2dbl(argv[0]);
    return korb_float_new(pow(a, b));
}

static VALUE flt_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_num2dbl(self) < korb_num2dbl(argv[0]));
}
static VALUE flt_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_num2dbl(self) <= korb_num2dbl(argv[0]));
}
static VALUE flt_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_num2dbl(self) > korb_num2dbl(argv[0]));
}
static VALUE flt_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_num2dbl(self) >= korb_num2dbl(argv[0]));
}
static VALUE flt_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    double a = korb_num2dbl(self);
    double b = korb_num2dbl(argv[0]);
    /* NaN compared to anything (including itself) is undefined — Ruby
     * returns nil to signal incomparable. */
    if (isnan(a) || isnan(b)) return Qnil;
    return INT2FIX(a < b ? -1 : a > b ? 1 : 0);
}
static VALUE flt_to_i(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)korb_num2dbl(self));
}
static VALUE flt_to_f(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
}
static VALUE flt_uminus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(-korb_num2dbl(self));
}
static VALUE flt_abs(CTX *c, VALUE self, int argc, VALUE *argv) {
    double v = korb_num2dbl(self);
    return korb_float_new(v < 0 ? -v : v);
}

static VALUE flt_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_eq(self, argv[0]));
}

