#include "builtin.h"

static VALUE ab_to_hash_key(VALUE v) {
    if (FIXNUM_P(v) || RB_FLONUM_P(v) || v == Qtrue || v == Qfalse || v == Qnil) return v;
    if (RB_TYPE_P(v, T_DATA)) {
        const struct abruby_header *h = (const struct abruby_header *)RTYPEDDATA_GET_DATA(v);
        if (h->klass->obj_type == ABRUBY_OBJ_STRING) return ((const struct abruby_string *)h)->rb_str;
    }
    return v;
}

static RESULT ab_hash_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE hash = RHSH(self);
    VALUE keys = rb_funcall(hash, rb_intern("keys"), 0);
    long len = RARRAY_LEN(keys);
    VALUE result = rb_str_new_cstr("{");
    for (long i = 0; i < len; i++) {
        if (i > 0) rb_str_cat_cstr(result, ", ");
        VALUE raw_k = RARRAY_AREF(keys, i);
        VALUE raw_v = rb_hash_aref(hash, raw_k);
        if (RB_TYPE_P(raw_k, T_STRING)) {
            rb_str_cat_cstr(result, "\"");
            rb_str_cat(result, RSTRING_PTR(raw_k), RSTRING_LEN(raw_k));
            rb_str_cat_cstr(result, "\"");
        } else if (FIXNUM_P(raw_k)) {
            char buf[32]; snprintf(buf, sizeof(buf), "%ld", FIX2LONG(raw_k));
            rb_str_cat_cstr(result, buf);
        } else {
            rb_str_cat_cstr(result, "?");
        }
        rb_str_cat_cstr(result, "=>");
        VALUE vs = ab_inspect_rstr(c, raw_v);
        rb_str_cat(result, RSTRING_PTR(vs), RSTRING_LEN(vs));
    }
    rb_str_cat_cstr(result, "}");
    return RESULT_OK(abruby_str_new(c, result));
}
static RESULT ab_hash_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(ab_hash_inspect(c, self, 0, NULL).value); }
static RESULT ab_hash_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE v = rb_hash_aref(RHSH(self), ab_to_hash_key(argv[0]));
    return RESULT_OK(NIL_P(v) ? Qnil : v);
}
static RESULT ab_hash_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    rb_hash_aset(RHSH(self), ab_to_hash_key(argv[0]), argv[1]);
    return RESULT_OK(argv[1]);
}
static RESULT ab_hash_length(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(LONG2FIX(RHASH_SIZE(RHSH(self)))); }
static RESULT ab_hash_empty_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) { return RESULT_OK(RHASH_SIZE(RHSH(self)) == 0 ? Qtrue : Qfalse); }
static RESULT ab_hash_has_key_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE v = rb_hash_aref(RHSH(self), ab_to_hash_key(argv[0]));
    return RESULT_OK(NIL_P(v) ? Qfalse : Qtrue);
}
static RESULT ab_hash_keys(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE raw_keys = rb_funcall(RHSH(self), rb_intern("keys"), 0);
    long len = RARRAY_LEN(raw_keys);
    VALUE result = rb_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE k = RARRAY_AREF(raw_keys, i);
        if (RB_TYPE_P(k, T_STRING)) k = abruby_str_new(c, k);
        rb_ary_push(result, k);
    }
    return RESULT_OK(abruby_ary_new(c, result));
}
static RESULT ab_hash_values(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_ary_new(c, rb_funcall(RHSH(self), rb_intern("values"), 0)));
}

// Hash#dup — shallow copy.
static RESULT ab_hash_dup(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    return RESULT_OK(abruby_hash_new_wrap(c, rb_hash_dup(RHSH(self))));
}

