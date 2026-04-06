#include "builtin.h"

static RESULT ab_kernel_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE str = ab_inspect_rstr(c, argv[0]);
    fwrite(RSTRING_PTR(str), 1, RSTRING_LEN(str), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return RESULT_OK(argv[0]);
}

static RESULT ab_kernel_raise(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE msg = (argc >= 1) ? argv[0] : abruby_str_new_cstr("");
    return (RESULT){msg, RESULT_RAISE};
}

void
Init_abruby_kernel(void)
{
    abruby_class_add_cfunc(ab_kernel_module, "p",     ab_kernel_p,     1);
    abruby_class_add_cfunc(ab_kernel_module, "raise", ab_kernel_raise, 1);
}
