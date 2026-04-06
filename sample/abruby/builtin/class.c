#include "builtin.h"

// Class#new (only on Class, not Module)
static VALUE ab_class_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    VALUE obj = abruby_new_object(klass);
    struct abruby_method *init = abruby_find_method(klass, "initialize");
    if (init) {
        if (init->type == ABRUBY_METHOD_CFUNC) {
            init->u.cfunc.func(c, obj, argc, argv);
        } else {
            VALUE *save_fp = c->fp;
            VALUE save_self = c->self;
            c->fp = argv;
            c->self = obj;
            EVAL(c, init->u.ast.body);
            c->fp = save_fp;
            c->self = save_self;
        }
    }
    return obj;
}

// Module#inspect (inherited by Class)
static VALUE ab_module_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return abruby_str_new_cstr(abruby_unwrap_class(self)->name);
}

// Module#include — insert module into super chain of current_class
static VALUE ab_module_include(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    // self is the class doing the include (current_class wrapped)
    // argv[0] is the module to include
    struct abruby_class *klass = abruby_unwrap_class(self);
    struct abruby_class *mod = abruby_unwrap_class(argv[0]);

    // Create a proxy that copies module's method table pointer
    // Insert between klass and klass->super
    // Simple approach: create a proxy class that delegates to the module
    struct abruby_class *proxy = (struct abruby_class *)calloc(1, sizeof(struct abruby_class));
    proxy->klass = ab_module_class;
    proxy->name = mod->name;
    proxy->super = klass->super;
    // Copy methods from module to proxy
    proxy->method_cnt = mod->method_cnt;
    memcpy(proxy->methods, mod->methods, sizeof(struct abruby_method) * mod->method_cnt);

    klass->super = proxy;
    return Qnil;
}

void
Init_abruby_class(void)
{
    // Module (parent of Class)
    abruby_class_add_cfunc(ab_module_class, "inspect", ab_module_inspect, 0);
    abruby_class_add_cfunc(ab_module_class, "include", ab_module_include, 1);

    // Class (inherits Module, adds new)
    abruby_class_add_cfunc(ab_class_class, "new", ab_class_new, -1);
}
