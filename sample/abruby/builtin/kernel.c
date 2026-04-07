#include "builtin.h"

static RESULT ab_kernel_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE str = ab_inspect_rstr(c, argv[0]);
    fwrite(RSTRING_PTR(str), 1, RSTRING_LEN(str), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return RESULT_OK(argv[0]);
}

static RESULT ab_kernel_raise(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE msg = (argc >= 1) ? argv[0] : abruby_str_new_cstr("");
    return (RESULT){msg, RESULT_RAISE};
}

// Rational(num, den) — create Rational
static RESULT ab_kernel_Rational(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE num = AB_NUM_UNWRAP(argv[0]);
    VALUE den = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(1);
    return RESULT_OK(abruby_rational_new(rb_rational_new(num, den)));
}

// Complex(real, imag) — create Complex
static RESULT ab_kernel_Complex(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE real = AB_NUM_UNWRAP(argv[0]);
    VALUE imag = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(0);
    return RESULT_OK(abruby_complex_new(rb_complex_new(real, imag)));
}

void
Init_abruby_kernel(void)
{
    abruby_class_add_cfunc(ab_kernel_module, "p",        ab_kernel_p,        1);
    abruby_class_add_cfunc(ab_kernel_module, "raise",    ab_kernel_raise,    1);
    abruby_class_add_cfunc(ab_kernel_module, "Rational", ab_kernel_Rational, 2);
    abruby_class_add_cfunc(ab_kernel_module, "Complex",  ab_kernel_Complex,  2);
}
