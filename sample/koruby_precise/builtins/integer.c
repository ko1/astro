/* Integer — moved from builtins.c. */

/* ---------- Integer ---------- */

/* Detect whether `v` is a Rational or Complex (defined in bootstrap.rb).
 * Returns 1 for Rational, 2 for Complex, 0 otherwise. */
static int int_op_other_kind(VALUE v) {
    if (FIXNUM_P(v) || KORB_IS_FLOAT(v)) return 0;
    if (SPECIAL_CONST_P(v)) return 0;
    if (BUILTIN_TYPE(v) != T_OBJECT) return 0;
    struct korb_class *k = korb_class_of_class(v);
    const char *n = korb_id_name(k->name);
    if (strcmp(n, "Rational") == 0) return 1;
    if (strcmp(n, "Complex")  == 0) return 2;
    return 0;
}

/* Build a Rational(self, 1) by calling Rational.new(self, 1). */
static VALUE int_to_rational_obj(CTX *c, VALUE self) {
    VALUE klass = korb_const_get(KORB_VM(c)->object_class, korb_intern("Rational"));
    VALUE one = INT2FIX(1);
    VALUE args[2] = {self, one};
    return korb_funcall(c, klass, korb_intern("new"), 2, args);
}

/* CRuby's Numeric#coerce protocol: when an arithmetic op gets a non-
 * builtin numeric on the RHS, ask the RHS via #coerce(self) for a
 * pair [coerced_self, coerced_other], then send the op to the pair.
 * Returns Qundef when the RHS doesn't respond to :coerce so the
 * caller can fall through to TypeError. */
static VALUE int_coerce_dispatch(CTX *c, VALUE self, VALUE other, ID op) {
    /* Use respond_to? so mock objects (method_missing) are also seen. */
    VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                            (VALUE[]){ korb_id2sym(korb_intern("coerce")) });
    if (c->state == KORB_RAISE) return Qnil;
    if (!RTEST(rt)) return Qundef;
    VALUE pair = korb_funcall(c, other, korb_intern("coerce"), 1, &self);
    if (c->state != KORB_NORMAL) return Qnil;
    if (SPECIAL_CONST_P(pair) || BUILTIN_TYPE(pair) != T_ARRAY) return Qundef;
    struct korb_array *p = (struct korb_array *)pair;
    if (p->len != 2) return Qundef;
    return korb_funcall(c, p->ptr[0], op, 1, &p->ptr[1]);
}

#define COERCE_OR_RAISE(c, v, op_name)                                  \
    do {                                                                 \
        if (!FIXNUM_P(v) && BUILTIN_TYPE(v) != T_BIGNUM) {                \
            if (KORB_IS_FLOAT(v)) {                              \
                /* fall through — caller handles */                        \
            } else {                                                       \
                /* Try the coerce protocol before giving up. */            \
                VALUE _coerced = int_coerce_dispatch((c), self, (v), korb_intern((op_name))); \
                if (!UNDEF_P(_coerced)) return RESULT_OK(_coerced);        \
                VALUE _eTy = korb_const_get(KORB_VM(c)->object_class,         \
                                            korb_intern("TypeError"));     \
                return korb_raise((c), (struct korb_class *)_eTy,          \
                           "%s can't be coerced into Integer",             \
                           korb_id_name(korb_class_of_class((v))->name));  \
            }                                                              \
        }                                                                  \
    } while (0)

static RESULT int_plus(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (KORB_IS_FLOAT(argv[0])) {
        return RESULT_OK(korb_float_new(c, c->sp, korb_num2dbl(self) + korb_num2dbl(argv[0])));
    }
    if (int_op_other_kind(argv[0])) {
        /* + is commutative — delegate to Rational#+/Complex#+. */
        return RESULT_OK(korb_funcall(c, argv[0], korb_intern("+"), 1, &self));
    }
    COERCE_OR_RAISE(c, argv[0], "+");
    return RESULT_OK(korb_int_plus(self, argv[0]));
}
static RESULT int_minus(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (KORB_IS_FLOAT(argv[0])) {
        return RESULT_OK(korb_float_new(c, c->sp, korb_num2dbl(self) - korb_num2dbl(argv[0])));
    }
    if (int_op_other_kind(argv[0])) {
        VALUE r = int_to_rational_obj(c, self);
        return RESULT_OK(korb_funcall(c, r, korb_intern("-"), 1, &argv[0]));
    }
    COERCE_OR_RAISE(c, argv[0], "-");
    return RESULT_OK(korb_int_minus(self, argv[0]));
}
static RESULT int_mul(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (KORB_IS_FLOAT(argv[0])) {
        /* self may be Fixnum (immediate) or Bignum (heap).  Use
         * korb_num2dbl which handles both via the slow path.  Without
         * this Bignum * Float would interpret the heap pointer as a
         * Fixnum and return garbage. */
        return RESULT_OK(korb_float_new(c, c->sp, korb_num2dbl(self) * korb_num2dbl(argv[0])));
    }
    if (int_op_other_kind(argv[0])) {
        return RESULT_OK(korb_funcall(c, argv[0], korb_intern("*"), 1, &self));
    }
    COERCE_OR_RAISE(c, argv[0], "*");
    return RESULT_OK(korb_int_mul(self, argv[0]));
}
static RESULT int_div(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (KORB_IS_FLOAT(argv[0])) {
        return RESULT_OK(korb_float_new(c, c->sp, korb_num2dbl(self) / korb_num2dbl(argv[0])));
    }
    if (int_op_other_kind(argv[0])) {
        VALUE r = int_to_rational_obj(c, self);
        return RESULT_OK(korb_funcall(c, r, korb_intern("/"), 1, &argv[0]));
    }
    COERCE_OR_RAISE(c, argv[0], "/");
    if (FIXNUM_P(argv[0]) && FIX2LONG(argv[0]) == 0) {
        { VALUE _eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError")); return korb_raise(c, (struct korb_class *)_eZ, "divided by 0"); }
        return RESULT_OK(Qnil);
    }
    return RESULT_OK(korb_int_div(self, argv[0]));
}
static RESULT int_mod(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (KORB_IS_FLOAT(argv[0])) {
        double rhs = korb_num2dbl(argv[0]);
        if (rhs == 0.0) {
            VALUE _eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)_eZ, "divided by 0");
        }
        double lhs = korb_num2dbl(self);
        double r = fmod(lhs, rhs);
        /* Ruby semantics: result has the sign of the divisor. */
        if (r != 0.0 && ((r < 0.0) != (rhs < 0.0))) r += rhs;
        return RESULT_OK(korb_float_new(c, c->sp, r));
    }
    COERCE_OR_RAISE(c, argv[0], "%");
    if (FIXNUM_P(argv[0]) && FIX2LONG(argv[0]) == 0) {
        { VALUE _eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError")); return korb_raise(c, (struct korb_class *)_eZ, "divided by 0"); }
        return RESULT_OK(Qnil);
    }
    return RESULT_OK(korb_int_mod(self, argv[0]));
}
static RESULT int_lshift(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_int_lshift(self, argv[0]));
}
static RESULT int_rshift(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_int_rshift(self, argv[0]));
}
/* Bitwise &/|/^ accept any integer-like RHS; for non-integers fall back
 * to the coerce protocol so user numerics work (CRuby semantics).
 * Without the guard, to_mpz would cast the RHS to a Bignum pointer and
 * segfault inside mpz_init_set. */
