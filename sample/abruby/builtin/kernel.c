#include "builtin.h"

static RESULT ab_kernel_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE str = ab_inspect_rstr(c, argv[0]);
    fwrite(RSTRING_PTR(str), 1, RSTRING_LEN(str), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return RESULT_OK(argv[0]);
}

static RESULT ab_kernel_raise(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    // Accepted forms:
    //   raise                      — re-raise or empty RuntimeError
    //   raise "msg"                — RuntimeError with message
    //   raise ExcClass             — ExcClass with empty message
    //   raise ExcClass, "msg"      — ExcClass with message
    //   raise ExcInstance          — re-raise the instance (message inherited)
    // abruby has a flat exception hierarchy (all aliased to RuntimeError
    // for now), so the class argument is accepted but only the message
    // is actually propagated.
    VALUE msg;
    if (argc == 0) {
        msg = abruby_str_new_cstr(c, "");
    } else if (argc == 1) {
        VALUE a = argv[0];
        if (ab_obj_type_p(a, ABRUBY_OBJ_EXCEPTION)) {
            // re-raise: propagate the existing exception as-is
            return (RESULT){a, RESULT_RAISE};
        }
        // String, integer, or anything else — treat as message.  The
        // exception constructor stores the value as-is and `message`
        // returns it.  (Existing tests rely on `raise 42` behaving like
        // this.)  An exception *class* with no message is handled by
        // Ruby-level code as `raise ExcClass, ""`, so we wouldn't get
        // here unless the user intentionally raised a class object
        // without a message — which we also tolerate.
        msg = a;
    } else {
        // argv[0] is expected to be the class, argv[1] the message
        msg = argv[1];
    }
    VALUE exc = abruby_exception_new(c, c->current_frame, msg);
    return (RESULT){exc, RESULT_RAISE};
}

// Rational(num, den) — create Rational
static RESULT ab_kernel_Rational(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE num = AB_NUM_UNWRAP(argv[0]);
    VALUE den = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(1);
    return RESULT_OK(abruby_rational_new(c, rb_rational_new(num, den)));
}

// Complex(real, imag) — create Complex
static RESULT ab_kernel_Complex(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE real = AB_NUM_UNWRAP(argv[0]);
    VALUE imag = (argc >= 2) ? AB_NUM_UNWRAP(argv[1]) : INT2FIX(0);
    return RESULT_OK(abruby_complex_new(c, rb_complex_new(real, imag)));
}

// Check if path has a file extension (e.g., ".rb", ".so")
// Only checks the basename to avoid false positives from ".." in paths.
static bool has_extension(VALUE path) {
    VALUE ext = rb_funcall(rb_cFile, rb_intern("extname"), 1, path);
    return RSTRING_LEN(ext) > 0;
}

// require(path) — load a file
static RESULT ab_kernel_require(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE path = RSTR(argv[0]);  // abruby string -> CRuby string
    if (!has_extension(path)) {
        path = rb_str_cat_cstr(rb_str_dup(path), ".rb");
    }
    return abruby_require_file(c, path);
}

// __dir__ — directory of the current source file (the closest enclosing
// AST method's source file, or the VM's current_file if at top level).
// Used by optcarrot-bench to find Lan_Master.nes next to the script.
static RESULT ab_kernel_dir(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    VALUE path = abruby_current_file(c);
    if (NIL_P(path)) return RESULT_OK(Qnil);
    VALUE dir = rb_funcall(rb_cFile, rb_intern("dirname"), 1, path);
    return RESULT_OK(abruby_str_new(c, dir));
}

// require_relative(path) — load a file relative to the current file
static RESULT ab_kernel_require_relative(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE path = RSTR(argv[0]);
    if (!has_extension(path)) {
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

// block_given? — true if the method invoking us was itself given a block.
//
// Implementation note: this cfunc gets its own frame pushed by
// dispatch_method_frame, so the "method that received the block" is one
// frame up (c->current_frame->prev).  For the top-level <main> case,
// prev may be NULL (or a frame with no method) — we return false then.
static RESULT ab_kernel_block_given_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_frame *caller = c->current_frame ? c->current_frame->prev : NULL;
    bool given = caller && caller->block != NULL;
    return RESULT_OK(given ? Qtrue : Qfalse);
}

void
Init_abruby_kernel(void)
{
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("p"),        ab_kernel_p,        1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("raise"),    ab_kernel_raise,    1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Rational"), ab_kernel_Rational, 2);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Complex"),          ab_kernel_Complex,          2);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("require"),          ab_kernel_require,          1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("require_relative"), ab_kernel_require_relative, 1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("eval"),             ab_kernel_eval,             1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("block_given?"),     ab_kernel_block_given_p,    0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("__dir__"),          ab_kernel_dir,              0);
}
