/* The C-API conformance extension: built unchanged against CRuby's headers and
 * against koruby's cext/include/ruby.h; api_test.rb prints what each entry
 * returns, and run.sh diffs the two outputs (plus a STRESS+PURGE koruby run).
 * Each T.<name> exercises one API area; errors are observed from Ruby. */
#include <ruby.h>
#include <errno.h>
#include <string.h>

static VALUE mT;

/* ---- immediates / numbers ---- */
static VALUE
t_imm(VALUE self)
{
    return rb_ary_new_from_args(12, Qnil, Qtrue, Qfalse, INT2FIX(42), LONG2NUM(-5),
                                RTEST(Qnil) ? Qtrue : Qfalse, RTEST(Qfalse) ? Qtrue : Qfalse, RTEST(INT2FIX(0)) ? Qtrue : Qfalse,
                                NIL_P(Qnil) ? Qtrue : Qfalse, FIXNUM_P(INT2FIX(7)) ? Qtrue : Qfalse,
                                SPECIAL_CONST_P(Qnil) ? Qtrue : Qfalse, SPECIAL_CONST_P(rb_str_new_cstr("x")) ? Qtrue : Qfalse);
}
static VALUE t_num2long(VALUE self, VALUE v) { return LONG2NUM(NUM2LONG(v)); }
static VALUE t_num2int(VALUE self, VALUE v) { return INT2NUM(NUM2INT(v)); }
static VALUE t_num2dbl(VALUE self, VALUE v) { return DBL2NUM(NUM2DBL(v) * 2); }
static VALUE t_num2ulong(VALUE self, VALUE v) { return ULONG2NUM(NUM2ULONG(v)); }
static VALUE
t_bignums(VALUE self)
{
    return rb_ary_new_from_args(6, LONG2NUM(LONG_MAX), LONG2NUM(LONG_MIN), ULONG2NUM(ULONG_MAX),
                                LL2NUM(-1234567890123LL), DBL2NUM(1.5), DBL2NUM(1e300));
}
static VALUE t_fix2long(VALUE self, VALUE v) { return FIXNUM_P(v) ? LONG2NUM(FIX2LONG(v) + 1) : Qnil; }

/* ---- types ---- */
static VALUE
t_type_of(VALUE self, VALUE v)
{
    switch (TYPE(v)) {
      case T_NIL: return rb_str_new_cstr("nil");
      case T_TRUE: return rb_str_new_cstr("true");
      case T_FALSE: return rb_str_new_cstr("false");
      case T_FIXNUM: return rb_str_new_cstr("fixnum");
      case T_BIGNUM: return rb_str_new_cstr("bignum");
      case T_FLOAT: return rb_str_new_cstr("float");
      case T_SYMBOL: return rb_str_new_cstr("symbol");
      case T_STRING: return rb_str_new_cstr("string");
      case T_ARRAY: return rb_str_new_cstr("array");
      case T_HASH: return rb_str_new_cstr("hash");
      case T_CLASS: return rb_str_new_cstr("class");
      case T_MODULE: return rb_str_new_cstr("module");
      case T_OBJECT: return rb_str_new_cstr("object");
      case T_DATA: return rb_str_new_cstr("data");
      case T_REGEXP: return rb_str_new_cstr("regexp");
      default: return rb_str_new_cstr("other");
    }
}
static VALUE t_check_string(VALUE self, VALUE v) { Check_Type(v, T_STRING); return Qtrue; }
static VALUE t_type_p(VALUE self, VALUE v) { return rb_ary_new_from_args(3, RB_TYPE_P(v, T_STRING) ? Qtrue : Qfalse, RB_TYPE_P(v, T_ARRAY) ? Qtrue : Qfalse, RB_INTEGER_TYPE_P(v) ? Qtrue : Qfalse); }

