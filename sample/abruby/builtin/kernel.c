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
    // Start from caller's frame (skip the "raise" frame itself)
    struct abruby_frame *caller = c->current_frame ? c->current_frame->prev : NULL;
    VALUE exc = abruby_exception_new(c, caller, msg);
    return (RESULT){exc, RESULT_RAISE};
}

// Rational(num, den) — create Rational
static RESULT ab_kernel_Rational(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE num = AB_NUM_UNWRAP(argv[0]);
    VALUE den = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(1);
    return RESULT_OK(abruby_rational_new(rb_rational_new(num, den)));
}

// Complex(real, imag) — create Complex
static RESULT ab_kernel_Complex(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE real = AB_NUM_UNWRAP(argv[0]);
    VALUE imag = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(0);
    return RESULT_OK(abruby_complex_new(rb_complex_new(real, imag)));
}

// require(path) — load a file
static RESULT ab_kernel_require(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE path = RSTR(argv[0]);  // abruby string -> CRuby string
    // Append .rb if no extension
    if (!strstr(RSTRING_PTR(path), ".")) {
        path = rb_str_cat_cstr(rb_str_dup(path), ".rb");
    }
    return abruby_require_file(c, path);
}

// require_relative(path) — load a file relative to the current file
static RESULT ab_kernel_require_relative(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE path = RSTR(argv[0]);
    // Append .rb if no extension
    if (!strstr(RSTRING_PTR(path), ".")) {
        path = rb_str_cat_cstr(rb_str_dup(path), ".rb");
    }
    VALUE cur = abruby_current_file(c);
    if (NIL_P(cur)) {
        return abruby_require_file(c, path);
    }
    VALUE dir = rb_funcall(rb_cFile, rb_intern("dirname"), 1, cur);
    VALUE full = rb_funcall(rb_cFile, rb_intern("join"), 2, dir, path);
    return abruby_require_file(c, full);
}

// eval(code) — parse and evaluate a string
static RESULT ab_kernel_eval(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE code = RSTR(argv[0]);  // abruby string -> CRuby string
    return abruby_eval_string(c, code);
}

void
Init_abruby_kernel(void)
{
    abruby_class_add_cfunc(ab_kernel_module, "p",        ab_kernel_p,        1);
    abruby_class_add_cfunc(ab_kernel_module, "raise",    ab_kernel_raise,    1);
    abruby_class_add_cfunc(ab_kernel_module, "Rational", ab_kernel_Rational, 2);
    abruby_class_add_cfunc(ab_kernel_module, "Complex",          ab_kernel_Complex,          2);
    abruby_class_add_cfunc(ab_kernel_module, "require",          ab_kernel_require,          1);
    abruby_class_add_cfunc(ab_kernel_module, "require_relative", ab_kernel_require_relative, 1);
    abruby_class_add_cfunc(ab_kernel_module, "eval",             ab_kernel_eval,             1);
}
