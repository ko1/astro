#include "builtin.h"

// Fixnum fast path: both Fixnum -> direct C arithmetic.
// Otherwise: AB_INT_UNWRAP/AB_NUM_UNWRAP to get raw CRuby values,
// call rb_funcall/rb_big_*, wrap result with AB_NUM_WRAP.

static RESULT ab_integer_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(AB_NUM_WRAP(LONG2NUM(FIX2LONG(self) + FIX2LONG(argv[0]))));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_plus(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("+"), 1, ra)));
}
static RESULT ab_integer_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(AB_NUM_WRAP(LONG2NUM(FIX2LONG(self) - FIX2LONG(argv[0]))));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_minus(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("-"), 1, ra)));
}
static RESULT ab_integer_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_mul(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("*"), 1, ra)));
}
static RESULT ab_integer_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        if (UNLIKELY(b == 0)) {
            VALUE exc = abruby_exception_new(c, c->current_frame->prev, abruby_str_new_cstr("divided by 0"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long d = a / b;
        // Ruby floor division: adjust if signs differ and there's a remainder
        if ((a ^ b) < 0 && d * b != a) d--;
        return RESULT_OK(LONG2FIX(d));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_div(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("/"), 1, ra)));
}
static RESULT ab_integer_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        if (UNLIKELY(b == 0)) {
            VALUE exc = abruby_exception_new(c, c->current_frame->prev, abruby_str_new_cstr("divided by 0"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long r = a % b;
        // Ruby modulo: result has same sign as divisor
        if (r != 0 && (r ^ b) < 0) r += b;
        return RESULT_OK(LONG2FIX(r));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_modulo(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("%"), 1, ra)));
}
static RESULT ab_integer_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(AB_NUM_WRAP(rb_big_pow(rs, ra)));
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("**"), 1, ra)));
}
static RESULT ab_integer_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(AB_NUM_WRAP(LONG2NUM(-FIX2LONG(self))));
    VALUE rs = AB_INT_UNWRAP(self);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("-@"), 0)));
}

static RESULT ab_integer_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(FIX2LONG(self) < FIX2LONG(argv[0]) ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(FIX2LONG(rb_big_cmp(rs, ra)) < 0 ? Qtrue : Qfalse);
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern("<"), 1, ra)) ? Qtrue : Qfalse);
}
static RESULT ab_integer_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(FIX2LONG(self) <= FIX2LONG(argv[0]) ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(FIX2LONG(rb_big_cmp(rs, ra)) <= 0 ? Qtrue : Qfalse);
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern("<="), 1, ra)) ? Qtrue : Qfalse);
}
static RESULT ab_integer_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(FIX2LONG(self) > FIX2LONG(argv[0]) ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(FIX2LONG(rb_big_cmp(rs, ra)) > 0 ? Qtrue : Qfalse);
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern(">"), 1, ra)) ? Qtrue : Qfalse);
}
static RESULT ab_integer_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(FIX2LONG(self) >= FIX2LONG(argv[0]) ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(FIX2LONG(rb_big_cmp(rs, ra)) >= 0 ? Qtrue : Qfalse);
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern(">="), 1, ra)) ? Qtrue : Qfalse);
}
static RESULT ab_integer_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(rb_big_eq(rs, ra));
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern("=="), 1, ra)) ? Qtrue : Qfalse);
}
static RESULT ab_integer_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(self != argv[0] ? Qtrue : Qfalse);
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM))
        return RESULT_OK(RTEST(rb_big_eq(rs, ra)) ? Qfalse : Qtrue);
    return RESULT_OK(RTEST(rb_funcall(rs, rb_intern("=="), 1, ra)) ? Qfalse : Qtrue);
}

static RESULT ab_integer_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) {
        char buf[32]; snprintf(buf, sizeof(buf), "%ld", FIX2LONG(self));
        return RESULT_OK(abruby_str_new_cstr(buf));
    }
    return RESULT_OK(abruby_str_new(rb_big2str(AB_INT_UNWRAP(self), 10)));
}
static RESULT ab_integer_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return ab_integer_inspect(c, self, 0, NULL);
}
static RESULT ab_integer_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) return RESULT_OK(FIX2LONG(self) == 0 ? Qtrue : Qfalse);
    return RESULT_OK(Qfalse);
}
static RESULT ab_integer_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(abruby_float_new_wrap(rb_float_new((double)FIX2LONG(self))));
    return RESULT_OK(abruby_float_new_wrap(rb_funcall(AB_INT_UNWRAP(self), rb_intern("to_f"), 0)));
}
static RESULT ab_integer_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) { long v = FIX2LONG(self); return RESULT_OK(AB_NUM_WRAP(LONG2NUM(v < 0 ? -v : v))); }
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(AB_INT_UNWRAP(self), rb_intern("abs"), 0)));
}