/* ---- strings ---- */
static VALUE
t_str_new(VALUE self)
{
    VALUE a = rb_str_new("a\0b", 3);
    VALUE b = rb_str_new_cstr("hello");
    VALUE c = rb_usascii_str_new("us", 2);
    VALUE d = rb_utf8_str_new("\xc3\xa9", 2);
    VALUE e = rb_str_buf_new(100);
    rb_str_buf_cat2(e, "buf");
    rb_str_cat(e, "+cat", 4);
    rb_str_cat2(e, "!");
    return rb_ary_new_from_args(5, a, b, c, d, e);
}
static VALUE
t_str_encodings(VALUE self)
{
    VALUE ary = rb_ary_new();
    rb_ary_push(ary, rb_funcall(rb_str_new("x", 1), rb_intern("encoding"), 0));
    rb_ary_push(ary, rb_funcall(rb_str_new_cstr("x"), rb_intern("encoding"), 0));
    rb_ary_push(ary, rb_funcall(rb_usascii_str_new_cstr("x"), rb_intern("encoding"), 0));
    rb_ary_push(ary, rb_funcall(rb_utf8_str_new_cstr("x"), rb_intern("encoding"), 0));
    return ary;
}
static VALUE
t_str_upcase_bang(VALUE self, VALUE s)
{
    StringValue(s);
    rb_check_frozen(s);
    char *p = RSTRING_PTR(s);
    for (long i = 0; i < RSTRING_LEN(s); i++) if (p[i] >= 'a' && p[i] <= 'z') p[i] -= 32;
    return s;
}
static VALUE
t_str_ptr_len(VALUE self, VALUE s)
{
    return rb_ary_new_from_args(2, LONG2NUM(RSTRING_LEN(s)), rb_str_new(RSTRING_PTR(s), RSTRING_LEN(s)));
}
static VALUE t_string_value(VALUE self, VALUE s) { StringValue(s); return s; }
static VALUE t_string_value_cstr(VALUE self, VALUE s) { return rb_str_new_cstr(StringValueCStr(s)); }
static VALUE
t_str_ops(VALUE self, VALUE a, VALUE b)
{
    VALUE d = rb_str_dup(a);
    rb_str_append(d, b);
    VALUE r = rb_str_resize(rb_str_dup(a), 2);
    VALUE big = rb_str_resize(rb_str_new_cstr("ab"), 5);
    return rb_ary_new_from_args(7, INT2FIX(rb_str_cmp(a, b)), d, rb_str_substr(a, 1, 2), r, LONG2NUM(RSTRING_LEN(big)),
                                rb_str_plus(a, b), rb_str_equal(a, rb_str_dup(a)));
}
static VALUE
t_str_set_len(VALUE self)
{
    VALUE s = rb_str_buf_new(10);
    rb_str_resize(s, 8);
    memcpy(RSTRING_PTR(s), "abcdefgh", 8);
    rb_str_set_len(s, 5);
    return s;
}
static VALUE
t_sprintf(VALUE self, VALUE v)
{
    return rb_ary_new_from_args(4,
        rb_sprintf("[%d|%5.2f|%s|%c|%%|%lu|%zu]", -3, 3.14159, "str", 'Z', 7UL, (size_t)9),
        rb_sprintf("v=%"PRIsVALUE" i=%+"PRIsVALUE, v, v),
        rb_sprintf("%-5s|%05d|%x|%lld", "ab", 42, 255, -9LL),
        rb_str_catf(rb_str_new_cstr("pre:"), "%d-%s", 1, "x"));
}
/* The pointer RSTRING_PTR hands out stays put across collections. */
static VALUE
t_pin_across_gc(VALUE self, VALUE s)
{
    const char *p1 = RSTRING_PTR(s);
    char copy[64];
    long n = RSTRING_LEN(s) < 63 ? RSTRING_LEN(s) : 63;
    memcpy(copy, p1, n);
    for (int i = 0; i < 2000; i++) rb_ary_new_from_args(2, rb_str_new_cstr("garbage"), INT2FIX(i));
    rb_gc();
    for (int i = 0; i < 2000; i++) rb_str_new_cstr("more garbage");
    const char *p2 = RSTRING_PTR(s);
    return rb_ary_new_from_args(2, p1 == p2 ? Qtrue : Qfalse, memcmp(p1, copy, n) == 0 ? Qtrue : Qfalse);
}

