#include "builtin.h"

#define RRANGE(v) ((struct abruby_range *)RTYPEDDATA_GET_DATA(v))

static VALUE ab_range_first(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RRANGE(self)->begin;
}

static VALUE ab_range_last(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RRANGE(self)->end;
}

static VALUE ab_range_exclude_end_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RRANGE(self)->exclude_end ? Qtrue : Qfalse;
}

static VALUE ab_range_begin(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RRANGE(self)->begin;
}

static VALUE ab_range_end(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RRANGE(self)->end;
}

static VALUE ab_range_size(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        long sz = r->exclude_end ? e - b : e - b + 1;
        return LONG2FIX(sz < 0 ? 0 : sz);
    }
    return Qnil;
}

static VALUE ab_range_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    VALUE val = argv[0];
    // Integer fast path
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end) && FIXNUM_P(val)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        long v = FIX2LONG(val);
        if (r->exclude_end)
            return (v >= b && v < e) ? Qtrue : Qfalse;
        else
            return (v >= b && v <= e) ? Qtrue : Qfalse;
    }
    return Qfalse;
}

static VALUE ab_range_to_a(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    if (FIXNUM_P(r->begin) && FIXNUM_P(r->end)) {
        long b = FIX2LONG(r->begin);
        long e = FIX2LONG(r->end);
        if (r->exclude_end) e--;
        VALUE ary = rb_ary_new_capa(e - b + 1);
        for (long i = b; i <= e; i++) {
            rb_ary_push(ary, LONG2FIX(i));
        }
        return abruby_ary_new(ary);
    }
    rb_raise(rb_eRuntimeError, "can't convert Range to Array (non-integer)");
    return Qnil;
}

static VALUE ab_range_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (AB_CLASS_OF(argv[0]) != ab_range_class) return Qfalse;
    struct abruby_range *a = RRANGE(self);
    struct abruby_range *b = RRANGE(argv[0]);
    if (a->exclude_end != b->exclude_end) return Qfalse;
    // Compare begin and end using ==
    struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(a->begin), "==");
    if (!eq) return Qfalse;
    VALUE args_b[1] = { b->begin };
    VALUE args_e[1] = { b->end };
    VALUE b_eq, e_eq;
    if (eq->type == ABRUBY_METHOD_CFUNC) {
        b_eq = eq->u.cfunc.func(c, a->begin, 1, args_b);
    } else {
        return Qfalse;
    }
    if (b_eq != Qtrue) return Qfalse;
    eq = abruby_find_method(AB_CLASS_OF(a->end), "==");
    if (!eq || eq->type != ABRUBY_METHOD_CFUNC) return Qfalse;
    e_eq = eq->u.cfunc.func(c, a->end, 1, args_e);
    return e_eq;
}

static VALUE ab_range_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    struct abruby_range *r = RRANGE(self);
    VALUE begin_s = ab_inspect_rstr(c, r->begin);
    VALUE end_s = ab_inspect_rstr(c, r->end);
    VALUE result = rb_str_new(RSTRING_PTR(begin_s), RSTRING_LEN(begin_s));
    rb_str_cat_cstr(result, r->exclude_end ? "..." : "..");
    rb_str_cat(result, RSTRING_PTR(end_s), RSTRING_LEN(end_s));
    return abruby_str_new(result);
}

static VALUE ab_range_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return ab_range_inspect(c, self, argc, argv);
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
    abruby_class_add_cfunc(ab_range_class, "to_a",         ab_range_to_a,          0);
    abruby_class_add_cfunc(ab_range_class, "==",           ab_range_eq,            1);
    abruby_class_add_cfunc(ab_range_class, "inspect",      ab_range_inspect,       0);
    abruby_class_add_cfunc(ab_range_class, "to_s",         ab_range_to_s,          0);
}
