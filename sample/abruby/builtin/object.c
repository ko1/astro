#include "builtin.h"

static RESULT ab_object_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    char buf[128];
    snprintf(buf, sizeof(buf), "#<%s:%p>", AB_CLASS_OF(self)->name, (void *)self);
    return RESULT_OK(abruby_str_new_cstr(buf));
}

static RESULT ab_object_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_method *inspect = abruby_find_method(AB_CLASS_OF(self), "inspect");
    if (inspect && inspect->type == ABRUBY_METHOD_CFUNC)
        return RESULT_OK(inspect->u.cfunc.func(c, self, 0, NULL).value);
    return RESULT_OK(ab_object_inspect(c, self, 0, NULL).value);
}

static RESULT ab_object_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
}

static RESULT ab_object_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(self), "==");
    return RESULT_OK(RTEST(eq->u.cfunc.func(c, self, 1, argv).value) ? Qfalse : Qtrue);
}

static RESULT ab_object_nil_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(Qfalse);
}

static RESULT ab_object_class_name(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(AB_CLASS_OF(self)->name));
}

static RESULT ab_object_not(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(self) ? Qfalse : Qtrue);
}

void
Init_abruby_object(void)
{
    abruby_class_add_cfunc(ab_object_class, "inspect",  ab_object_inspect,    0);
    abruby_class_add_cfunc(ab_object_class, "to_s",     ab_object_to_s,       0);
    abruby_class_add_cfunc(ab_object_class, "==",       ab_object_eq,         1);
    abruby_class_add_cfunc(ab_object_class, "!=",       ab_object_neq,        1);
    abruby_class_add_cfunc(ab_object_class, "nil?",     ab_object_nil_p,      0);
    abruby_class_add_cfunc(ab_object_class, "class",    ab_object_class_name, 0);
    abruby_class_add_cfunc(ab_object_class, "!",        ab_object_not,        0);
}