/* ---- arrays / hashes ---- */
static VALUE
t_ary(VALUE self)
{
    VALUE a = rb_ary_new();
    for (int i = 0; i < 5; i++) rb_ary_push(a, INT2FIX(i * 10));
    VALUE vals[3] = { INT2FIX(1), rb_str_new_cstr("two"), Qnil };
    VALUE b = rb_ary_new_from_values(3, vals);
    rb_ary_store(a, 7, INT2FIX(70));
    VALUE popped = rb_ary_pop(a), shifted = rb_ary_shift(a);
    rb_ary_unshift(a, INT2FIX(-1));
    VALUE e = rb_ary_new_from_args(4, rb_ary_entry(a, 0), rb_ary_entry(a, -1), rb_ary_entry(a, 100), LONG2NUM(RARRAY_LEN(a)));
    VALUE c = rb_ary_dup(b);
    rb_ary_concat(c, rb_ary_new_from_args(1, INT2FIX(9)));
    return rb_ary_new_from_args(8, a, b, popped, shifted, e, c, rb_ary_join(b, rb_str_new_cstr("-")),
                                rb_ary_includes(b, rb_str_new_cstr("two")));
}
static VALUE t_check_ary(VALUE self, VALUE v) { return rb_check_array_type(v); }
static VALUE t_to_ary(VALUE self, VALUE v) { return rb_Array(v); }
static int
foreach_cb(VALUE k, VALUE v, VALUE acc)
{
    rb_ary_push(acc, rb_ary_new_from_args(2, k, v));
    if (k == ID2SYM(rb_intern("stop"))) return ST_STOP;
    if (k == ID2SYM(rb_intern("del"))) return ST_DELETE;
    return ST_CONTINUE;
}
static VALUE
t_hash(VALUE self, VALUE h)
{
    VALUE n = rb_hash_new();
    rb_hash_aset(n, rb_str_new_cstr("k"), INT2FIX(1));
    rb_hash_aset(n, ID2SYM(rb_intern("s")), INT2FIX(2));
    VALUE acc = rb_ary_new();
    rb_hash_foreach(h, foreach_cb, acc);
    return rb_ary_new_from_args(9, n, rb_hash_aref(n, rb_str_new_cstr("k")), rb_hash_aref(h, ID2SYM(rb_intern("nope"))),
                                rb_hash_lookup(h, ID2SYM(rb_intern("nope"))), rb_hash_lookup2(h, ID2SYM(rb_intern("nope")), INT2FIX(-1)),
                                rb_hash_size(n), LONG2NUM((long)RHASH_SIZE(h)), acc, rb_funcall(n, rb_intern("keys"), 0));
}
static VALUE t_hash_fetch(VALUE self, VALUE h, VALUE k) { return rb_hash_fetch(h, k); }
static VALUE t_hash_delete(VALUE self, VALUE h, VALUE k) { VALUE r = rb_hash_delete(h, k); return rb_ary_new_from_args(2, r, h); }
static VALUE t_check_hash(VALUE self, VALUE v) { return rb_check_hash_type(v); }

/* ---- calls / blocks ---- */
static VALUE t_funcall(VALUE self, VALUE recv) { return rb_funcall(recv, rb_intern("+"), 1, INT2FIX(1)); }
static VALUE t_funcallv(VALUE self, VALUE recv, VALUE mid, VALUE args)
{
    long n = RARRAY_LEN(args);
    VALUE *argv = ALLOCA_N(VALUE, n > 0 ? n : 1);
    for (long i = 0; i < n; i++) argv[i] = rb_ary_entry(args, i);
    return rb_funcallv(recv, SYM2ID(mid), (int)n, argv);
}
static VALUE t_funcall_public(VALUE self, VALUE recv, VALUE mid) { return rb_funcallv_public(recv, SYM2ID(mid), 0, NULL); }
static VALUE t_block_given(VALUE self) { return rb_block_given_p() ? Qtrue : Qfalse; }
static VALUE t_yield(VALUE self, VALUE v) { return rb_yield(v); }
static VALUE t_yield_values(VALUE self) { return rb_yield_values(3, INT2FIX(1), INT2FIX(2), INT2FIX(3)); }
static VALUE t_yield_splat(VALUE self, VALUE a) { return rb_yield_splat(a); }
static VALUE t_block_proc(VALUE self) { VALUE p = rb_block_proc(); return rb_funcall(p, rb_intern("call"), 1, INT2FIX(5)); }
static VALUE t_need_block(VALUE self) { rb_need_block(); return Qtrue; }
static VALUE t_passing_block(VALUE self, VALUE ary) { return rb_funcall_passing_block(ary, rb_intern("map"), 0, NULL); }
static VALUE
t_yield_each(VALUE self, VALUE n)
{
    long i, max = NUM2LONG(n);
    for (i = 0; i < max; i++) rb_yield(LONG2NUM(i));
    return rb_str_new_cstr("finished");
}

