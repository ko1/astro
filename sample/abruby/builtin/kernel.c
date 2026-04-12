#include "builtin.h"

static RESULT ab_kernel_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE str = ab_inspect_rstr(c, argv[0]);
    fwrite(RSTRING_PTR(str), 1, RSTRING_LEN(str), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return RESULT_OK(argv[0]);
}

// Kernel#puts — prints each arg with a trailing newline.  Strings go
// straight to stdout; non-strings are coerced via to_s dispatch.
static RESULT ab_kernel_puts(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    if (argc == 0) {
        fputc('\n', stdout);
        fflush(stdout);
        return RESULT_OK(Qnil);
    }
    for (unsigned int i = 0; i < argc; i++) {
        VALUE v = argv[i];
        VALUE rs;
        if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) {
            rs = RSTR(v);
        } else {
            const struct abruby_method *m = abruby_find_method(AB_CLASS_OF(c, v), rb_intern("to_s"));
            if (m) {
                RESULT r = abruby_call_method(c, v, m, 0, NULL);
                if (r.state != RESULT_NORMAL) return r;
                rs = ab_obj_type_p(r.value, ABRUBY_OBJ_STRING) ? RSTR(r.value) :
                     RB_TYPE_P(r.value, T_STRING) ? r.value : rb_str_new_cstr("");
            } else {
                rs = rb_str_new_cstr("");
            }
        }
        fwrite(RSTRING_PTR(rs), 1, RSTRING_LEN(rs), stdout);
        if (RSTRING_LEN(rs) == 0 || RSTRING_PTR(rs)[RSTRING_LEN(rs) - 1] != '\n') {
            fputc('\n', stdout);
        }
    }
    fflush(stdout);
    return RESULT_OK(Qnil);
}

// Kernel#print — like puts but no newline.
static RESULT ab_kernel_print(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self;
    for (unsigned int i = 0; i < argc; i++) {
        VALUE v = argv[i];
        VALUE rs;
        if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) {
            rs = RSTR(v);
        } else {
            const struct abruby_method *m = abruby_find_method(AB_CLASS_OF(c, v), rb_intern("to_s"));
            if (m) {
                RESULT r = abruby_call_method(c, v, m, 0, NULL);
                if (r.state != RESULT_NORMAL) return r;
                rs = ab_obj_type_p(r.value, ABRUBY_OBJ_STRING) ? RSTR(r.value) :
                     RB_TYPE_P(r.value, T_STRING) ? r.value : rb_str_new_cstr("");
            } else {
                rs = rb_str_new_cstr("");
            }
        }
        fwrite(RSTRING_PTR(rs), 1, RSTRING_LEN(rs), stdout);
    }
    fflush(stdout);
    return RESULT_OK(Qnil);
}

// Kernel#exit — abort execution.  Single integer arg is the status.
static RESULT ab_kernel_exit(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self;
    int code = (argc >= 1 && FIXNUM_P(argv[0])) ? (int)FIX2LONG(argv[0]) : 0;
    fflush(stdout);
    fflush(stderr);
    exit(code);
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

// Integer(value) — convert to integer.  Raises if the value can't be
// represented (matches Ruby's strictness vs. String#to_i).
static RESULT ab_kernel_Integer(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    VALUE v = argv[0];
    if (FIXNUM_P(v)) return RESULT_OK(v);
    if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) {
        VALUE rs = RSTR(v);
        const char *p = RSTRING_PTR(rs);
        long len = RSTRING_LEN(rs);
        // Trim leading whitespace
        long i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        long sign = 1;
        if (i < len && (p[i] == '-' || p[i] == '+')) {
            if (p[i] == '-') sign = -1;
            i++;
        }
        if (i >= len || p[i] < '0' || p[i] > '9') {
            VALUE exc = abruby_exception_new(c, c->current_frame,
                abruby_str_new_cstr(c, "invalid value for Integer()"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long n = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            n = n * 10 + (p[i] - '0');
            i++;
        }
        // Allow trailing whitespace only
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i != len) {
            VALUE exc = abruby_exception_new(c, c->current_frame,
                abruby_str_new_cstr(c, "invalid value for Integer()"));
            return (RESULT){exc, RESULT_RAISE};
        }
        return RESULT_OK(LONG2FIX(n * sign));
    }
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, "can't convert to Integer"));
    return (RESULT){exc, RESULT_RAISE};
}