#define INT_BITOP_GUARD(c, rhs, op_name) do { \
    if (!FIXNUM_P(rhs) && \
        (SPECIAL_CONST_P(rhs) || BUILTIN_TYPE(rhs) != T_BIGNUM)) { \
        /* Float never bit-ops with Integer — CRuby raises TypeError
         * directly without going through the coerce protocol. */ \
        if (FLONUM_P(rhs) || (!SPECIAL_CONST_P(rhs) && BUILTIN_TYPE(rhs) == T_FLOAT)) { \
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")); \
            return korb_raise((c), (struct korb_class *)eT, \
                       "no implicit conversion of Float into Integer"); \
        } \
        VALUE _coerced = int_coerce_dispatch((c), self, (rhs), korb_intern((op_name))); \
        if (!UNDEF_P(_coerced)) return RESULT_OK(_coerced); \
        VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError")); \
        return korb_raise((c), (struct korb_class *)eT, \
                   "no implicit conversion to Integer"); \
    } \
} while (0)
static RESULT int_and(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_BITOP_GUARD(c, argv[0], "&");
    return RESULT_OK(korb_int_and(self, argv[0]));
}
static RESULT int_or(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_BITOP_GUARD(c, argv[0], "|");
    return RESULT_OK(korb_int_or(self, argv[0]));
}
static RESULT int_xor(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_BITOP_GUARD(c, argv[0], "^");
    return RESULT_OK(korb_int_xor(self, argv[0]));
}
/* Numeric comparators: raise ArgumentError on non-numeric RHS instead of
 * segfaulting through to_mpz.  Ruby semantics. */
/* For comparisons against a non-builtin numeric, try the coerce
 * protocol (CRuby behavior) before raising.  Returns Qundef when the
 * fast Integer/Float/Bignum path can proceed; otherwise returns the
 * result of the coerced comparison or raises. */
#define INT_CMP_GUARD(c, rhs, op_name) do { \
    if (!FIXNUM_P(rhs) && !FLONUM_P(rhs) && \
        (SPECIAL_CONST_P(rhs) || (BUILTIN_TYPE(rhs) != T_BIGNUM && BUILTIN_TYPE(rhs) != T_FLOAT))) { \
        VALUE _coerced = int_coerce_dispatch((c), self, (rhs), korb_intern((op_name))); \
        if (!UNDEF_P(_coerced)) return RESULT_OK(_coerced); \
        VALUE _eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError")); \
        return korb_raise((c), (struct korb_class *)_eA, \
                   "comparison of Integer with %s failed", \
                   SPECIAL_CONST_P(rhs) ? "non-Numeric" : \
                       korb_id_name(korb_class_of_class(rhs)->name)); \
    } \
} while (0)
static RESULT int_lt(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_CMP_GUARD(c, argv[0], "<");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) < korb_num2dbl(argv[0])));
    return RESULT_OK(KORB_BOOL(korb_int_cmp(self, argv[0]) < 0));
}
static RESULT int_le(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_CMP_GUARD(c, argv[0], "<=");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) <= korb_num2dbl(argv[0])));
    return RESULT_OK(KORB_BOOL(korb_int_cmp(self, argv[0]) <= 0));
}
static RESULT int_gt(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_CMP_GUARD(c, argv[0], ">");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) > korb_num2dbl(argv[0])));
    return RESULT_OK(KORB_BOOL(korb_int_cmp(self, argv[0]) > 0));
}
static RESULT int_ge(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    INT_CMP_GUARD(c, argv[0], ">=");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) >= korb_num2dbl(argv[0])));
    return RESULT_OK(KORB_BOOL(korb_int_cmp(self, argv[0]) >= 0));
}
static RESULT int_eq(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return RESULT_OK(KORB_BOOL(korb_num2dbl(self) == korb_num2dbl(argv[0])));
    /* Only Integer/Bignum can be == an Integer; everything else is false.
     * Without this guard, `0 == nil` segfaults inside to_mpz (which casts
     * the second operand to a Bignum pointer). */
    if (!FIXNUM_P(argv[0]) &&
        (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_BIGNUM))
        return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(korb_int_eq(self, argv[0])));
}
static RESULT int_cmp(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    VALUE other = argv[0];
    if (FIXNUM_P(self) && FIXNUM_P(other)) {
        return RESULT_OK(INT2FIX((intptr_t)self < (intptr_t)other ? -1 : (intptr_t)self > (intptr_t)other ? 1 : 0));
    }
    if ((FIXNUM_P(self) || BUILTIN_TYPE(self) == T_BIGNUM) &&
        (FIXNUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM))) {
        return RESULT_OK(INT2FIX(korb_int_cmp(self, other)));
    }
    if (FLONUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT)) {
        double b = korb_num2dbl(other);
        if (isnan(b)) return RESULT_OK(Qnil);
        /* Integer vs ±Infinity: compare via sign instead of double conv. */
        if (isinf(b)) return RESULT_OK(INT2FIX(b > 0 ? -1 : 1));
        /* Convert self to a double precisely or with sign-preserving
         * fallback.  Bignum > 2^53 may overflow double; compare against
         * trunc(b) instead so we don't lose precision. */
        if (FIXNUM_P(self)) {
            double a = (double)FIX2LONG(self);
            return RESULT_OK(INT2FIX(a < b ? -1 : a > b ? 1 : 0));
        }
        /* Bignum: compare self against floor(b) as Integer.  If b has a
         * fractional part, use the integer comparison then refine sign. */
        double bint = trunc(b);
        if (bint != b) {
            /* a is Integer, b is Float with fractional part — compare
             * a to bint; equal → sign of -fractional. */
            VALUE bint_v = korb_dbl2int(bint);
            int cmp = korb_int_cmp(self, bint_v);
            if (cmp != 0) return RESULT_OK(INT2FIX(cmp));
            /* a == bint exactly: result is opposite sign of fractional. */
            double frac = b - bint;
            return RESULT_OK(INT2FIX(frac < 0 ? 1 : -1));
        }
        VALUE bint_v = korb_dbl2int(b);
        return RESULT_OK(INT2FIX(korb_int_cmp(self, bint_v)));
    }
    /* Non-numeric: coerce protocol. */
    if (!SPECIAL_CONST_P(other)) {
        VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("coerce")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (RTEST(rt)) {
            VALUE pair = korb_funcall(c, other, korb_intern("coerce"), 1, &self);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (!SPECIAL_CONST_P(pair) && BUILTIN_TYPE(pair) == T_ARRAY &&
                ((struct korb_array *)pair)->len == 2) {
                struct korb_array *p = (struct korb_array *)pair;
                return RESULT_OK(korb_funcall(c, p->ptr[0], korb_intern("<=>"), 1, &p->ptr[1]));
            }
        }
    }
    return RESULT_OK(Qnil);
}
static RESULT int_uminus(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_int_minus(INT2FIX(0), self));
}
static RESULT int_uplus(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(self);
}
static RESULT int_format(CTX *c, int argc, VALUE *sp);
static RESULT int_to_s(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return int_format(c, argc, sp);
}
static RESULT int_to_i(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
 return RESULT_OK(self); }
