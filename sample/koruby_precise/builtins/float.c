/* Float — moved from builtins.c. */

/* True iff `v` is a Float (FLONUM or heap T_FLOAT) or Integer. */
static bool flt_op_native(VALUE v) {
    if (FIXNUM_P(v) || FLONUM_P(v)) return true;
    if (SPECIAL_CONST_P(v)) return false;
    int t = BUILTIN_TYPE(v);
    return t == T_FLOAT || t == T_BIGNUM;
}

/* Run the coerce protocol on `other` for Float `self`.  On NORMAL,
 * value is the coerced op result (or Qundef when no #coerce or invalid
 * pair). */
static RESULT flt_coerce_dispatch(CTX *c, VALUE self, VALUE other, ID op) {
    struct korb_class *ok = korb_class_of_class(other);
    if (!korb_class_find_method(ok, korb_intern("coerce"))) return RESULT_OK(Qundef);
    VALUE pair = UNWRAP(korb_funcall(c, other, korb_intern("coerce"), 1, &self));
    if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY) return RESULT_OK(Qundef);
    struct korb_array *p = (struct korb_array *)pair;
    if (p->len != 2) return RESULT_OK(Qundef);
    return korb_funcall(c, korb_ary_items(p)[0], op, 1, &korb_ary_items(p)[1]);
}

#define FLT_BINOP_COERCE_OR_RAISE(c, v, op_name)                          \
    do {                                                                   \
        if (!flt_op_native((v))) {                                          \
            VALUE _co = UNWRAP(flt_coerce_dispatch((c), self, (v),           \
                                            korb_intern((op_name))));        \
            if (!UNDEF_P(_co)) return RESULT_OK(_co);                         \
            VALUE _eTy = korb_const_get(KORB_VM(c)->object_class,              \
                                        korb_intern("TypeError"));            \
            return korb_raise((c), (struct korb_class *)_eTy,                 \
                       "%s can't be coerced into Float",                       \
                       korb_id_name(korb_class_of_class((v))->name));          \
        }                                                                      \
    } while (0)

/* ---------- Float ---------- */
static RESULT flt_plus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    FLT_BINOP_COERCE_OR_RAISE(c, argv[0], "+");
    return RESULT_OK(korb_float_new(c, c->sp_top, korb_num2dbl(self) + korb_num2dbl(argv[0])));
}
static RESULT flt_minus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    FLT_BINOP_COERCE_OR_RAISE(c, argv[0], "-");
    return RESULT_OK(korb_float_new(c, c->sp_top, korb_num2dbl(self) - korb_num2dbl(argv[0])));
}
static RESULT flt_mul(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    FLT_BINOP_COERCE_OR_RAISE(c, argv[0], "*");
    return RESULT_OK(korb_float_new(c, c->sp_top, korb_num2dbl(self) * korb_num2dbl(argv[0])));
}
static RESULT flt_div(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* fdiv / quo / `/` take exactly one argument (CRuby raises ArgumentError
     * for `1.0.fdiv(1, 2)`). */
    if (argc != 1) {
        return korb_raise_argument_error(c,
                   "wrong number of arguments (given %d, expected 1)", argc);
    }
    FLT_BINOP_COERCE_OR_RAISE(c, argv[0], "/");
    return RESULT_OK(korb_float_new(c, c->sp_top, korb_num2dbl(self) / korb_num2dbl(argv[0])));
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
static RESULT flt_step(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    double start = korb_num2dbl(self);
    double limit = korb_num2dbl(argv[0]);
    double step  = (argc >= 2) ? korb_num2dbl(argv[1]) : 1.0;
    bool has_block = korb_block_given(c);
    sp[0] = has_block ? Qnil : korb_ary_new(c, sp + 1);
    if (step == 0.0) return RESULT_OK(self);
    if (step > 0.0) {
        for (double v = start; v <= limit + 1e-12; v += step) {
            VALUE fv = korb_float_new(c, sp + 1, v);
            if (has_block) {
                CHECK(korb_yield(c, 1, &fv));
            } else {
                korb_ary_push(c, sp + 1, sp[0], fv);
            }
        }
    } else {
        for (double v = start; v >= limit - 1e-12; v += step) {
            VALUE fv = korb_float_new(c, sp + 1, v);
            if (has_block) {
                CHECK(korb_yield(c, 1, &fv));
            } else {
                korb_ary_push(c, sp + 1, sp[0], fv);
            }
        }
    }
    return RESULT_OK(has_block ? self : sp[0]);
}

static RESULT flt_nan_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(isnan(korb_num2dbl(self))));
}

