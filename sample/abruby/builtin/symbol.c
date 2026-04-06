#include "builtin.h"

static RESULT ab_symbol_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const char *name = rb_id2name(SYM2ID(self));
    VALUE result = rb_str_new_cstr(":");
    rb_str_cat_cstr(result, name);
    return RESULT_OK(abruby_str_new(result));
}

static RESULT ab_symbol_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(rb_id2name(SYM2ID(self))));
}

static RESULT ab_symbol_to_sym(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self);
}

static RESULT ab_symbol_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
}

static RESULT ab_symbol_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self != argv[0] ? Qtrue : Qfalse);
}

void
Init_abruby_symbol(void)
{
    abruby_class_add_cfunc(ab_symbol_class, "inspect", ab_symbol_inspect, 0);
    abruby_class_add_cfunc(ab_symbol_class, "to_s",    ab_symbol_to_s,    0);
    abruby_class_add_cfunc(ab_symbol_class, "to_sym",  ab_symbol_to_sym,  0);
    abruby_class_add_cfunc(ab_symbol_class, "==",      ab_symbol_eq,      1);
    abruby_class_add_cfunc(ab_symbol_class, "!=",      ab_symbol_neq,     1);
}
