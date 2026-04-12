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

// Array#reverse — return a new array reversed.
static RESULT ab_array_reverse(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    VALUE result = rb_ary_new_capa(len);
    for (long i = len - 1; i >= 0; i--) rb_ary_push(result, RARRAY_AREF(ary, i));
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#dup — shallow copy.
static RESULT ab_array_dup(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    return RESULT_OK(abruby_ary_new(c, rb_ary_dup(RARY(self))));
}

// Array#flatten(depth=nil) — flat shallow (depth=1) or fully (default).
static RESULT ab_array_flatten(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    long depth = -1;
    if (argc >= 1 && FIXNUM_P(argv[0])) depth = FIX2LONG(argv[0]);
    VALUE ary = RARY(self);
    // Use CRuby rb_ary_flatten via VALUE; fall back to manual recursive copy.
    VALUE result = rb_ary_new();
    long len = RARRAY_LEN(ary);
    for (long i = 0; i < len; i++) {
        VALUE e = RARRAY_AREF(ary, i);
        if (depth != 0 && ab_obj_type_p(e, ABRUBY_OBJ_ARRAY)) {
            VALUE sub_argv[1];
            sub_argv[0] = LONG2FIX(depth - 1);
            RESULT r = ab_array_flatten(c, e, depth >= 0 ? 1 : 0, sub_argv);
            if (r.state != RESULT_NORMAL) return r;
            VALUE sub = RARY(r.value);
            long sl = RARRAY_LEN(sub);
            for (long j = 0; j < sl; j++) rb_ary_push(result, RARRAY_AREF(sub, j));
        } else {
            rb_ary_push(result, e);
        }
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#concat(other) — append elements of `other` (in place).
static RESULT ab_array_concat(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    rb_ary_concat(RARY(self), RARY(argv[0]));
    return RESULT_OK(self);
}

// Array#join(sep="") — concatenate string-coerced elements.
static RESULT ab_array_join(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    VALUE sep_str = rb_str_new_cstr("");
    if (argc >= 1 && ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) {
        sep_str = RSTR(argv[0]);
    }
    long len = RARRAY_LEN(ary);
    VALUE result = rb_str_new_cstr("");
    for (long i = 0; i < len; i++) {
        if (i > 0) rb_str_cat(result, RSTRING_PTR(sep_str), RSTRING_LEN(sep_str));
        VALUE e = RARRAY_AREF(ary, i);
        VALUE s;
        if (ab_obj_type_p(e, ABRUBY_OBJ_STRING)) {
            s = RSTR(e);
        } else {
            // call to_s on the element via dispatch
            const struct abruby_method *m = abruby_find_method(AB_CLASS_OF(c, e), rb_intern("to_s"));
            if (m) {
                RESULT r = abruby_call_method(c, e, m, 0, NULL);
                if (r.state != RESULT_NORMAL) return r;
                s = ab_obj_type_p(r.value, ABRUBY_OBJ_STRING) ? RSTR(r.value) : rb_str_new_cstr("");
            } else {
                s = rb_str_new_cstr("");
            }
        }
        rb_str_cat(result, RSTRING_PTR(s), RSTRING_LEN(s));
    }
    return RESULT_OK(abruby_str_new(c, result));
}

// Array#min / Array#max — without block, uses <=> via dispatch is too heavy;
// for optcarrot we only need integer ranges, so iterate via Fixnum compare.
static RESULT ab_array_min(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    if (len == 0) return RESULT_OK(Qnil);
    VALUE best = RARRAY_AREF(ary, 0);
    for (long i = 1; i < len; i++) {
        VALUE e = RARRAY_AREF(ary, i);
        if (FIXNUM_P(best) && FIXNUM_P(e)) {
            if (FIX2LONG(e) < FIX2LONG(best)) best = e;
        }
    }
    return RESULT_OK(best);
}
static RESULT ab_array_max(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    if (len == 0) return RESULT_OK(Qnil);
    VALUE best = RARRAY_AREF(ary, 0);
    for (long i = 1; i < len; i++) {
        VALUE e = RARRAY_AREF(ary, i);
        if (FIXNUM_P(best) && FIXNUM_P(e)) {
            if (FIX2LONG(e) > FIX2LONG(best)) best = e;
        }
    }
    return RESULT_OK(best);
}

// Array#sort — quick wrapper around CRuby rb_ary_sort.  Block form not
// supported (optcarrot doesn't need it for the bench).
static RESULT ab_array_sort(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    return RESULT_OK(abruby_ary_new(c, rb_ary_sort(rb_ary_dup(RARY(self)))));
}

// Array#fill — fill the array with a value (no block, no range).
static RESULT ab_array_fill(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c;
    if (argc < 1) return RESULT_OK(self);
    VALUE val = argv[0];
    long len = RARRAY_LEN(RARY(self));
    for (long i = 0; i < len; i++) rb_ary_store(RARY(self), i, val);
    return RESULT_OK(self);
}

// Array#clear — remove all elements (in place).
static RESULT ab_array_clear(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    rb_ary_clear(RARY(self));
    return RESULT_OK(self);
}

// Array#replace(other) — copy contents from other in place.
static RESULT ab_array_replace(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    rb_ary_replace(RARY(self), RARY(argv[0]));
    return RESULT_OK(self);
}

// Array#shift — remove and return the first element.
static RESULT ab_array_shift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(rb_ary_shift(RARY(self)));
}

// Array#unshift(elem) — insert at the front.
static RESULT ab_array_unshift(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c;
    rb_ary_unshift(RARY(self), argv[0]);
    return RESULT_OK(self);
}

// Array#transpose — returns a new array of arrays where rows become cols.
static RESULT ab_array_transpose(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE ary = RARY(self);
    long n = RARRAY_LEN(ary);
    if (n == 0) return RESULT_OK(abruby_ary_new(c, rb_ary_new()));
    long m = 0;
    for (long i = 0; i < n; i++) {
        VALUE row = RARRAY_AREF(ary, i);
        if (!ab_obj_type_p(row, ABRUBY_OBJ_ARRAY)) {
            VALUE exc = abruby_exception_new(c, c->current_frame,
                abruby_str_new_cstr(c, "transpose: rows must be arrays"));
            return (RESULT){exc, RESULT_RAISE};
        }
        long rl = RARRAY_LEN(RARY(row));
        if (i == 0) m = rl;
        else if (rl != m) {
            VALUE exc = abruby_exception_new(c, c->current_frame,
                abruby_str_new_cstr(c, "transpose: rows must have equal length"));
            return (RESULT){exc, RESULT_RAISE};
        }
    }
    VALUE result = rb_ary_new_capa(m);
    for (long j = 0; j < m; j++) {
        VALUE col = rb_ary_new_capa(n);
        for (long i = 0; i < n; i++) {
            rb_ary_push(col, RARRAY_AREF(RARY(RARRAY_AREF(ary, i)), j));
        }
        rb_ary_push(result, abruby_ary_new(c, col));
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#zip(other) — returns new array of pairs.  Optcarrot uses 1-arg form.
static RESULT ab_array_zip(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE ary = RARY(self);
    long n = RARRAY_LEN(ary);
    VALUE result = rb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE pair = rb_ary_new_capa(argc + 1);
        rb_ary_push(pair, RARRAY_AREF(ary, i));
        for (unsigned int k = 0; k < argc; k++) {
            VALUE other = RARY(argv[k]);
            VALUE e = (i < RARRAY_LEN(other)) ? RARRAY_AREF(other, i) : Qnil;
            rb_ary_push(pair, e);
        }
        rb_ary_push(result, abruby_ary_new(c, pair));
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// Array#each_with_index { |e, i| ... }
static RESULT ab_array_each_with_index(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE ary = RARY(self);
    long len = RARRAY_LEN(ary);
    for (long i = 0; i < len; i++) {
        VALUE pair[2] = { RARRAY_AREF(ary, i), LONG2FIX(i) };
        RESULT r = abruby_yield(c, 2, pair);
        if (r.state != RESULT_NORMAL) return r;
    }
    return RESULT_OK(self);
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
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("reverse"),  ab_array_reverse,   0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("dup"),      ab_array_dup,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("flatten"),  ab_array_flatten,   0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("concat"),   ab_array_concat,    1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("join"),     ab_array_join,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("min"),      ab_array_min,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("max"),      ab_array_max,       0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("sort"),     ab_array_sort,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("fill"),     ab_array_fill,      0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("clear"),    ab_array_clear,     0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("replace"),  ab_array_replace,   1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("shift"),    ab_array_shift,     0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("unshift"),  ab_array_unshift,   1);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("each_with_index"), ab_array_each_with_index, 0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("transpose"), ab_array_transpose, 0);
    abruby_class_add_cfunc(ab_tmpl_array_class, rb_intern("zip"),       ab_array_zip,       1);
}
