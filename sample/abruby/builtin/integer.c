#include "builtin.h"

// Fixnum fast path, Bignum via rb_big_* (self must be Bignum for rb_big_*)
// When self is Fixnum but other is Bignum, use rb_funcall as safe fallback

#define BIGNUM_OP(op_name, rb_big_func) \
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) { \
        return rb_funcall(self, rb_intern(op_name), 1, argv[0]); \
    } \
    if (RB_TYPE_P(self, T_BIGNUM)) \
        return rb_big_func(self, argv[0]); \
    return rb_funcall(self, rb_intern(op_name), 1, argv[0]);

static VALUE ab_integer_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return LONG2NUM(FIX2LONG(self) + FIX2LONG(argv[0]));
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_plus(self, argv[0]);
    return rb_funcall(self, rb_intern("+"), 1, argv[0]);
}
static VALUE ab_integer_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return LONG2NUM(FIX2LONG(self) - FIX2LONG(argv[0]));
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_minus(self, argv[0]);
    return rb_funcall(self, rb_intern("-"), 1, argv[0]);
}
static VALUE ab_integer_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_mul(self, argv[0]);
    return rb_funcall(self, rb_intern("*"), 1, argv[0]);
}
static VALUE ab_integer_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        long d = a / b;
        // Ruby floor division: adjust if signs differ and there's a remainder
        if ((a ^ b) < 0 && d * b != a) d--;
        return LONG2FIX(d);
    }
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_div(self, argv[0]);
    return rb_funcall(self, rb_intern("/"), 1, argv[0]);
}
static VALUE ab_integer_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0]))) {
        long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
        long r = a % b;
        // Ruby modulo: result has same sign as divisor
        if (r != 0 && (r ^ b) < 0) r += b;
        return LONG2FIX(r);
    }
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_modulo(self, argv[0]);
    return rb_funcall(self, rb_intern("%"), 1, argv[0]);
}
static VALUE ab_integer_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (RB_TYPE_P(self, T_BIGNUM)) return rb_big_pow(self, argv[0]);
    return rb_funcall(self, rb_intern("**"), 1, argv[0]);
}
static VALUE ab_integer_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self)))
        return LONG2NUM(-FIX2LONG(self));
    return rb_funcall(self, rb_intern("-@"), 0);
}

static VALUE ab_integer_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return FIX2LONG(self) < FIX2LONG(argv[0]) ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return FIX2LONG(rb_big_cmp(self, argv[0])) < 0 ? Qtrue : Qfalse;
    return RTEST(rb_funcall(self, rb_intern("<"), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_integer_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return FIX2LONG(self) <= FIX2LONG(argv[0]) ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return FIX2LONG(rb_big_cmp(self, argv[0])) <= 0 ? Qtrue : Qfalse;
    return RTEST(rb_funcall(self, rb_intern("<="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_integer_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return FIX2LONG(self) > FIX2LONG(argv[0]) ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return FIX2LONG(rb_big_cmp(self, argv[0])) > 0 ? Qtrue : Qfalse;
    return RTEST(rb_funcall(self, rb_intern(">"), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_integer_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return FIX2LONG(self) >= FIX2LONG(argv[0]) ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return FIX2LONG(rb_big_cmp(self, argv[0])) >= 0 ? Qtrue : Qfalse;
    return RTEST(rb_funcall(self, rb_intern(">="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_integer_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return self == argv[0] ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return rb_big_eq(self, argv[0]);
    return RTEST(rb_funcall(self, rb_intern("=="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_integer_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self) && FIXNUM_P(argv[0])))
        return self != argv[0] ? Qtrue : Qfalse;
    if (RB_TYPE_P(self, T_BIGNUM))
        return RTEST(rb_big_eq(self, argv[0])) ? Qfalse : Qtrue;
    return RTEST(rb_funcall(self, rb_intern("=="), 1, argv[0])) ? Qfalse : Qtrue;
}

static VALUE ab_integer_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) {
        char buf[32]; snprintf(buf, sizeof(buf), "%ld", FIX2LONG(self));
        return abruby_str_new_cstr(buf);
    }
    return abruby_str_new(rb_big2str(self, 10));
}
static VALUE ab_integer_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return ab_integer_inspect(c, self, 0, NULL);
}
static VALUE ab_integer_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) return FIX2LONG(self) == 0 ? Qtrue : Qfalse;
    return Qfalse;
}
static VALUE ab_integer_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) return rb_float_new((double)FIX2LONG(self));
    return rb_funcall(self, rb_intern("to_f"), 0);
}
static VALUE ab_integer_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (LIKELY(FIXNUM_P(self))) { long v = FIX2LONG(self); return LONG2NUM(v < 0 ? -v : v); }
    return rb_funcall(self, rb_intern("abs"), 0);
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
    abruby_class_add_cfunc(ab_integer_class, "to_f",   ab_integer_to_f,    0);
    abruby_class_add_cfunc(ab_integer_class, "zero?",  ab_integer_zero_p,  0);
    abruby_class_add_cfunc(ab_integer_class, "abs",    ab_integer_abs,     0);
}
