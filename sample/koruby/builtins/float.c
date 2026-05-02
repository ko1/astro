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
static VALUE flt_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    char b[64]; snprintf(b, 64, "%.17g", korb_num2dbl(self));
    return korb_str_new_cstr(b);
}


/* ---------- Float methods (extended) ---------- */
static VALUE flt_floor(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* largest integer <= v.  C's floor() handles negatives correctly. */
    return INT2FIX((long)floor(korb_num2dbl(self)));
}
static VALUE flt_ceil(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)ceil(korb_num2dbl(self)));
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