/* ---- exceptions ---- */
static VALUE t_raise(VALUE self, VALUE klass, VALUE v) { rb_raise(klass, "boom %d %"PRIsVALUE" %+"PRIsVALUE, 7, v, v); return Qnil; }
static VALUE raise_thunk(VALUE arg) { rb_raise(rb_eArgError, "inner %s", (const char *)arg); return Qnil; }
static VALUE ruby_raise_thunk(VALUE recv) { return rb_funcall(recv, rb_intern("go"), 0); }
static VALUE ok_thunk(VALUE arg) { return rb_ary_new_from_args(1, arg); }
static VALUE
t_protect(VALUE self, VALUE recv)
{
    int state = 0;
    VALUE r1 = rb_protect(raise_thunk, (VALUE)"msg", &state);
    int s1 = state;
    VALUE e1 = rb_errinfo();
    rb_set_errinfo(Qnil);
    rb_protect(ruby_raise_thunk, recv, &state);
    int s2 = state;
    VALUE e2 = rb_errinfo();
    rb_set_errinfo(Qnil);
    VALUE r3 = rb_protect(ok_thunk, INT2FIX(3), &state);
    return rb_ary_new_from_args(8, r1, s1 ? Qtrue : Qfalse, rb_funcall(e1, rb_intern("message"), 0), rb_obj_class(e1),
                                s2 ? Qtrue : Qfalse, rb_obj_class(e2), r3, state ? Qtrue : Qfalse);
}
static VALUE
t_protect_rethrow(VALUE self)
{
    int state = 0;
    rb_protect(raise_thunk, (VALUE)"again", &state);
    if (state) rb_jump_tag(state);
    return Qnil;
}
static VALUE log_ary;
static VALUE ensure_body(VALUE arg) { rb_ary_push(log_ary, rb_str_new_cstr("body")); if (RTEST(arg)) rb_raise(rb_eRuntimeError, "in body"); return INT2FIX(1); }
static VALUE ensure_fin(VALUE arg) { rb_ary_push(log_ary, rb_str_new_cstr("ensure")); return Qnil; }
static VALUE
t_ensure(VALUE self, VALUE do_raise)
{
    log_ary = rb_ary_new();
    rb_gc_register_address(&log_ary);
    VALUE r = rb_ensure(ensure_body, do_raise, ensure_fin, Qnil);
    rb_ary_push(log_ary, r);
    return log_ary;
}
static VALUE t_ensure_log(VALUE self) { return log_ary; }
static VALUE rescue_handler(VALUE arg, VALUE exc) { return rb_ary_new_from_args(3, rb_str_new_cstr("rescued"), arg, rb_funcall(exc, rb_intern("message"), 0)); }
static VALUE raise_klass_thunk(VALUE klass) { rb_raise(klass, "raised %s", rb_class2name(klass)); return Qnil; }
static VALUE t_rescue(VALUE self, VALUE klass) { return rb_rescue(raise_klass_thunk, klass, rescue_handler, INT2FIX(9)); }
static VALUE t_rescue2(VALUE self, VALUE klass) { return rb_rescue2(raise_klass_thunk, klass, rescue_handler, INT2FIX(8), rb_eTypeError, rb_eKeyError, (VALUE)0); }
static VALUE t_exc_new(VALUE self) { return rb_exc_new_str(rb_eKeyError, rb_str_new_cstr("made")); }
static VALUE t_exc_raise(VALUE self, VALUE e) { rb_exc_raise(e); return Qnil; }
static VALUE t_sys_fail(VALUE self) { errno = ENOENT; rb_sys_fail("/no/such/path"); return Qnil; }
static VALUE t_error_arity(VALUE self, VALUE argc) { rb_error_arity(NUM2INT(argc), 1, 3); return Qnil; }
static VALUE t_check_arity(VALUE self, VALUE argc) { rb_check_arity(NUM2INT(argc), 2, UNLIMITED_ARGUMENTS); return Qtrue; }
static VALUE t_frozen(VALUE self, VALUE v) { rb_check_frozen(v); return rb_obj_frozen_p(v); }
static VALUE nest_inner(VALUE arg) { return rb_yield(arg); }
static VALUE
t_protect_yield(VALUE self)
{
    int state = 0;
    VALUE r = rb_protect(nest_inner, INT2FIX(1), &state);
    if (state) rb_jump_tag(state);   /* raise / break / throw continue past the C frame */
    return rb_ary_new_from_args(2, rb_str_new_cstr("returned"), r);
}

