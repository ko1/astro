#include "builtin.h"

static VALUE ab_array_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_str_new_cstr("[");
    for (long i = 0; i < len; i++) {
        if (i > 0) rb_str_cat_cstr(result, ", ");
        VALUE rs = ab_inspect_rstr(c, RARRAY_AREF(ary, i));
        rb_str_cat(result, RSTRING_PTR(rs), RSTRING_LEN(rs));
    }
    rb_str_cat_cstr(result, "]");
    return abruby_str_new(result);
}
static VALUE ab_array_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return ab_array_inspect(c, self, 0, NULL); }
static VALUE ab_array_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return rb_ary_entry(RARY(self), FIX2LONG(argv[0])); }
static VALUE ab_array_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { rb_ary_store(RARY(self), FIX2LONG(argv[0]), argv[1]); return argv[1]; }
static VALUE ab_array_push(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { rb_ary_push(RARY(self), argv[0]); return self; }
static VALUE ab_array_pop(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return rb_ary_pop(RARY(self)); }
static VALUE ab_array_length(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return LONG2FIX(RARRAY_LEN(RARY(self))); }
static VALUE ab_array_empty_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RARRAY_LEN(RARY(self)) == 0 ? Qtrue : Qfalse; }
static VALUE ab_array_first(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { VALUE ary = RARY(self); return RARRAY_LEN(ary) > 0 ? RARRAY_AREF(ary, 0) : Qnil; }
static VALUE ab_array_last(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { VALUE ary = RARY(self); long len = RARRAY_LEN(ary); return len > 0 ? RARRAY_AREF(ary, len - 1) : Qnil; }
static VALUE ab_array_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE result = rb_ary_dup(RARY(self));
    rb_ary_concat(result, RARY(argv[0]));
    return abruby_ary_new(result);
}
static VALUE ab_array_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self); long len = RARRAY_LEN(ary);
    for (long i = 0; i < len; i++) { if (rb_equal(RARRAY_AREF(ary, i), argv[0])) return Qtrue; }
    return Qfalse;
}

void
Init_abruby_array(void)
{
    abruby_class_add_cfunc(ab_array_class, "inspect",  ab_array_inspect,   0);
    abruby_class_add_cfunc(ab_array_class, "to_s",     ab_array_to_s,      0);
    abruby_class_add_cfunc(ab_array_class, "[]",       ab_array_get,       1);
    abruby_class_add_cfunc(ab_array_class, "[]=",      ab_array_set,       2);
    abruby_class_add_cfunc(ab_array_class, "push",     ab_array_push,      1);
    abruby_class_add_cfunc(ab_array_class, "pop",      ab_array_pop,       0);
    abruby_class_add_cfunc(ab_array_class, "length",   ab_array_length,    0);
    abruby_class_add_cfunc(ab_array_class, "size",     ab_array_length,    0);
    abruby_class_add_cfunc(ab_array_class, "empty?",   ab_array_empty_p,   0);
    abruby_class_add_cfunc(ab_array_class, "first",    ab_array_first,     0);
    abruby_class_add_cfunc(ab_array_class, "last",     ab_array_last,      0);
    abruby_class_add_cfunc(ab_array_class, "+",        ab_array_add,       1);
    abruby_class_add_cfunc(ab_array_class, "include?", ab_array_include_p, 1);
}
