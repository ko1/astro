#include "builtin.h"

static RESULT ab_object_inspect(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    char buf[128];
    snprintf(buf, sizeof(buf), "#<%s:%p>", rb_id2name(AB_CLASS_OF(c, self)->name), (void *)self);
    return RESULT_OK(abruby_str_new_cstr(c, buf));
}

static RESULT ab_object_to_s(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_method *inspect = abruby_find_method(AB_CLASS_OF(c, self), rb_intern("inspect"));
    if (inspect)
        return abruby_call_method(c, self, inspect, 0, NULL);
    return ab_object_inspect(c, self, 0, NULL);
}

RESULT ab_object_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
}

static RESULT ab_object_neq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(c, self), rb_intern("=="));
    RESULT r = abruby_call_method(c, self, eq, 1, argv);
    if (r.state != RESULT_NORMAL) return r;
    return RESULT_OK(RTEST(r.value) ? Qfalse : Qtrue);
}

static RESULT ab_object_nil_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(Qfalse);
}

// Method#call / Method#[] — invoke the bound method on its receiver.
// Used heavily by optcarrot's CPU dispatch table where each entry is
// `obj.method(:foo)` and is called via `entry[arg1, arg2]`.
RESULT ab_method_call(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_bound_method *bm =
        (const struct abruby_bound_method *)ABRUBY_DATA_PTR(self);
    VALUE recv = bm->recv;
    ID name = bm->method_name;
    const struct abruby_class *recv_klass = AB_CLASS_OF(c, recv);
    const struct abruby_method *m = abruby_find_method(recv_klass, name);
    if (!m) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "Method object: undefined method"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return abruby_call_method(c, recv, m, argc, argv);
}

// Object#equal?(other) — strict identity.
static RESULT ab_object_equal_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    return RESULT_OK(self == argv[0] ? Qtrue : Qfalse);
}
// Object#dup — abruby has no copy semantics for arbitrary objects;
// return self for the rare case (`@conf.dup`-like) where the caller
// just wants a "copy" they won't mutate.
static RESULT ab_object_dup(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(self);
}
// Object#hash — VALUE-based.
static RESULT ab_object_hash(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(LONG2FIX((long)(self >> 1)));
}

// Object#send(name, *args) — dispatch any method by name.  Implemented
// inline rather than as a separate apply node because it's only used
// from optcarrot's CPU dispatch table at this stage.
extern RESULT abruby_call_method(CTX *c, VALUE recv, const struct abruby_method *m,
                                  unsigned int argc, VALUE *argv);
static RESULT ab_object_send(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    if (argc == 0) {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "send: method name required"));
        return (RESULT){exc, RESULT_RAISE};
    }
    VALUE n = argv[0];
    ID name;
    if (ab_obj_type_p(n, ABRUBY_OBJ_SYMBOL)) name = SYM2ID(ab_sym_unwrap(n));
    else if (ab_obj_type_p(n, ABRUBY_OBJ_STRING)) name = rb_intern_str(RSTR(n));
    else {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "send: method name must be a Symbol or String"));
        return (RESULT){exc, RESULT_RAISE};
    }
    const struct abruby_method *m = abruby_find_method(AB_CLASS_OF(c, self), name);
    if (!m) {
        // Singleton-table fallback for class/module receivers.
        if (!RB_SPECIAL_CONST_P(self)) {
            struct abruby_header *h = (struct abruby_header *)ABRUBY_DATA_PTR(self);
            if (h->klass == c->abm->class_class || h->klass == c->abm->module_class) {
                m = abruby_find_method((struct abruby_class *)h, name);
            }
        }
    }
    if (!m) {
        VALUE exc;
        VALUE mstr = rb_str_new_cstr("undefined method `");
        rb_str_cat_cstr(mstr, rb_id2name(name));
        rb_str_cat_cstr(mstr, "'");
        exc = abruby_exception_new(c, c->current_frame, mstr);
        return (RESULT){exc, RESULT_RAISE};
    }
    return abruby_call_method(c, self, m, argc - 1, argv + 1);
}

// Object#freeze / frozen? — abruby has no frozen state, so freeze is
// a no-op returning self and frozen? returns false.
static RESULT ab_object_freeze(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return RESULT_OK(self);
}
static RESULT ab_object_frozen_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)self; (void)argc; (void)argv;
    return RESULT_OK(Qfalse);
}
static RESULT ab_object_tap(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc; (void)argv;
    RESULT r = abruby_yield(c, 1, &self);
    if (r.state != RESULT_NORMAL) return r;
    return RESULT_OK(self);
}
static RESULT ab_object_object_id(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    // Use the VALUE itself as a stand-in object_id (good enough for
    // the few uses in optcarrot logging).
    return RESULT_OK(LONG2FIX((long)(self >> 1)));
}

// Object#method(name) — return a bound method object.  Used by
// optcarrot to grab references like `@ram.method(:[]=)` for fast
// dispatch from a generated method-dispatch table.
extern VALUE abruby_bound_method_new(CTX *c, VALUE recv, ID name);
static RESULT ab_object_method(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc;
    VALUE n = argv[0];
    ID name;
    if (ab_obj_type_p(n, ABRUBY_OBJ_SYMBOL)) name = SYM2ID(ab_sym_unwrap(n));
    else if (ab_obj_type_p(n, ABRUBY_OBJ_STRING)) name = rb_intern_str(RSTR(n));
    else {
        VALUE exc = abruby_exception_new(c, c->current_frame,
            abruby_str_new_cstr(c, "method name must be a Symbol or String"));
        return (RESULT){exc, RESULT_RAISE};
    }
    return RESULT_OK(abruby_bound_method_new(c, self, name));
}

