/* koruby builtin methods */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "context.h"
#include "object.h"
#include "node.h"

/* ---------- Kernel ---------- */
static VALUE kernel_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) ko_p(argv[i]);
    if (argc == 0) return Qnil;
    if (argc == 1) return argv[0];
    return ko_ary_new_from_values(argc, argv);
}

static VALUE kernel_puts(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) { fputc('\n', stdout); return Qnil; }
    for (int i = 0; i < argc; i++) {
        VALUE v = argv[i];
        if (BUILTIN_TYPE(v) == T_ARRAY) {
            struct ko_array *a = (struct ko_array *)v;
            for (long j = 0; j < a->len; j++) {
                VALUE s = ko_to_s(a->ptr[j]);
                fwrite(((struct ko_string *)s)->ptr, 1, ((struct ko_string *)s)->len, stdout);
                fputc('\n', stdout);
            }
        } else {
            VALUE s = ko_to_s(v);
            struct ko_string *str = (struct ko_string *)s;
            fwrite(str->ptr, 1, str->len, stdout);
            if (str->len == 0 || str->ptr[str->len-1] != '\n') fputc('\n', stdout);
        }
    }
    return Qnil;
}

static VALUE kernel_print(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        VALUE s = ko_to_s(argv[i]);
        fwrite(((struct ko_string *)s)->ptr, 1, ((struct ko_string *)s)->len, stdout);
    }
    return Qnil;
}

static VALUE kernel_raise(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) {
        ko_raise(c, NULL, "unhandled exception");
    } else if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        ko_raise(c, NULL, "%s", ko_str_cstr(argv[0]));
    } else {
        c->state = KO_RAISE;
        c->state_value = argv[0];
    }
    return Qnil;
}

static VALUE kernel_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_inspect(self);
}

static VALUE kernel_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_to_s(self);
}

static VALUE kernel_class(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_class_of(self);
}

static VALUE kernel_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KO_BOOL(ko_eq(self, argv[0]));
}

static VALUE kernel_neq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KO_BOOL(!ko_eq(self, argv[0]));
}

static VALUE kernel_not(CTX *c, VALUE self, int argc, VALUE *argv) {
    return RTEST(self) ? Qfalse : Qtrue;
}

static VALUE kernel_nil_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KO_BOOL(NIL_P(self));
}

static VALUE kernel_object_id(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)self / 8);
}

static VALUE kernel_freeze(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* no-op for now */
    return self;
}

static VALUE kernel_frozen_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* immediates are always frozen */
    if (SPECIAL_CONST_P(self)) return Qtrue;
    return Qfalse;
}

static VALUE kernel_respond_to_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    ID name = SYMBOL_P(argv[0]) ? ko_sym2id(argv[0]) : ko_intern_n(((struct ko_string *)argv[0])->ptr, ((struct ko_string *)argv[0])->len);
    return KO_BOOL(ko_class_find_method(ko_class_of_class(self), name) != NULL);
}

static VALUE kernel_is_a_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return Qfalse;
    struct ko_class *target = (struct ko_class *)argv[0];
    for (struct ko_class *k = ko_class_of_class(self); k; k = k->super) {
        if (k == target) return Qtrue;
    }
    return Qfalse;
}

static VALUE kernel_block_given(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct ko_proc *ko_current_block(void); /* TODO */
    /* approximate: always false here unless we wire up block tracking */
    return Qfalse;
}