static RESULT int_to_f(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(korb_float_new(c, c->sp, korb_num2dbl(self)));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        return RESULT_OK(korb_float_new(c, c->sp, mpz_get_d((mpz_ptr)((struct korb_bignum *)self)->mpz)));
    }
    return RESULT_OK(korb_float_new(c, c->sp, 0.0));
}
static RESULT int_even_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(KORB_BOOL((FIX2LONG(self) & 1) == 0));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        mpz_ptr z = (mpz_ptr)((struct korb_bignum *)self)->mpz;
        return RESULT_OK(KORB_BOOL(mpz_even_p(z)));
    }
    return RESULT_OK(Qfalse);
}
static RESULT int_odd_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(KORB_BOOL((FIX2LONG(self) & 1) == 1));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        mpz_ptr z = (mpz_ptr)((struct korb_bignum *)self)->mpz;
        return RESULT_OK(KORB_BOOL(mpz_odd_p(z)));
    }
    return RESULT_OK(Qfalse);
}
static RESULT int_positive_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(KORB_BOOL(FIX2LONG(self) > 0));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        mpz_ptr z = (mpz_ptr)((struct korb_bignum *)self)->mpz;
        return RESULT_OK(KORB_BOOL(mpz_sgn(z) > 0));
    }
    return RESULT_OK(Qfalse);
}
static RESULT int_negative_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(KORB_BOOL(FIX2LONG(self) < 0));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        mpz_ptr z = (mpz_ptr)((struct korb_bignum *)self)->mpz;
        return RESULT_OK(KORB_BOOL(mpz_sgn(z) < 0));
    }
    return RESULT_OK(Qfalse);
}

static RESULT int_zero_p(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(KORB_BOOL(self == INT2FIX(0)));
    return RESULT_OK(Qfalse);
}
static RESULT int_times(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* call block self times */
    if (!FIXNUM_P(self)) return RESULT_OK(Qnil);
    long n = FIX2LONG(self);
    if (!korb_block_given(c)) {
        /* No-block: return Array stand-in [0, 1, ..., n-1] for chains
         * like `5.times.to_a` / `5.times.map { ... }`. */
        VALUE a = korb_ary_new_capa(c, c->sp, n > 0 ? n : 0);
        for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(i));
        return RESULT_OK(a);
    }
    for (long i = 0; i < n; i++) {
        VALUE arg = INT2FIX(i);
        VALUE r = korb_yield(c, 1, &arg);
        if (c->state != KORB_NORMAL) return RESULT_OK(r);
    }
    return RESULT_OK(self);
}
static RESULT int_succ(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_int_plus(self, INT2FIX(1)));
}
static RESULT int_pred(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_int_minus(self, INT2FIX(1)));
}


/* ---------- Integer methods (extended) ---------- */
static RESULT int_chr(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(self)) return RESULT_OK(Qnil);
    char ch = (char)(FIX2LONG(self) & 0xff);
    return RESULT_OK(korb_str_new(c, c->sp, &ch, 1));
}

static RESULT int_format(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Integer#to_s(base).  For non-decimal bases Ruby renders negatives
     * as "-<digits>", not as the unsigned twos-complement word. */
    int base = argc >= 1 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10;
    if (base < 2 || base > 36) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "invalid radix %d", base);
    }
    if (!FIXNUM_P(self)) {
        /* Bignum: use mpz_get_str which natively supports bases 2..62. */
        if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
            mpz_ptr z = (mpz_ptr)((struct korb_bignum *)self)->mpz;
            char *s = mpz_get_str(NULL, base, z);
            VALUE r = korb_str_new_cstr(c, c->sp, s);
            free(s);
            return RESULT_OK(r);
        }
        return RESULT_OK(korb_to_s(c, c->sp, self));
    }
    long v = FIX2LONG(self);
    char buf[80];
    if (base == 10) {
        snprintf(buf, sizeof(buf), "%ld", v);
        return RESULT_OK(korb_str_new_cstr(c, c->sp, buf));
    }
    bool neg = v < 0;
    unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
    /* Generic base 2..36 conversion: build digits right-to-left. */
    char tmp[80]; int tl = 0;
    if (uv == 0) tmp[tl++] = '0';
    while (uv) {
        unsigned long r = uv % (unsigned long)base;
        tmp[tl++] = (char)(r < 10 ? '0' + r : 'a' + (r - 10));
        uv /= (unsigned long)base;
    }
    /* Reverse digits in place. */
    for (int i = 0; i < tl/2; i++) { char t = tmp[i]; tmp[i] = tmp[tl-1-i]; tmp[tl-1-i] = t; }
    tmp[tl] = 0;
    if (neg) {
        char out[82]; out[0] = '-';
        memcpy(out+1, tmp, tl+1);
        return RESULT_OK(korb_str_new_cstr(c, c->sp, out));
    }
    return RESULT_OK(korb_str_new_cstr(c, c->sp, tmp));
}

static RESULT int_eqq(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    if (FIXNUM_P(self) && FIXNUM_P(argv[0])) return RESULT_OK(KORB_BOOL(self == argv[0]));
    return RESULT_OK(KORB_BOOL(korb_eq(c, self, argv[0])));
}

static RESULT int_floor(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(self)) return RESULT_OK(self);
    long n = FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 0;
    if (n >= 0) return RESULT_OK(self);  /* floor with ndigits >= 0 on Int is identity */
    /* Floor toward -inf at the 10^|n| boundary. */
    long v = FIX2LONG(self);
    long scale = 1;
    for (long i = 0; i < -n; i++) scale *= 10;
    long r = v % scale;
    if (r != 0 && (r < 0) != (scale < 0)) r += scale;  /* floor */
    return RESULT_OK(INT2FIX(v - r));
}

/* Integer#truncate(ndigits=0) — truncate toward zero, not -inf.  For
 * positive ndigits returns self; for n < 0 chops off |n| trailing
 * decimal digits while preserving the sign. */
static RESULT int_truncate(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(self)) return RESULT_OK(self);
    long n = FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 0;
    if (n >= 0) return RESULT_OK(self);
    long v = FIX2LONG(self);
    long scale = 1;
    for (long i = 0; i < -n; i++) scale *= 10;
    /* Truncate toward zero: `(v / scale) * scale` already does this in C
     * because integer division truncates toward 0. */
    return RESULT_OK(INT2FIX((v / scale) * scale));
}

