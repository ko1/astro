/* Math module — moved from builtins.c (was builtins/math.c stub). */

/* ---------- Math module ---------- */
#include <math.h>
static double num_d(VALUE v) {
    if (FIXNUM_P(v)) return (double)FIX2LONG(v);
    return korb_num2dbl(v);
}
#define MATH1(name, fn) \
    static VALUE math_##name(CTX *c, VALUE self, int argc, VALUE *argv) { \
        return korb_float_new(fn(num_d(argv[0]))); \
    }
#define MATH2(name, fn) \
    static VALUE math_##name(CTX *c, VALUE self, int argc, VALUE *argv) { \
        return korb_float_new(fn(num_d(argv[0]), num_d(argv[1]))); \
    }
MATH1(sqrt, sqrt) MATH1(sin, sin) MATH1(cos, cos) MATH1(tan, tan)
MATH1(asin, asin) MATH1(acos, acos) MATH1(atan, atan)
MATH1(sinh, sinh) MATH1(cosh, cosh) MATH1(tanh, tanh)
MATH1(exp, exp) MATH1(log2, log2) MATH1(log10, log10) MATH1(cbrt, cbrt)
MATH2(atan2, atan2) MATH2(hypot, hypot)
static VALUE math_log(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 2) return korb_float_new(log(num_d(argv[0])) / log(num_d(argv[1])));
    return korb_float_new(log(num_d(argv[0])));
}
static VALUE math_pow(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(pow(num_d(argv[0]), num_d(argv[1])));
}