/* ---------- Integer ---------- */
static VALUE int_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_plus(self, argv[0]);
}
static VALUE int_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_minus(self, argv[0]);
}
static VALUE int_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_mul(self, argv[0]);
}
static VALUE int_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_div(self, argv[0]);
}
static VALUE int_mod(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_mod(self, argv[0]);
}
static VALUE int_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_lshift(self, argv[0]);
}
static VALUE int_rshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_rshift(self, argv[0]);
}
static VALUE int_and(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_and(self, argv[0]);
}
static VALUE int_or(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_or(self, argv[0]);
}
static VALUE int_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_xor(self, argv[0]);
}
static VALUE int_lt(CTX *c, VALUE self, int argc, VALUE *argv) { return KO_BOOL(ko_int_cmp(self, argv[0]) < 0); }
static VALUE int_le(CTX *c, VALUE self, int argc, VALUE *argv) { return KO_BOOL(ko_int_cmp(self, argv[0]) <= 0); }
static VALUE int_gt(CTX *c, VALUE self, int argc, VALUE *argv) { return KO_BOOL(ko_int_cmp(self, argv[0]) > 0); }
static VALUE int_ge(CTX *c, VALUE self, int argc, VALUE *argv) { return KO_BOOL(ko_int_cmp(self, argv[0]) >= 0); }
static VALUE int_eq(CTX *c, VALUE self, int argc, VALUE *argv) { return KO_BOOL(ko_int_eq(self, argv[0])); }
static VALUE int_uminus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_minus(INT2FIX(0), self);
}
static VALUE int_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_to_s(self);
}
static VALUE int_to_i(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE int_to_f(CTX *c, VALUE self, int argc, VALUE *argv) { return ko_float_new((double)FIX2LONG(self)); }
static VALUE int_zero_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KO_BOOL(self == INT2FIX(0));
    return Qfalse;
}
static VALUE int_times(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* call block self times */
    if (!FIXNUM_P(self)) return Qnil;
    long n = FIX2LONG(self);
    /* yield current block: for simplicity we use ko_yield */
    for (long i = 0; i < n; i++) {
        VALUE arg = INT2FIX(i);
        VALUE r = ko_yield(c, 1, &arg);
        if (c->state != KO_NORMAL) return r;
    }
    return self;
}
static VALUE int_succ(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_plus(self, INT2FIX(1));
}
static VALUE int_pred(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_int_minus(self, INT2FIX(1));
}

/* ---------- Float ---------- */
static VALUE flt_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_float_new(((struct ko_float *)self)->value + ko_num2dbl(argv[0]));
}
static VALUE flt_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_float_new(((struct ko_float *)self)->value - ko_num2dbl(argv[0]));
}
static VALUE flt_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_float_new(((struct ko_float *)self)->value * ko_num2dbl(argv[0]));
}
static VALUE flt_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_float_new(((struct ko_float *)self)->value / ko_num2dbl(argv[0]));
}
static VALUE flt_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    char b[64]; snprintf(b, 64, "%.17g", ((struct ko_float *)self)->value);
    return ko_str_new_cstr(b);
}

/* ---------- String ---------- */
static VALUE str_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    VALUE r = ko_str_dup(self);
    return ko_str_concat(r, argv[0]);
}
static VALUE str_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_str_concat(self, argv[0]);
}
static VALUE str_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(((struct ko_string *)self)->len);
}
static VALUE str_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KO_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && ko_eql(self, argv[0]));
}
static VALUE str_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE str_to_sym(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_str_to_sym(self);
}

