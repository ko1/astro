#include "builtin.h"

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
static RESULT ab_string_to_sym(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    // Symbols are CRuby immediates; intern the underlying rb_str.
    return RESULT_OK(rb_str_intern(RSTR(self)));
}
static RESULT ab_string_add(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
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
}