static RESULT ab_integer_cmp(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        return RESULT_OK(LONG2FIX(a < b ? -1 : (a > b ? 1 : 0)));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    if (RB_TYPE_P(rs, T_BIGNUM)) return RESULT_OK(rb_big_cmp(rs, ra));
    return RESULT_OK(rb_funcall(rs, rb_intern("<=>"), 1, ra));
}
static RESULT ab_integer_lshift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("<<"), 1, ra)));
}
static RESULT ab_integer_rshift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern(">>"), 1, ra)));
}
static RESULT ab_integer_band(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) & FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("&"), 1, ra)));
}
static RESULT ab_integer_bor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) | FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("|"), 1, ra)));
}
static RESULT ab_integer_bxor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return RESULT_OK(LONG2FIX(FIX2LONG(self) ^ FIX2LONG(argv[0])));
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_NUM_UNWRAP(argv[0]);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("^"), 1, ra)));
}
static RESULT ab_integer_bnot(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return RESULT_OK(AB_NUM_WRAP(LONG2NUM(~FIX2LONG(self))));
    VALUE rs = AB_INT_UNWRAP(self);
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(rs, rb_intern("~"), 0)));
}

static RESULT ab_integer_aref(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long val = FIX2LONG(self);
        long idx = FIX2LONG(argv[0]);
        if (idx < 0) return RESULT_OK(INT2FIX(0));
        if (idx >= (long)(sizeof(long) * 8)) return RESULT_OK(INT2FIX(val < 0 ? 1 : 0));
        return RESULT_OK(INT2FIX((val >> idx) & 1));
    }
    VALUE rs = AB_INT_UNWRAP(self), ra = AB_INT_UNWRAP(argv[0]);
    return RESULT_OK(rb_funcall(rs, rb_intern("[]"), 1, ra));
}

void
Init_abruby_integer(void)
{
    abruby_class_add_cfunc(ab_integer_class, "inspect", ab_integer_inspect, 0);
    abruby_class_add_cfunc(ab_integer_class, "to_s",   ab_integer_to_s,    0);
    abruby_class_add_cfunc(ab_integer_class, "+",      ab_integer_add,     1);
    abruby_class_add_cfunc(ab_integer_class, "-",      ab_integer_sub,     1);
    abruby_class_add_cfunc(ab_integer_class, "*",      ab_integer_mul,     1);
    abruby_class_add_cfunc(ab_integer_class, "/",      ab_integer_div,     1);
    abruby_class_add_cfunc(ab_integer_class, "%",      ab_integer_mod,     1);
    abruby_class_add_cfunc(ab_integer_class, "**",     ab_integer_pow,     1);
    abruby_class_add_cfunc(ab_integer_class, "-@",     ab_integer_neg,     0);
    abruby_class_add_cfunc(ab_integer_class, "<",      ab_integer_lt,      1);
    abruby_class_add_cfunc(ab_integer_class, "<=",     ab_integer_le,      1);
    abruby_class_add_cfunc(ab_integer_class, ">",      ab_integer_gt,      1);
    abruby_class_add_cfunc(ab_integer_class, ">=",     ab_integer_ge,      1);
    abruby_class_add_cfunc(ab_integer_class, "==",     ab_integer_eq,      1);
    abruby_class_add_cfunc(ab_integer_class, "!=",     ab_integer_neq,     1);
    abruby_class_add_cfunc(ab_integer_class, "<=>",    ab_integer_cmp,     1);
    abruby_class_add_cfunc(ab_integer_class, "<<",     ab_integer_lshift,  1);
    abruby_class_add_cfunc(ab_integer_class, ">>",     ab_integer_rshift,  1);
    abruby_class_add_cfunc(ab_integer_class, "&",      ab_integer_band,    1);
    abruby_class_add_cfunc(ab_integer_class, "|",      ab_integer_bor,     1);
    abruby_class_add_cfunc(ab_integer_class, "^",      ab_integer_bxor,    1);
    abruby_class_add_cfunc(ab_integer_class, "~",      ab_integer_bnot,    0);
    abruby_class_add_cfunc(ab_integer_class, "to_f",   ab_integer_to_f,    0);
    abruby_class_add_cfunc(ab_integer_class, "zero?",  ab_integer_zero_p,  0);
    abruby_class_add_cfunc(ab_integer_class, "abs",    ab_integer_abs,     0);
    abruby_class_add_cfunc(ab_integer_class, "[]",     ab_integer_aref,    1);
}