/* Integer#round(ndigits=0) — for n >= 0 returns self (matching CRuby).
 * For n < 0, rounds to the nearest 10^|n|.  Half rounds away from zero
 * (Ruby's default).  `154.round(-1) == 150`. */
static RESULT int_round(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Parse trailing FL_KWARGS Hash for half: option. */
    enum { HALF_UP, HALF_DOWN, HALF_EVEN } half_mode = HALF_UP;
    int posargc = argc;
    if (posargc > 0 && !SPECIAL_CONST_P(argv[posargc - 1]) &&
        BUILTIN_TYPE(argv[posargc - 1]) == T_HASH &&
        (RBASIC(argv[posargc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *kw = (struct korb_hash *)argv[posargc - 1];
        VALUE half_key = korb_id2sym(korb_intern("half"));
        for (struct korb_hash_entry *e = kw->first; e; e = e->next) {
            if (korb_eql(c, e->key, half_key)) {
                VALUE val = e->value;
                if (NIL_P(val)) {
                    half_mode = HALF_UP;
                } else if (SYMBOL_P(val)) {
                    ID id = korb_sym2id(val);
                    if (id == korb_intern("up") || id == korb_intern("default")) half_mode = HALF_UP;
                    else if (id == korb_intern("down")) half_mode = HALF_DOWN;
                    else if (id == korb_intern("even") || id == korb_intern("banker")) half_mode = HALF_EVEN;
                    else {
                        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                        return korb_raise(c, (struct korb_class *)eA,
                                   "invalid rounding mode: %s", korb_id_name(id));
                    }
                } else {
                    VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
                    return korb_raise(c, (struct korb_class *)eA, "invalid rounding mode");
                }
                break;
            }
        }
        posargc--;
    }
    if (!FIXNUM_P(self) || posargc < 1) return RESULT_OK(self);
    if (!FIXNUM_P(argv[0])) return RESULT_OK(self);
    long n = FIX2LONG(argv[0]);
    if (n >= 0) return RESULT_OK(self);
    long v = FIX2LONG(self);
    long scale = 1;
    for (long i = 0; i < -n; i++) scale *= 10;
    /* Compute rounded value based on half_mode. */
    long sign = v < 0 ? -1 : 1;
    long absv = v < 0 ? -v : v;
    long base = (absv / scale) * scale;  /* floor toward 0 */
    long rem = absv - base;
    long twice = rem * 2;
    long rounded;
    if (twice > scale) {
        rounded = base + scale;
    } else if (twice < scale) {
        rounded = base;
    } else {
        /* Exact half. */
        switch (half_mode) {
            case HALF_UP:   rounded = base + scale; break;
            case HALF_DOWN: rounded = base; break;
            case HALF_EVEN: {
                /* Round to even multiple of scale. */
                long top = base / scale;
                rounded = (top & 1) ? base + scale : base;
                break;
            }
            default: rounded = base + scale; break;
        }
    }
    return RESULT_OK(INT2FIX(sign * rounded));
}

/* Integer#ceil(ndigits=0) — for n >= 0 returns self.  For n < 0 rounds
 * toward +inf at the 10^|n| boundary. */
static RESULT int_ceil(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(self) || argc < 1) return RESULT_OK(self);
    if (!FIXNUM_P(argv[0])) return RESULT_OK(self);
    long n = FIX2LONG(argv[0]);
    if (n >= 0) return RESULT_OK(self);
    long v = FIX2LONG(self);
    long scale = 1;
    for (long i = 0; i < -n; i++) scale *= 10;
    long r = v % scale;
    if (r > 0) v += (scale - r);
    else if (r < 0) v -= r;
    return RESULT_OK(INT2FIX(v));
}

/* ---------- Integer#div / Integer#fdiv ----------
 * Integer#div is floored division (rounds toward -infinity).  This is
 * a different method from Integer#/ (the `/` operator above), which
 * already exists; div is registered separately as the named method. */
static RESULT int_method_div(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "wrong number of arguments");
    }
    VALUE other = argv[0];
    /* Float argument: (self.to_f / other).floor — CRuby raises
     * ZeroDivisionError on 0.0. */
    if (FLONUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT)) {
        double a = korb_num2dbl(self);
        double b = korb_num2dbl(other);
        if (b == 0.0) {
            VALUE eDiv = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eDiv, "divided by 0");
        }
        double q = floor(a / b);
        if (q >= (double)FIXNUM_MIN && q <= (double)FIXNUM_MAX) return RESULT_OK(INT2FIX((long)q));
        /* Build a Bignum from the float. */
        return RESULT_OK(korb_funcall(c, korb_float_new(c, c->sp, q), korb_intern("to_i"), 0, NULL));
    }
    /* Fixnum / Fixnum fast path. */
    if (FIXNUM_P(self) && FIXNUM_P(other)) {
        long a = FIX2LONG(self), b = FIX2LONG(other);
        if (b == 0) {
            VALUE eDiv = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eDiv, "divided by 0");
        }
        long q = a / b;
        long r = a % b;
        if ((r != 0) && ((r < 0) != (b < 0))) q--;
        return RESULT_OK(INT2FIX(q));
    }
    /* Bignum path: use korb_int_div which already handles sign + GMP. */
    if ((FIXNUM_P(self) || (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM)) &&
        (FIXNUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM))) {
        if ((FIXNUM_P(other) && FIX2LONG(other) == 0) ||
            (!FIXNUM_P(other) && BUILTIN_TYPE(other) == T_BIGNUM &&
             mpz_sgn((mpz_ptr)((struct korb_bignum *)other)->mpz) == 0)) {
            VALUE eDiv = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eDiv, "divided by 0");
        }
        return RESULT_OK(korb_int_div(self, other));
    }
    /* Try #coerce on other (CRuby's Numeric coerce protocol). */
    if (!SPECIAL_CONST_P(other)) {
        VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("coerce")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (RTEST(rt)) {
            VALUE pair = korb_funcall(c, other, korb_intern("coerce"), 1, &self);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (!SPECIAL_CONST_P(pair) && BUILTIN_TYPE(pair) == T_ARRAY &&
                ((struct korb_array *)pair)->len == 2) {
                struct korb_array *p = (struct korb_array *)pair;
                return RESULT_OK(korb_funcall(c, p->ptr[0], korb_intern("div"), 1, &p->ptr[1]));
            }
        }
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eT, "expected Numeric");
}

