#include "builtin.h"

static RESULT ab_object_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    char buf[128];
    snprintf(buf, sizeof(buf), "#<%s:%p>", AB_CLASS_OF(self)->name, (void *)self);
    return RESULT_OK(abruby_str_new_cstr(buf));
}

static RESULT ab_object_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_method *inspect = abruby_find_method(AB_CLASS_OF(self), "inspect");
    if (inspect)
        return abruby_call_method(c, self, inspect, 0, NULL);
    return ab_object_inspect(c, self, 0, NULL);
}

static RESULT ab_object_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
}

static RESULT ab_object_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(self), "==");
    RESULT r = abruby_call_method(c, self, eq, 1, argv);
    if (r.state != RESULT_NORMAL) return r;
    return RESULT_OK(RTEST(r.value) ? Qfalse : Qtrue);
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

// Object#=== defaults to ==
static RESULT ab_object_case_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(self), "==");
    return abruby_call_method(c, self, eq, 1, argv);
}

static RESULT ab_object_is_a(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *obj_class = AB_CLASS_OF(self);
    struct abruby_class *check_class = abruby_unwrap_class(argv[0]);
    while (obj_class) {
        if (obj_class == check_class) return RESULT_OK(Qtrue);
        obj_class = obj_class->super;
    }
    return RESULT_OK(Qfalse);
}

static RESULT ab_object_instance_of(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_class *obj_class = AB_CLASS_OF(self);
    struct abruby_class *check_class = abruby_unwrap_class(argv[0]);
    return RESULT_OK(obj_class == check_class ? Qtrue : Qfalse);
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
    abruby_class_add_cfunc(ab_object_class, "===",       ab_object_case_eq,    1);
    abruby_class_add_cfunc(ab_object_class, "is_a?",     ab_object_is_a,       1);
    abruby_class_add_cfunc(ab_object_class, "kind_of?",  ab_object_is_a,       1);
    abruby_class_add_cfunc(ab_object_class, "instance_of?", ab_object_instance_of, 1);
}
