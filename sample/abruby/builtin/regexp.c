#include "builtin.h"

#define RREGEXP_VAL(v) (((struct abruby_regexp *)RTYPEDDATA_GET_DATA(v))->rb_regexp)

static VALUE ab_regexp_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE s = rb_funcall(re, rb_intern("inspect"), 0);
    return abruby_str_new(s);
}

static VALUE ab_regexp_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE s = rb_funcall(re, rb_intern("to_s"), 0);
    return abruby_str_new(s);
}

static VALUE ab_regexp_source(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE s = rb_funcall(re, rb_intern("source"), 0);
    return abruby_str_new(s);
}

static VALUE ab_regexp_match_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE str = RSTR(argv[0]);
    VALUE result = rb_funcall(re, rb_intern("match?"), 1, str);
    return RTEST(result) ? Qtrue : Qfalse;
}

static VALUE ab_regexp_match(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE str = RSTR(argv[0]);
    VALUE result = rb_funcall(re, rb_intern("match"), 1, str);
    return NIL_P(result) ? Qnil : Qtrue;
}

static VALUE ab_regexp_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (AB_CLASS_OF(argv[0]) != ab_regexp_class) return Qfalse;
    VALUE a = RREGEXP_VAL(self);
    VALUE b = RREGEXP_VAL(argv[0]);
    return rb_funcall(a, rb_intern("=="), 1, b);
}

static VALUE ab_regexp_eqtilde(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE re = RREGEXP_VAL(self);
    VALUE str = RSTR(argv[0]);
    VALUE result = rb_funcall(re, rb_intern("=~"), 1, str);
    return result;
}

void
Init_abruby_regexp(void)
{
    abruby_class_add_cfunc(ab_regexp_class, "inspect", ab_regexp_inspect, 0);
    abruby_class_add_cfunc(ab_regexp_class, "to_s",    ab_regexp_to_s,    0);
    abruby_class_add_cfunc(ab_regexp_class, "source",  ab_regexp_source,  0);
    abruby_class_add_cfunc(ab_regexp_class, "match?",  ab_regexp_match_p, 1);
    abruby_class_add_cfunc(ab_regexp_class, "match",   ab_regexp_match,   1);
    abruby_class_add_cfunc(ab_regexp_class, "==",      ab_regexp_eq,      1);
    abruby_class_add_cfunc(ab_regexp_class, "=~",      ab_regexp_eqtilde, 1);
}