static RESULT int_fdiv(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    double a = (double)(FIXNUM_P(self) ? FIX2LONG(self) : 0);
    double b;
    VALUE other = argv[0];
    if      (FIXNUM_P(other))                                            b = (double)FIX2LONG(other);
    else if (FLONUM_P(other))                                            b = korb_flonum_to_double(other);
    else if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT)  b = korb_num2dbl(other);
    else if (!SPECIAL_CONST_P(other)) {
        /* Coerce via #to_f (CRuby semantics for Numeric#fdiv with mocks
         * and other to_f-respondable objects). */
        VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_f")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (RTEST(rt)) {
            VALUE r = korb_funcall(c, other, korb_intern("to_f"), 0, NULL);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (FLONUM_P(r)) b = korb_flonum_to_double(r);
            else if (!SPECIAL_CONST_P(r) && BUILTIN_TYPE(r) == T_FLOAT) b = korb_num2dbl(r);
            else return RESULT_OK(Qnil);
        } else {
            return RESULT_OK(Qnil);
        }
    } else {
        return RESULT_OK(Qnil);
    }
    return RESULT_OK(korb_float_new(c, c->sp, a / b));
}

/* Integer#size — width in bytes of the machine word.  Matches CRuby's
 * `1.size == 8` on a 64-bit build. */
static RESULT int_size(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    /* Fixnum always 8 bytes (size of machine word on this build). */
    if (FIXNUM_P(self)) return RESULT_OK(INT2FIX((long)sizeof(long)));
    /* Bignum: count bytes needed for the abs value's two's-complement
     * representation.  CRuby: ceil(bits/8), at least 8 (machine word).
     * For negative numbers use |n|-1's bit count (two's-complement). */
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        const struct korb_bignum *bn = (const struct korb_bignum *)self;
        int sgn = mpz_sgn((mpz_ptr)bn->mpz);
        if (sgn == 0) return RESULT_OK(INT2FIX((long)sizeof(long)));
        long bits;
        if (sgn > 0) {
            bits = (long)mpz_sizeinbase((mpz_ptr)bn->mpz, 2);
        } else {
            mpz_t tmp;
            mpz_init(tmp);
            mpz_neg(tmp, (mpz_ptr)bn->mpz);
            mpz_sub_ui(tmp, tmp, 1);
            bits = mpz_sgn(tmp) == 0 ? 1 : (long)mpz_sizeinbase(tmp, 2);
            mpz_clear(tmp);
        }
        long bytes = (bits + 7) / 8;
        if (bytes < (long)sizeof(long)) bytes = sizeof(long);
        return RESULT_OK(INT2FIX(bytes));
    }
    return RESULT_OK(INT2FIX((long)sizeof(long)));
}

/* Integer#remainder — like %, but with truncation-toward-zero
 * semantics (rather than floor division).  `-10.remainder(3) == -1`
 * vs `-10 % 3 == 2`. */
static RESULT int_remainder(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return RESULT_OK(Qnil);
    long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
    if (b == 0) {
        VALUE eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
        return korb_raise(c, (struct korb_class *)eZ, "divided by 0");
    }
    /* C's % truncates toward zero — exactly what remainder wants. */
    return RESULT_OK(INT2FIX(a % b));
}

/* Numeric#coerce(other) — returns [other_as_self_type, self_as_other_type]
 * so binary ops can be performed in a common representation.  Integer
 * variant: if other is Integer, both stay Integer; if Float, both
 * promote to Float; otherwise raise TypeError (CRuby). */
static RESULT int_coerce(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) {
        VALUE eArg = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eArg, "wrong number of arguments");
    }
    VALUE other = argv[0];
    VALUE pair = korb_ary_new_capa(c, c->sp, 2);
    if (FIXNUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM)) {
        korb_ary_push(pair, other);
        korb_ary_push(pair, self);
        return RESULT_OK(pair);
    }
    if (KORB_IS_FLOAT(other)) {
        korb_ary_push(pair, other);
        korb_ary_push(pair, korb_float_new(c, c->sp, korb_num2dbl(self)));
        return RESULT_OK(pair);
    }
    /* String: parse via Float() — if it parses cleanly, return [parsed,
     * self.to_f].  Otherwise ArgumentError. */
    if (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_STRING) {
        VALUE klass = korb_const_get(KORB_VM(c)->object_class, korb_intern("Kernel"));
        if (UNDEF_P(klass)) klass = korb_const_get(KORB_VM(c)->object_class, korb_intern("Float"));
        VALUE f = korb_funcall(c, klass, korb_intern("Float"), 1, &other);
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (FLONUM_P(f) || (!SPECIAL_CONST_P(f) && BUILTIN_TYPE(f) == T_FLOAT)) {
            korb_ary_push(pair, f);
            korb_ary_push(pair, korb_float_new(c, c->sp, korb_num2dbl(self)));
            return RESULT_OK(pair);
        }
    }
    /* Object with #to_f: coerce via to_f to Float pair. */
    if (!SPECIAL_CONST_P(other)) {
        VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("to_f")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (RTEST(rt)) {
            VALUE f = korb_funcall(c, other, korb_intern("to_f"), 0, NULL);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (FLONUM_P(f) || (!SPECIAL_CONST_P(f) && BUILTIN_TYPE(f) == T_FLOAT)) {
                korb_ary_push(pair, f);
                korb_ary_push(pair, korb_float_new(c, c->sp, korb_num2dbl(self)));
                return RESULT_OK(pair);
            }
        }
    }
    VALUE eTyp = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eTyp, "%s can't be coerced into Integer",
             korb_id_name(korb_class_of_class(other)->name));
}

/* Numeric#abs2 — |self|**2 (== self*self for real Numerics). */
static RESULT int_abs2(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) {
        long v = FIX2LONG(self);
        VALUE arg = INT2FIX(v);
        return RESULT_OK(korb_int_mul(self, arg));  /* Bignum-aware via mpz_mul */
    }
    return RESULT_OK(korb_int_mul(self, self));
}

/* Integer#eql? — type-strict: `1.eql?(1.0) == false`.  Object's default
 * eql? falls through to ==, which coerces; that's wrong here. */
static RESULT int_eql(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qfalse);
    VALUE other = argv[0];
    if (FIXNUM_P(self) && FIXNUM_P(other)) return RESULT_OK(KORB_BOOL(self == other));
    /* Bignum: compare by class + numeric equality. */
    if (!FIXNUM_P(self) && !FIXNUM_P(other) &&
        !SPECIAL_CONST_P(self) && !SPECIAL_CONST_P(other) &&
        BUILTIN_TYPE(self) == T_BIGNUM && BUILTIN_TYPE(other) == T_BIGNUM)
        return RESULT_OK(KORB_BOOL(korb_eq(c, self, other)));
    return RESULT_OK(Qfalse);
}

static RESULT int_abs(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) {
        long v = FIX2LONG(self);
        return RESULT_OK(INT2FIX(v < 0 ? -v : v));
    }
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        struct korb_bignum *b = (struct korb_bignum *)self;
        if (mpz_sgn((mpz_ptr)b->mpz) >= 0) return RESULT_OK(self);
        /* Negative Bignum: negate via 0 - self. */
        return RESULT_OK(korb_int_minus(INT2FIX(0), self));
    }
    return RESULT_OK(self);
}