/* ---- classes, methods, ivars, constants ---- */
static VALUE m0(VALUE self) { return rb_str_new_cstr("m0"); }
static VALUE m1(VALUE self, VALUE a) { return rb_ary_new_from_args(2, rb_str_new_cstr("m1"), a); }
static VALUE m3(VALUE self, VALUE a, VALUE b, VALUE c) { return rb_ary_new_from_args(3, a, b, c); }
static VALUE mv(int argc, VALUE *argv, VALUE self) { return rb_ary_new_from_values(argc, argv); }
static VALUE ma(VALUE self, VALUE args) { return rb_ary_new_from_args(2, rb_str_new_cstr("ma"), args); }
static VALUE mpriv(VALUE self) { return rb_str_new_cstr("private!"); }
static VALUE msing(VALUE self) { return rb_str_new_cstr("singleton"); }
static VALUE mfun(VALUE self, VALUE x) { return rb_funcall(x, rb_intern("*"), 1, INT2FIX(2)); }
static VALUE mself(VALUE self) { return self; }
static VALUE
t_define(VALUE self)
{
    VALUE base = rb_define_class_under(mT, "Base", rb_cObject);
    VALUE sub = rb_define_class_under(mT, "Sub", base);
    VALUE mod = rb_define_module_under(mT, "Mixin");
    rb_define_method(base, "m0", m0, 0);
    rb_define_method(base, "m1", m1, 1);
    rb_define_method(base, "m3", m3, 3);
    rb_define_method(base, "mv", mv, -1);
    rb_define_method(base, "ma", ma, -2);
    rb_define_method(base, "mself", mself, 0);
    rb_define_private_method(base, "mpriv", mpriv, 0);
    rb_define_singleton_method(base, "sing", msing, 0);
    rb_define_module_function(mod, "twice", mfun, 1);
    rb_define_method(mod, "from_mixin", m0, 0);
    rb_include_module(sub, mod);
    rb_define_alias(base, "m0_alias", "m0");
    rb_define_attr(base, "attr_x", 1, 1);
    rb_define_const(base, "ANSWER", INT2FIX(42));
    VALUE again = rb_define_class_under(mT, "Base", rb_cObject);   /* reopen: the same class */
    return rb_ary_new_from_args(4, base, sub, mod, again == base ? Qtrue : Qfalse);
}
static VALUE t_redefine_mismatch(VALUE self) { rb_define_class_under(mT, "Plain", rb_cObject); return rb_define_class_under(mT, "Plain", rb_cString); }
static VALUE t_define_not_class(VALUE self) { rb_define_const(mT, "NOT_A_CLASS", INT2FIX(1)); return rb_define_class_under(mT, "NOT_A_CLASS", rb_cObject); }
static VALUE
t_class_info(VALUE self, VALUE obj)
{
    VALUE k = rb_obj_class(obj);
    return rb_ary_new_from_args(7, k, rb_class_name(k), rb_str_new_cstr(rb_class2name(k)), rb_str_new_cstr(rb_obj_classname(obj)),
                                rb_obj_is_kind_of(obj, rb_cObject), rb_obj_is_instance_of(obj, k),
                                rb_respond_to(obj, rb_intern("m0")) ? Qtrue : Qfalse);
}
static VALUE
t_consts(VALUE self)
{
    VALUE base = rb_const_get(mT, rb_intern("Base"));
    return rb_ary_new_from_args(6, rb_const_get(base, rb_intern("ANSWER")), rb_const_defined(mT, rb_intern("Base")) ? Qtrue : Qfalse,
                                rb_const_defined(mT, rb_intern("Nope")) ? Qtrue : Qfalse, rb_path2class("CextAPITest::Sub"),
                                rb_const_get_at(mT, rb_intern("Mixin")), rb_class_superclass(rb_path2class("CextAPITest::Sub")));
}
static VALUE t_path2class_bad(VALUE self) { return rb_path2class("CextAPITest::Base::ANSWER"); }
static VALUE
t_ivars(VALUE self, VALUE obj)
{
    rb_ivar_set(obj, rb_intern("@a"), INT2FIX(1));
    rb_iv_set(obj, "@b", rb_str_new_cstr("bee"));
    rb_ivar_set(obj, rb_intern("hidden"), INT2FIX(99));   /* no '@': invisible from Ruby */
    return rb_ary_new_from_args(6, rb_ivar_get(obj, rb_intern("@a")), rb_iv_get(obj, "@b"), rb_ivar_get(obj, rb_intern("@zz")),
                                rb_ivar_defined(obj, rb_intern("@a")), rb_ivar_defined(obj, rb_intern("@zz")),
                                rb_ivar_get(obj, rb_intern("hidden")));
}
static VALUE t_class_ivar(VALUE self, VALUE k) { rb_ivar_set(k, rb_intern("meta"), rb_str_new_cstr("classmeta")); return rb_ivar_get(k, rb_intern("meta")); }

