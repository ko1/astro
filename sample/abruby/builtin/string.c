#include "builtin.h"

static VALUE arg_to_rstr_for_str(VALUE v) {
    if (ab_obj_type_p(v, ABRUBY_OBJ_STRING)) return RSTR(v);
    if (RB_TYPE_P(v, T_STRING)) return v;
    return rb_str_new_cstr("");
}

static RESULT ab_string_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    VALUE result = rb_str_new_cstr("\"");
    rb_str_cat(result, RSTRING_PTR(rs), RSTRING_LEN(rs));
    rb_str_cat_cstr(result, "\"");
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(self); }
static RESULT ab_string_to_i(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(LONG2FIX(strtol(RSTRING_PTR(RSTR(self)), NULL, 10))); }
static RESULT ab_string_start_with_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c;
    VALUE rs = RSTR(self);
    long rs_len = RSTRING_LEN(rs);
    const char *rs_ptr = RSTRING_PTR(rs);
    for (unsigned int i = 0; i < argc; i++) {
        if (!ab_obj_type_p(argv[i], ABRUBY_OBJ_STRING)) continue;
        VALUE pat = RSTR(argv[i]);
        long pl = RSTRING_LEN(pat);
        if (pl <= rs_len && memcmp(rs_ptr, RSTRING_PTR(pat), pl) == 0) {
            return RESULT_OK(Qtrue);
        }
    }
    return RESULT_OK(Qfalse);
}
static RESULT ab_string_end_with_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c;
    VALUE rs = RSTR(self);
    long rs_len = RSTRING_LEN(rs);
    const char *rs_ptr = RSTRING_PTR(rs);
    for (unsigned int i = 0; i < argc; i++) {
        if (!ab_obj_type_p(argv[i], ABRUBY_OBJ_STRING)) continue;
        VALUE pat = RSTR(argv[i]);
        long pl = RSTRING_LEN(pat);
        if (pl <= rs_len && memcmp(rs_ptr + rs_len - pl, RSTRING_PTR(pat), pl) == 0) {
            return RESULT_OK(Qtrue);
        }
    }
    return RESULT_OK(Qfalse);
}
static RESULT ab_string_chomp(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE rs = RSTR(self);
    long len = RSTRING_LEN(rs);
    const char *p = RSTRING_PTR(rs);
    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) len--;
    return RESULT_OK(abruby_str_new(c, rb_str_new(p, len)));
}
static RESULT ab_string_strip(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE rs = RSTR(self);
    long len = RSTRING_LEN(rs);
    const char *p = RSTRING_PTR(rs);
    long start = 0, end = len;
    while (start < end && (p[start] == ' ' || p[start] == '\t' || p[start] == '\n' || p[start] == '\r')) start++;
    while (end > start && (p[end-1] == ' ' || p[end-1] == '\t' || p[end-1] == '\n' || p[end-1] == '\r')) end--;
    return RESULT_OK(abruby_str_new(c, rb_str_new(p + start, end - start)));
}
// String#bytes — array of byte integers.
static RESULT ab_string_bytes(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE rs = RSTR(self);
    long len = RSTRING_LEN(rs);
    const unsigned char *p = (const unsigned char *)RSTRING_PTR(rs);
    VALUE ary = rb_ary_new_capa(len);
    for (long i = 0; i < len; i++) rb_ary_push(ary, LONG2FIX(p[i]));
    return RESULT_OK(abruby_ary_new(c, ary));
}
// String#bytesize — number of bytes.
static RESULT ab_string_bytesize(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(LONG2FIX(RSTRING_LEN(RSTR(self))));
}
// String#unpack — delegate to CRuby.
static RESULT ab_string_unpack(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc;
    if (!ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) return RESULT_OK(abruby_ary_new(c, rb_ary_new()));
    VALUE r = rb_funcall(RSTR(self), rb_intern("unpack"), 1, RSTR(argv[0]));
    // Convert any string elements back to abruby strings.
    long len = RARRAY_LEN(r);
    VALUE result = rb_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE e = RARRAY_AREF(r, i);
        rb_ary_push(result, RB_TYPE_P(e, T_STRING) ? abruby_str_new(c, e) : e);
    }
    return RESULT_OK(abruby_ary_new(c, result));
}