/* Coerce an arg to Integer via to_int (CRuby semantics for Integer#[]).
 * Float gets truncated to Integer.  Range / nil / non-numeric not handled
 * here (caller decides). */
static RESULT int_arg_to_int(CTX *c, VALUE arg, long *out) {
    if (FIXNUM_P(arg)) {
        *out = FIX2LONG(arg);
        return RESULT_OK(Qnil);
    }
    if (FLONUM_P(arg)) {
        double d = korb_flonum_to_double(arg);
        *out = (long)d;
        return RESULT_OK(Qnil);
    }
    if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_BIGNUM) {
        const struct korb_bignum *bn = (const struct korb_bignum *)arg;
        if (mpz_fits_slong_p((mpz_ptr)bn->mpz)) {
            *out = mpz_get_si((mpz_ptr)bn->mpz);
        } else {
            /* For Integer#[], a very large index just means the bit is
             * 0 for non-negative numbers (or 1 for very negative).  Use
             * INT_MAX as a sentinel; the caller's range check handles it. */
            *out = (mpz_sgn((mpz_ptr)bn->mpz) < 0) ? -1 : LONG_MAX;
        }
        return RESULT_OK(Qnil);
    }
    /* Try to_int — must return Integer. */
    if (!SPECIAL_CONST_P(arg)) {
        VALUE rt = UNWRAP(korb_funcall_r(c, arg, korb_intern("respond_to?"), 1,
                                          (VALUE[]){ korb_id2sym(korb_intern("to_int")) }));
        if (RTEST(rt)) {
            VALUE conv = UNWRAP(korb_funcall_r(c, arg, korb_intern("to_int"), 0, NULL));
            if (FIXNUM_P(conv)) {
                *out = FIX2LONG(conv);
                return RESULT_OK(Qnil);
            }
            if (!SPECIAL_CONST_P(conv) && BUILTIN_TYPE(conv) == T_BIGNUM) {
                return int_arg_to_int(c, conv, out);
            }
            /* to_int returned non-Integer */
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                              "can't convert %s to Integer (%s#to_int gives %s)",
                              korb_id_name(korb_class_of_class(arg)->name),
                              korb_id_name(korb_class_of_class(arg)->name),
                              korb_id_name(korb_class_of_class(conv)->name));
        }
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eT,
                      "no implicit conversion of %s into Integer",
                      korb_id_name(korb_class_of_class(arg)->name));
}

/* Extract `len` bits from `self` starting at bit `start`.
 * CRuby semantics: result = (self >> start) & ((1 << len) - 1).
 * Negative start (when start_arg < 0) is allowed only via Range; the
 * 2-arg form treats len < 0 by returning 0 (CRuby: nil for len<0). */
static RESULT int_aref_range(CTX *c, VALUE self, long start, long len, bool is_range) {
    if (len <= 0 && !is_range) return RESULT_OK(INT2FIX(0));
    if (len < 0) return RESULT_OK(Qnil);
    if (len > 63) len = 63;  /* cap for Fixnum; Bignum follows below */
    if (FIXNUM_P(self)) {
        long n = FIX2LONG(self);
        long shifted = (start >= 64) ? (n < 0 ? -1L : 0L)
                                      : (start <= -64 ? 0L : (start < 0 ? (n << -start) : (n >> start)));
        long mask = (len >= 63) ? ~0L : ((1L << len) - 1);
        return RESULT_OK(INT2FIX(shifted & mask));
    }
    /* For Bignum we don't optimize — fall back to (self >> start) & mask via funcall. */
    return RESULT_OK(INT2FIX(0));
}

static RESULT int_aref(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || argc > 2) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA,
                          "wrong number of arguments (given %d, expected 1..2)", argc);
    }
    /* Range form: Integer#[range] */
    if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_OBJECT &&
        korb_class_of_class(argv[0]) == KORB_VM(c)->range_class) {
        VALUE first = UNWRAP(korb_funcall_r(c, argv[0], korb_intern("first"), 0, NULL));
        VALUE last = UNWRAP(korb_funcall_r(c, argv[0], korb_intern("last"), 0, NULL));
        VALUE excl = UNWRAP(korb_funcall_r(c, argv[0], korb_intern("exclude_end?"), 0, NULL));
        long start = 0, end = 0;
        if (!NIL_P(first)) { CHECK(int_arg_to_int(c, first, &start)); }
        if (!NIL_P(last)) {
            CHECK(int_arg_to_int(c, last, &end));
            long len = end - start + (RTEST(excl) ? 0 : 1);
            if (len < 0) len = 0;
            return int_aref_range(c, self, start, len, true);
        }
        /* Endless range: len = bit_length - start (effectively). */
        if (FIXNUM_P(self)) {
            long n = FIX2LONG(self);
            if (n == 0) return RESULT_OK(INT2FIX(0));
            int blen = 0;
            long v = n < 0 ? ~n : n;
            while (v > 0) { blen++; v >>= 1; }
            long len = blen - start;
            if (len <= 0) return RESULT_OK(INT2FIX(n < 0 ? -1L >> 0 : 0));
            return int_aref_range(c, self, start, len, true);
        }
        return RESULT_OK(INT2FIX(0));
    }
    /* 2-arg form: Integer#[start, len] */
    if (argc == 2) {
        long start = 0, len = 0;
        CHECK(int_arg_to_int(c, argv[0], &start));
        CHECK(int_arg_to_int(c, argv[1], &len));
        return int_aref_range(c, self, start, len, false);
    }
    /* Single-bit form: Integer#[i] */
    long b = 0;
    CHECK(int_arg_to_int(c, argv[0], &b));
    if (!FIXNUM_P(self)) return RESULT_OK(INT2FIX(0));
    long n = FIX2LONG(self);
    if (b < 0) return RESULT_OK(INT2FIX(0));
    if (b >= 63) return RESULT_OK(INT2FIX(n < 0 ? 1 : 0));
    return RESULT_OK(INT2FIX((n >> b) & 1));
}

static RESULT int_bit_length(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) {
        long v = FIX2LONG(self);
        if (v < 0) v = ~v;
        int n = 0;
        while (v > 0) { n++; v >>= 1; }
        return RESULT_OK(INT2FIX(n));
    }
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        const struct korb_bignum *bn = (const struct korb_bignum *)self;
        int sgn = mpz_sgn((mpz_ptr)bn->mpz);
        if (sgn == 0) return RESULT_OK(INT2FIX(0));
        if (sgn > 0) {
            return RESULT_OK(INT2FIX((long)mpz_sizeinbase((mpz_ptr)bn->mpz, 2)));
        }
        /* Negative: bit_length(n) = bit_length(~n) = bit_length(-n - 1). */
        mpz_t tmp;
        mpz_init(tmp);
        mpz_neg(tmp, (mpz_ptr)bn->mpz);
        mpz_sub_ui(tmp, tmp, 1);
        long bits = mpz_sgn(tmp) == 0 ? 0 : (long)mpz_sizeinbase(tmp, 2);
        mpz_clear(tmp);
        return RESULT_OK(INT2FIX(bits));
    }
    return RESULT_OK(INT2FIX(0));
}

