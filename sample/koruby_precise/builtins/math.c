/* Math module — sp/RESULT ABI.
 *
 * Convention: argv[i] = sp[-argc + i].  For fixed-arity MATH1 (argc=1),
 * arg0 is at sp[-1].  For MATH2 (argc=2), arg0 at sp[-2], arg1 at sp[-1]. */

/* ---------- Math module ---------- */
#include <math.h>
static double num_d(VALUE v) {
    if (FIXNUM_P(v)) return (double)FIX2LONG(v);
    return korb_num2dbl(v);
}
#define MATH1(name, fn) \
    static RESULT math_##name(CTX *c, int argc, VALUE *sp) { \
        return RESULT_OK(korb_float_new(fn(num_d(sp[-1])))); \
    }
#define MATH2(name, fn) \
    static RESULT math_##name(CTX *c, int argc, VALUE *sp) { \
        return RESULT_OK(korb_float_new(fn(num_d(sp[-2]), num_d(sp[-1])))); \
    }
MATH1(sqrt, sqrt) MATH1(sin, sin) MATH1(cos, cos) MATH1(tan, tan)
MATH1(asin, asin) MATH1(acos, acos) MATH1(atan, atan)
MATH1(sinh, sinh) MATH1(cosh, cosh) MATH1(tanh, tanh)
MATH1(exp, exp) MATH1(log2, log2) MATH1(log10, log10) MATH1(cbrt, cbrt)
MATH2(atan2, atan2) MATH2(hypot, hypot)
static RESULT math_log(CTX *c, int argc, VALUE *sp) {
    if (argc == 2) return RESULT_OK(korb_float_new(log(num_d(sp[-2])) / log(num_d(sp[-1]))));
    return RESULT_OK(korb_float_new(log(num_d(sp[-1]))));
}
static RESULT math_pow(CTX *c, int argc, VALUE *sp) {
    return RESULT_OK(korb_float_new(pow(num_d(sp[-2]), num_d(sp[-1]))));
}

