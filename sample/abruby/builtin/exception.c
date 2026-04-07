#include "builtin.h"

// RuntimeError#message
static RESULT ab_exception_message(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    struct abruby_exception *exc = (struct abruby_exception *)RTYPEDDATA_GET_DATA(self);
    return RESULT_OK(exc->message);
}

// RuntimeError#backtrace — returns abruby Array of abruby Strings
static RESULT ab_exception_backtrace(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    struct abruby_exception *exc = (struct abruby_exception *)RTYPEDDATA_GET_DATA(self);
    VALUE bt = exc->backtrace;
    if (NIL_P(bt)) return RESULT_OK(Qnil);
    // Convert Ruby Array of Ruby Strings to abruby Array of abruby Strings
    VALUE ary = rb_ary_new();
    long len = RARRAY_LEN(bt);
    for (long i = 0; i < len; i++) {
        rb_ary_push(ary, abruby_str_new(RARRAY_AREF(bt, i)));
    }
    return RESULT_OK(abruby_ary_new(ary));
}

// RuntimeError#to_s — returns message
static RESULT ab_exception_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    struct abruby_exception *exc = (struct abruby_exception *)RTYPEDDATA_GET_DATA(self);
    return RESULT_OK(exc->message);
}

// RuntimeError#inspect
static RESULT ab_exception_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    struct abruby_exception *exc = (struct abruby_exception *)RTYPEDDATA_GET_DATA(self);
    VALUE msg_rs = RSTR(exc->message);
    VALUE result = rb_sprintf("#<RuntimeError: %s>", RSTRING_PTR(msg_rs));
    return RESULT_OK(abruby_str_new(result));
}

void
Init_abruby_exception(void)
{
    abruby_class_add_cfunc(ab_runtime_error_class, "message",   ab_exception_message,   0);
    abruby_class_add_cfunc(ab_runtime_error_class, "backtrace", ab_exception_backtrace, 0);
    abruby_class_add_cfunc(ab_runtime_error_class, "to_s",      ab_exception_to_s,      0);
    abruby_class_add_cfunc(ab_runtime_error_class, "inspect",   ab_exception_inspect,   0);
}