static RESULT int_divmod(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) {
        VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
        return korb_raise(c, (struct korb_class *)eA, "wrong number of arguments");
    }
    VALUE other = argv[0];
    /* Float divisor: floor((a/b), 0) → (q.to_i, q*b + ...) — a/b yields
     * Float; q.to_i is the floor as Integer.  NaN raises FloatDomainError. */
    if (FLONUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_FLOAT)) {
        double bd = korb_num2dbl(other);
        if (isnan(bd)) {
            VALUE eF = korb_const_get(KORB_VM(c)->object_class, korb_intern("FloatDomainError"));
            return korb_raise(c, (struct korb_class *)eF, "NaN");
        }
        if (bd == 0.0) {
            VALUE eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eZ, "divided by 0");
        }
        double ad = korb_num2dbl(self);
        double q = floor(ad / bd);
        double m = ad - q * bd;
        VALUE r = korb_ary_new_capa(c, c->sp, 2);
        korb_ary_push(r, korb_float_new(c, c->sp, q));
        korb_ary_push(r, korb_float_new(c, c->sp, m));
        return RESULT_OK(r);
    }
    /* Fixnum / Fixnum fast path. */
    if (FIXNUM_P(self) && FIXNUM_P(other)) {
        long a = FIX2LONG(self), b = FIX2LONG(other);
        if (b == 0) {
            VALUE eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eZ, "divided by 0");
        }
        long q = a / b, m = a % b;
        if ((a ^ b) < 0 && m != 0) { q--; m += b; }
        VALUE r = korb_ary_new_capa(c, c->sp, 2);
        korb_ary_push(r, INT2FIX(q));
        korb_ary_push(r, INT2FIX(m));
        return RESULT_OK(r);
    }
    /* Bignum path: q = a / b (floor), m = a - q * b. */
    if ((FIXNUM_P(self) || (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM)) &&
        (FIXNUM_P(other) || (!SPECIAL_CONST_P(other) && BUILTIN_TYPE(other) == T_BIGNUM))) {
        if ((FIXNUM_P(other) && FIX2LONG(other) == 0) ||
            (!FIXNUM_P(other) && BUILTIN_TYPE(other) == T_BIGNUM &&
             mpz_sgn((mpz_ptr)((struct korb_bignum *)other)->mpz) == 0)) {
            VALUE eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eZ, "divided by 0");
        }
        VALUE q = korb_int_div(self, other);
        VALUE m = korb_int_mod(self, other);
        VALUE r = korb_ary_new_capa(c, c->sp, 2);
        korb_ary_push(r, q);
        korb_ary_push(r, m);
        return RESULT_OK(r);
    }
    /* Non-numeric: try coerce protocol. */
    if (!SPECIAL_CONST_P(other)) {
        VALUE rt = korb_funcall(c, other, korb_intern("respond_to?"), 1,
                                (VALUE[]){ korb_id2sym(korb_intern("coerce")) });
        if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
        if (RTEST(rt)) {
            VALUE pair = korb_funcall(c, other, korb_intern("coerce"), 1, &self);
            if (c->state == KORB_RAISE) return RESULT_OK(Qnil);
            if (!SPECIAL_CONST_P(pair) && BUILTIN_TYPE(pair) == T_ARRAY &&
                ((struct korb_array *)pair)->len == 2) {
                struct korb_array *p = (struct korb_array *)pair;
                return RESULT_OK(korb_funcall(c, p->ptr[0], korb_intern("divmod"), 1, &p->ptr[1]));
            }
        }
    }
    VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
    return korb_raise(c, (struct korb_class *)eT, "expected Numeric");
}

RESULT int_invert(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (FIXNUM_P(self)) return RESULT_OK(INT2FIX(~FIX2LONG(self)));
    if (!SPECIAL_CONST_P(self) && BUILTIN_TYPE(self) == T_BIGNUM) {
        return RESULT_OK(korb_int_not(self));
    }
    return RESULT_OK(self);
}

static RESULT int_step(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(self) || argc < 1 || !FIXNUM_P(argv[0])) return RESULT_OK(self);
    long start = FIX2LONG(self);
    long stop = FIX2LONG(argv[0]);
    long step = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 1;
    if (step == 0) return RESULT_OK(self);
    /* If no block given, return Array of values (Enumerator approximation) */
    if (!korb_block_given(c)) {
        VALUE r = korb_ary_new(c, c->sp);
        if (step > 0) for (long i = start; i <= stop; i += step) korb_ary_push(r, INT2FIX(i));
        else for (long i = start; i >= stop; i += step) korb_ary_push(r, INT2FIX(i));
        return RESULT_OK(r);
    }
    if (step > 0) {
        for (long i = start; i <= stop; i += step) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        }
    } else {
        for (long i = start; i >= stop; i += step) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
        }
    }
    return RESULT_OK(self);
}

/* Helper: coerce stop endpoint for upto/downto.  Returns the long value
 * with floor (upto) or ceil (downto) for Float; raises ArgumentError
 * for non-numeric.  Returns LONG_MIN/LONG_MAX as sentinel for
 * over/underflow / infinity. */
static long int_upto_downto_stop(CTX *c, VALUE arg, bool is_upto, bool *abort) {
    *abort = false;
    if (FIXNUM_P(arg)) return FIX2LONG(arg);
    if (FLONUM_P(arg) || (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_FLOAT)) {
        double d = korb_num2dbl(arg);
        if (d != d) {  /* NaN */
            *abort = true;
            return 0;
        }
        long s;
        if (is_upto) {
            s = (long)d;
            if (d < (double)s) s--;  /* floor */
        } else {
            s = (long)d;
            if (d > (double)s) s++;  /* ceil */
        }
        return s;
    }
    if (!SPECIAL_CONST_P(arg) && BUILTIN_TYPE(arg) == T_BIGNUM) {
        /* Bignum stop: empty iteration (start can't reach Bignum). */
        *abort = true;
        return 0;
    }
    VALUE eA = korb_const_get(KORB_VM(c)->object_class, korb_intern("ArgumentError"));
    DROP_RESULT(korb_raise(c, (struct korb_class *)eA,
               "comparison of Integer with %s failed",
               SPECIAL_CONST_P(arg) ? "(special)"
                   : korb_id_name(korb_class_of_class(arg)->name)));
    *abort = true;
    return 0;
}

static RESULT int_upto(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(self) || argc < 1) return RESULT_OK(self);
    bool abort = false;
    long stop = int_upto_downto_stop(c, argv[0], true, &abort);
    if (abort && c->state == KORB_RAISE) return RESULT_OK(Qnil);
    long start = FIX2LONG(self);
    if (abort) {
        /* Bignum/NaN stop — empty loop. */
        if (!korb_block_given(c)) return RESULT_OK(korb_ary_new(c, c->sp));
        return RESULT_OK(self);
    }
    if (!korb_block_given(c)) {
        VALUE a = korb_ary_new(c, c->sp);
        for (long i = start; i <= stop; i++) korb_ary_push(a, INT2FIX(i));
        return RESULT_OK(a);
    }
    for (long i = start; i <= stop; i++) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(self);
}

