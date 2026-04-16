#include "builtin.h"

static RESULT ab_symbol_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const char *name = rb_id2name(SYM2ID(ab_sym_unwrap(self)));
    VALUE result = rb_str_new_cstr(":");
    rb_str_cat_cstr(result, name);
    return RESULT_OK(abruby_str_new(c, result));
}

static RESULT ab_symbol_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(c, rb_id2name(SYM2ID(ab_sym_unwrap(self)))));
}

static RESULT ab_symbol_to_sym(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self);
}

static RESULT ab_symbol_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE self_sym = ab_sym_unwrap(self);
    VALUE other = argv[0];
    if (!ab_obj_type_p(other, ABRUBY_OBJ_SYMBOL)) return RESULT_OK(Qfalse);
    return RESULT_OK(self_sym == ab_sym_unwrap(other) ? Qtrue : Qfalse);
}

static RESULT ab_symbol_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE self_sym = ab_sym_unwrap(self);
    VALUE other = argv[0];
    if (!ab_obj_type_p(other, ABRUBY_OBJ_SYMBOL)) return RESULT_OK(Qtrue);
    return RESULT_OK(self_sym != ab_sym_unwrap(other) ? Qtrue : Qfalse);
}

void
Init_abruby_symbol(void)
{
    abruby_class_add_cfunc(ab_tmpl_symbol_class, rb_intern("inspect"), ab_symbol_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_symbol_class, rb_intern("to_s"),    ab_symbol_to_s,    0);
    abruby_class_add_cfunc(ab_tmpl_symbol_class, rb_intern("to_sym"),  ab_symbol_to_sym,  0);
    abruby_class_add_cfunc(ab_tmpl_symbol_class, rb_intern("=="),      ab_symbol_eq,      1);
    abruby_class_add_cfunc(ab_tmpl_symbol_class, rb_intern("!="),      ab_symbol_neq,     1);
}