/* ---------- Array ---------- */
static VALUE ary_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(ko_ary_len(self));
}
static VALUE ary_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    return ko_ary_aref(self, FIX2LONG(argv[0]));
}
static VALUE ary_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    ko_ary_aset(self, FIX2LONG(argv[0]), argv[1]);
    return argv[1];
}
static VALUE ary_push(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) ko_ary_push(self, argv[i]);
    return self;
}
static VALUE ary_pop(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_ary_pop(self);
}
static VALUE ary_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_ary_aref(self, 0);
}
static VALUE ary_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    return ko_ary_aref(self, len - 1);
}
static VALUE ary_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE v = ko_ary_aref(self, i);
        ko_yield(c, 1, &v);
        if (c->state != KO_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_each_with_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE args[2] = { ko_ary_aref(self, i), INT2FIX(i) };
        ko_yield(c, 2, args);
        if (c->state != KO_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    VALUE r = ko_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE v = ko_ary_aref(self, i);
        VALUE m = ko_yield(c, 1, &v);
        if (c->state != KO_NORMAL) return Qnil;
        ko_ary_push(r, m);
    }
    return r;
}
static VALUE ary_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    VALUE r = ko_ary_new();
    for (long i = 0; i < len; i++) {
        VALUE v = ko_ary_aref(self, i);
        VALUE m = ko_yield(c, 1, &v);
        if (c->state != KO_NORMAL) return Qnil;
        if (RTEST(m)) ko_ary_push(r, v);
    }
    return r;
}
static VALUE ary_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    VALUE acc = argc > 0 ? argv[0] : ko_ary_aref(self, 0);
    long i = argc > 0 ? 0 : 1;
    for (; i < len; i++) {
        VALUE args[2] = { acc, ko_ary_aref(self, i) };
        acc = ko_yield(c, 2, args);
        if (c->state != KO_NORMAL) return Qnil;
    }
    return acc;
}
static VALUE ary_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    VALUE sep = argc > 0 ? argv[0] : ko_str_new_cstr("");
    VALUE r = ko_str_new("", 0);
    for (long i = 0; i < len; i++) {
        if (i > 0 && BUILTIN_TYPE(sep) == T_STRING) ko_str_concat(r, sep);
        VALUE v = ko_ary_aref(self, i);
        if (BUILTIN_TYPE(v) != T_STRING) v = ko_to_s(v);
        ko_str_concat(r, v);
    }
    return r;
}
static VALUE ary_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_inspect(self);
}
static VALUE ary_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qfalse;
    long la = ko_ary_len(self), lb = ko_ary_len(argv[0]);
    if (la != lb) return Qfalse;
    for (long i = 0; i < la; i++) {
        if (!ko_eq(ko_ary_aref(self, i), ko_ary_aref(argv[0], i))) return Qfalse;
    }
    return Qtrue;
}
static VALUE ary_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    ko_ary_push(self, argv[0]);
    return self;
}
static VALUE ary_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = ko_ary_len(self);
    VALUE r = ko_ary_new_capa(len);
    for (long i = 0; i < len; i++) ko_ary_push(r, ko_ary_aref(self, i));
    return r;
}

/* ---------- Hash ---------- */
static VALUE hash_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_hash_aref(self, argv[0]);
}
static VALUE hash_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_hash_aset(self, argv[0], argv[1]);
}
static VALUE hash_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(ko_hash_size(self));
}
static VALUE hash_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct ko_hash *h = (struct ko_hash *)self;
    for (struct ko_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        ko_yield(c, 2, args);
        if (c->state != KO_NORMAL) return Qnil;
    }
    return self;
}

/* ---------- Range ---------- */
static VALUE rng_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct ko_range *r = (struct ko_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) {
        for (long i = b; i < e; i++) {
            VALUE v = INT2FIX(i);
            ko_yield(c, 1, &v);
            if (c->state != KO_NORMAL) return Qnil;
        }
    } else {
        for (long i = b; i <= e; i++) {
            VALUE v = INT2FIX(i);
            ko_yield(c, 1, &v);
            if (c->state != KO_NORMAL) return Qnil;
        }
    }
    return self;
}

static VALUE rng_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct ko_range *)self)->begin;
}
static VALUE rng_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct ko_range *)self)->end;
}
static VALUE rng_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct ko_range *r = (struct ko_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return ko_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    long n = e - b + 1; if (n < 0) n = 0;
    VALUE a = ko_ary_new_capa(n);
    for (long i = 0; i < n; i++) ko_ary_push(a, INT2FIX(b + i));
    return a;
}

/* ---------- Class.new etc ---------- */
static VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) {
        ko_raise(c, NULL, "Class.new called on non-class");
        return Qnil;
    }
    struct ko_class *klass = (struct ko_class *)self;
    VALUE obj = ko_object_new(klass);
    /* call initialize if defined */
    struct ko_method *m = ko_class_find_method(klass, id_initialize);
    if (m) {
        ko_funcall(c, obj, id_initialize, argc, argv);
    }
    return obj;
}