// String#tr(from, to) — translate.  Delegates to CRuby.
static RESULT ab_string_tr(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc;
    VALUE r = rb_funcall(RSTR(self), rb_intern("tr"), 2,
                         arg_to_rstr_for_str(argv[0]), arg_to_rstr_for_str(argv[1]));
    return RESULT_OK(abruby_str_new(c, r));
}

// String#=~(regexp) — return the match position or nil.  Delegates to
// CRuby's String#=~ which handles abruby-wrapped regexp via the inner
// rb_regexp pointer.
static RESULT ab_string_match_op(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE re = argv[0];
    VALUE inner;
    if (ab_obj_type_p(re, ABRUBY_OBJ_REGEXP)) {
        inner = ((const struct abruby_regexp *)RTYPEDDATA_GET_DATA(re))->rb_regexp;
    } else if (RB_TYPE_P(re, T_REGEXP)) {
        inner = re;
    } else {
        return RESULT_OK(Qnil);
    }
    VALUE r = rb_funcall(RSTR(self), rb_intern("=~"), 1, inner);
    return RESULT_OK(r);
}

// String#split(sep=" ") -> Array of substrings.  abruby's split only
// handles literal-string separators (no regex, no max-splits).  That's
// enough for optcarrot's `arg.split("=")` and similar.
static RESULT ab_string_split(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    long len = RSTRING_LEN(rs);
    const char *p = RSTRING_PTR(rs);
    const char *sep_p;
    long sep_len;
    if (argc >= 1 && ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) {
        VALUE sep = RSTR(argv[0]);
        sep_p = RSTRING_PTR(sep);
        sep_len = RSTRING_LEN(sep);
    } else {
        sep_p = " ";
        sep_len = 1;
    }
    VALUE result = rb_ary_new();
    if (sep_len == 0) {
        // Split into individual chars.
        for (long i = 0; i < len; i++) {
            rb_ary_push(result, abruby_str_new(c, rb_str_new(p + i, 1)));
        }
        return RESULT_OK(abruby_ary_new(c, result));
    }
    long start = 0;
    for (long i = 0; i + sep_len <= len; ) {
        if (memcmp(p + i, sep_p, sep_len) == 0) {
            rb_ary_push(result, abruby_str_new(c, rb_str_new(p + start, i - start)));
            i += sep_len;
            start = i;
        } else {
            i++;
        }
    }
    rb_ary_push(result, abruby_str_new(c, rb_str_new(p + start, len - start)));
    return RESULT_OK(abruby_ary_new(c, result));
}

static RESULT ab_string_to_sym(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    // Symbols are CRuby immediates; intern the underlying rb_str.
    return RESULT_OK(rb_str_intern(RSTR(self)));
}
static RESULT ab_string_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc;
    if (!ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "no implicit conversion into String"));
        return (RESULT){exc, RESULT_RAISE};
    }
    // Pre-size the result buffer so rb_str_cat doesn't need to realloc.
    VALUE rs = RSTR(self), ra = RSTR(argv[0]);
    long rs_len = RSTRING_LEN(rs), ra_len = RSTRING_LEN(ra);
    VALUE result = rb_str_buf_new(rs_len + ra_len);
    rb_str_cat(result, RSTRING_PTR(rs), rs_len);
    rb_str_cat(result, RSTRING_PTR(ra), ra_len);
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_mul(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    long times = FIX2LONG(argv[0]);
    long rs_len = RSTRING_LEN(rs);
    VALUE result = rb_str_buf_new(rs_len * times);  // pre-size exact
    const char *src = RSTRING_PTR(rs);
    for (long i = 0; i < times; i++) rb_str_cat(result, src, rs_len);
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (!ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) return RESULT_OK(Qfalse);
    return RESULT_OK(rb_str_equal(RSTR(self), RSTR(argv[0])));
}
static RESULT ab_string_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (!ab_obj_type_p(argv[0], ABRUBY_OBJ_STRING)) return RESULT_OK(Qtrue);
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
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_downcase(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    VALUE result = rb_str_new(RSTRING_PTR(rs), RSTRING_LEN(rs));
    char *p = RSTRING_PTR(result); long len = RSTRING_LEN(result);
    for (long i = 0; i < len; i++) { if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32; }
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_reverse(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self); long len = RSTRING_LEN(rs);
    VALUE result = rb_str_new(NULL, len);
    const char *src = RSTRING_PTR(rs); char *dst = RSTRING_PTR(result);
    for (long i = 0; i < len; i++) dst[i] = src[len - 1 - i];
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_string_include_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(strstr(RSTRING_PTR(RSTR(self)), RSTRING_PTR(RSTR(argv[0]))) ? Qtrue : Qfalse);
}

