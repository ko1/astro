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
    VALUE klass = korb_const_get(korb_vm->object_class, korb_intern("Rational"));
    VALUE one = INT2FIX(1);
    VALUE args[2] = {self, one};
    return korb_funcall(c, klass, korb_intern("new"), 2, args);
}

#define COERCE_OR_RAISE(c, v, op_name)                                  \
    do {                                                                 \
        if (!FIXNUM_P(v) && BUILTIN_TYPE(v) != T_BIGNUM) {                \
            if (KORB_IS_FLOAT(v)) {                              \
                /* fall through — caller handles */                        \
            } else {                                                       \
                korb_raise((c), NULL, "%s expected Integer, got %s",       \
                           (op_name), korb_id_name(korb_class_of_class((v))->name)); \
                return Qnil;                                               \
            }                                                              \
        }                                                                  \
    } while (0)

static VALUE int_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (KORB_IS_FLOAT(argv[0])) {
        return korb_float_new((double)FIX2LONG(self) + korb_num2dbl(argv[0]));
    }
    if (int_op_other_kind(argv[0])) {
        /* + is commutative — delegate to Rational#+/Complex#+. */
        return korb_funcall(c, argv[0], korb_intern("+"), 1, &self);
    }
    COERCE_OR_RAISE(c, argv[0], "+");
    return korb_int_plus(self, argv[0]);
}
static VALUE int_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (KORB_IS_FLOAT(argv[0])) {
        return korb_float_new((double)FIX2LONG(self) - korb_num2dbl(argv[0]));
    }
    if (int_op_other_kind(argv[0])) {
        VALUE r = int_to_rational_obj(c, self);
        return korb_funcall(c, r, korb_intern("-"), 1, &argv[0]);
    }
    COERCE_OR_RAISE(c, argv[0], "-");
    return korb_int_minus(self, argv[0]);
}
static VALUE int_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (KORB_IS_FLOAT(argv[0])) {
        return korb_float_new((double)FIX2LONG(self) * korb_num2dbl(argv[0]));
    }
    if (int_op_other_kind(argv[0])) {
        return korb_funcall(c, argv[0], korb_intern("*"), 1, &self);
    }
    COERCE_OR_RAISE(c, argv[0], "*");
    return korb_int_mul(self, argv[0]);
}
static VALUE int_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (KORB_IS_FLOAT(argv[0])) {
        return korb_float_new((double)FIX2LONG(self) / korb_num2dbl(argv[0]));
    }
    if (int_op_other_kind(argv[0])) {
        VALUE r = int_to_rational_obj(c, self);
        return korb_funcall(c, r, korb_intern("/"), 1, &argv[0]);
    }
    COERCE_OR_RAISE(c, argv[0], "/");
    if (FIXNUM_P(argv[0]) && FIX2LONG(argv[0]) == 0) {
        korb_raise(c, NULL, "divided by 0");
        return Qnil;
    }
    return korb_int_div(self, argv[0]);
}
static VALUE int_mod(CTX *c, VALUE self, int argc, VALUE *argv) {
    COERCE_OR_RAISE(c, argv[0], "%");
    if (FIXNUM_P(argv[0]) && FIX2LONG(argv[0]) == 0) {
        korb_raise(c, NULL, "divided by 0");
        return Qnil;
    }
    return korb_int_mod(self, argv[0]);
}
static VALUE int_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_lshift(self, argv[0]);
}
static VALUE int_rshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_rshift(self, argv[0]);
}
static VALUE int_and(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_and(self, argv[0]);
}
static VALUE int_or(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_or(self, argv[0]);
}
static VALUE int_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_xor(self, argv[0]);
}
/* Numeric comparators: raise ArgumentError on non-numeric RHS instead of
 * segfaulting through to_mpz.  Ruby semantics. */
