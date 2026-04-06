#include "builtin.h"

static VALUE ab_true_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return abruby_str_new_cstr("true"); }
static VALUE ab_true_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return abruby_str_new_cstr("true"); }

void
Init_abruby_true(void)
{
    abruby_class_add_cfunc(ab_true_class, "inspect", ab_true_inspect, 0);
    abruby_class_add_cfunc(ab_true_class, "to_s",    ab_true_to_s,    0);
}