static RESULT ab_string_concat(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self), ra = RSTR(argv[0]);
    rb_str_cat(rs, RSTRING_PTR(ra), RSTRING_LEN(ra));
    return RESULT_OK(self);
}

static RESULT ab_string_format(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE fmt = RSTR(self);
    VALUE arg = argv[0];
    VALUE rb_arg;
    if (ab_obj_type_p(arg, ABRUBY_OBJ_ARRAY)) {
        rb_arg = RARY(arg);
    } else {
        rb_arg = rb_ary_new3(1, RB_SPECIAL_CONST_P(arg) ? arg :
                 (ab_obj_type_p(arg, ABRUBY_OBJ_FLOAT) ?
                  ((const struct abruby_float *)RTYPEDDATA_GET_DATA(arg))->rb_float : arg));
    }
    VALUE result = rb_str_format(RARRAY_LEN(rb_arg), RARRAY_CONST_PTR(rb_arg), fmt);
    return RESULT_OK(abruby_str_new(c, result));
}

static RESULT ab_string_sum(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE rs = RSTR(self);
    long bits = (argc >= 1 && FIXNUM_P(argv[0])) ? FIX2LONG(argv[0]) : 16;
    VALUE result = rb_funcall(rs, rb_intern("sum"), 1, LONG2FIX(bits));
    return RESULT_OK(FIXNUM_P(result) ? result : abruby_bignum_new(c, result));
}

void
Init_abruby_string(void)
{
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("inspect"),  ab_string_inspect,   0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("to_s"),     ab_string_to_s,      0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("to_i"),     ab_string_to_i,      0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("+"),        ab_string_add,       1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("<<"),       ab_string_concat,    1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("*"),        ab_string_mul,       1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("=="),       ab_string_eq,        1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("!="),       ab_string_neq,       1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("<"),        ab_string_lt,        1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("<="),       ab_string_le,        1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern(">"),        ab_string_gt,        1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern(">="),       ab_string_ge,        1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("length"),   ab_string_length,    0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("size"),     ab_string_length,    0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("empty?"),   ab_string_empty_p,   0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("upcase"),   ab_string_upcase,    0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("downcase"), ab_string_downcase,  0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("reverse"),  ab_string_reverse,   0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("include?"), ab_string_include_p, 1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("to_sym"),   ab_string_to_sym,    0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("intern"),   ab_string_to_sym,    0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("start_with?"), ab_string_start_with_p, 1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("end_with?"),   ab_string_end_with_p,   1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("chomp"),    ab_string_chomp,     0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("strip"),    ab_string_strip,     0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("split"),    ab_string_split,     0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("=~"),       ab_string_match_op,  1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("tr"),       ab_string_tr,        2);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("bytes"),    ab_string_bytes,     0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("%"),       ab_string_format,    1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("sum"),     ab_string_sum,      -1);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("bytesize"), ab_string_bytesize,  0);
    abruby_class_add_cfunc(ab_tmpl_string_class, rb_intern("unpack"),   ab_string_unpack,    1);
}
