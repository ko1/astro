#include "builtin.h"

static RESULT ab_array_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_str_new_cstr("[");
    for (long i = 0; i < len; i++) {
        if (i > 0) rb_str_cat_cstr(result, ", ");
        VALUE rs = ab_inspect_rstr(c, RARRAY_AREF(ary, i));
        rb_str_cat(result, RSTRING_PTR(rs), RSTRING_LEN(rs));
    }
    rb_str_cat_cstr(result, "]");
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_array_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(ab_array_inspect(c, self, 0, NULL).value); }
static RESULT ab_array_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_ary_entry(RARY(self), FIX2LONG(argv[0]))); }
static RESULT ab_array_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { rb_ary_store(RARY(self), FIX2LONG(argv[0]), argv[1]); return RESULT_OK(argv[1]); }
static RESULT ab_array_push(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { rb_ary_push(RARY(self), argv[0]); return RESULT_OK(self); }
static RESULT ab_array_pop(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(rb_ary_pop(RARY(self))); }
static RESULT ab_array_length(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(LONG2FIX(RARRAY_LEN(RARY(self)))); }
static RESULT ab_array_empty_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(RARRAY_LEN(RARY(self)) == 0 ? Qtrue : Qfalse); }
static RESULT ab_array_first(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { VALUE ary = RARY(self); return RESULT_OK(RARRAY_LEN(ary) > 0 ? RARRAY_AREF(ary, 0) : Qnil); }
static RESULT ab_array_last(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { VALUE ary = RARY(self); long len = RARRAY_LEN(ary); return RESULT_OK(len > 0 ? RARRAY_AREF(ary, len - 1) : Qnil); }
static RESULT ab_array_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE result = rb_ary_dup(RARY(self));
    rb_ary_concat(result, RARY(argv[0]));
    return RESULT_OK(abruby_ary_new(c, result));
}
static RESULT ab_array_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self); long len = RARRAY_LEN(ary);
    for (long i = 0; i < len; i++) { if (rb_equal(RARRAY_AREF(ary, i), argv[0])) return RESULT_OK(Qtrue); }
    return RESULT_OK(Qfalse);
}

// Array#each { |x| ... } — yields each element in order, returns self.
// BREAK (block-break) is demoted to its payload value by the surrounding
// dispatch_method_frame_with_block boundary, so we just return it and
// the outer dispatch will unwrap.  The same holds for RETURN/RAISE.
static RESULT ab_array_each(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    for (long i = 0; i < len; i++) {
        VALUE elem = RARRAY_AREF(ary, i);
        RESULT r = abruby_yield(c, 1, &elem);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
    }
    return RESULT_OK(self);
}

// Array#map { |x| ... } — collects yielded values into a new array.
// If a block breaks, the break value is returned (the in-progress array
// is discarded, matching Ruby).  If the block raises or does a non-local
// return, the state propagates up unchanged.
static RESULT ab_array_map(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE elem = RARRAY_AREF(ary, i);
        RESULT r = abruby_yield(c, 1, &elem);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
        rb_ary_push(result, r.value);
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#select { |x| pred } — keeps elements where the block returns truthy.
static RESULT ab_array_select(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_ary_new_capa(0);
    for (long i = 0; i < len; i++) {
        VALUE elem = RARRAY_AREF(ary, i);
        RESULT r = abruby_yield(c, 1, &elem);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
        if (RTEST(r.value)) rb_ary_push(result, elem);
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#reject { |x| pred } — keeps elements where the block returns falsy.
static RESULT ab_array_reject(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_ary_new_capa(0);
    for (long i = 0; i < len; i++) {
        VALUE elem = RARRAY_AREF(ary, i);
        RESULT r = abruby_yield(c, 1, &elem);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
        if (!RTEST(r.value)) rb_ary_push(result, elem);
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

void
Init_abruby_array(void)
{
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("inspect"),  ab_array_inspect,   0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("to_s"),     ab_array_to_s,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("[]"),       ab_array_get,       1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("[]="),      ab_array_set,       2);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("push"),     ab_array_push,      1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("<<"),       ab_array_push,      1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("pop"),      ab_array_pop,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("length"),   ab_array_length,    0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("size"),     ab_array_length,    0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("empty?"),   ab_array_empty_p,   0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("first"),    ab_array_first,     0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("last"),     ab_array_last,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("+"),        ab_array_add,       1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("include?"), ab_array_include_p, 1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("each"),     ab_array_each,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("map"),      ab_array_map,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("collect"),  ab_array_map,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("select"),   ab_array_select,    0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("filter"),   ab_array_select,    0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("reject"),   ab_array_reject,    0);
}
