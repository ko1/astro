#include "builtin.h"

// Class#new (only on Class, not Module)
static RESULT ab_class_new(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    VALUE obj = abruby_new_object(klass);
    struct abruby_method *init = abruby_find_method(klass, rb_intern("initialize"));
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
    return RESULT_OK(abruby_str_new_cstr(c, rb_id2name(abruby_unwrap_class(self)->name)));
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
    proxy->klass = c->abm->module_class;
    proxy->obj_type = ABRUBY_OBJ_MODULE;
    proxy->name = mod->name;
    proxy->super = klass->super;
    // Copy method table from module to proxy
    ab_id_table_foreach(&mod->methods, _k, _v, {
        ab_id_table_insert(&proxy->methods, _k, _v);
    });

    klass->super = proxy;
    c->abm->method_serial++;
    return RESULT_OK(Qnil);
}

// Module#const_get(name) — get constant by name (String or Symbol)
static RESULT ab_module_const_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    ID name_id;
    if (SYMBOL_P(argv[0])) {
        name_id = SYM2ID(argv[0]);
    } else {
        name_id = rb_intern_str(RSTR(argv[0]));
    }
    VALUE v;
    if (ab_id_table_lookup(&klass->constants, name_id, &v)) {
        return RESULT_OK(v);
    }
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, "uninitialized constant"));
    return (RESULT){exc, RESULT_RAISE};
}

// Module#const_set(name, value) — set constant by name
static RESULT ab_module_const_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *klass = abruby_unwrap_class(self);
    ID name_id;
    if (SYMBOL_P(argv[0])) {
        name_id = SYM2ID(argv[0]);
    } else {
        name_id = rb_intern_str(RSTR(argv[0]));
    }
    abruby_class_set_const(klass, name_id, argv[1]);
    return RESULT_OK(argv[1]);
}

// Module#=== — check if argv[0] is_a? self (class matching for case/when)
static RESULT ab_module_case_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *check_class = abruby_unwrap_class(self);
    struct abruby_class *obj_class = AB_CLASS_OF(c, argv[0]);
    while (obj_class) {
        if (obj_class == check_class) return RESULT_OK(Qtrue);
        obj_class = obj_class->super;
    }
    return RESULT_OK(Qfalse);
}

void
Init_abruby_class(void)
{
    // Module (parent of Class)
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("==="),       ab_module_case_eq,   1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("inspect"),   ab_module_inspect,   0);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("include"),   ab_module_include,   1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("const_get"), ab_module_const_get, 1);
    abruby_class_add_cfunc(ab_tmpl_module_class, rb_intern("const_set"), ab_module_const_set, 2);

    // Class (inherits Module, adds new)
    abruby_class_add_cfunc(ab_tmpl_class_class, rb_intern("new"), ab_class_new, -1);
}
