/* koruby_precise — math.c: the Math module (module functions on Math's singleton).
 * #included into korb_runtime.c's TU (after set.c, for korb_obj_singleton). */
#include <math.h>
#include <float.h>   /* DBL_MAX / DBL_MIN / DBL_EPSILON for Float:: constants */

static bool korb_math_d(VALUE v, double *out) { return korb_num_to_d(v, out); }

/* Coerce v to a double for a Math function: fast numeric path, else Float(v) via
 * #to_f (CRuby rb_to_float semantics — accepts any object that defines #to_f).
 * Returns a RAISE result (TypeError, or whatever #to_f raised) on failure. */
static RESULT korb_math_coerce_d(CTX *c, VALUE *slots, VALUE v, double *out) {
    if (LIKELY(korb_num_to_d(v, out))) return RESULT_OK(KORB_NIL);
    slots[0] = v;                                   /* root for the dispatch + error message */
    /* CRuby rb_to_float: only a Numeric (subclass) is coerced via #to_f — a plain
     * object that merely defines #to_f is still rejected with TypeError. */
    if (KORB_OBJECT_P(v)) {
        const uint32_t to_f = korb_intern(c->vm, "to_f", 4);
        const VALUE numeric = korb_const_get(c->vm, korb_intern(c->vm, "Numeric", 7));
        const VALUE vcls = korb_class_obj_of(c, v);
        if (KORB_CLASS_P(numeric) && KORB_CLASS_P(vcls) && korb_class_le(vcls, numeric)
            && korb_responds_to(c, v, to_f)) {
            RESULT fr = korb_send_impl(c, slots + 1, to_f, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
            if (LIKELY(korb_num_to_d(fr.value, out))) return RESULT_OK(KORB_NIL);
        }
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into Float", korb_type_name(slots[0]));
}

/* CRuby refines glibc's cbrt with one Newton step for correct rounding. */
static double korb_cbrt(double f) {
    double r = cbrt(f);
#if defined __GLIBC__
    if (isfinite(r) && r != 0.0) r = (2.0 * r + (f / r / r)) / 3.0;   /* Newton step (skip r==0: avoids 0/0) */
#endif
    return r;
}

#define KORB_MATH1(nm, fn)                                                              \
    static RESULT korb_m_math_##nm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        (void)self; double x;                                                           \
        { RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &x); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; } \
        const double r_ = fn(x);                                                        \
        if (UNLIKELY(isnan(r_) && !isnan(x) && !isinf(x)))   /* finite input → NaN = out of domain */ \
            return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "Numerical argument is out of domain - %s", #nm); \
        return korb_float_new(c, slots, r_);                                            \
    }
#define KORB_MATH2(nm, fn)                                                              \
    static RESULT korb_m_math_##nm(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
        (void)self; double x, y;                                                        \
        { RESULT _c1 = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &x); if (UNLIKELY(_c1.state != KORB_NORMAL)) return _c1; } \
        { RESULT _c2 = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 1), &y); if (UNLIKELY(_c2.state != KORB_NORMAL)) return _c2; } \
        return korb_float_new(c, slots, fn(x, y));                                      \
    }

KORB_MATH1(sqrt, sqrt)   KORB_MATH1(cbrt, korb_cbrt)
KORB_MATH1(sin, sin)     KORB_MATH1(cos, cos)     KORB_MATH1(tan, tan)
KORB_MATH1(asin, asin)   KORB_MATH1(acos, acos)   KORB_MATH1(atan, atan)
KORB_MATH1(sinh, sinh)   KORB_MATH1(cosh, cosh)   KORB_MATH1(tanh, tanh)
KORB_MATH1(asinh, asinh) KORB_MATH1(acosh, acosh) KORB_MATH1(atanh, atanh)
KORB_MATH1(exp, exp)
KORB_MATH1(gamma, tgamma) KORB_MATH1(erf, erf)    KORB_MATH1(erfc, erfc)
KORB_MATH1(expm1, expm1)  KORB_MATH1(log1p, log1p)

/* Decompose v into mantissa `d` and binary exponent `e` so v == d * 2**e, with
 * |d| in [0.5, 1) for a Bignum (via GMP, no double overflow) or the plain double
 * (e = 0) otherwise.  This lets Math.log{,2,10} stay finite for Bignums that
 * exceed the double range (e.g. Math.log2(2**10000) == 10000.0, not Infinity). */