// Object#respond_to?(name, include_private=false)
static RESULT ab_object_respond_to_p(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)argc;
    VALUE n = argv[0];
    ID id;
    if (ab_obj_type_p(n, ABRUBY_OBJ_SYMBOL)) id = SYM2ID(ab_sym_unwrap(n));
    else if (ab_obj_type_p(n, ABRUBY_OBJ_STRING)) id = rb_intern_str(RSTR(n));
    else return RESULT_OK(Qfalse);
    const struct abruby_method *m = abruby_find_method(AB_CLASS_OF(c, self), id);
    if (m) return RESULT_OK(Qtrue);
    // Also check the receiver's own methods table if it's a class /
    // module — that's where `def self.foo` lives in abruby.
    if (!RB_SPECIAL_CONST_P(self)) {
        struct abruby_header *h = (struct abruby_header *)ABRUBY_DATA_PTR(self);
        if (h->klass == c->abm->class_class || h->klass == c->abm->module_class) {
            const struct abruby_class *k = (struct abruby_class *)h;
            if (abruby_find_method(k, id)) return RESULT_OK(Qtrue);
        }
    }
    return RESULT_OK(Qfalse);
}

// Object#instance_variable_get(name) — name is symbol or string
static RESULT ab_object_ivar_get(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE n = argv[0];
    ID id;
    if (ab_obj_type_p(n, ABRUBY_OBJ_SYMBOL)) id = SYM2ID(ab_sym_unwrap(n));
    else if (ab_obj_type_p(n, ABRUBY_OBJ_STRING)) id = rb_intern_str(RSTR(n));
    else return RESULT_OK(Qnil);
    return RESULT_OK(abruby_ivar_get(self, id));
}
// Object#instance_variable_set(name, value)
static RESULT ab_object_ivar_set(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    (void)c; (void)argc;
    VALUE n = argv[0];
    ID id;
    if (ab_obj_type_p(n, ABRUBY_OBJ_SYMBOL)) id = SYM2ID(ab_sym_unwrap(n));
    else if (ab_obj_type_p(n, ABRUBY_OBJ_STRING)) id = rb_intern_str(RSTR(n));
    else return RESULT_OK(Qnil);
    abruby_ivar_set(c, self, id, argv[1]);
    return RESULT_OK(argv[1]);
}

static RESULT ab_object_class_name(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(abruby_str_new_cstr(c, rb_id2name(AB_CLASS_OF(c, self)->name)));
}

static RESULT ab_object_not(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    return RESULT_OK(RTEST(self) ? Qfalse : Qtrue);
}

// Object#=== defaults to ==
static RESULT ab_object_case_eq(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_method *eq = abruby_find_method(AB_CLASS_OF(c, self), rb_intern("=="));
    return abruby_call_method(c, self, eq, 1, argv);
}

static RESULT ab_object_is_a(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_class *obj_class = AB_CLASS_OF(c, self);
    const struct abruby_class *check_class = abruby_unwrap_class(argv[0]);
    while (obj_class) {
        if (obj_class == check_class) return RESULT_OK(Qtrue);
        obj_class = obj_class->super;
    }
    return RESULT_OK(Qfalse);
}

static RESULT ab_object_instance_of(CTX *c, VALUE self, unsigned int argc, VALUE *argv) {
    const struct abruby_class *obj_class = AB_CLASS_OF(c, self);
    const struct abruby_class *check_class = abruby_unwrap_class(argv[0]);
    return RESULT_OK(obj_class == check_class ? Qtrue : Qfalse);
}

void
Init_abruby_object(void)
{
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("inspect"),  ab_object_inspect,    0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("to_s"),     ab_object_to_s,       0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("=="),       ab_object_eq,         1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("!="),       ab_object_neq,        1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("nil?"),     ab_object_nil_p,      0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("class"),    ab_object_class_name, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("instance_variable_get"), ab_object_ivar_get, 1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("instance_variable_set"), ab_object_ivar_set, 2);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("respond_to?"),           ab_object_respond_to_p, 1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("method"),                ab_object_method, 1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("freeze"),                ab_object_freeze, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("frozen?"),               ab_object_frozen_p, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("tap"),                   ab_object_tap, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("object_id"),             ab_object_object_id, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("equal?"),                ab_object_equal_p, 1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("dup"),                   ab_object_dup, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("hash"),                  ab_object_hash, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("send"),                  ab_object_send, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("__send__"),              ab_object_send, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("public_send"),           ab_object_send, 0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("!"),        ab_object_not,        0);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("==="),       ab_object_case_eq,    1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("is_a?"),     ab_object_is_a,       1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("kind_of?"),  ab_object_is_a,       1);
    abruby_class_add_cfunc(ab_tmpl_object_class, rb_intern("instance_of?"), ab_object_instance_of, 1);
}