static VALUE class_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_str_new_cstr(ko_id_name(((struct ko_class *)self)->name));
}

/* ---------- Symbol ---------- */
static VALUE sym_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ko_str_new_cstr(ko_id_name(ko_sym2id(self)));
}
static VALUE sym_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KO_BOOL(self == argv[0]);
}

/* ---------- Boolean ---------- */
static VALUE true_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return ko_str_new_cstr("true"); }
static VALUE false_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return ko_str_new_cstr("false"); }
static VALUE nil_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return ko_str_new_cstr(""); }
static VALUE nil_inspect(CTX *c, VALUE self, int argc, VALUE *argv) { return ko_str_new_cstr("nil"); }

/* ---------- Proc ---------- */
extern VALUE ko_yield(CTX *c, uint32_t argc, VALUE *argv);

static VALUE proc_call(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return Qnil;
    struct ko_proc *p = (struct ko_proc *)self;
    VALUE *fp = p->env;
    VALUE prev_self = c->self;
    for (int i = 0; i < argc && (uint32_t)i < p->params_cnt; i++) {
        fp[p->param_base + i] = argv[i];
    }
    c->self = p->self;
    VALUE r = EVAL(c, p->body);
    c->self = prev_self;
    if (c->state == KO_RETURN || c->state == KO_BREAK) {
        r = c->state_value;
        c->state = KO_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

#define DEF(klass, name, fn, argc) \
    ko_class_add_method_cfunc((klass), ko_intern(name), (fn), (argc))

void ko_init_builtins(void) {
    /* Object methods */
    struct ko_class *cObj = ko_vm->object_class;
    DEF(cObj, "p", kernel_p, -1);
    DEF(cObj, "puts", kernel_puts, -1);
    DEF(cObj, "print", kernel_print, -1);
    DEF(cObj, "raise", kernel_raise, -1);
    DEF(cObj, "inspect", kernel_inspect, 0);
    DEF(cObj, "to_s", kernel_to_s, 0);
    DEF(cObj, "class", kernel_class, 0);
    DEF(cObj, "==", kernel_eq, 1);
    DEF(cObj, "!=", kernel_neq, 1);
    DEF(cObj, "!", kernel_not, 0);
    DEF(cObj, "nil?", kernel_nil_p, 0);
    DEF(cObj, "object_id", kernel_object_id, 0);
    DEF(cObj, "freeze", kernel_freeze, 0);
    DEF(cObj, "frozen?", kernel_frozen_p, 0);
    DEF(cObj, "respond_to?", kernel_respond_to_p, 1);
    DEF(cObj, "is_a?", kernel_is_a_p, 1);
    DEF(cObj, "kind_of?", kernel_is_a_p, 1);
    DEF(cObj, "block_given?", kernel_block_given, 0);

    /* Integer */
    struct ko_class *cInt = ko_vm->integer_class;
    DEF(cInt, "+", int_plus, 1);
    DEF(cInt, "-", int_minus, 1);
    DEF(cInt, "*", int_mul, 1);
    DEF(cInt, "/", int_div, 1);
    DEF(cInt, "%", int_mod, 1);
    DEF(cInt, "<<", int_lshift, 1);
    DEF(cInt, ">>", int_rshift, 1);
    DEF(cInt, "&", int_and, 1);
    DEF(cInt, "|", int_or, 1);
    DEF(cInt, "^", int_xor, 1);
    DEF(cInt, "<", int_lt, 1);
    DEF(cInt, "<=", int_le, 1);
    DEF(cInt, ">", int_gt, 1);
    DEF(cInt, ">=", int_ge, 1);
    DEF(cInt, "==", int_eq, 1);
    DEF(cInt, "-@", int_uminus, 0);
    DEF(cInt, "to_s", int_to_s, 0);
    DEF(cInt, "to_i", int_to_i, 0);
    DEF(cInt, "to_f", int_to_f, 0);
    DEF(cInt, "zero?", int_zero_p, 0);
    DEF(cInt, "times", int_times, 0);
    DEF(cInt, "succ", int_succ, 0);
    DEF(cInt, "next", int_succ, 0);
    DEF(cInt, "pred", int_pred, 0);

    /* Float */
    struct ko_class *cFlt = ko_vm->float_class;
    DEF(cFlt, "+", flt_plus, 1);
    DEF(cFlt, "-", flt_minus, 1);
    DEF(cFlt, "*", flt_mul, 1);
    DEF(cFlt, "/", flt_div, 1);
    DEF(cFlt, "to_s", flt_to_s, 0);

    /* String */
    struct ko_class *cStr = ko_vm->string_class;
    DEF(cStr, "+", str_plus, 1);
    DEF(cStr, "<<", str_concat, 1);
    DEF(cStr, "concat", str_concat, 1);
    DEF(cStr, "size", str_size, 0);
    DEF(cStr, "length", str_size, 0);
    DEF(cStr, "==", str_eq, 1);
    DEF(cStr, "to_s", str_to_s, 0);
    DEF(cStr, "to_sym", str_to_sym, 0);

    /* Array */
    struct ko_class *cAry = ko_vm->array_class;
    DEF(cAry, "size", ary_size, 0);
    DEF(cAry, "length", ary_size, 0);
    DEF(cAry, "[]", ary_aref, 1);
    DEF(cAry, "[]=", ary_aset, 2);
    DEF(cAry, "push", ary_push, -1);
    DEF(cAry, "<<", ary_lshift, 1);
    DEF(cAry, "pop", ary_pop, 0);
    DEF(cAry, "first", ary_first, 0);
    DEF(cAry, "last", ary_last, 0);
    DEF(cAry, "each", ary_each, 0);
    DEF(cAry, "each_with_index", ary_each_with_index, 0);
    DEF(cAry, "map", ary_map, 0);
    DEF(cAry, "collect", ary_map, 0);
    DEF(cAry, "select", ary_select, 0);
    DEF(cAry, "filter", ary_select, 0);
    DEF(cAry, "reduce", ary_reduce, -1);
    DEF(cAry, "inject", ary_reduce, -1);
    DEF(cAry, "join", ary_join, -1);
    DEF(cAry, "inspect", ary_inspect, 0);
    DEF(cAry, "to_s", ary_inspect, 0);
    DEF(cAry, "==", ary_eq, 1);
    DEF(cAry, "dup", ary_dup, 0);

    /* Hash */
    struct ko_class *cHsh = ko_vm->hash_class;
    DEF(cHsh, "[]", hash_aref, 1);
    DEF(cHsh, "[]=", hash_aset, 2);
    DEF(cHsh, "size", hash_size, 0);
    DEF(cHsh, "length", hash_size, 0);
    DEF(cHsh, "each", hash_each, 0);

    /* Range */
    struct ko_class *cRng = ko_vm->range_class;
    DEF(cRng, "each", rng_each, 0);
    DEF(cRng, "first", rng_first, 0);
    DEF(cRng, "last", rng_last, 0);
    DEF(cRng, "to_a", rng_to_a, 0);

    /* Class */
    struct ko_class *cCls = ko_vm->class_class;
    DEF(cCls, "new", class_new, -1);
    DEF(cCls, "name", class_name, 0);

    /* Symbol */
    struct ko_class *cSym = ko_vm->symbol_class;
    DEF(cSym, "to_s", sym_to_s, 0);
    DEF(cSym, "==", sym_eq, 1);

    /* Boolean / Nil */
    DEF(ko_vm->true_class, "to_s", true_to_s, 0);
    DEF(ko_vm->false_class, "to_s", false_to_s, 0);
    DEF(ko_vm->nil_class, "to_s", nil_to_s, 0);
    DEF(ko_vm->nil_class, "inspect", nil_inspect, 0);

    /* Proc */
    struct ko_class *cPrc = ko_vm->proc_class;
    DEF(cPrc, "call", proc_call, -1);
    DEF(cPrc, "[]", proc_call, -1);
}