static bool korb_math_frexp_val(VALUE v, double *d, long *e) {
    if (KORB_BIGNUM_P(v)) { *d = korb_mp_get_d_2exp(e, VAL2BIG(v)->z); return true; }   /* v = d·2^e */
    double x;
    if (!korb_num_to_d(v, &x)) return false;
    *d = x; *e = 0;
    return true;
}
static RESULT korb_m_math_log2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double d; long e;
    if (UNLIKELY(!korb_math_frexp_val(VALUE_SLICE_GET(a, 0), &d, &e))) {
        RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &d); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; e = 0;
    }
    const double r2 = log2(d) + (double)e;
    if (UNLIKELY(isnan(r2) && !isnan(d))) return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "Numerical argument is out of domain - log2");
    return korb_float_new(c, slots, r2);
}
static RESULT korb_m_math_log10(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double d; long e;
    if (UNLIKELY(!korb_math_frexp_val(VALUE_SLICE_GET(a, 0), &d, &e))) {
        RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &d); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; e = 0;
    }
    const double r10 = log10(d) + (double)e * 0.301029995663981195213738894724; /* + e·log10(2) */
    if (UNLIKELY(isnan(r10) && !isnan(d))) return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "Numerical argument is out of domain - log10");
    return korb_float_new(c, slots, r10);
}
KORB_MATH2(atan2, atan2) KORB_MATH2(hypot, hypot) KORB_MATH2(copysign, copysign)

/* Math.log(x) = ln; Math.log(x, base) = log_base(x).  Bignum-aware via frexp
 * decomposition so ln stays finite past the double range. */
static RESULT korb_m_math_log(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double d; long e;
    if (UNLIKELY(!korb_math_frexp_val(VALUE_SLICE_GET(a, 0), &d, &e))) {
        RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &d); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; e = 0;
    }
    const double lnx = log(d) + (double)e * M_LN2;   /* ln(d·2^e) */
    if (UNLIKELY(isnan(lnx) && !isnan(d))) return korb_raise(c, slots, KORB_E_MATH_DOMAIN, 0, "Numerical argument is out of domain - log");
    if (VALUE_SLICE_LEN(a) >= 2) {
        double db; long eb;
        if (UNLIKELY(!korb_math_frexp_val(VALUE_SLICE_GET(a, 1), &db, &eb))) {
            RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 1), &db); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; eb = 0;
        }
        return korb_float_new(c, slots, lnx / (log(db) + (double)eb * M_LN2));
    }
    return korb_float_new(c, slots, lnx);
}
/* Math.ldexp(frac, exp) = frac * 2**exp. */
static RESULT korb_m_math_ldexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x; korb_sword_t n;
    { RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &x); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; }
    if (UNLIKELY(!korb_to_index(VALUE_SLICE_GET(a, 1), &n)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert into Float/Integer");
    return korb_float_new(c, slots, ldexp(x, (int)n));
}
/* Math.frexp(x) → [fraction, exponent]. */
static RESULT korb_m_math_frexp(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; double x; int e = 0;
    { RESULT _cr = korb_math_coerce_d(c, slots, VALUE_SLICE_GET(a, 0), &x); if (UNLIKELY(_cr.state != KORB_NORMAL)) return _cr; }
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

/* Register a Math module function (module_function semantics): a CFUNC both on
 * Math's singleton class (Math.sqrt) AND as an instance method on the module
 * itself, so `include Math` makes it callable (sqrt(x) / obj.send(:sqrt, x)). */
static void korb_def_modfunc(CTX *c, VALUE *slots, VALUE modobj, const char *name, korb_method_fn fn, int32_t arity) {
    const uint32_t mid = korb_intern(c->vm, name, strlen(name));
    slots[0] = modobj;                                         /* root across the singleton/table allocs */
    VALUE sing = korb_obj_singleton(c, slots + 1, slots[0]).value;   /* created once, reused after */
    struct korb_method *m = korb_class_method_slot(VAL2CLASS(sing), mid);
    m->kind = KORB_METHOD_CFUNC; m->owner = sing; m->params_cnt = arity; m->rfn = fn; m->rbfn = NULL; m->uses_block = 0;
    struct korb_method *im = korb_class_method_slot(VAL2CLASS(slots[0]), mid);   /* slots[0] = (re-read) Math module */
    im->kind = KORB_METHOD_CFUNC; im->owner = slots[0]; im->params_cnt = arity; im->rfn = fn; im->rbfn = NULL; im->uses_block = 0;
}

void korb_init_math(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "Math", 4), KORB_NIL)).value;
    VAL2CLASS(slots[0])->is_module = 1;
    korb_const_define(c, korb_intern(vm, "Math", 4), slots[0]);
    /* Owned so `Float.constants` / `Math.constants` report them; a bare read
     * still finds them, since the by-name lookup ignores the owner. */
    const VALUE mathm = slots[0];
    const VALUE fltc = korb_builtin_class_obj(vm, KORB_C_FLOAT);
    const VALUE cpxc = korb_builtin_class_obj(vm, KORB_C_COMPLEX);
