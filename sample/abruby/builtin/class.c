#include "builtin.h"

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

static VALUE ab_class_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return abruby_str_new_cstr(abruby_unwrap_class(self)->name);
}

void
Init_abruby_class(void)
{
    abruby_class_add_cfunc(ab_class_class, "new",     ab_class_new,     -1);
    abruby_class_add_cfunc(ab_class_class, "inspect", ab_class_inspect,  0);
}