// Float(value) — convert to float.
static RESULT ab_kernel_Float(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc;
    VALUE v = argv[0];
    if (FIXNUM_P(v)) return RESULT_OK(abruby_float_new_wrap(c, rb_float_new((double)FIX2LONG(v))));
    if (RB_FLONUM_P(v)) return RESULT_OK(v);
    if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) {
        VALUE r = rb_funcall(rb_cFloat, rb_intern("("), 1, RSTR(v));
        return RESULT_OK(abruby_float_new_wrap(c, r));
    }
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, "can't convert to Float"));
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

// GC.{disable,enable,start} — facade over CRuby GC for diagnostic
// scripts that toggle the collector.
RESULT ab_gc_disable(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(rb_funcall(rb_const_get(rb_cObject, rb_intern("GC")), rb_intern("disable"), 0));
}
RESULT ab_gc_enable(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(rb_funcall(rb_const_get(rb_cObject, rb_intern("GC")), rb_intern("enable"), 0));
}
RESULT ab_gc_start(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(rb_funcall(rb_const_get(rb_cObject, rb_intern("GC")), rb_intern("start"), 0));
}

// Process.clock_gettime(*) — return monotonic time as a Float.  We
// ignore the clock-id argument and always use CLOCK_MONOTONIC.
RESULT ab_process_clock_gettime(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    VALUE r = rb_funcall(rb_const_get(rb_cObject, rb_intern("Process")),
                         rb_intern("clock_gettime"), 1,
                         rb_const_get(rb_const_get(rb_cObject, rb_intern("Process")),
                                      rb_intern("CLOCK_MONOTONIC")));
    // r is a CRuby Float; wrap as abruby Float.
    return RESULT_OK(abruby_float_new_wrap(c, r));
}

// Kernel#loop { ... } — yield to the block forever (until break or
// StopIteration).  Used by optcarrot's CPU dispatch loop.
RESULT ab_kernel_loop(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)self; (void)argc; (void)argv;
    for (;;) {
        RESULT r = abruby_yield(c, 0, NULL);
        if (r.state != RESULT_NORMAL) {
            // Plain `break` from the block exits the loop with the
            // payload value; the dispatch_method_frame_with_block path
            // already demoted RESULT_BREAK on its way out, so we just
            // forward whatever's in r.
            return r;
        }
    }
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
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("puts"),     ab_kernel_puts,     0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("print"),    ab_kernel_print,    0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("exit"),     ab_kernel_exit,     0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("raise"),    ab_kernel_raise,    1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Rational"), ab_kernel_Rational, 2);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Complex"),          ab_kernel_Complex,          2);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("require"),          ab_kernel_require,          1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("require_relative"), ab_kernel_require_relative, 1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("eval"),             ab_kernel_eval,             1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("block_given?"),     ab_kernel_block_given_p,    0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("__dir__"),          ab_kernel_dir,              0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Integer"),          ab_kernel_Integer,          1);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("Float"),            ab_kernel_Float,            1);
    extern RESULT ab_kernel_proc(CTX *, VALUE, unsigned int, VALUE *);
    extern RESULT ab_kernel_lambda(CTX *, VALUE, unsigned int, VALUE *);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("proc"),             ab_kernel_proc,             0);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("lambda"),           ab_kernel_lambda,           0);
    extern RESULT ab_kernel_loop(CTX *, VALUE, unsigned int, VALUE *);
    abruby_class_add_cfunc(ab_tmpl_kernel_module, rb_intern("loop"),             ab_kernel_loop,             0);
}