#define INT_CMP_GUARD(c, rhs, op) do { \
    if (!FIXNUM_P(rhs) && !FLONUM_P(rhs) && \
        (SPECIAL_CONST_P(rhs) || (BUILTIN_TYPE(rhs) != T_BIGNUM && BUILTIN_TYPE(rhs) != T_FLOAT))) { \
        korb_raise((c), NULL, "comparison of Integer with non-numeric failed"); \
        return Qnil; \
    } \
} while (0)
static VALUE int_lt(CTX *c, VALUE self, int argc, VALUE *argv) {
    INT_CMP_GUARD(c, argv[0], "<");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return KORB_BOOL((double)FIX2LONG(self) < korb_num2dbl(argv[0]));
    return KORB_BOOL(korb_int_cmp(self, argv[0]) < 0);
}
static VALUE int_le(CTX *c, VALUE self, int argc, VALUE *argv) {
    INT_CMP_GUARD(c, argv[0], "<=");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return KORB_BOOL((double)FIX2LONG(self) <= korb_num2dbl(argv[0]));
    return KORB_BOOL(korb_int_cmp(self, argv[0]) <= 0);
}
static VALUE int_gt(CTX *c, VALUE self, int argc, VALUE *argv) {
    INT_CMP_GUARD(c, argv[0], ">");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return KORB_BOOL((double)FIX2LONG(self) > korb_num2dbl(argv[0]));
    return KORB_BOOL(korb_int_cmp(self, argv[0]) > 0);
}
static VALUE int_ge(CTX *c, VALUE self, int argc, VALUE *argv) {
    INT_CMP_GUARD(c, argv[0], ">=");
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return KORB_BOOL((double)FIX2LONG(self) >= korb_num2dbl(argv[0]));
    return KORB_BOOL(korb_int_cmp(self, argv[0]) >= 0);
}
static VALUE int_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0]))
        return KORB_BOOL((double)FIX2LONG(self) == korb_num2dbl(argv[0]));
    /* Only Integer/Bignum can be == an Integer; everything else is false.
     * Without this guard, `0 == nil` segfaults inside to_mpz (which casts
     * the second operand to a Bignum pointer). */
    if (!FIXNUM_P(argv[0]) &&
        (SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_BIGNUM))
        return Qfalse;
    return KORB_BOOL(korb_int_eq(self, argv[0]));
}
static VALUE int_cmp(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    if (FIXNUM_P(self) && FIXNUM_P(argv[0])) {
        return INT2FIX((intptr_t)self < (intptr_t)argv[0] ? -1 : (intptr_t)self > (intptr_t)argv[0] ? 1 : 0);
    }
    if ((FIXNUM_P(self) || BUILTIN_TYPE(self) == T_BIGNUM) &&
        (FIXNUM_P(argv[0]) || BUILTIN_TYPE(argv[0]) == T_BIGNUM)) {
        return INT2FIX(korb_int_cmp(self, argv[0]));
    }
    if (FLONUM_P(argv[0]) || KORB_IS_FLOAT(argv[0])) {
        double a = (double)FIX2LONG(self);
        double b = korb_num2dbl(argv[0]);
        return INT2FIX(a < b ? -1 : a > b ? 1 : 0);
    }
    return Qnil;
}
static VALUE int_uminus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_minus(INT2FIX(0), self);
}
static VALUE int_format(CTX *c, VALUE self, int argc, VALUE *argv);
static VALUE int_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return int_format(c, self, argc, argv);
}
static VALUE int_to_i(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE int_to_f(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_float_new((double)FIX2LONG(self)); }
static VALUE int_even_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL((FIX2LONG(self) & 1) == 0);
    return Qfalse;
}
static VALUE int_odd_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL((FIX2LONG(self) & 1) == 1);
    return Qfalse;
}
static VALUE int_positive_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL(FIX2LONG(self) > 0);
    return Qfalse;
}
static VALUE int_negative_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL(FIX2LONG(self) < 0);
    return Qfalse;
}

static VALUE int_zero_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL(self == INT2FIX(0));
    return Qfalse;
}
static VALUE int_times(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* call block self times */
    if (!FIXNUM_P(self)) return Qnil;
    long n = FIX2LONG(self);
    /* yield current block: for simplicity we use korb_yield */
    for (long i = 0; i < n; i++) {
        VALUE arg = INT2FIX(i);
        VALUE r = korb_yield(c, 1, &arg);
        if (c->state != KORB_NORMAL) return r;
    }
    return self;
}
static VALUE int_succ(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_plus(self, INT2FIX(1));
}
static VALUE int_pred(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_minus(self, INT2FIX(1));
}