/* ---- T_DATA ---- */
static long dfree_count;
struct counter { long n; VALUE stash; };
static void counter_mark(void *p) { rb_gc_mark(((struct counter *)p)->stash); }
static void counter_free(void *p) { dfree_count++; xfree(p); }
static size_t counter_size(const void *p) { return sizeof(struct counter); }
static const rb_data_type_t counter_type = {
    "cext_api_test/counter",
    { counter_mark, counter_free, counter_size, },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY,
};
static VALUE
counter_alloc(VALUE klass)
{
    struct counter *c;
    VALUE obj = TypedData_Make_Struct(klass, struct counter, &counter_type, c);
    c->n = 0; c->stash = Qnil;
    return obj;
}
static struct counter *getc_(VALUE self) { struct counter *c; TypedData_Get_Struct(self, struct counter, &counter_type, c); return c; }
static VALUE counter_init(int argc, VALUE *argv, VALUE self) { VALUE n; rb_scan_args(argc, argv, "01", &n); getc_(self)->n = NIL_P(n) ? 0 : NUM2LONG(n); return self; }
static VALUE counter_incr(VALUE self) { getc_(self)->n++; return self; }
static VALUE counter_value(VALUE self) { return LONG2NUM(getc_(self)->n); }
static VALUE counter_stash(VALUE self, VALUE v) { getc_(self)->stash = v; return v; }
static VALUE counter_stashed(VALUE self) { return getc_(self)->stash; }
static VALUE t_dfree_count(VALUE self) { return LONG2NUM(dfree_count); }
static VALUE t_get_counter(VALUE self, VALUE v) { return LONG2NUM(getc_(v)->n); }
static VALUE t_data_p(VALUE self, VALUE v) { return rb_ary_new_from_args(2, RB_TYPE_P(v, T_DATA) ? Qtrue : Qfalse, rb_typeddata_is_kind_of(v, &counter_type) ? Qtrue : Qfalse); }
/* ---- handles and GC ---- */
static VALUE g_registered, g_global;
static VALUE
t_identity(VALUE self, VALUE obj)
{
    VALUE a = rb_funcall(obj, rb_intern("itself"), 0);
    VALUE k1 = rb_obj_class(obj);
    for (int i = 0; i < 3000; i++) rb_str_new_cstr("churn");
    rb_gc();
    VALUE k2 = rb_obj_class(obj);
    VALUE b = rb_funcall(obj, rb_intern("itself"), 0);
    return rb_ary_new_from_args(3, a == obj ? Qtrue : Qfalse, k1 == k2 ? Qtrue : Qfalse, b == obj ? Qtrue : Qfalse);
}
static VALUE t_store_globals(VALUE self, VALUE a, VALUE b) { g_registered = a; g_global = b; return Qnil; }
static VALUE t_load_globals(VALUE self) { return rb_ary_new_from_args(2, g_registered, g_global); }
static VALUE
t_gc_churn(VALUE self, VALUE n)
{
    VALUE keep = rb_ary_new();
    for (long i = 0; i < NUM2LONG(n); i++) {
        VALUE s = rb_str_new_cstr("x");
        if (i % 100 == 0) rb_ary_push(keep, s);
    }
    rb_gc();
    return LONG2NUM(RARRAY_LEN(keep));
}
static VALUE t_values_across_gc(VALUE self, VALUE s, VALUE a, VALUE h)
{
    rb_gc();
    for (int i = 0; i < 1000; i++) rb_ary_new_from_args(1, INT2FIX(i));
    rb_gc();
    return rb_ary_new_from_args(3, rb_str_dup(s), rb_ary_dup(a), rb_hash_aref(h, INT2FIX(1)));
}
static VALUE t_nested(VALUE self, VALUE obj) { return rb_funcall(obj, rb_intern("call"), 1, rb_str_new_cstr("from C")); }

/* ---- scan_args / kwargs ---- */
static VALUE t_scan_11(int argc, VALUE *argv, VALUE self) { VALUE a, b; int n = rb_scan_args(argc, argv, "11", &a, &b); return rb_ary_new_from_args(3, INT2FIX(n), a, b); }
static VALUE t_scan_rest(int argc, VALUE *argv, VALUE self) { VALUE a, r, t; rb_scan_args(argc, argv, "1*1", &a, &r, &t); return rb_ary_new_from_args(3, a, r, t); }
static VALUE t_scan_02(int argc, VALUE *argv, VALUE self) { VALUE a, b; rb_scan_args(argc, argv, "02", &a, &b); return rb_ary_new_from_args(2, a, b); }
static VALUE t_scan_kw(int argc, VALUE *argv, VALUE self) { VALUE a, kw; rb_scan_args(argc, argv, "1:", &a, &kw); return rb_ary_new_from_args(2, a, kw); }
static VALUE t_scan_blk(int argc, VALUE *argv, VALUE self) { VALUE a, blk; rb_scan_args(argc, argv, "1&", &a, &blk); return rb_ary_new_from_args(2, a, NIL_P(blk) ? Qnil : rb_funcall(blk, rb_intern("call"), 1, a)); }
static VALUE
t_get_kwargs(int argc, VALUE *argv, VALUE self)
{
    VALUE opts = Qnil;
    rb_scan_args(argc, argv, ":", &opts);
    ID tbl[3] = { rb_intern("req"), rb_intern("opt1"), rb_intern("opt2") };
    VALUE vals[3];
    rb_get_kwargs(opts, tbl, 1, 2, vals);
    return rb_ary_new_from_args(3, vals[0], vals[1] == Qundef ? rb_str_new_cstr("undef") : vals[1], vals[2] == Qundef ? rb_str_new_cstr("undef") : vals[2]);
}

