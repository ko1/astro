/* koruby_precise — math.c: the Math module (module functions on Math's singleton).
 * #included into korb_runtime.c's TU (after set.c, for korb_obj_singleton). */
#include <math.h>

static bool korb_math_d(VALUE v, double *out) { return korb_num_to_d(v, out); }

/* CRuby refines glibc's cbrt with one Newton step for correct rounding. */
static double korb_cbrt(double f) {
    double r = cbrt(f);
#if defined __GLIBC__
    if (isfinite(r)) r = (2.0 * r + (f / r / r)) / 3.0;
#endif
    return r;
}

#define KORB_MATH1(nm, fn)                                                              \
    static RESULT korb_m_math_##nm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        (void)self; double x;                                                           \
        if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x)))                          \
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Float", korb_type_name(VALUE_SLICE_GET(a, 0))); \
        return korb_float_new(c, slots, fn(x));                                         \
    }
#define KORB_MATH2(nm, fn)                                                              \
    static RESULT korb_m_math_##nm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        (void)self; double x, y;                                                        \
        if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x) || !korb_math_d(VALUE_SLICE_GET(a, 1), &y))) \
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float");     \
        return korb_float_new(c, slots, fn(x, y));                                      \
    }

KORB_MATH1(sqrt, sqrt)   KORB_MATH1(cbrt, korb_cbrt)
KORB_MATH1(sin, sin)     KORB_MATH1(cos, cos)     KORB_MATH1(tan, tan)
KORB_MATH1(asin, asin)   KORB_MATH1(acos, acos)   KORB_MATH1(atan, atan)
KORB_MATH1(sinh, sinh)   KORB_MATH1(cosh, cosh)   KORB_MATH1(tanh, tanh)
KORB_MATH1(asinh, asinh) KORB_MATH1(acosh, acosh) KORB_MATH1(atanh, atanh)
KORB_MATH1(exp, exp)     KORB_MATH1(log2, log2)   KORB_MATH1(log10, log10)
KORB_MATH1(gamma, tgamma) KORB_MATH1(erf, erf)    KORB_MATH1(erfc, erfc)
KORB_MATH2(atan2, atan2) KORB_MATH2(hypot, hypot) KORB_MATH2(copysign, copysign)
KORB_MATH2(pow, pow)

/* Math.log(x) = ln; Math.log(x, base) = log_base(x). */
static RESULT korb_m_math_log(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x;
    if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float");
    if (VALUE_SLICE_LEN(a) >= 2) {
        double b;
        if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 1), &b)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float");
        return korb_float_new(c, slots, log(x) / log(b));
    }
    return korb_float_new(c, slots, log(x));
}
/* Math.ldexp(frac, exp) = frac * 2**exp. */
static RESULT korb_m_math_ldexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x; intptr_t n;
    if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x) || !korb_to_index(VALUE_SLICE_GET(a, 1), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float/Integer");
    return korb_float_new(c, slots, ldexp(x, (int)n));
}
/* Math.frexp(x) → [fraction, exponent]. */
static RESULT korb_m_math_frexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x; int e = 0;
    if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float");
    double frac = frexp(x, &e);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, frac));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, arr, LONG2FIX(e)));
    return RESULT_OK(VALUE_REF_GET(arr));
}
/* Math.lgamma(x) → [log|gamma(x)|, sign]. */
static RESULT korb_m_math_lgamma(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x;
    if (UNLIKELY(!korb_math_d(VALUE_SLICE_GET(a, 0), &x)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float");
    double v = lgamma(x);
    int sign = signbit(tgamma(x)) ? -1 : 1;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_float_new(c, slots + 1, v));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, arr, LONG2FIX(sign)));
    return RESULT_OK(VALUE_REF_GET(arr));
}

/* register a Math module function as a CFUNC on Math's singleton class. */
static void korb_def_modfunc(CTX *c, VALUE *slots, VALUE modobj, const char *name, korb_method_fn fn, int32_t arity) {
    VALUE sing = korb_obj_singleton(c, slots, modobj).value;   /* created once, reused after */
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(sing), korb_intern(c->vm, name, strlen(name)));
    m->kind = KORB_METHOD_CFUNC; m->owner = sing; m->params_cnt = arity; m->rfn = fn; m->rbfn = NULL; m->uses_block = 0;
}

void korb_init_math(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "Math", 4), KORB_NIL)).value;
    VAL2CLASS(slots[0])->is_module = 1;
    korb_const_define(c, korb_intern(vm, "Math", 4), slots[0]);
    /* Math::PI / Math::E (flat const table). */
    korb_const_define(c, korb_intern(vm, "PI", 2), korb_float_new(c, slots + 1, M_PI).value);
    korb_const_define(c, korb_intern(vm, "E", 1),  korb_float_new(c, slots + 1, M_E).value);
    /* slots[0] holds Math; re-read it each call — singleton alloc may move it. */
#define MF(name, fn, ar) korb_def_modfunc(c, slots + 1, slots[0], name, korb_m_math_##fn, ar)
    MF("sqrt", sqrt, 1); MF("cbrt", cbrt, 1);
    MF("sin", sin, 1); MF("cos", cos, 1); MF("tan", tan, 1);
    MF("asin", asin, 1); MF("acos", acos, 1); MF("atan", atan, 1);
    MF("sinh", sinh, 1); MF("cosh", cosh, 1); MF("tanh", tanh, 1);
    MF("asinh", asinh, 1); MF("acosh", acosh, 1); MF("atanh", atanh, 1);
    MF("exp", exp, 1); MF("log2", log2, 1); MF("log10", log10, 1);
    MF("gamma", gamma, 1); MF("erf", erf, 1); MF("erfc", erfc, 1);
    MF("atan2", atan2, 2); MF("hypot", hypot, 2); MF("copysign", copysign, 2); MF("pow", pow, 2);
    MF("log", log, -1); MF("ldexp", ldexp, 2); MF("frexp", frexp, 1); MF("lgamma", lgamma, 1);
#undef MF
}