/* Float#infinite? — returns 1, -1, or nil (CRuby convention). */
static RESULT flt_infinite_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double d = korb_num2dbl(self);
    if (!isinf(d)) return RESULT_OK(Qnil);
    return RESULT_OK(INT2FIX(d > 0 ? 1 : -1));
}

static RESULT flt_finite_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double d = korb_num2dbl(self);
    return RESULT_OK(KORB_BOOL(!isnan(d) && !isinf(d)));
}

static RESULT flt_zero_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) == 0.0));
}

static RESULT flt_positive_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) > 0.0));
}

static RESULT flt_negative_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) < 0.0));
}

static RESULT flt_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double d = korb_num2dbl(self);
    /* Ruby uses fixed names for special values, not C's "inf" / "nan". */
    if (isnan(d)) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, "NaN"));
    if (isinf(d)) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, d < 0 ? "-Infinity" : "Infinity"));
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
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, b));
}


/* Numeric#coerce — Float variant.  Integer/Bignum/Float other all
 * promoted to a Float pair. */
/* Numeric#coerce — Float variant.  Phase 8 RESULT 化: 直接 RESULT を返し、
 * raise も `return korb_raise(...)` で in-band 伝搬。 caller は cfunc_r
 * 経由で UNWRAP する。 */
static RESULT flt_coerce(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    if (argc < 1) {
        return korb_raise_argument_error(c, "wrong number of arguments");
    }
    sp[0] = korb_ary_new_capa(c, sp + 1, 2);
    /* korb_ary_new_capa / korb_float_new fire GC and the receiver Float (and
     * a boxed `other`) are moving handles — re-read other from its arg slot
     * and push the re-read self (sp[-argc-1]). */
    VALUE other = sp[-1];
    if (FIXNUM_P(other)) {
        korb_ary_push(c, sp + 1, sp[0], korb_float_new(c, sp + 1, (double)FIX2LONG(other)));
        korb_ary_push(c, sp + 1, sp[0], sp[-argc - 1]);
        return RESULT_OK(sp[0]);
    }
    if (KORB_IS_FLOAT(other)) {
        korb_ary_push(c, sp + 1, sp[0], sp[-1]);
        korb_ary_push(c, sp + 1, sp[0], sp[-argc - 1]);
        return RESULT_OK(sp[0]);
    }
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM) {
        korb_ary_push(c, sp + 1, sp[0], korb_float_new(c, sp + 1, korb_num2dbl(sp[-1])));
        korb_ary_push(c, sp + 1, sp[0], sp[-argc - 1]);
        return RESULT_OK(sp[0]);
    }
    return korb_raise_type_error(c, "%s can't be coerced into Float",
                                 korb_id_name(korb_class_of_class(other)->name));
}

static RESULT flt_abs2(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    return RESULT_OK(korb_float_new(c, c->sp_top, v * v));
}

/* ---------- Float methods (extended) ---------- */
static RESULT flt_floor(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    long n = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 0;
    if (n == 0) return RESULT_OK(korb_dbl2int(c, c->sp_top, floor(v)));
    /* Float#floor(n) returns Float for n > 0, Integer for n < 0. */
    if (n > 0) {
        double m = pow(10.0, (double)n);
        return RESULT_OK(korb_float_new(c, c->sp_top, floor(v * m) / m));
    }
    /* n < 0: round to nearest 10^|n|, return Integer.  Compute via
     * scale-divide-floor-multiply (instead of v * pow(10, n) which
     * underflows to 0 for very negative n and triggered a SIGFPE). */
    double scale = pow(10.0, (double)(-n));
    return RESULT_OK(korb_dbl2int(c, c->sp_top, floor(v / scale) * scale));
}
static RESULT flt_ceil(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    long n = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 0;
    if (n == 0) return RESULT_OK(korb_dbl2int(c, c->sp_top, ceil(v)));
    if (n > 0) {
        double m = pow(10.0, (double)n);
        return RESULT_OK(korb_float_new(c, c->sp_top, ceil(v * m) / m));
    }
    double scale = pow(10.0, (double)(-n));
    return RESULT_OK(korb_dbl2int(c, c->sp_top, ceil(v / scale) * scale));
}
/* Float#eql? — type-strict.  `1.0.eql?(1) == false` in CRuby; the
 * default Object#eql? falls through to ==, which coerces, so we need
 * a bespoke version here. */
static RESULT flt_eql(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    VALUE other = argv[0];
    if (FLONUM_P(self) && FLONUM_P(other)) return RESULT_OK(KORB_BOOL(korb_num2dbl(self) == korb_num2dbl(other)));
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT &&
        (!SPECIAL_CONST_P(self) ? BUILTIN_TYPE(self) == T_FLOAT : FLONUM_P(self)))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) == korb_num2dbl(other)));
    return RESULT_OK(Qfalse);
}

