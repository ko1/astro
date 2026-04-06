#include "builtin.h"

static RESULT ab_nil_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr("nil")); }
static RESULT ab_nil_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr("")); }
static RESULT ab_nil_nil_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(Qtrue); }

void
Init_abruby_nil(void)
{
    abruby_class_add_cfunc(ab_nil_class, "inspect", ab_nil_inspect, 0);
    abruby_class_add_cfunc(ab_nil_class, "to_s",    ab_nil_to_s,    0);
    abruby_class_add_cfunc(ab_nil_class, "nil?",    ab_nil_nil_p,   0);
}
