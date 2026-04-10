#include "builtin.h"

#define RCMPLX(v) (((struct abruby_complex *)RTYPEDDATA_GET_DATA(v))->rb_complex)

static RESULT ab_complex_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new(c, rb_funcall(RCMPLX(self), rb_intern("inspect"), 0)));
}
static RESULT ab_complex_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new(c, rb_funcall(RCMPLX(self), rb_intern("to_s"), 0)));
}
static RESULT ab_complex_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_float_new_wrap(c, rb_funcall(RCMPLX(self), rb_intern("to_f"), 0)));
}
static RESULT ab_complex_to_c(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self);
}

static RESULT ab_complex_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("+"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_complex_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("-"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_complex_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("*"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_complex_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("/"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_complex_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("**"), 1, AB_NUM_UNWRAP(argv[0]))));
}
static RESULT ab_complex_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("-@"), 0)));
}

static RESULT ab_complex_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(RCMPLX(self), rb_intern("=="), 1, AB_NUM_UNWRAP(argv[0]))) ? Qtrue : Qfalse);
}

static RESULT ab_complex_real(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("real"), 0)));
}
static RESULT ab_complex_imaginary(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("imaginary"), 0)));
}
static RESULT ab_complex_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("abs"), 0)));
}
static RESULT ab_complex_conjugate(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_complex_new(c, rb_funcall(RCMPLX(self), rb_intern("conjugate"), 0)));
}
static RESULT ab_complex_rectangular(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE real = AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("real"), 0));
    VALUE imag = AB_NUM_WRAP(c, rb_funcall(RCMPLX(self), rb_intern("imaginary"), 0));
    VALUE ary = rb_ary_new_from_args(2, real, imag);
    return RESULT_OK(abruby_ary_new(c, ary));
}

void
Init_abruby_complex(void)
{
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("inspect"),     ab_complex_inspect,     0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("to_s"),        ab_complex_to_s,        0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("to_f"),        ab_complex_to_f,        0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("to_c"),        ab_complex_to_c,        0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("+"),           ab_complex_add,         1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("-"),           ab_complex_sub,         1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("*"),           ab_complex_mul,         1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("/"),           ab_complex_div,         1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("**"),          ab_complex_pow,         1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("-@"),          ab_complex_neg,         0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("=="),          ab_complex_eq,          1);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("real"),        ab_complex_real,        0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("imaginary"),   ab_complex_imaginary,   0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("abs"),         ab_complex_abs,         0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("conjugate"),   ab_complex_conjugate,   0);
    abruby_class_add_cfunc(ab_tmpl_complex_class, rb_intern("rectangular"),  ab_complex_rectangular, 0);
}
