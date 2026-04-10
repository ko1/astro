#include "builtin.h"

static RESULT ab_true_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "true")); }
static RESULT ab_true_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "true")); }

void
Init_abruby_true(void)
{
    abruby_class_add_cfunc(ab_tmpl_true_class, rb_intern("inspect"), ab_true_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_true_class, rb_intern("to_s"),    ab_true_to_s,    0);
}
