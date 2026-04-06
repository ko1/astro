#include "builtin.h"

static RESULT ab_string_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    VALUE result = rb_str_new_cstr("\"");
    rb_str_cat(result, RSTRING_PTR(rs), RSTRING_LEN(rs));
    rb_str_cat_cstr(result, "\"");
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(self); }
static RESULT ab_string_to_i(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(LONG2FIX(strtol(RSTRING_PTR(RSTR(self)), NULL, 10))); }
static RESULT ab_string_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self), ra = RSTR(argv[0]);
    VALUE result = rb_str_new(RSTRING_PTR(rs), RSTRING_LEN(rs));
    rb_str_cat(result, RSTRING_PTR(ra), RSTRING_LEN(ra));
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    long times = FIX2LONG(argv[0]);
    VALUE result = rb_str_new(NULL, 0);
    for (long i = 0; i < times; i++) rb_str_cat(result, RSTRING_PTR(rs), RSTRING_LEN(rs));
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (AB_CLASS_OF(argv[0]) != ab_string_class) return RESULT_OK(Qfalse);
    return RESULT_OK(rb_str_equal(RSTR(self), RSTR(argv[0])));
}
static RESULT ab_string_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (AB_CLASS_OF(argv[0]) != ab_string_class) return RESULT_OK(Qtrue);
    return RESULT_OK(rb_str_equal(RSTR(self), RSTR(argv[0])) == Qtrue ? Qfalse : Qtrue);
}
static RESULT ab_string_lt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_str_cmp(RSTR(self), RSTR(argv[0])) < 0 ? Qtrue : Qfalse); }
static RESULT ab_string_le(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_str_cmp(RSTR(self), RSTR(argv[0])) <= 0 ? Qtrue : Qfalse); }
static RESULT ab_string_gt(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_str_cmp(RSTR(self), RSTR(argv[0])) > 0 ? Qtrue : Qfalse); }
static RESULT ab_string_ge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_str_cmp(RSTR(self), RSTR(argv[0])) >= 0 ? Qtrue : Qfalse); }
static RESULT ab_string_length(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(LONG2FIX(RSTRING_LEN(RSTR(self)))); }
static RESULT ab_string_empty_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(RSTRING_LEN(RSTR(self)) == 0 ? Qtrue : Qfalse); }
static RESULT ab_string_upcase(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    VALUE result = rb_str_new(RSTRING_PTR(rs), RSTRING_LEN(rs));
    char *p = RSTRING_PTR(result); long len = RSTRING_LEN(result);
    for (long i = 0; i < len; i++) { if (p[i] >= 'a' && p[i] <= 'z') p[i] -= 32; }
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_downcase(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    VALUE result = rb_str_new(RSTRING_PTR(rs), RSTRING_LEN(rs));
    char *p = RSTRING_PTR(result); long len = RSTRING_LEN(result);
    for (long i = 0; i < len; i++) { if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32; }
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_reverse(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self); long len = RSTRING_LEN(rs);
    VALUE result = rb_str_new(NULL, len);
    const char *src = RSTRING_PTR(rs); char *dst = RSTRING_PTR(result);
    for (long i = 0; i < len; i++) dst[i] = src[len - 1 - i];
    return RESULT_OK(abruby_str_new(result));
}
static RESULT ab_string_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(strstr(RSTRING_PTR(RSTR(self)), RSTRING_PTR(RSTR(argv[0]))) ? Qtrue : Qfalse);
}

static RESULT ab_string_concat(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self), ra = RSTR(argv[0]);
    rb_str_cat(rs, RSTRING_PTR(ra), RSTRING_LEN(ra));
    return RESULT_OK(self);
}

void
Init_abruby_string(void)
{
    abruby_class_add_cfunc(ab_string_class, "inspect",  ab_string_inspect,   0);
    abruby_class_add_cfunc(ab_string_class, "to_s",     ab_string_to_s,      0);
    abruby_class_add_cfunc(ab_string_class, "to_i",     ab_string_to_i,      0);
    abruby_class_add_cfunc(ab_string_class, "+",        ab_string_add,       1);
    abruby_class_add_cfunc(ab_string_class, "<<",       ab_string_concat,    1);
    abruby_class_add_cfunc(ab_string_class, "*",        ab_string_mul,       1);
    abruby_class_add_cfunc(ab_string_class, "==",       ab_string_eq,        1);
    abruby_class_add_cfunc(ab_string_class, "!=",       ab_string_neq,       1);
    abruby_class_add_cfunc(ab_string_class, "<",        ab_string_lt,        1);
    abruby_class_add_cfunc(ab_string_class, "<=",       ab_string_le,        1);
    abruby_class_add_cfunc(ab_string_class, ">",        ab_string_gt,        1);
    abruby_class_add_cfunc(ab_string_class, ">=",       ab_string_ge,        1);
    abruby_class_add_cfunc(ab_string_class, "length",   ab_string_length,    0);
    abruby_class_add_cfunc(ab_string_class, "size",     ab_string_length,    0);
    abruby_class_add_cfunc(ab_string_class, "empty?",   ab_string_empty_p,   0);
    abruby_class_add_cfunc(ab_string_class, "upcase",   ab_string_upcase,    0);
    abruby_class_add_cfunc(ab_string_class, "downcase", ab_string_downcase,  0);
    abruby_class_add_cfunc(ab_string_class, "reverse",  ab_string_reverse,   0);
    abruby_class_add_cfunc(ab_string_class, "include?", ab_string_include_p, 1);
}
