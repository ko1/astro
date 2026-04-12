#include "builtin.h"

static RESULT ab_nil_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "nil")); }
static RESULT ab_nil_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(abruby_str_new_cstr(c, "")); }
static RESULT ab_nil_nil_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(Qtrue); }
// nil[i] -> nil.  Strictly non-Ruby but our multi-assign decomposition
// (`a, b = some_method_returning_nil`) lowers to `tmp[0]`, `tmp[1]`,
// which would otherwise raise.  Real Ruby's parser handles this case
// specially; we tolerate it on the receiver side instead.
static RESULT ab_nil_index(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(Qnil);
}

void
Init_abruby_nil(void)
{
    abruby_class_add_cfunc(ab_tmpl_nil_class, rb_intern("inspect"), ab_nil_inspect, 0);
    abruby_class_add_cfunc(ab_tmpl_nil_class, rb_intern("to_s"),    ab_nil_to_s,    0);
    abruby_class_add_cfunc(ab_tmpl_nil_class, rb_intern("nil?"),    ab_nil_nil_p,   0);
    abruby_class_add_cfunc(ab_tmpl_nil_class, rb_intern("[]"),      ab_nil_index,   1);
}
