#include "builtin.h"

// All Float ops use rb_funcall for correctness (handles int/float coercion).
// RFLOAT_VALUE / rb_float_new for Flonum-aware fast paths where possible.

static VALUE ab_float_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("+"), 1, argv[0]);
}
static VALUE ab_float_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("-"), 1, argv[0]);
}
static VALUE ab_float_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("*"), 1, argv[0]);
}
static VALUE ab_float_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("/"), 1, argv[0]);
}
static VALUE ab_float_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("%"), 1, argv[0]);
}
static VALUE ab_float_pow(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("**"), 1, argv[0]);
}
static VALUE ab_float_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_float_new(-RFLOAT_VALUE(self));
}

static VALUE ab_float_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern("<"), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_float_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern("<="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_float_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern(">"), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_float_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern(">="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_float_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern("=="), 1, argv[0])) ? Qtrue : Qfalse;
}
static VALUE ab_float_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RTEST(rb_funcall(self, rb_intern("=="), 1, argv[0])) ? Qfalse : Qtrue;
}

static VALUE ab_float_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return abruby_str_new(rb_funcall(self, rb_intern("to_s"), 0));
}
static VALUE ab_float_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return ab_float_inspect(c, self, 0, NULL);
}
static VALUE ab_float_to_i(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("to_i"), 0);
}
static VALUE ab_float_to_f(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return self;
}
static VALUE ab_float_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    double v = RFLOAT_VALUE(self);
    return rb_float_new(v < 0 ? -v : v);
}
static VALUE ab_float_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RFLOAT_VALUE(self) == 0.0 ? Qtrue : Qfalse;
}
static VALUE ab_float_floor(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("floor"), 0);
}
static VALUE ab_float_ceil(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("ceil"), 0);
}
static VALUE ab_float_round(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return rb_funcall(self, rb_intern("round"), 0);
}

void
Init_abruby_float(void)
{
    // constants
    abruby_class_set_const(ab_float_class, "INFINITY", rb_float_new(HUGE_VAL));
    abruby_class_set_const(ab_float_class, "NAN", rb_float_new(nan("")));


    abruby_class_add_cfunc(ab_float_class, "inspect", ab_float_inspect, 0);
    abruby_class_add_cfunc(ab_float_class, "to_s",   ab_float_to_s,    0);
    abruby_class_add_cfunc(ab_float_class, "to_i",   ab_float_to_i,    0);
    abruby_class_add_cfunc(ab_float_class, "to_f",   ab_float_to_f,    0);
    abruby_class_add_cfunc(ab_float_class, "+",      ab_float_add,     1);
    abruby_class_add_cfunc(ab_float_class, "-",      ab_float_sub,     1);
    abruby_class_add_cfunc(ab_float_class, "*",      ab_float_mul,     1);
    abruby_class_add_cfunc(ab_float_class, "/",      ab_float_div,     1);
    abruby_class_add_cfunc(ab_float_class, "%",      ab_float_mod,     1);
    abruby_class_add_cfunc(ab_float_class, "**",     ab_float_pow,     1);
    abruby_class_add_cfunc(ab_float_class, "-@",     ab_float_neg,     0);
    abruby_class_add_cfunc(ab_float_class, "<",      ab_float_lt,      1);
    abruby_class_add_cfunc(ab_float_class, "<=",     ab_float_le,      1);
    abruby_class_add_cfunc(ab_float_class, ">",      ab_float_gt,      1);
    abruby_class_add_cfunc(ab_float_class, ">=",     ab_float_ge,      1);
    abruby_class_add_cfunc(ab_float_class, "==",     ab_float_eq,      1);
    abruby_class_add_cfunc(ab_float_class, "!=",     ab_float_neq,     1);
    abruby_class_add_cfunc(ab_float_class, "abs",    ab_float_abs,     0);
    abruby_class_add_cfunc(ab_float_class, "zero?",  ab_float_zero_p,  0);
    abruby_class_add_cfunc(ab_float_class, "floor",  ab_float_floor,   0);
    abruby_class_add_cfunc(ab_float_class, "ceil",   ab_float_ceil,    0);
    abruby_class_add_cfunc(ab_float_class, "round",  ab_float_round,   0);
}