static RESULT flt_round(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    long n = 0;
    /* Drop trailing FL_KWARGS hash — caller may pass `half: :up` etc. */
    int posargc = argc;
    if (posargc > 0 && !SPECIAL_CONST_P(argv[posargc - 1]) &&
        BUILTIN_TYPE(argv[posargc - 1]) == T_HASH &&
        (RBASIC(argv[posargc - 1])->head.flags & FL_KWARGS)) {
        posargc--;
    }
    if (posargc >= 1) {
        VALUE nv = argv[0];
        if (!FIXNUM_P(nv)) {
            if (!SPECIAL_CONST_P(nv)) {
                VALUE rt = UNWRAP(korb_funcall(c, nv, korb_intern("respond_to?"), 1,
                                        (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
                if (RTEST(rt)) {
                    nv = UNWRAP(korb_funcall(c, nv, korb_intern("to_int"), 0, NULL));
                }
            }
            if (!FIXNUM_P(nv)) {
                VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
                return korb_raise(c, (struct korb_class *)eT,
                           "no implicit conversion into Integer");
            }
        }
        n = FIX2LONG(nv);
    }
    /* NaN / Infinity:
     *  - no precision (or n <= 0) → FloatDomainError / RangeError (NaN
     *    gets RangeError for the negative-precision case in CRuby).
     *  - positive precision → return self unchanged (CRuby rounds NaN to
     *    NaN, Infinity to Infinity). */
    if (isnan(v) || isinf(v)) {
        if (n > 0) return RESULT_OK(korb_float_new(c, c->sp_top, v));
        if (isnan(v)) {
            VALUE eR = (n < 0)
                ? korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"))
                : korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError"));
            return korb_raise(c, (struct korb_class *)eR, "NaN");
        }
        VALUE eD = korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError"));
        return korb_raise(c, (struct korb_class *)eD, "Infinity");
    }
    if (posargc < 1 || n == 0) {
        /* No arg / arg==0 → round to integer, return Integer. */
        return RESULT_OK(korb_dbl2int(c, c->sp_top, round(v)));
    }
    if (n > 0) {
        /* Round to n decimals, return Float.  Big n may saturate. */
        if (n > 308) return RESULT_OK(korb_float_new(c, c->sp_top, v));
        double scale = pow(10.0, (double)n);
        return RESULT_OK(korb_float_new(c, c->sp_top, round(v * scale) / scale));
    }
    /* Negative precision → round to nearest 10^|n|, return Integer. */
    if (-n > 308) return RESULT_OK(INT2FIX(0));
    double scale = pow(10.0, (double)(-n));
    return RESULT_OK(korb_dbl2int(c, c->sp_top, round(v / scale) * scale));
}
static RESULT flt_truncate(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    if (isnan(v)) {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError")),
                          "NaN");
    }
    if (isinf(v)) {
        return korb_raise(c, (struct korb_class *)korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError")),
                          v > 0 ? "Infinity" : "-Infinity");
    }
    /* Optional precision arg: positive → return Float scaled to `prec`
     * decimal places (CRuby semantics).  Negative → round to nearest 10^n. */
    long prec = 0;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) prec = FIX2LONG(argv[0]);
    }
    if (prec > 0) {
        double scale = pow(10.0, (double)prec);
        double scaled = v * scale;
        double truncated = scaled >= 0 ? floor(scaled) : ceil(scaled);
        return RESULT_OK(korb_float_new(c, c->sp_top, truncated / scale));
    }
    if (prec < 0) {
        double scale = pow(10.0, (double)(-prec));
        double scaled = v / scale;
        double truncated = scaled >= 0 ? floor(scaled) : ceil(scaled);
        return RESULT_OK(korb_dbl2int(c, c->sp_top, truncated * scale));
    }
    /* truncate toward zero — same as to_i for Float. */
    return RESULT_OK(korb_dbl2int(c, c->sp_top, v >= 0 ? floor(v) : ceil(v)));
}

static RESULT flt_pow(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(self);
    double a = korb_num2dbl(self);
    double b = korb_num2dbl(argv[0]);
    return RESULT_OK(korb_float_new(c, c->sp_top, pow(a, b)));
}

/* Reject non-Numeric arg with ArgumentError (CRuby semantics).
 * Returns RESULT_OK(Qnil) when numeric; raise RESULT otherwise. */