/* ---- C-function blocks / super ---- */
static VALUE
collect_cb(RB_BLOCK_CALL_FUNC_ARGLIST(y, data))
{
    rb_ary_push(data, rb_ary_new_from_args(2, y, INT2FIX(argc)));
    return rb_funcall(y, rb_intern("*"), 1, INT2FIX(10));
}
static VALUE
t_block_call(VALUE self, VALUE obj)
{
    VALUE log = rb_ary_new();
    VALUE r = rb_block_call(obj, rb_intern("map"), 0, NULL, collect_cb, log);
    return rb_ary_new_from_args(2, r, log);
}
static VALUE
pairs_cb(RB_BLOCK_CALL_FUNC_ARGLIST(y, data))
{
    rb_ary_push(data, rb_ary_new_from_values(argc, argv));
    return Qnil;
}
static VALUE
t_block_call_multi(VALUE self, VALUE h)
{
    VALUE log = rb_ary_new();
    rb_block_call(h, rb_intern("each_pair"), 0, NULL, pairs_cb, log);
    return log;
}
static VALUE adder_cb(RB_BLOCK_CALL_FUNC_ARGLIST(y, data)) { return rb_funcall(y, rb_intern("+"), 1, data); }
static VALUE t_proc_new(VALUE self, VALUE n) { return rb_proc_new(adder_cb, n); }
static VALUE t_with_block(VALUE self, VALUE ary, VALUE blk) { return rb_funcall_with_block(ary, rb_intern("map"), 0, NULL, blk); }
static VALUE child_greet(int argc, VALUE *argv, VALUE self) { VALUE r = rb_call_super(argc, argv); return rb_ary_new_from_args(2, rb_str_new_cstr("C"), r); }
static VALUE t_def_child(VALUE self, VALUE child) { rb_define_method(child, "greet", child_greet, -1); return Qnil; }

/* ---- misc ---- */
static VALUE
t_ids(VALUE self)
{
    ID id = rb_intern("some_name");
    VALUE sym = ID2SYM(id);
    return rb_ary_new_from_args(6, sym, rb_str_new_cstr(rb_id2name(id)), rb_id2str(id), SYM2ID(sym) == id ? Qtrue : Qfalse,
                                rb_to_id(rb_str_new_cstr("some_name")) == id ? Qtrue : Qfalse, rb_sym2str(sym));
}
static VALUE t_eval(VALUE self, VALUE src) { return rb_eval_string(StringValueCStr(src)); }
static VALUE t_gvar(VALUE self) { rb_gv_set("$cext_api_gv", INT2FIX(123)); return rb_gv_get("$cext_api_gv"); }
static VALUE t_inspect(VALUE self, VALUE v) { return rb_ary_new_from_args(3, rb_inspect(v), rb_obj_as_string(v), rb_equal(v, v)); }
static VALUE t_freeze(VALUE self, VALUE v) { rb_obj_freeze(v); return rb_obj_frozen_p(v); }
static VALUE t_new_instance(VALUE self, VALUE k, VALUE a) { VALUE argv[1] = { a }; return rb_class_new_instance(1, argv, k); }
static VALUE t_require(VALUE self, VALUE f) { return rb_require(StringValueCStr(f)); }

