#include "builtin.h"

#define RRANGE(v) ((struct abruby_range *)RTYPEDDATA_GET_DATA(v))

static RESULT ab_range_first(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RRANGE(self)->begin);
}

static RESULT ab_range_last(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RRANGE(self)->end);
}

static RESULT ab_range_exclude_end_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RRANGE(self)->exclude_end ? Qtrue : Qfalse);
}

static RESULT ab_range_begin(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RRANGE(self)->begin);
}

static RESULT ab_range_end(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RRANGE(self)->end);
}

static RESULT ab_range_size(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        long sz = r->exclude_end ? e - b : e - b + 1;
        return RESULT_OK(LONG2FIX(sz < 0 ? 0 : sz));
    }
    return RESULT_OK(Qnil);
}

static RESULT ab_range_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    VALUE val = argv[0];
    // Integer fast path
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end) && FIXNUM_P(val)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        long v = FIX2LONG(val);
        if (r->exclude_end)
            return RESULT_OK((v >= b && v < e) ? Qtrue : Qfalse);
        else
            return RESULT_OK((v >= b && v <= e) ? Qtrue : Qfalse);
    }
    return RESULT_OK(Qfalse);
}

static RESULT ab_range_to_a(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        VALUE ary = rb_ary_new_capa(e - b + 1);
        for (long i = b; i <= e; i++) {
            rb_ary_push(ary, LONG2FIX(i));
        }
        return RESULT_OK(abruby_ary_new(ary));
    }
    rb_raise(rb_eRuntimeError, "can't convert Range to Array (non-integer)");
    return RESULT_OK(Qnil);
}

static RESULT ab_range_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (AB_CLASS_OF(argv[0]) != ab_range_class) return RESULT_OK(Qfalse);
    struct abruby_range *a = RRANGE(self);
    struct abruby_range *b = RRANGE(argv[0]);
    if (a->exclude_end != b->exclude_end) return RESULT_OK(Qfalse);
    // Compare begin and end using ==
    struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(a->begin), "==");
    if (!eq) return RESULT_OK(Qfalse);
    VALUE args_b[1] = { b->begin };
    RESULT rb = abruby_call_method(c, a->begin, eq, 1, args_b);
    if (rb.state != RESULT_NORMAL) return rb;
    if (rb.value != Qtrue) return RESULT_OK(Qfalse);
    eq = abruby_find_method(AB_CLASS_OF(a->end), "==");
    if (!eq) return RESULT_OK(Qfalse);
    VALUE args_e[1] = { b->end };
    RESULT re = abruby_call_method(c, a->end, eq, 1, args_e);
    if (re.state != RESULT_NORMAL) return re;
    return RESULT_OK(re.value);
}

static RESULT ab_range_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    VALUE begin_s = ab_inspect_rstr(c, r->begin);
    VALUE end_s = ab_inspect_rstr(c, r->end);
    VALUE result = rb_str_new(RSTRING_PTR(begin_s), RSTRING_LEN(begin_s));
    rb_str_cat_cstr(result, r->exclude_end ? "..." : "..");
    rb_str_cat(result, RSTRING_PTR(end_s), RSTRING_LEN(end_s));
    return RESULT_OK(abruby_str_new(result));
}

static RESULT ab_range_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(ab_range_inspect(c, self, argc, argv).value);
}

void
Init_abruby_range(void)
{
    abruby_class_add_cfunc(ab_range_class, "first",        ab_range_first,         0);
    abruby_class_add_cfunc(ab_range_class, "last",         ab_range_last,          0);
    abruby_class_add_cfunc(ab_range_class, "begin",        ab_range_begin,         0);
    abruby_class_add_cfunc(ab_range_class, "end",          ab_range_end,           0);
    abruby_class_add_cfunc(ab_range_class, "exclude_end?", ab_range_exclude_end_p, 0);
    abruby_class_add_cfunc(ab_range_class, "size",         ab_range_size,          0);
    abruby_class_add_cfunc(ab_range_class, "length",       ab_range_size,          0);
    abruby_class_add_cfunc(ab_range_class, "include?",     ab_range_include_p,     1);
    abruby_class_add_cfunc(ab_range_class, "===",          ab_range_include_p,     1);
    abruby_class_add_cfunc(ab_range_class, "to_a",         ab_range_to_a,          0);
    abruby_class_add_cfunc(ab_range_class, "==",           ab_range_eq,            1);
    abruby_class_add_cfunc(ab_range_class, "inspect",      ab_range_inspect,       0);
    abruby_class_add_cfunc(ab_range_class, "to_s",         ab_range_to_s,          0);
}
