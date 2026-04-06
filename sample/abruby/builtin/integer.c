#include "builtin.h"

static VALUE ab_integer_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(FIX2LONG(self) + FIX2LONG(argv[0])); }
static VALUE ab_integer_sub(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(FIX2LONG(self) - FIX2LONG(argv[0])); }
static VALUE ab_integer_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(FIX2LONG(self) * FIX2LONG(argv[0])); }
static VALUE ab_integer_div(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(FIX2LONG(self) / FIX2LONG(argv[0])); }
static VALUE ab_integer_mod(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(FIX2LONG(self) % FIX2LONG(argv[0])); }
static VALUE ab_integer_neg(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(-FIX2LONG(self)); }
static VALUE ab_integer_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv)  { return (FIX2LONG(self) < FIX2LONG(argv[0])) ? Qtrue : Qfalse; }
static VALUE ab_integer_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv)  { return (FIX2LONG(self) <= FIX2LONG(argv[0])) ? Qtrue : Qfalse; }
static VALUE ab_integer_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv)  { return (FIX2LONG(self) > FIX2LONG(argv[0])) ? Qtrue : Qfalse; }
static VALUE ab_integer_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv)  { return (FIX2LONG(self) >= FIX2LONG(argv[0])) ? Qtrue : Qfalse; }
static VALUE ab_integer_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv)  { return (FIX2LONG(self) == FIX2LONG(argv[0])) ? Qtrue : Qfalse; }
static VALUE ab_integer_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return (FIX2LONG(self) != FIX2LONG(argv[0])) ? Qtrue : Qfalse; }

static VALUE ab_integer_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    char buf[32]; snprintf(buf, sizeof(buf), "%ld", FIX2LONG(self));
    return abruby_str_new_cstr(buf);
}
static VALUE ab_integer_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return ab_integer_inspect(c, self, 0, NULL); }
static VALUE ab_integer_zero_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return FIX2LONG(self) == 0 ? Qtrue : Qfalse; }
static VALUE ab_integer_abs(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { long v = FIX2LONG(self); return LONG2FIX(v < 0 ? -v : v); }

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
    abruby_class_add_cfunc(ab_integer_class, "-@",     ab_integer_neg,     0);
    abruby_class_add_cfunc(ab_integer_class, "<",      ab_integer_lt,      1);
    abruby_class_add_cfunc(ab_integer_class, "<=",     ab_integer_le,      1);
    abruby_class_add_cfunc(ab_integer_class, ">",      ab_integer_gt,      1);
    abruby_class_add_cfunc(ab_integer_class, ">=",     ab_integer_ge,      1);
    abruby_class_add_cfunc(ab_integer_class, "==",     ab_integer_eq,      1);
    abruby_class_add_cfunc(ab_integer_class, "!=",     ab_integer_neq,     1);
    abruby_class_add_cfunc(ab_integer_class, "zero?",  ab_integer_zero_p,  0);
    abruby_class_add_cfunc(ab_integer_class, "abs",    ab_integer_abs,     0);
}