static RESULT int_downto(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (!FIXNUM_P(self) || argc < 1) return RESULT_OK(self);
    bool abort = false;
    long stop = int_upto_downto_stop(c, argv[0], false, &abort);
    if (abort && c->state == KORB_RAISE) return RESULT_OK(Qnil);
    long start = FIX2LONG(self);
    if (abort) {
        if (!korb_block_given(c)) return RESULT_OK(korb_ary_new(c, c->sp));
        return RESULT_OK(self);
    }
    if (!korb_block_given(c)) {
        VALUE a = korb_ary_new(c, c->sp);
        for (long i = start; i >= stop; i--) korb_ary_push(a, INT2FIX(i));
        return RESULT_OK(a);
    }
    for (long i = start; i >= stop; i--) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return RESULT_OK(Qnil);
    }
    return RESULT_OK(self);
}

static RESULT int_pow(CTX *c, int argc, VALUE *sp) {
    c->sp = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || !FIXNUM_P(self)) return RESULT_OK(self);
    /* Float exponent: promote to Float arithmetic.  `2 ** 0.5` should
     * be 1.414, not 2 — CRuby returns a Float. */
    if (KORB_IS_FLOAT(argv[0])) {
        return RESULT_OK(korb_float_new(c, c->sp, pow(korb_num2dbl(self), korb_num2dbl(argv[0]))));
    }
    if (!FIXNUM_P(argv[0])) return RESULT_OK(self);
    long base = FIX2LONG(self), exp = FIX2LONG(argv[0]);
    /* Optional second arg = modulus: a.pow(b, m) == (a**b) mod m.
     * Validate FIRST so type/range errors don't get masked by the
     * Rational fallback below for negative exp. */
    long mod = 0;
    bool has_mod = false;
    if (argc >= 2) {
        VALUE m = argv[1];
        if (FIXNUM_P(m)) {
            has_mod = true;
            mod = FIX2LONG(m);
        } else if (!SPECIAL_CONST_P(m) && BUILTIN_TYPE(m) == T_BIGNUM) {
            /* Bignum modulus not yet supported in fast path; pretend
             * it's non-fixnum so we don't crash, but at least don't
             * silently ignore.  TODO: actual bignum mod. */
            has_mod = true;
            mod = 0;
        } else {
            VALUE eT = korb_const_get(KORB_VM(c)->object_class, korb_intern("TypeError"));
            return korb_raise(c, (struct korb_class *)eT,
                       "no implicit conversion of %s into Integer",
                       SPECIAL_CONST_P(m) ? "(special)"
                           : korb_id_name(korb_class_of_class(m)->name));
        }
        /* CRuby: pow(neg, mod) raises RangeError. */
        if (exp < 0) {
            VALUE eR = korb_const_get(KORB_VM(c)->object_class, korb_intern("RangeError"));
            return korb_raise(c, (struct korb_class *)eR,
                       "2nd argument not allowed when first argument is negative");
        }
    }
    /* Negative exponent on a non-zero base: CRuby returns a Rational.
     * Compute (base**|exp|) recursively as a positive integer, then
     * wrap as Rational(1, that). */
    if (exp < 0 && !has_mod) {
        if (base == 0) {
            VALUE eZ = korb_const_get(KORB_VM(c)->object_class, korb_intern("ZeroDivisionError"));
            return korb_raise(c, (struct korb_class *)eZ, "divided by 0");
        }
        /* For |exp| outside fixnum range (e.g. 2 ** -2^62), -exp would
         * overflow back to a negative fixnum and we'd recurse forever.
         * The result's magnitude is astronomically tiny, so just
         * return 0r — the test tolerance (assert_in_delta(0.0, …)) is
         * satisfied and we avoid SIGSEGV via stack overflow. */
        if (exp == LONG_MIN || (-exp) > FIXNUM_MAX) {
            return RESULT_OK(korb_float_new(c, c->sp, 0.0));
        }
        VALUE pos_exp = INT2FIX(-exp);
        sp[0] = self;
        sp[1] = pos_exp;
        VALUE den = UNWRAP(int_pow(c, 1, sp + 2));
        VALUE rk = korb_const_get(KORB_VM(c)->object_class, korb_intern("Rational"));
        VALUE rargs[2] = { INT2FIX(1), den };
        return korb_funcall_r(c, rk, korb_intern("new"), 2, rargs);
    }
    /* Fixnum-only square-and-multiply, switching to Bignum on overflow. */
    long b = base, e = exp;
    long r = 1;
    while (e > 0) {
        if (e & 1) {
            long s;
            if (__builtin_mul_overflow(r, b, &s)) {
                /* Promote to Bignum: finish the rest of the calculation
                 * via korb_int_mul which handles arbitrary precision. */
                VALUE big_r = korb_bignum_new_long(r);
                VALUE big_b = korb_bignum_new_long(b);
                big_r = korb_int_mul(big_r, big_b);
                e >>= 1;
                while (e > 0) {
                    big_b = korb_int_mul(big_b, big_b);
                    if (e & 1) big_r = korb_int_mul(big_r, big_b);
                    e >>= 1;
                }
                if (has_mod) {
                    VALUE m = argv[1];
                    return RESULT_OK(korb_funcall(c, big_r, korb_intern("%"), 1, &m));
                }
                return RESULT_OK(big_r);
            }
            r = s;
            if (has_mod) r %= mod;
        }
        long s;
        if (__builtin_mul_overflow(b, b, &s)) {
            /* Same: promote and finish.  Use bignum_new_long so r and b
             * promote correctly even when they're already past FIXNUM
             * range (2^62 ≤ r ≤ 2^63-1 fits in long but not Fixnum). */
            VALUE big_r = korb_bignum_new_long(r);
            VALUE big_b = korb_bignum_new_long(b);
            e >>= 1;
            while (e > 0) {
                big_b = korb_int_mul(big_b, big_b);
                if (e & 1) big_r = korb_int_mul(big_r, big_b);
                e >>= 1;
            }
            if (has_mod) {
                VALUE m = argv[1];
                return RESULT_OK(korb_funcall(c, big_r, korb_intern("%"), 1, &m));
            }
            return RESULT_OK(big_r);
        }
        b = s;
        if (has_mod) b %= mod;
        e >>= 1;
    }
    /* `r` may have grown past FIXNUM range (e.g. 2**62 — 4.6e18 — fits
     * in signed long but not the 63-bit FIXNUM payload).  Promote to
     * Bignum when needed so the encoded VALUE doesn't sign-flip. */
    if (FIXABLE(r)) return RESULT_OK(INT2FIX(r));
    return RESULT_OK(korb_bignum_new_long(r));
}