/* ---------- Integer methods (extended) ---------- */
static VALUE int_chr(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self)) return Qnil;
    char ch = (char)(FIX2LONG(self) & 0xff);
    return korb_str_new(&ch, 1);
}

static VALUE int_format(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Integer#to_s(base).  For non-decimal bases Ruby renders negatives
     * as "-<digits>", not as the unsigned twos-complement word. */
    if (!FIXNUM_P(self)) return korb_to_s(self);
    long v = FIX2LONG(self);
    int base = argc >= 1 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10;
    char buf[80];
    if (base == 10) {
        snprintf(buf, sizeof(buf), "%ld", v);
        return korb_str_new_cstr(buf);
    }
    bool neg = v < 0;
    unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (base == 16) snprintf(buf, sizeof(buf), neg ? "-%lx" : "%lx", uv);
    else if (base == 8) snprintf(buf, sizeof(buf), neg ? "-%lo" : "%lo", uv);
    else if (base == 2) {
        char tmp[80]; int tl = 0;
        if (uv == 0) tmp[tl++] = '0';
        while (uv) { tmp[tl++] = '0' + (uv & 1); uv >>= 1; }
        for (int i = 0; i < tl/2; i++) { char t = tmp[i]; tmp[i] = tmp[tl-1-i]; tmp[tl-1-i] = t; }
        tmp[tl] = 0;
        if (neg) {
            char out[82]; out[0] = '-';
            memcpy(out+1, tmp, tl+1);
            return korb_str_new_cstr(out);
        }
        return korb_str_new_cstr(tmp);
    }
    else snprintf(buf, sizeof(buf), "%ld", v);
    return korb_str_new_cstr(buf);
}

static VALUE int_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (FIXNUM_P(self) && FIXNUM_P(argv[0])) return KORB_BOOL(self == argv[0]);
    return KORB_BOOL(korb_eq(self, argv[0]));
}

static VALUE int_floor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
}

/* ---------- Integer#div / Integer#fdiv ----------
 * Integer#div is floored division (rounds toward -infinity).  This is
 * a different method from Integer#/ (the `/` operator above), which
 * already exists; div is registered separately as the named method. */
static VALUE int_method_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return Qnil;
    long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
    if (b == 0) {
        VALUE eDiv = korb_const_get(korb_vm->object_class, korb_intern("ZeroDivisionError"));
        korb_raise(c, (struct korb_class *)eDiv, "divided by 0");
        return Qnil;
    }
    long q = a / b;
    long r = a % b;
    if ((r != 0) && ((r < 0) != (b < 0))) q--;
    return INT2FIX(q);
}

static VALUE int_fdiv(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    double a = (double)(FIXNUM_P(self) ? FIX2LONG(self) : 0);
    double b;
    if      (FIXNUM_P(argv[0]))                                            b = (double)FIX2LONG(argv[0]);
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_FLOAT) b = korb_num2dbl(argv[0]);
    else return Qnil;
    return korb_float_new(a / b);
}

/* Integer#size — width in bytes of the machine word.  Matches CRuby's
 * `1.size == 8` on a 64-bit build. */
static VALUE int_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)sizeof(long));
}

/* Integer#eql? — type-strict: `1.eql?(1.0) == false`.  Object's default
 * eql? falls through to ==, which coerces; that's wrong here. */
static VALUE int_eql(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    VALUE other = argv[0];
    if (FIXNUM_P(self) && FIXNUM_P(other)) return KORB_BOOL(self == other);
    /* Bignum: compare by class + numeric equality. */
    if (!FIXNUM_P(self) && !FIXNUM_P(other) &&
        !SPECIAL_CONST_P(self) && !SPECIAL_CONST_P(other) &&
        BUILTIN_TYPE(self) == T_BIGNUM && BUILTIN_TYPE(other) == T_BIGNUM)
        return KORB_BOOL(korb_eq(self, other));
    return Qfalse;
}

static VALUE int_abs(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) {
        long v = FIX2LONG(self);
        return INT2FIX(v < 0 ? -v : v);
    }
    return self;
}

