#include "builtin.h"

// All Float ops: unwrap self/args to CRuby values, operate, wrap result.
// RFLOAT_VALUE works on CRuby Float (Flonum or heap Float).

#define SELF_F AB_FLOAT_UNWRAP(self)
#define ARG_F AB_NUM_UNWRAP(argv[0])

static RESULT ab_float_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("+"), 1, ARG_F)));
}
static RESULT ab_float_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("-"), 1, ARG_F)));
}
static RESULT ab_float_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("*"), 1, ARG_F)));
}
static RESULT ab_float_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("/"), 1, ARG_F)));
}
static RESULT ab_float_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("%"), 1, ARG_F)));
}
static RESULT ab_float_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("**"), 1, ARG_F)));
}
static RESULT ab_float_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_float_new_wrap(c, rb_float_new(-RFLOAT_VALUE(SELF_F))));
}

static RESULT ab_float_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern("<"), 1, ARG_F)) ? Qtrue : Qfalse);
}
static RESULT ab_float_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern("<="), 1, ARG_F)) ? Qtrue : Qfalse);
}
static RESULT ab_float_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern(">"), 1, ARG_F)) ? Qtrue : Qfalse);
}
static RESULT ab_float_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern(">="), 1, ARG_F)) ? Qtrue : Qfalse);
}
static RESULT ab_float_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern("=="), 1, ARG_F)) ? Qtrue : Qfalse);
}
static RESULT ab_float_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(rb_funcall(SELF_F, rb_intern("=="), 1, ARG_F)) ? Qfalse : Qtrue);
}

static RESULT ab_float_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new(c, rb_funcall(SELF_F, rb_intern("to_s"), 0)));
}
static RESULT ab_float_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(ab_float_inspect(c, self, 0, NULL).value);
}
static RESULT ab_float_to_i(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("to_i"), 0)));
}
static RESULT ab_float_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self);
}
static RESULT ab_float_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    double v = RFLOAT_VALUE(SELF_F);
    return RESULT_OK(abruby_float_new_wrap(c, rb_float_new(v < 0 ? -v : v)));
}
static RESULT ab_float_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RFLOAT_VALUE(SELF_F) == 0.0 ? Qtrue : Qfalse);
}
static RESULT ab_float_floor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("floor"), 0)));
}
static RESULT ab_float_ceil(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("ceil"), 0)));
}
static RESULT ab_float_round(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(AB_NUM_WRAP(c, rb_funcall(SELF_F, rb_intern("round"), 0)));
}

void
Init_abruby_float(void)
{
    // Float::INFINITY and Float::NAN moved to init_instance_classes (per-instance)

    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("inspect"), ab_float_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("to_s"),   ab_float_to_s,    0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("to_i"),   ab_float_to_i,    0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("to_f"),   ab_float_to_f,    0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("+"),      ab_float_add,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("-"),      ab_float_sub,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("*"),      ab_float_mul,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("/"),      ab_float_div,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("%"),      ab_float_mod,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("**"),     ab_float_pow,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("-@"),     ab_float_neg,     0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("<"),      ab_float_lt,      1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("<="),     ab_float_le,      1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern(">"),      ab_float_gt,      1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern(">="),     ab_float_ge,      1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("=="),     ab_float_eq,      1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("!="),     ab_float_neq,     1);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("abs"),    ab_float_abs,     0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("zero?"),  ab_float_zero_p,  0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("floor"),  ab_float_floor,   0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("ceil"),   ab_float_ceil,    0);
    abruby_class_add_cfunc(ab_tmpl_float_class, rb_intern("round"),  ab_float_round,   0);
}
