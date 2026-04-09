#include "builtin.h"

#define RRAT(v) (((struct abruby_rational *)RTYPEDDATA_GET_DATA(v))->rb_rational)

static RESULT ab_rational_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new(rb_funcall(RRAT(self), rb_intern("inspect"), 0)));
}
static RESULT ab_rational_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new(rb_funcall(RRAT(self), rb_intern("to_s"), 0)));
}
static RESULT ab_rational_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_float_new_wrap(rb_funcall(RRAT(self), rb_intern("to_f"), 0)));
}
static RESULT ab_rational_to_i(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("to_i"), 0)));
}
static RESULT ab_rational_to_r(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self);
}

static RESULT ab_rational_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("+"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_rational_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("-"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_rational_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("*"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_rational_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("/"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_rational_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("**"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_rational_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("-@"), 0)));
}

static RESULT ab_rational_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RRAT(self), rb_intern("=="), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}
static RESULT ab_rational_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RRAT(self), rb_intern("<"), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}
static RESULT ab_rational_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RRAT(self), rb_intern("<="), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}
static RESULT ab_rational_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RRAT(self), rb_intern(">"), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}
static RESULT ab_rational_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RRAT(self), rb_intern(">="), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}
static RESULT ab_rational_cmp(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(rb_funcall(RRAT(self), rb_intern("<=>"), 1, AB_NUM_UNWRAP(argv[0])));
}

static RESULT ab_rational_numerator(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("numerator"), 0)));
}
static RESULT ab_rational_denominator(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(rb_funcall(RRAT(self), rb_intern("denominator"), 0)));
}

void
Init_abruby_rational(void)
{
    abruby_class_add_cfunc(ab_rational_class, rb_intern("inspect"),     ab_rational_inspect,     0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("to_s"),        ab_rational_to_s,        0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("to_f"),        ab_rational_to_f,        0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("to_i"),        ab_rational_to_i,        0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("to_r"),        ab_rational_to_r,        0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("+"),           ab_rational_add,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("-"),           ab_rational_sub,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("*"),           ab_rational_mul,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("/"),           ab_rational_div,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("**"),          ab_rational_pow,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("-@"),          ab_rational_neg,         0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("=="),          ab_rational_eq,          1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("<"),           ab_rational_lt,          1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("<="),          ab_rational_le,          1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern(">"),           ab_rational_gt,          1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern(">="),          ab_rational_ge,          1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("<=>"),         ab_rational_cmp,         1);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("numerator"),   ab_rational_numerator,   0);
    abruby_class_add_cfunc(ab_rational_class, rb_intern("denominator"), ab_rational_denominator, 0);
}