static RESULT flt_check_numeric(CTX *c, VALUE other) {
    if (FIXNUM_P(other) || KORB_IS_FLOAT(other)) return RESULT_OK(Qnil);
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM) return RESULT_OK(Qnil);
    if (!SPECIAL_CONST_P(other)) {
        struct korb_class *k = korb_class_of_class(other);
        for (; k; k = k->super) {
            if (k == KORB_VM(c)->numeric_class) return RESULT_OK(Qnil);
        }
    }
    VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    return korb_raise(c, (struct korb_class *)eA,
               "comparison of Float with %s failed",
               SPECIAL_CONST_P(other) ? "non-Numeric" :
                   korb_id_name(korb_class_of_class(other)->name));
}
static RESULT flt_lt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK(flt_check_numeric(c, argv[0]));
    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) < korb_num2dbl(argv[0])));
}
static RESULT flt_le(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK(flt_check_numeric(c, argv[0]));
    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) <= korb_num2dbl(argv[0])));
}
static RESULT flt_gt(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK(flt_check_numeric(c, argv[0]));
    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) > korb_num2dbl(argv[0])));
}
static RESULT flt_ge(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    CHECK(flt_check_numeric(c, argv[0]));
    return RESULT_OK(KORB_BOOL(korb_num2dbl(self) >= korb_num2dbl(argv[0])));
}
static RESULT flt_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double a = korb_num2dbl(self);
    VALUE other = argv[0];
    /* Determine if `other` is one of the directly-comparable numeric
     * kinds; if not, fall back to the coerce protocol. */
    bool numeric_kind = FIXNUM_P(other) || FLONUM_P(other) ||
        (!SPECIAL_CONST_P(other) &&
         (BUILTIN_TYPE(other) == T_FLOAT || BUILTIN_TYPE(other) == T_BIGNUM));
    if (!numeric_kind) {
        if (SPECIAL_CONST_P(other)) return RESULT_OK(Qnil);
        /* Special: when self is ±Infinity and `other` responds to
         * #infinite?, use the sign comparison instead of the coerce path. */
        if (isinf(a)) {
            VALUE rtinf = UNWRAP(korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                       (VALUE[]){ korb_id2sym(korb_intern("infinite?")) }));
            if (RTEST(rtinf)) {
                VALUE ov = UNWRAP(korb_funcall(c, other, korb_intern("infinite?"), 0, NULL));
                long osign;
                if (NIL_P(ov)) osign = 0;
                else if (FIXNUM_P(ov)) osign = FIX2LONG(ov);
                else osign = 0;
                long asign = a > 0 ? 1 : -1;
                if (asign == osign) return RESULT_OK(INT2FIX(0));
                return RESULT_OK(INT2FIX(asign > osign ? 1 : -1));
            }
        }
        VALUE rt = UNWRAP(korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("coerce")) }));
        if (!RTEST(rt)) return RESULT_OK(Qnil);
        VALUE pair = UNWRAP(korb_funcall(c, other, korb_intern("coerce"), 1, &self));
        if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY ||
            ((struct korb_array *)pair)->len != 2) {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT, "coerce must return [x, y]");
        }
        struct korb_array *p = (struct korb_array *)pair;
        return korb_funcall(c, korb_ary_items(p)[0], korb_intern("<=>"), 1, &korb_ary_items(p)[1]);
    }
    /* Self == ±Infinity, other == finite Integer/Bignum: ±Infinity wins. */
    if (isinf(a) && (FIXNUM_P(other) ||
                     (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM))) {
        return RESULT_OK(INT2FIX(a > 0 ? 1 : -1));
    }
    double b = korb_num2dbl(other);
    /* NaN compared to anything (including itself) is undefined — Ruby
     * returns nil to signal incomparable. */
    if (isnan(a) || isnan(b)) return RESULT_OK(Qnil);
    return RESULT_OK(INT2FIX(a < b ? -1 : a > b ? 1 : 0));
}
RESULT flt_to_i(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    /* CRuby: Float#to_i / #to_int raise FloatDomainError for NaN / Infinity. */
    if (isnan(v) || isinf(v)) {
        VALUE eD = korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError"));
        return korb_raise(c, (struct korb_class *)eD, isnan(v) ? "NaN" : (v < 0 ? "-Infinity" : "Infinity"));
    }
    return RESULT_OK(korb_dbl2int(c, c->sp_top, v >= 0 ? floor(v) : ceil(v)));
}
static RESULT flt_to_f(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(self);
}
static RESULT flt_uminus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_float_new(c, c->sp_top, -korb_num2dbl(self)));
}
static RESULT flt_uplus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(self);
}
static RESULT flt_abs(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double v = korb_num2dbl(self);
    /* fabs handles -0.0 → +0.0 correctly (sign bit cleared). */
    return RESULT_OK(korb_float_new(c, c->sp_top, fabs(v)));
}

static RESULT flt_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(KORB_BOOL(korb_eq(c, self, argv[0])));
}