void
Init_cext_api_test(void)
{
    mT = rb_define_module("CextAPITest");
    rb_gc_register_address(&g_registered);
    rb_global_variable(&g_global);
    rb_define_module_function(mT, "imm", t_imm, 0);
    rb_define_module_function(mT, "num2long", t_num2long, 1);
    rb_define_module_function(mT, "num2int", t_num2int, 1);
    rb_define_module_function(mT, "num2dbl", t_num2dbl, 1);
    rb_define_module_function(mT, "num2ulong", t_num2ulong, 1);
    rb_define_module_function(mT, "bignums", t_bignums, 0);
    rb_define_module_function(mT, "fix2long", t_fix2long, 1);
    rb_define_module_function(mT, "type_of", t_type_of, 1);
    rb_define_module_function(mT, "check_string", t_check_string, 1);
    rb_define_module_function(mT, "type_p", t_type_p, 1);
    rb_define_module_function(mT, "str_new", t_str_new, 0);
    rb_define_module_function(mT, "str_encodings", t_str_encodings, 0);
    rb_define_module_function(mT, "str_upcase!", t_str_upcase_bang, 1);
    rb_define_module_function(mT, "str_ptr_len", t_str_ptr_len, 1);
    rb_define_module_function(mT, "string_value", t_string_value, 1);
    rb_define_module_function(mT, "string_value_cstr", t_string_value_cstr, 1);
    rb_define_module_function(mT, "str_ops", t_str_ops, 2);
    rb_define_module_function(mT, "str_set_len", t_str_set_len, 0);
    rb_define_module_function(mT, "sprintf", t_sprintf, 1);
    rb_define_module_function(mT, "pin_across_gc", t_pin_across_gc, 1);
    rb_define_module_function(mT, "ary", t_ary, 0);
    rb_define_module_function(mT, "check_ary", t_check_ary, 1);
    rb_define_module_function(mT, "to_ary", t_to_ary, 1);
    rb_define_module_function(mT, "hash", t_hash, 1);
    rb_define_module_function(mT, "hash_fetch", t_hash_fetch, 2);
    rb_define_module_function(mT, "hash_delete", t_hash_delete, 2);
    rb_define_module_function(mT, "check_hash", t_check_hash, 1);
    rb_define_module_function(mT, "funcall", t_funcall, 1);
    rb_define_module_function(mT, "funcallv", t_funcallv, 3);
    rb_define_module_function(mT, "funcall_public", t_funcall_public, 2);
    rb_define_module_function(mT, "block_given", t_block_given, 0);
    rb_define_module_function(mT, "yield", t_yield, 1);
    rb_define_module_function(mT, "yield_values", t_yield_values, 0);
    rb_define_module_function(mT, "yield_splat", t_yield_splat, 1);
    rb_define_module_function(mT, "block_proc", t_block_proc, 0);
    rb_define_module_function(mT, "need_block", t_need_block, 0);
    rb_define_module_function(mT, "passing_block", t_passing_block, 1);
    rb_define_module_function(mT, "yield_each", t_yield_each, 1);
    rb_define_module_function(mT, "raise", t_raise, 2);
    rb_define_module_function(mT, "protect", t_protect, 1);
    rb_define_module_function(mT, "protect_rethrow", t_protect_rethrow, 0);
    rb_define_module_function(mT, "ensure", t_ensure, 1);
    rb_define_module_function(mT, "ensure_log", t_ensure_log, 0);
    rb_define_module_function(mT, "rescue", t_rescue, 1);
    rb_define_module_function(mT, "rescue2", t_rescue2, 1);
    rb_define_module_function(mT, "exc_new", t_exc_new, 0);
    rb_define_module_function(mT, "exc_raise", t_exc_raise, 1);
    rb_define_module_function(mT, "sys_fail", t_sys_fail, 0);
    rb_define_module_function(mT, "error_arity", t_error_arity, 1);
    rb_define_module_function(mT, "check_arity", t_check_arity, 1);
    rb_define_module_function(mT, "frozen", t_frozen, 1);
    rb_define_module_function(mT, "protect_yield", t_protect_yield, 0);
    rb_define_module_function(mT, "define", t_define, 0);
    rb_define_module_function(mT, "redefine_mismatch", t_redefine_mismatch, 0);
    rb_define_module_function(mT, "define_not_class", t_define_not_class, 0);
    rb_define_module_function(mT, "class_info", t_class_info, 1);
    rb_define_module_function(mT, "consts", t_consts, 0);
    rb_define_module_function(mT, "path2class_bad", t_path2class_bad, 0);
    rb_define_module_function(mT, "ivars", t_ivars, 1);
    rb_define_module_function(mT, "class_ivar", t_class_ivar, 1);
    rb_define_module_function(mT, "dfree_count", t_dfree_count, 0);
    rb_define_module_function(mT, "get_counter", t_get_counter, 1);
    rb_define_module_function(mT, "data_p", t_data_p, 1);
    rb_define_module_function(mT, "identity", t_identity, 1);
    rb_define_module_function(mT, "store_globals", t_store_globals, 2);
    rb_define_module_function(mT, "load_globals", t_load_globals, 0);
    rb_define_module_function(mT, "gc_churn", t_gc_churn, 1);
    rb_define_module_function(mT, "values_across_gc", t_values_across_gc, 3);
    rb_define_module_function(mT, "nested", t_nested, 1);
    rb_define_module_function(mT, "scan_11", t_scan_11, -1);
    rb_define_module_function(mT, "scan_rest", t_scan_rest, -1);
    rb_define_module_function(mT, "scan_02", t_scan_02, -1);
    rb_define_module_function(mT, "scan_kw", t_scan_kw, -1);
    rb_define_module_function(mT, "scan_blk", t_scan_blk, -1);
    rb_define_module_function(mT, "get_kwargs", t_get_kwargs, -1);
    rb_define_module_function(mT, "ids", t_ids, 0);
    rb_define_module_function(mT, "eval", t_eval, 1);
    rb_define_module_function(mT, "gvar", t_gvar, 0);
    rb_define_module_function(mT, "inspect_of", t_inspect, 1);
    rb_define_module_function(mT, "freeze", t_freeze, 1);
    rb_define_module_function(mT, "new_instance", t_new_instance, 2);
    rb_define_module_function(mT, "require", t_require, 1);
    rb_define_module_function(mT, "block_call", t_block_call, 1);
    rb_define_module_function(mT, "block_call_multi", t_block_call_multi, 1);
    rb_define_module_function(mT, "proc_new", t_proc_new, 1);
    rb_define_module_function(mT, "with_block", t_with_block, 2);
    rb_define_module_function(mT, "def_child", t_def_child, 1);

    VALUE cCounter = rb_define_class_under(mT, "Counter", rb_cObject);
    rb_define_alloc_func(cCounter, counter_alloc);
    rb_define_method(cCounter, "initialize", counter_init, -1);
    rb_define_method(cCounter, "incr", counter_incr, 0);
    rb_define_method(cCounter, "value", counter_value, 0);
    rb_define_method(cCounter, "stash", counter_stash, 1);
    rb_define_method(cCounter, "stashed", counter_stashed, 0);
}