static VALUE int_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Integer#[i] — extract bit i */
    if (argc < 1 || !FIXNUM_P(argv[0])) return INT2FIX(0);
    if (!FIXNUM_P(self)) return INT2FIX(0);
    long n = FIX2LONG(self);
    long b = FIX2LONG(argv[0]);
    if (b < 0 || b >= 63) return INT2FIX(0);
    return INT2FIX((n >> b) & 1);
}

static VALUE int_bit_length(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self)) return INT2FIX(0);
    long v = FIX2LONG(self);
    if (v < 0) v = ~v;
    int n = 0;
    while (v > 0) { n++; v >>= 1; }
    return INT2FIX(n);
}

static VALUE int_divmod(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return korb_ary_new();
    long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
    if (b == 0) { korb_raise(c, NULL, "divided by 0"); return Qnil; }
    long q = a / b, m = a % b;
    if ((a ^ b) < 0 && m != 0) { q--; m += b; }
    VALUE r = korb_ary_new_capa(2);
    korb_ary_push(r, INT2FIX(q));
    korb_ary_push(r, INT2FIX(m));
    return r;
}

VALUE int_invert(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return INT2FIX(~FIX2LONG(self));
    return self;
}

static VALUE int_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self) || argc < 1 || !FIXNUM_P(argv[0])) return self;
    long start = FIX2LONG(self);
    long stop = FIX2LONG(argv[0]);
    long step = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 1;
    if (step == 0) return self;
    /* If no block given, return Array of values (Enumerator approximation) */
    if (!korb_block_given()) {
        VALUE r = korb_ary_new();
        if (step > 0) for (long i = start; i <= stop; i += step) korb_ary_push(r, INT2FIX(i));
        else for (long i = start; i >= stop; i += step) korb_ary_push(r, INT2FIX(i));
        return r;
    }
    if (step > 0) {
        for (long i = start; i <= stop; i += step) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return Qnil;
        }
    } else {
        for (long i = start; i >= stop; i += step) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return Qnil;
        }
    }
    return self;
}

static VALUE int_upto(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self) || argc < 1 || !FIXNUM_P(argv[0])) return self;
    long start = FIX2LONG(self), stop = FIX2LONG(argv[0]);
    for (long i = start; i <= stop; i++) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE int_downto(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self) || argc < 1 || !FIXNUM_P(argv[0])) return self;
    long start = FIX2LONG(self), stop = FIX2LONG(argv[0]);
    for (long i = start; i >= stop; i--) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE int_pow(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return self;
    long base = FIX2LONG(self), exp = FIX2LONG(argv[0]);
    /* Optional second arg = modulus: a.pow(b, m) == (a**b) mod m. */
    long mod = 0;
    bool has_mod = (argc >= 2 && FIXNUM_P(argv[1]));
    if (has_mod) mod = FIX2LONG(argv[1]);
    if (exp < 0) {
        if (has_mod) return INT2FIX(0);
        /* a ** -k → Float reciprocal of a ** k. */
        return korb_float_new(1.0 / pow((double)base, (double)-exp));
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
                VALUE big_r = INT2FIX(r);
                VALUE big_b = INT2FIX(b);
                big_r = korb_int_mul(big_r, big_b);
                e >>= 1;
                while (e > 0) {
                    big_b = korb_int_mul(big_b, big_b);
                    if (e & 1) big_r = korb_int_mul(big_r, big_b);
                    e >>= 1;
                }
                if (has_mod) {
                    VALUE m = argv[1];
                    return korb_funcall(c, big_r, korb_intern("%"), 1, &m);
                }
                return big_r;
            }
            r = s;
            if (has_mod) r %= mod;
        }
        long s;
        if (__builtin_mul_overflow(b, b, &s)) {
            /* Same: promote and finish. */
            VALUE big_r = INT2FIX(r);
            VALUE big_b = INT2FIX(b);
            e >>= 1;
            while (e > 0) {
                big_b = korb_int_mul(big_b, big_b);
                if (e & 1) big_r = korb_int_mul(big_r, big_b);
                e >>= 1;
            }
            if (has_mod) {
                VALUE m = argv[1];
                return korb_funcall(c, big_r, korb_intern("%"), 1, &m);
            }
            return big_r;
        }
        b = s;
        if (has_mod) b %= mod;
        e >>= 1;
    }
    return INT2FIX(r);
}

