#include "builtin.h"

// Class#new (only on Class, not Module)
static RESULT ab_class_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    VALUE obj = abruby_new_object(klass);
    struct abruby_method *init = abruby_find_method(klass, "initialize");
    if (init) {
        // Push frame for initialize (needed for super to find the class)
        struct abruby_frame frame;
        frame.prev = c->current_frame;
        frame.method = init;
        frame.klass = klass;
        c->current_frame = &frame;

        RESULT r;
        if (init->type == ABRUBY_METHOD_CFUNC) {
            r = init->u.cfunc.func(c, obj, argc, argv);
        } else {
            VALUE *save_fp = c->fp;
            VALUE save_self = c->self;
            c->fp = argv;
            c->self = obj;
            r = EVAL(c, init->u.ast.body);
            c->fp = save_fp;
            c->self = save_self;
        }

        c->current_frame = frame.prev;
        if (r.state != RESULT_NORMAL) return r;
    }
    return RESULT_OK(obj);
}

// Module#inspect (inherited by Class)
static RESULT ab_module_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(abruby_unwrap_class(self)->name));
}

// Module#include — insert module into super chain of current_class
static RESULT ab_module_include(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    // self is the class doing the include (current_class wrapped)
    // argv[0] is the module to include
    struct abruby_class *klass = abruby_unwrap_class(self);
    struct abruby_class *mod = abruby_unwrap_class(argv[0]);

    // Create a proxy that copies module's method table pointer
    // Insert between klass and klass->super
    // Simple approach: create a proxy class that delegates to the module
    struct abruby_class *proxy = (struct abruby_class *)ruby_xcalloc(1, sizeof(struct abruby_class));
    proxy->klass = ab_module_class;
    proxy->name = mod->name;
    proxy->super = klass->super;
    // Copy methods from module to proxy
    proxy->method_cnt = mod->method_cnt;
    memcpy(proxy->methods, mod->methods, sizeof(struct abruby_method) * mod->method_cnt);

    klass->super = proxy;
    return RESULT_OK(Qnil);
}

// Module#const_get(name) — get constant by name (String or Symbol)
static RESULT ab_module_const_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    const char *name;
    if (SYMBOL_P(argv[0])) {
        name = rb_id2name(SYM2ID(argv[0]));
    } else {
        name = RSTRING_PTR(RSTR(argv[0]));
    }
    for (unsigned int i = 0; i < klass->const_cnt; i++) {
        if (strcmp(klass->constants[i].name, name) == 0) {
            return RESULT_OK(klass->constants[i].value);
        }
    }
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr("uninitialized constant"));
    return (RESULT){exc, RESULT_RAISE};
}

// Module#const_set(name, value) — set constant by name
static RESULT ab_module_const_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    const char *name;
    if (SYMBOL_P(argv[0])) {
        name = rb_id2name(SYM2ID(argv[0]));
    } else {
        name = RSTRING_PTR(RSTR(argv[0]));
    }
    abruby_class_set_const(klass, name, argv[1]);
    return RESULT_OK(argv[1]);
}

void
Init_abruby_class(void)
{
    // Module (parent of Class)
    abruby_class_add_cfunc(ab_module_class, "inspect",   ab_module_inspect,   0);
    abruby_class_add_cfunc(ab_module_class, "include",   ab_module_include,   1);
    abruby_class_add_cfunc(ab_module_class, "const_get", ab_module_const_get, 1);
    abruby_class_add_cfunc(ab_module_class, "const_set", ab_module_const_set, 2);

    // Class (inherits Module, adds new)
    abruby_class_add_cfunc(ab_class_class, "new", ab_class_new, -1);
}
