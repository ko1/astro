#include "builtin.h"

static RESULT ab_false_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "false")); }
static RESULT ab_false_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "false")); }

void
Init_abruby_false(void)
{
    abruby_class_add_cfunc(ab_tmpl_false_class, rb_intern("inspect"), ab_false_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_false_class, rb_intern("to_s"),    ab_false_to_s,    0);
}