// Hash#each_key / Hash#each_value
static RESULT ab_hash_each_key(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE hash = RHSH(self);
    VALUE raw_keys = rb_funcall(hash, rb_intern("keys"), 0);
    long len = RARRAY_LEN(raw_keys);
    for (long i = 0; i < len; i++) {
        VALUE raw_k = RARRAY_AREF(raw_keys, i);
        VALUE k = RB_TYPE_P(raw_k, T_STRING) ? abruby_str_new(c, raw_k) : raw_k;
        RESULT r = abruby_yield(c, 1, &k);
        if (r.state != RESULT_NORMAL) return r;
    }
    return RESULT_OK(self);
}
static RESULT ab_hash_each_value(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    VALUE hash = RHSH(self);
    VALUE raw_keys = rb_funcall(hash, rb_intern("keys"), 0);
    long len = RARRAY_LEN(raw_keys);
    for (long i = 0; i < len; i++) {
        VALUE raw_k = RARRAY_AREF(raw_keys, i);
        VALUE v = rb_hash_aref(hash, raw_k);
        RESULT r = abruby_yield(c, 1, &v);
        if (r.state != RESULT_NORMAL) return r;
    }
    return RESULT_OK(self);
}

// Hash#merge(other) — return a new hash combining self with other.
static RESULT ab_hash_merge(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE result = rb_hash_dup(RHSH(self));
    for (unsigned int i = 0; i < argc; i++) {
        if (!ab_obj_type_p(argv[i], ABRUBY_OBJ_HASH)) continue;
        rb_hash_update_by(result, RHSH(argv[i]), NULL);
    }
    return RESULT_OK(abruby_hash_new_wrap(c, result));
}

// Hash#delete(key)
static RESULT ab_hash_delete(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    return RESULT_OK(rb_hash_delete(RHSH(self), ab_to_hash_key(argv[0])));
}

// Hash#fetch(key) / fetch(key, default)
static RESULT ab_hash_fetch(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE v = rb_hash_lookup2(RHSH(self), ab_to_hash_key(argv[0]), Qundef);
    if (v != Qundef) return RESULT_OK(v);
    if (argc >= 2) return RESULT_OK(argv[1]);
    VALUE exc = abruby_exception_new(c, c->current_frame,
        abruby_str_new_cstr(c, "key not found"));
    return (RESULT){exc, RESULT_RAISE};
}

// Hash#each { |k, v| ... } — yields key/value pairs and returns self.
// Hash keys stored as raw CRuby strings are re-wrapped as abruby strings
// before yielding, matching the abruby VALUE invariant.
static RESULT ab_hash_each(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    VALUE hash = RHSH(self);
    VALUE raw_keys = rb_funcall(hash, rb_intern("keys"), 0);
    long len = RARRAY_LEN(raw_keys);
    for (long i = 0; i < len; i++) {
        VALUE raw_k = RARRAY_AREF(raw_keys, i);
        VALUE raw_v = rb_hash_aref(hash, raw_k);
        VALUE k = RB_TYPE_P(raw_k, T_STRING) ? abruby_str_new(c, raw_k) : raw_k;
        VALUE pair[2] = { k, raw_v };
        RESULT r = abruby_yield(c, 2, pair);
        if (UNLIKELY(r.state != RESULT_NORMAL)) return r;
    }
    return RESULT_OK(self);
}

void
Init_abruby_hash(void)
{
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("inspect"),  ab_hash_inspect,  0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("to_s"),     ab_hash_to_s,     0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("[]"),       ab_hash_get,      1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("[]="),      ab_hash_set,      2);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("length"),   ab_hash_length,   0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("size"),     ab_hash_length,   0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("empty?"),   ab_hash_empty_p,  0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("has_key?"), ab_hash_has_key_p,1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("key?"),     ab_hash_has_key_p,1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("keys"),     ab_hash_keys,     0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("values"),   ab_hash_values,   0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("each"),     ab_hash_each,     0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("each_pair"),ab_hash_each,     0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("each_key"), ab_hash_each_key, 0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("each_value"),ab_hash_each_value, 0);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("merge"),    ab_hash_merge,    1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("delete"),   ab_hash_delete,   1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("fetch"),    ab_hash_fetch,    1);
    abruby_class_add_cfunc(ab_tmpl_hash_class, rb_intern("dup"),      ab_hash_dup,      0);
}