#define MCONST(o, nm, n, v) korb_const_define_owned(c, korb_intern(vm, nm, n), (v), (o))
    MCONST(mathm, "PI", 2, korb_float_new(c, slots + 1, M_PI).value);
    MCONST(mathm, "E",  1, korb_float_new(c, slots + 1, M_E).value);
    MCONST(fltc, "INFINITY", 8, korb_float_new(c, slots + 1, INFINITY).value);
    MCONST(fltc, "NAN", 3,      korb_float_new(c, slots + 1, NAN).value);
    MCONST(fltc, "MAX", 3,      korb_float_new(c, slots + 1, DBL_MAX).value);
    MCONST(fltc, "MIN", 3,      korb_float_new(c, slots + 1, DBL_MIN).value);
    MCONST(fltc, "EPSILON", 7,  korb_float_new(c, slots + 1, DBL_EPSILON).value);
    MCONST(cpxc, "I", 1,        korb_cpx_new(c, slots + 1, LONG2FIX(0), LONG2FIX(1)).value);
    /* integer-valued Float:: constants (IEEE-754 double properties). */
    MCONST(fltc, "DIG", 3,         LONG2FIX(DBL_DIG));
    MCONST(fltc, "MANT_DIG", 8,    LONG2FIX(DBL_MANT_DIG));
    MCONST(fltc, "MIN_EXP", 7,     LONG2FIX(DBL_MIN_EXP));
    MCONST(fltc, "MAX_EXP", 7,     LONG2FIX(DBL_MAX_EXP));
    MCONST(fltc, "MIN_10_EXP", 10, LONG2FIX(DBL_MIN_10_EXP));
    MCONST(fltc, "MAX_10_EXP", 10, LONG2FIX(DBL_MAX_10_EXP));
    MCONST(fltc, "RADIX", 5,       LONG2FIX(FLT_RADIX));
#undef MCONST
    {   /* Math::DomainError is created with the exception classes, before Math
         * exists; re-own the existing entry rather than adding a second one. */
        const uint32_t de = korb_intern(vm, "DomainError", 11);
        for (uint32_t i = 0; i < vm->const_cnt; i++)
            if (vm->const_names[i] == de && vm->const_owners[i] == KORB_NIL) {
                vm->const_owners[i] = mathm;
                const VALUE dec = vm->const_vals[i];                     /* also gives it the Math:: qualified name */
                if (KORB_CLASS_P(dec)) ARO_STORE(c, VAL2CLASS(dec), (VALUE *)(uintptr_t)&VAL2CLASS(dec)->enclosing, mathm);
                break;
            }
    }
    /* slots[0] holds Math; re-read it each call — singleton alloc may move it. */
#define MF(name, fn, ar) korb_def_modfunc(c, slots + 1, slots[0], name, korb_m_math_##fn, ar)
    MF("sqrt", sqrt, 1); MF("cbrt", cbrt, 1);
    MF("sin", sin, 1); MF("cos", cos, 1); MF("tan", tan, 1);
    MF("asin", asin, 1); MF("acos", acos, 1); MF("atan", atan, 1);
    MF("sinh", sinh, 1); MF("cosh", cosh, 1); MF("tanh", tanh, 1);
    MF("asinh", asinh, 1); MF("acosh", acosh, 1); MF("atanh", atanh, 1);
    MF("exp", exp, 1); MF("log2", log2, 1); MF("log10", log10, 1);
    MF("gamma", gamma, 1); MF("erf", erf, 1); MF("erfc", erfc, 1);
    MF("expm1", expm1, 1); MF("log1p", log1p, 1);
    MF("atan2", atan2, 2); MF("hypot", hypot, 2); MF("copysign", copysign, 2);   /* no Math.pow — CRuby has none */
    MF("log", log, -1); MF("ldexp", ldexp, 2); MF("frexp", frexp, 1); MF("lgamma", lgamma, 1);
#undef MF
}
