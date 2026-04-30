/* koruby builtin methods */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "context.h"
#include "object.h"
#include "node.h"

/* ---------- Kernel ---------- */
static VALUE kernel_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) korb_p(argv[i]);
    if (argc == 0) return Qnil;
    if (argc == 1) return argv[0];
    return korb_ary_new_from_values(argc, argv);
}

static VALUE kernel_puts(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) { fputc('\n', stdout); return Qnil; }
    for (int i = 0; i < argc; i++) {
        VALUE v = argv[i];
        if (BUILTIN_TYPE(v) == T_ARRAY) {
            struct korb_array *a = (struct korb_array *)v;
            for (long j = 0; j < a->len; j++) {
                VALUE s = korb_to_s(a->ptr[j]);
                fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
                fputc('\n', stdout);
            }
        } else {
            VALUE s = korb_to_s(v);
            struct korb_string *str = (struct korb_string *)s;
            fwrite(str->ptr, 1, str->len, stdout);
            if (str->len == 0 || str->ptr[str->len-1] != '\n') fputc('\n', stdout);
        }
    }
    return Qnil;
}

static VALUE kernel_print(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s(argv[i]);
        fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
    }
    return Qnil;
}

static VALUE kernel_raise(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc == 0) {
        korb_raise(c, NULL, "unhandled exception");
    } else if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        korb_raise(c, NULL, "%s", korb_str_cstr(argv[0]));
    } else if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_CLASS) {
        /* raise Klass, msg */
        const char *msg = "(unspecified)";
        if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
            msg = korb_str_cstr(argv[1]);
        }
        VALUE e = korb_exc_new((struct korb_class *)argv[0], msg);
        c->state = KORB_RAISE;
        c->state_value = e;
    } else {
        /* assume argv[0] is already an exception instance */
        c->state = KORB_RAISE;
        c->state_value = argv[0];
    }
    return Qnil;
}

static VALUE kernel_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_inspect(self);
}

static VALUE kernel_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_to_s(self);
}

static VALUE kernel_class(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_class_of(self);
}

static VALUE kernel_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_eq(self, argv[0]));
}

static VALUE kernel_neq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(!korb_eq(self, argv[0]));
}

static VALUE kernel_not(CTX *c, VALUE self, int argc, VALUE *argv) {
    return RTEST(self) ? Qfalse : Qtrue;
}

static VALUE kernel_nil_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(NIL_P(self));
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
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    return KORB_BOOL(korb_class_find_method(korb_class_of_class(self), name) != NULL);
}

static VALUE kernel_is_a_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_CLASS && BUILTIN_TYPE(argv[0]) != T_MODULE) return Qfalse;
    struct korb_class *target = (struct korb_class *)argv[0];
    for (struct korb_class *k = korb_class_of_class(self); k; k = k->super) {
        if (k == target) return Qtrue;
    }
    return Qfalse;
}

static VALUE kernel_block_given(CTX *c, VALUE self, int argc, VALUE *argv) {
    extern struct korb_proc *korb_current_block(void); /* TODO */
    /* approximate: always false here unless we wire up block tracking */
    return Qfalse;
}

static VALUE kernel_require_relative(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "require_relative: expected 1 String");
        return Qnil;
    }
    const char *name = korb_str_cstr(argv[0]);
    char *resolved = korb_resolve_relative(c->current_file, name);
    if (!resolved) {
        korb_raise(c, NULL, "cannot load such file -- %s", name);
        return Qnil;
    }
    return korb_load_file(c, resolved);
}

static VALUE kernel_require(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc != 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "require: expected 1 String");
        return Qnil;
    }
    const char *name = korb_str_cstr(argv[0]);
    /* Bare path: try as is, then as .rb in cwd */
    if (korb_file_exists(name)) return korb_load_file(c, name);
    long nl = strlen(name);
    bool has_rb = nl >= 3 && strcmp(name + nl - 3, ".rb") == 0;
    if (!has_rb) {
        char *with = korb_xmalloc_atomic(nl + 4);
        sprintf(with, "%s.rb", name);
        if (korb_file_exists(with)) return korb_load_file(c, with);
    }
    /* Stub: pretend stdlib gems aren't available, return false */
    if (strcmp(name, "stackprof") == 0) return Qfalse;
    if (strcmp(name, "fiddle") == 0) return Qfalse;
    if (strcmp(name, "rbconfig") == 0) return Qfalse;
    if (strcmp(name, "ffi") == 0) return Qfalse;
    /* unknown — don't raise, just return false (CRuby would raise but be lenient) */
    return Qfalse;
}

static VALUE kernel_load(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    return korb_load_file(c, korb_str_cstr(argv[0]));
}

static VALUE kernel_abort(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc >= 1 && BUILTIN_TYPE(argv[0]) == T_STRING) {
        fprintf(stderr, "%s\n", korb_str_cstr(argv[0]));
    }
    exit(1);
}

static VALUE kernel_exit(CTX *c, VALUE self, int argc, VALUE *argv) {
    int code = 0;
    if (argc >= 1 && FIXNUM_P(argv[0])) code = (int)FIX2LONG(argv[0]);
    else if (argc >= 1 && argv[0] == Qfalse) code = 1;
    exit(code);
}

/* ---------- Integer ---------- */
static VALUE int_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_plus(self, argv[0]);
}
static VALUE int_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_minus(self, argv[0]);
}
static VALUE int_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_mul(self, argv[0]);
}
static VALUE int_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_div(self, argv[0]);
}
static VALUE int_mod(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_mod(self, argv[0]);
}
static VALUE int_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_lshift(self, argv[0]);
}
static VALUE int_rshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_rshift(self, argv[0]);
}
static VALUE int_and(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_and(self, argv[0]);
}
static VALUE int_or(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_or(self, argv[0]);
}
static VALUE int_xor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_xor(self, argv[0]);
}
static VALUE int_lt(CTX *c, VALUE self, int argc, VALUE *argv) { return KORB_BOOL(korb_int_cmp(self, argv[0]) < 0); }
static VALUE int_le(CTX *c, VALUE self, int argc, VALUE *argv) { return KORB_BOOL(korb_int_cmp(self, argv[0]) <= 0); }
static VALUE int_gt(CTX *c, VALUE self, int argc, VALUE *argv) { return KORB_BOOL(korb_int_cmp(self, argv[0]) > 0); }
static VALUE int_ge(CTX *c, VALUE self, int argc, VALUE *argv) { return KORB_BOOL(korb_int_cmp(self, argv[0]) >= 0); }
static VALUE int_eq(CTX *c, VALUE self, int argc, VALUE *argv) { return KORB_BOOL(korb_int_eq(self, argv[0])); }
static VALUE int_uminus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_minus(INT2FIX(0), self);
}
static VALUE int_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_to_s(self);
}
static VALUE int_to_i(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE int_to_f(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_float_new((double)FIX2LONG(self)); }
static VALUE int_zero_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) return KORB_BOOL(self == INT2FIX(0));
    return Qfalse;
}
static VALUE int_times(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* call block self times */
    if (!FIXNUM_P(self)) return Qnil;
    long n = FIX2LONG(self);
    /* yield current block: for simplicity we use korb_yield */
    for (long i = 0; i < n; i++) {
        VALUE arg = INT2FIX(i);
        VALUE r = korb_yield(c, 1, &arg);
        if (c->state != KORB_NORMAL) return r;
    }
    return self;
}
static VALUE int_succ(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_plus(self, INT2FIX(1));
}
static VALUE int_pred(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_int_minus(self, INT2FIX(1));
}

/* ---------- Float ---------- */
static VALUE flt_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(((struct korb_float *)self)->value + korb_num2dbl(argv[0]));
}
static VALUE flt_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(((struct korb_float *)self)->value - korb_num2dbl(argv[0]));
}
static VALUE flt_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(((struct korb_float *)self)->value * korb_num2dbl(argv[0]));
}
static VALUE flt_div(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(((struct korb_float *)self)->value / korb_num2dbl(argv[0]));
}
static VALUE flt_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    char b[64]; snprintf(b, 64, "%.17g", ((struct korb_float *)self)->value);
    return korb_str_new_cstr(b);
}

/* ---------- String ---------- */
static VALUE str_plus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    VALUE r = korb_str_dup(self);
    return korb_str_concat(r, argv[0]);
}
static VALUE str_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_concat(self, argv[0]);
}
static VALUE str_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(((struct korb_string *)self)->len);
}
static VALUE str_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(self, argv[0]));
}
static VALUE str_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE str_to_sym(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_to_sym(self);
}

/* ---------- Array ---------- */
static VALUE ary_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(korb_ary_len(self));
}
static VALUE ary_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    return korb_ary_aref(self, FIX2LONG(argv[0]));
}
static VALUE ary_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return Qnil;
    korb_ary_aset(self, FIX2LONG(argv[0]), argv[1]);
    return argv[1];
}
static VALUE ary_push(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) korb_ary_push(self, argv[i]);
    return self;
}
static VALUE ary_pop(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_pop(self);
}
static VALUE ary_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_aref(self, 0);
}
static VALUE ary_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    return korb_ary_aref(self, len - 1);
}
static VALUE ary_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_each_with_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    for (long i = 0; i < len; i++) {
        VALUE args[2] = { korb_ary_aref(self, i), INT2FIX(i) };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}
static VALUE ary_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(len);
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(r, m);
    }
    return r;
}
static VALUE ary_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new();
    for (long i = 0; i < len; i++) {
        VALUE v = korb_ary_aref(self, i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(r, v);
    }
    return r;
}
static VALUE ary_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE acc = argc > 0 ? argv[0] : korb_ary_aref(self, 0);
    long i = argc > 0 ? 0 : 1;
    for (; i < len; i++) {
        VALUE args[2] = { acc, korb_ary_aref(self, i) };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}
static VALUE ary_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE sep = argc > 0 ? argv[0] : korb_str_new_cstr("");
    VALUE r = korb_str_new("", 0);
    for (long i = 0; i < len; i++) {
        if (i > 0 && BUILTIN_TYPE(sep) == T_STRING) korb_str_concat(r, sep);
        VALUE v = korb_ary_aref(self, i);
        if (BUILTIN_TYPE(v) != T_STRING) v = korb_to_s(v);
        korb_str_concat(r, v);
    }
    return r;
}
static VALUE ary_inspect(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_inspect(self);
}
static VALUE ary_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(argv[0]) != T_ARRAY) return Qfalse;
    long la = korb_ary_len(self), lb = korb_ary_len(argv[0]);
    if (la != lb) return Qfalse;
    for (long i = 0; i < la; i++) {
        if (!korb_eq(korb_ary_aref(self, i), korb_ary_aref(argv[0], i))) return Qfalse;
    }
    return Qtrue;
}
static VALUE ary_lshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    korb_ary_push(self, argv[0]);
    return self;
}
static VALUE ary_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    long len = korb_ary_len(self);
    VALUE r = korb_ary_new_capa(len);
    for (long i = 0; i < len; i++) korb_ary_push(r, korb_ary_aref(self, i));
    return r;
}

/* ---------- Hash ---------- */
static VALUE hash_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_hash_aref(self, argv[0]);
}
static VALUE hash_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_hash_aset(self, argv[0], argv[1]);
}
static VALUE hash_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX(korb_hash_size(self));
}
static VALUE hash_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

/* ---------- Range ---------- */
static VALUE rng_each(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) {
        for (long i = b; i < e; i++) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return Qnil;
        }
    } else {
        for (long i = b; i <= e; i++) {
            VALUE v = INT2FIX(i);
            korb_yield(c, 1, &v);
            if (c->state != KORB_NORMAL) return Qnil;
        }
    }
    return self;
}

static VALUE rng_first(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct korb_range *)self)->begin;
}
static VALUE rng_last(CTX *c, VALUE self, int argc, VALUE *argv) {
    return ((struct korb_range *)self)->end;
}
static VALUE rng_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return korb_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    long n = e - b + 1; if (n < 0) n = 0;
    VALUE a = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(a, INT2FIX(b + i));
    return a;
}

/* ---------- Module / Class metaprogramming ---------- */

static VALUE ivar_getter_dispatch(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* the getter method's "name" tells us the @ivar; we encode the ivar name
     * as the method's name without the leading @, so name "x" → @x. */
    /* Actually simpler: we install the cfunc with a side-channel.  In our
     * scheme cfuncs receive the same args; we need to pass the ivar name.
     * Instead, the getter's cfunc captures the ID at definition time via
     * a closure-style structure. */
    (void)argc; (void)argv;
    return Qnil; /* never called directly */
}

/* attr_reader / attr_writer / attr_accessor implementation:
 * We install AST methods whose body is node_ivar_get / node_ivar_set.
 */
static VALUE module_attr_reader(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_reader: not on a class/module");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (BUILTIN_TYPE(argv[i]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[i])->ptr, ((struct korb_string *)argv[i])->len);
        else continue;
        /* @name */
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        /* body: node_ivar_get(iv_id) */
        NODE *body = ALLOC_node_ivar_get(iv_id);
        korb_class_add_method_ast(klass, name, body, 0, 0);
    }
    return Qnil;
}

static VALUE module_attr_writer(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) {
        korb_raise(c, NULL, "attr_writer: not on a class/module");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        ID name;
        if (SYMBOL_P(argv[i])) name = korb_sym2id(argv[i]);
        else if (BUILTIN_TYPE(argv[i]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[i])->ptr, ((struct korb_string *)argv[i])->len);
        else continue;
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        /* setter name: name=  */
        char *sn = korb_xmalloc_atomic(bl + 2);
        memcpy(sn, base, bl); sn[bl] = '='; sn[bl + 1] = 0;
        ID setter_id = korb_intern(sn);
        /* @name */
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        /* body: node_ivar_set(iv_id, node_lvar_get(0)) */
        NODE *body = ALLOC_node_ivar_set(iv_id, ALLOC_node_lvar_get(0));
        korb_class_add_method_ast(klass, setter_id, body, 1, 1);
    }
    return Qnil;
}

static VALUE module_attr_accessor(CTX *c, VALUE self, int argc, VALUE *argv) {
    module_attr_reader(c, self, argc, argv);
    module_attr_writer(c, self, argc, argv);
    return Qnil;
}

static VALUE module_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Simplified include: copy module's methods/constants into the class.
     * Real Ruby inserts the module into the ancestor chain; we flatten. */
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return self;
    struct korb_class *klass = (struct korb_class *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_MODULE && BUILTIN_TYPE(argv[i]) != T_CLASS) continue;
        korb_module_include(klass, (struct korb_class *)argv[i]);
    }
    if (korb_vm) korb_vm->method_serial++;
    return self;
}

static VALUE module_define_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* define_method(:foo) { ... } — not commonly used in optcarrot */
    return Qnil;
}

static VALUE module_private(CTX *c, VALUE self, int argc, VALUE *argv) { return self; /* no-op */ }
static VALUE module_public(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE module_protected(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }
static VALUE module_module_function(CTX *c, VALUE self, int argc, VALUE *argv) { return self; }

/* Struct.new(:a, :b) → returns a new Class with attr_accessor for each */
static VALUE struct_initialize(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* attr_accessor for each member */
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return Qnil;
    struct korb_array *members = (struct korb_array *)members_v;
    long n = members->len;
    for (long i = 0; i < n && i < argc; i++) {
        ID name = SYMBOL_P(members->ptr[i]) ? korb_sym2id(members->ptr[i]) :
                  korb_intern(korb_str_cstr(members->ptr[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ivar_set(self, iv_id, argv[i]);
    }
    return self;
}

static VALUE struct_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_class *klass = (struct korb_class *)((struct korb_object *)self)->basic.klass;
    VALUE members_v = korb_const_get(klass, korb_intern("__members__"));
    if (UNDEF_P(members_v) || BUILTIN_TYPE(members_v) != T_ARRAY) return korb_ary_new();
    struct korb_array *members = (struct korb_array *)members_v;
    VALUE r = korb_ary_new_capa(members->len);
    for (long i = 0; i < members->len; i++) {
        ID name = SYMBOL_P(members->ptr[i]) ? korb_sym2id(members->ptr[i]) :
                  korb_intern(korb_str_cstr(members->ptr[i]));
        const char *base = korb_id_name(name);
        long bl = strlen(base);
        char *iv = korb_xmalloc_atomic(bl + 2);
        iv[0] = '@'; memcpy(iv + 1, base, bl); iv[bl + 1] = 0;
        ID iv_id = korb_intern(iv);
        korb_ary_push(r, korb_ivar_get(self, iv_id));
    }
    return r;
}

static VALUE struct_class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Struct.new(:a, :b) — create new class */
    struct korb_class *klass = korb_class_new(korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    /* save members */
    VALUE members = korb_ary_new_from_values(argc, argv);
    korb_const_set(klass, korb_intern("__members__"), members);
    /* attr_accessor for each member */
    module_attr_accessor(c, (VALUE)klass, argc, argv);
    /* initialize */
    korb_class_add_method_cfunc(klass, korb_intern("initialize"), struct_initialize, -1);
    korb_class_add_method_cfunc(klass, korb_intern("to_a"), struct_to_a, 0);
    korb_class_add_method_cfunc(klass, korb_intern("members"), struct_to_a, 0);
    return (VALUE)klass;
}

static VALUE module_const_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    VALUE v = korb_const_get((struct korb_class *)self, name);
    if (UNDEF_P(v)) return Qnil;
    return v;
}

static VALUE module_const_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qnil;
    if (argc < 2) return Qnil;
    ID name = SYMBOL_P(argv[0]) ? korb_sym2id(argv[0]) : korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    korb_const_set((struct korb_class *)self, name, argv[1]);
    return argv[1];
}

/* ---------- String formatting / methods (extended) ---------- */

static VALUE str_format_self(CTX *c, VALUE self, int argc, VALUE *argv);

static VALUE str_split(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new();
    if (argc == 0 || NIL_P(argv[0])) {
        /* split on whitespace */
        long i = 0;
        while (i < s->len) {
            while (i < s->len && (s->ptr[i] == ' ' || s->ptr[i] == '\t' || s->ptr[i] == '\n')) i++;
            if (i >= s->len) break;
            long start = i;
            while (i < s->len && s->ptr[i] != ' ' && s->ptr[i] != '\t' && s->ptr[i] != '\n') i++;
            korb_ary_push(r, korb_str_new(s->ptr + start, i - start));
        }
        return r;
    }
    if (BUILTIN_TYPE(argv[0]) != T_STRING) return r;
    struct korb_string *sep = (struct korb_string *)argv[0];
    if (sep->len == 0) {
        for (long i = 0; i < s->len; i++) korb_ary_push(r, korb_str_new(s->ptr + i, 1));
        return r;
    }
    long start = 0;
    for (long i = 0; i + sep->len <= s->len; ) {
        if (memcmp(s->ptr + i, sep->ptr, sep->len) == 0) {
            korb_ary_push(r, korb_str_new(s->ptr + start, i - start));
            i += sep->len;
            start = i;
        } else i++;
    }
    korb_ary_push(r, korb_str_new(s->ptr + start, s->len - start));
    return r;
}

static VALUE str_chomp(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long n = s->len;
    if (n > 0 && s->ptr[n-1] == '\n') n--;
    if (n > 0 && s->ptr[n-1] == '\r') n--;
    return korb_str_new(s->ptr, n);
}

static VALUE str_strip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    long start = 0, end = s->len;
    while (start < end && (s->ptr[start] == ' ' || s->ptr[start] == '\t' || s->ptr[start] == '\n' || s->ptr[start] == '\r')) start++;
    while (end > start && (s->ptr[end-1] == ' ' || s->ptr[end-1] == '\t' || s->ptr[end-1] == '\n' || s->ptr[end-1] == '\r')) end--;
    return korb_str_new(s->ptr + start, end - start);
}

static VALUE str_to_i(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *end;
    long v = strtol(s->ptr, &end, argc > 0 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10);
    return INT2FIX(v);
}

static VALUE str_to_f(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_float_new(strtod(((struct korb_string *)self)->ptr, NULL));
}

static VALUE str_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    if (argc == 1 && FIXNUM_P(argv[0])) {
        long i = FIX2LONG(argv[0]);
        if (i < 0) i += s->len;
        if (i < 0 || i >= s->len) return Qnil;
        return korb_str_new(s->ptr + i, 1);
    }
    if (argc == 2 && FIXNUM_P(argv[0]) && FIXNUM_P(argv[1])) {
        long i = FIX2LONG(argv[0]);
        long len = FIX2LONG(argv[1]);
        if (i < 0) i += s->len;
        if (i < 0 || i > s->len) return Qnil;
        if (i + len > s->len) len = s->len - i;
        if (len < 0) len = 0;
        return korb_str_new(s->ptr + i, len);
    }
    return Qnil;
}

static VALUE str_aset(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* not used by optcarrot main path; stub */
    return Qnil;
}

static VALUE str_chars(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new_capa(s->len);
    for (long i = 0; i < s->len; i++) korb_ary_push(r, korb_str_new(s->ptr + i, 1));
    return r;
}

static VALUE str_bytes(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_ary_new_capa(s->len);
    for (long i = 0; i < s->len; i++) korb_ary_push(r, INT2FIX((unsigned char)s->ptr[i]));
    return r;
}

static VALUE str_each_char(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (long i = 0; i < s->len; i++) {
        VALUE ch = korb_str_new(s->ptr + i, 1);
        korb_yield(c, 1, &ch);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE str_start_with(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_end_with(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        struct korb_string *p = (struct korb_string *)argv[i];
        if (p->len <= s->len && memcmp(s->ptr + s->len - p->len, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    if (p->len == 0) return Qtrue;
    for (long i = 0; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) return Qtrue;
    }
    return Qfalse;
}

static VALUE str_replace(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return self;
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *o = (struct korb_string *)argv[0];
    s->ptr = korb_xmalloc_atomic(o->len + 1);
    memcpy(s->ptr, o->ptr, o->len);
    s->ptr[o->len] = 0;
    s->len = o->len;
    s->capa = o->len;
    return self;
}

static VALUE str_reverse(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) r[i] = s->ptr[s->len - 1 - i];
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_upcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_downcase(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_string *s = (struct korb_string *)self;
    char *r = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        r[i] = ch;
    }
    r[s->len] = 0;
    return korb_str_new(r, s->len);
}

static VALUE str_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_string *)self)->len == 0);
}

static VALUE str_mul(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return korb_str_new("", 0);
    struct korb_string *s = (struct korb_string *)self;
    VALUE r = korb_str_new("", 0);
    for (long i = 0; i < n; i++) korb_str_concat(r, self);
    (void)s;
    return r;
}

static VALUE str_hash(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)(korb_hash_value(self) >> 1));
}

static VALUE str_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String === other ⇒ same as == */
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_STRING && korb_eql(self, argv[0]));
}

static VALUE str_match_op(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String#=~ regex — we don't have regex, return nil */
    (void)c; (void)self; (void)argc; (void)argv;
    return Qnil;
}

static VALUE str_match_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* String#match? — false (no regex) */
    return Qfalse;
}

static VALUE str_match(CTX *c, VALUE self, int argc, VALUE *argv) {
    return Qnil;
}

static VALUE str_scan(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ary_new();
}

/* simplistic gsub: replace all non-overlapping occurrences of pattern in self.
 * pattern is treated as a literal string (no regex support). */
static VALUE str_gsub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    struct korb_string *r = (struct korb_string *)argv[1];
    if (p->len == 0) return korb_str_dup(self);
    VALUE out = korb_str_new("", 0);
    long start = 0;
    for (long i = 0; i + p->len <= s->len; ) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) {
            korb_str_concat(out, korb_str_new(s->ptr + start, i - start));
            korb_str_concat(out, korb_str_new(r->ptr, r->len));
            i += p->len;
            start = i;
        } else i++;
    }
    korb_str_concat(out, korb_str_new(s->ptr + start, s->len - start));
    return out;
}

static VALUE str_sub(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *p = (struct korb_string *)argv[0];
    struct korb_string *r = (struct korb_string *)argv[1];
    if (p->len == 0) return korb_str_dup(self);
    for (long i = 0; i + p->len <= s->len; i++) {
        if (memcmp(s->ptr + i, p->ptr, p->len) == 0) {
            VALUE out = korb_str_new(s->ptr, i);
            korb_str_concat(out, korb_str_new(r->ptr, r->len));
            korb_str_concat(out, korb_str_new(s->ptr + i + p->len, s->len - i - p->len));
            return out;
        }
    }
    return korb_str_dup(self);
}

static VALUE str_tr(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* simplistic: each char in arg0 maps to corresponding char in arg1 */
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING || BUILTIN_TYPE(argv[1]) != T_STRING) return korb_str_dup(self);
    struct korb_string *s = (struct korb_string *)self;
    struct korb_string *from = (struct korb_string *)argv[0];
    struct korb_string *to = (struct korb_string *)argv[1];
    char *out = korb_xmalloc_atomic(s->len + 1);
    for (long i = 0; i < s->len; i++) {
        char ch = s->ptr[i];
        out[i] = ch;
        for (long j = 0; j < from->len; j++) {
            if (from->ptr[j] == ch) {
                if (j < to->len) out[i] = to->ptr[j];
                else if (to->len > 0) out[i] = to->ptr[to->len-1];
                break;
            }
        }
    }
    out[s->len] = 0;
    return korb_str_new(out, s->len);
}

/* sprintf — limited; supports %d %s %x %o %X %b %f %g %% %c, with width/0pad */
static VALUE kernel_format(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    struct korb_string *fmt = (struct korb_string *)argv[0];
    int ai = 1;
    VALUE out = korb_str_new("", 0);
    for (long i = 0; i < fmt->len; i++) {
        if (fmt->ptr[i] != '%') { korb_str_concat(out, korb_str_new(fmt->ptr + i, 1)); continue; }
        i++;
        char spec[64]; int sl = 0;
        spec[sl++] = '%';
        /* flags */
        while (i < fmt->len && (fmt->ptr[i] == '-' || fmt->ptr[i] == '+' || fmt->ptr[i] == ' ' || fmt->ptr[i] == '#' || fmt->ptr[i] == '0')) {
            spec[sl++] = fmt->ptr[i++];
        }
        /* width */
        while (i < fmt->len && fmt->ptr[i] >= '0' && fmt->ptr[i] <= '9') spec[sl++] = fmt->ptr[i++];
        /* precision */
        if (i < fmt->len && fmt->ptr[i] == '.') {
            spec[sl++] = fmt->ptr[i++];
            while (i < fmt->len && fmt->ptr[i] >= '0' && fmt->ptr[i] <= '9') spec[sl++] = fmt->ptr[i++];
        }
        if (i >= fmt->len) break;
        char conv = fmt->ptr[i];
        spec[sl++] = conv;
        spec[sl] = 0;
        char buf[256];
        switch (conv) {
            case '%': buf[0] = '%'; buf[1] = 0; break;
            case 'd': case 'i': case 'u':
            case 'x': case 'X': case 'o': case 'b': case 'c': {
                long v = ai < argc && FIXNUM_P(argv[ai]) ? FIX2LONG(argv[ai]) : 0;
                if (conv == 'b') {
                    /* binary — manually */
                    char tmp[64]; int tl = 0;
                    unsigned long uv = (unsigned long)v;
                    if (uv == 0) tmp[tl++] = '0';
                    while (uv) { tmp[tl++] = '0' + (uv & 1); uv >>= 1; }
                    for (int j = 0; j < tl/2; j++) { char tch = tmp[j]; tmp[j] = tmp[tl-1-j]; tmp[tl-1-j] = tch; }
                    tmp[tl] = 0;
                    snprintf(buf, sizeof(buf), "%s", tmp);
                } else {
                    /* replace conv with ld */
                    if (conv == 'd' || conv == 'i' || conv == 'u') {
                        spec[sl-1] = 'l'; spec[sl++] = 'd'; spec[sl] = 0;
                        snprintf(buf, sizeof(buf), spec, v);
                    } else {
                        snprintf(buf, sizeof(buf), spec, (unsigned long)v);
                    }
                }
                ai++;
                break;
            }
            case 'f': case 'g': case 'e': case 'E': case 'G': {
                double dv = ai < argc ? korb_num2dbl(argv[ai]) : 0.0;
                snprintf(buf, sizeof(buf), spec, dv);
                ai++;
                break;
            }
            case 's': {
                VALUE v = ai < argc ? argv[ai] : korb_str_new("", 0);
                if (BUILTIN_TYPE(v) != T_STRING) v = korb_to_s(v);
                snprintf(buf, sizeof(buf), spec, ((struct korb_string *)v)->ptr);
                ai++;
                break;
            }
            default:
                snprintf(buf, sizeof(buf), "%%%c", conv);
        }
        korb_str_concat(out, korb_str_new_cstr(buf));
    }
    return out;
}

/* String#% — same as format but self is the format string */
static VALUE str_percent(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* arg may be a single value or an Array */
    VALUE *fargv;
    int fargc;
    VALUE single[2];
    if (argc == 1 && BUILTIN_TYPE(argv[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)argv[0];
        single[0] = self;
        VALUE *full = korb_xmalloc((1 + a->len) * sizeof(VALUE));
        full[0] = self;
        for (long i = 0; i < a->len; i++) full[1+i] = a->ptr[i];
        return kernel_format(c, self, 1 + (int)a->len, full);
    }
    /* single arg or multiple */
    VALUE *full = korb_xmalloc((1 + argc) * sizeof(VALUE));
    full[0] = self;
    for (int i = 0; i < argc; i++) full[1+i] = argv[i];
    return kernel_format(c, self, 1 + argc, full);
}

/* ---------- Array methods (extended) ---------- */

static VALUE ary_sort(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    /* shallow copy */
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    /* simple insertion sort using <=> via < */
    struct korb_array *ra = (struct korb_array *)r;
    for (long i = 1; i < n; i++) {
        VALUE v = ra->ptr[i];
        long j = i - 1;
        while (j >= 0) {
            VALUE comp;
            if (FIXNUM_P(ra->ptr[j]) && FIXNUM_P(v)) {
                if ((intptr_t)v >= (intptr_t)ra->ptr[j]) break;
            } else {
                /* fallback */
                comp = korb_funcall(c, ra->ptr[j], korb_intern("<=>"), 1, &v);
                if (FIXNUM_P(comp) && FIX2LONG(comp) <= 0) break;
                if (!FIXNUM_P(comp)) break;
            }
            ra->ptr[j+1] = ra->ptr[j];
            j--;
        }
        ra->ptr[j+1] = v;
    }
    return r;
}

static VALUE ary_sort_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* yield each, then sort by yielded value */
    struct korb_array *a = (struct korb_array *)self;
    long n = a->len;
    VALUE pairs = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, k);
        korb_ary_push(pair, a->ptr[i]);
        korb_ary_push(pairs, pair);
    }
    /* sort pairs by [0] */
    struct korb_array *p = (struct korb_array *)pairs;
    for (long i = 1; i < n; i++) {
        VALUE pi = p->ptr[i];
        VALUE ki = ((struct korb_array *)pi)->ptr[0];
        long j = i - 1;
        while (j >= 0) {
            VALUE pj = p->ptr[j];
            VALUE kj = ((struct korb_array *)pj)->ptr[0];
            VALUE cmp = korb_funcall(c, kj, korb_intern("<=>"), 1, &ki);
            if (FIXNUM_P(cmp) && FIX2LONG(cmp) <= 0) break;
            p->ptr[j+1] = p->ptr[j];
            j--;
        }
        p->ptr[j+1] = pi;
    }
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, ((struct korb_array *)p->ptr[i])->ptr[1]);
    return r;
}

static VALUE ary_zip(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = 0; i < a->len; i++) {
        VALUE tup = korb_ary_new_capa(1 + argc);
        korb_ary_push(tup, a->ptr[i]);
        for (int j = 0; j < argc; j++) {
            if (BUILTIN_TYPE(argv[j]) == T_ARRAY) {
                korb_ary_push(tup, korb_ary_aref(argv[j], i));
            } else korb_ary_push(tup, Qnil);
        }
        korb_ary_push(r, tup);
    }
    return r;
}

static VALUE ary_flatten(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* depth 1 */
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        if (BUILTIN_TYPE(a->ptr[i]) == T_ARRAY) {
            struct korb_array *b = (struct korb_array *)a->ptr[i];
            for (long j = 0; j < b->len; j++) korb_ary_push(r, b->ptr[j]);
        } else korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_compact(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) if (!NIL_P(a->ptr[i])) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_uniq(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        bool dup = false;
        struct korb_array *ra = (struct korb_array *)r;
        for (long j = 0; j < ra->len; j++) {
            if (korb_eq(ra->ptr[j], a->ptr[i])) { dup = true; break; }
        }
        if (!dup) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) return Qtrue;
    return Qfalse;
}

static VALUE ary_any_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (RTEST(a->ptr[i])) return Qtrue;
    }
    return Qfalse;
}

static VALUE ary_all_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        if (!RTEST(a->ptr[i])) return Qfalse;
    }
    return Qtrue;
}

static VALUE ary_min(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    for (long i = 1; i < a->len; i++) {
        VALUE cmp = korb_funcall(c, m, korb_intern("<=>"), 1, &a->ptr[i]);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) > 0) m = a->ptr[i];
    }
    return m;
}

static VALUE ary_max(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    for (long i = 1; i < a->len; i++) {
        VALUE cmp = korb_funcall(c, m, korb_intern("<=>"), 1, &a->ptr[i]);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) < 0) m = a->ptr[i];
    }
    return m;
}

static VALUE ary_sum(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE acc = argc > 0 ? argv[0] : INT2FIX(0);
    for (long i = 0; i < a->len; i++) {
        if (FIXNUM_P(acc) && FIXNUM_P(a->ptr[i])) {
            long s;
            if (!__builtin_add_overflow(FIX2LONG(acc), FIX2LONG(a->ptr[i]), &s) && FIXABLE(s))
                acc = INT2FIX(s);
            else acc = korb_int_plus(acc, a->ptr[i]);
        } else {
            acc = korb_funcall(c, acc, korb_intern("+"), 1, &a->ptr[i]);
        }
    }
    return acc;
}

static VALUE ary_each_slice(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qnil;
    long n = FIX2LONG(argv[0]);
    if (n <= 0) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i += n) {
        long end = i + n; if (end > a->len) end = a->len;
        VALUE slice = korb_ary_new_capa(end - i);
        for (long j = i; j < end; j++) korb_ary_push(slice, a->ptr[j]);
        korb_yield(c, 1, &slice);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE ary_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* not real Array#step, but stub */
    return self;
}

static VALUE ary_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_ARRAY && korb_eq(self, argv[0]));
}

static VALUE ary_pack(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* very limited pack — just "C*" (bytes) */
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *fmt = korb_str_cstr(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (strcmp(fmt, "C*") == 0) {
        char *buf = korb_xmalloc_atomic(a->len + 1);
        for (long i = 0; i < a->len; i++) {
            buf[i] = FIXNUM_P(a->ptr[i]) ? (char)(FIX2LONG(a->ptr[i]) & 0xff) : 0;
        }
        buf[a->len] = 0;
        return korb_str_new(buf, a->len);
    }
    return korb_str_new("", 0);
}

static VALUE ary_concat(CTX *c, VALUE self, int argc, VALUE *argv) {
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) == T_ARRAY) {
            struct korb_array *o = (struct korb_array *)argv[i];
            for (long j = 0; j < o->len; j++) korb_ary_push(self, o->ptr[j]);
        }
    }
    return self;
}

static VALUE ary_minus(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_ARRAY) return korb_ary_new();
    struct korb_array *a = (struct korb_array *)self;
    struct korb_array *b = (struct korb_array *)argv[0];
    VALUE r = korb_ary_new();
    for (long i = 0; i < a->len; i++) {
        bool found = false;
        for (long j = 0; j < b->len; j++) if (korb_eq(a->ptr[i], b->ptr[j])) { found = true; break; }
        if (!found) korb_ary_push(r, a->ptr[i]);
    }
    return r;
}

static VALUE ary_index(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) return INT2FIX(i);
    return Qnil;
}

static VALUE ary_reverse(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    VALUE r = korb_ary_new_capa(a->len);
    for (long i = a->len - 1; i >= 0; i--) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_clear(CTX *c, VALUE self, int argc, VALUE *argv) {
    ((struct korb_array *)self)->len = 0;
    return self;
}

static VALUE ary_unshift(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    /* shift right argc times */
    long oldlen = a->len;
    for (int i = 0; i < argc; i++) korb_ary_push(self, Qnil);
    for (long i = oldlen - 1; i >= 0; i--) a->ptr[i + argc] = a->ptr[i];
    for (int i = 0; i < argc; i++) a->ptr[i] = argv[i];
    return self;
}

static VALUE ary_shift(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE v = a->ptr[0];
    for (long i = 0; i + 1 < a->len; i++) a->ptr[i] = a->ptr[i+1];
    a->len--;
    return v;
}

static VALUE ary_transpose(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return korb_ary_new();
    /* All inner arrays must be same length */
    long n_outer = a->len;
    long n_inner = (BUILTIN_TYPE(a->ptr[0]) == T_ARRAY) ? ((struct korb_array *)a->ptr[0])->len : 0;
    VALUE r = korb_ary_new_capa(n_inner);
    for (long i = 0; i < n_inner; i++) {
        VALUE row = korb_ary_new_capa(n_outer);
        for (long j = 0; j < n_outer; j++) {
            VALUE inner = a->ptr[j];
            if (BUILTIN_TYPE(inner) == T_ARRAY && i < ((struct korb_array *)inner)->len) {
                korb_ary_push(row, ((struct korb_array *)inner)->ptr[i]);
            } else korb_ary_push(row, Qnil);
        }
        korb_ary_push(r, row);
    }
    return r;
}

static VALUE ary_count(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (argc == 0) return INT2FIX(a->len);
    long n = 0;
    for (long i = 0; i < a->len; i++) if (korb_eq(a->ptr[i], argv[0])) n++;
    return INT2FIX(n);
}

static VALUE ary_drop(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (n < 0) n = 0;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(a->len - n);
    for (long i = n; i < a->len; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_take(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return self;
    long n = FIX2LONG(argv[0]);
    struct korb_array *a = (struct korb_array *)self;
    if (n < 0) n = 0;
    if (n > a->len) n = a->len;
    VALUE r = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(r, a->ptr[i]);
    return r;
}

static VALUE ary_fill(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Array#fill(val) — fill all slots with val */
    if (argc < 1) return self;
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) a->ptr[i] = argv[0];
    return self;
}

static VALUE ary_sample(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    return a->ptr[0]; /* deterministic stub */
}

static VALUE ary_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_array *)self)->len == 0);
}

static VALUE ary_find(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE m = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) return a->ptr[i];
    }
    return Qnil;
}

static VALUE ary_min_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    VALUE mk = korb_yield(c, 1, &m);
    if (c->state != KORB_NORMAL) return Qnil;
    for (long i = 1; i < a->len; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE cmp = korb_funcall(c, mk, korb_intern("<=>"), 1, &k);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) > 0) { m = a->ptr[i]; mk = k; }
    }
    return m;
}

static VALUE ary_max_by(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_array *a = (struct korb_array *)self;
    if (a->len == 0) return Qnil;
    VALUE m = a->ptr[0];
    VALUE mk = korb_yield(c, 1, &m);
    if (c->state != KORB_NORMAL) return Qnil;
    for (long i = 1; i < a->len; i++) {
        VALUE k = korb_yield(c, 1, &a->ptr[i]);
        if (c->state != KORB_NORMAL) return Qnil;
        VALUE cmp = korb_funcall(c, mk, korb_intern("<=>"), 1, &k);
        if (FIXNUM_P(cmp) && FIX2LONG(cmp) < 0) { m = a->ptr[i]; mk = k; }
    }
    return m;
}

static VALUE ary_each_with_object(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    VALUE memo = argv[0];
    struct korb_array *a = (struct korb_array *)self;
    for (long i = 0; i < a->len; i++) {
        VALUE args[2] = { a->ptr[i], memo };
        korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return memo;
}

/* ---------- Hash methods (extended) ---------- */

static VALUE hash_keys(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(r, e->key);
    return r;
}

static VALUE hash_values(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) korb_ary_push(r, e->value);
    return r;
}

static VALUE hash_each_value(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_yield(c, 1, &e->value);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE hash_each_key(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_yield(c, 1, &e->key);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE hash_key_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    struct korb_hash *h = (struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) return Qtrue;
    }
    return Qfalse;
}

static VALUE hash_merge(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* shallow copy then merge args */
    struct korb_hash *src = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = src->first; e; e = e->next) {
        korb_hash_aset(r, e->key, e->value);
    }
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_HASH) continue;
        struct korb_hash *o = (struct korb_hash *)argv[i];
        for (struct korb_hash_entry *e = o->first; e; e = e->next) {
            korb_hash_aset(r, e->key, e->value);
        }
    }
    return r;
}

static VALUE hash_invert(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        korb_hash_aset(r, e->value, e->key);
    }
    return r;
}

static VALUE hash_to_a(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        korb_ary_push(r, pair);
    }
    return r;
}

static VALUE hash_delete(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    struct korb_hash *h = (struct korb_hash *)self;
    uint64_t hh = korb_hash_value(argv[0]);
    struct korb_hash_entry *prev = NULL;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, argv[0])) {
            VALUE v = e->value;
            if (prev) prev->next = e->next;
            else h->first = e->next;
            if (h->last == e) h->last = prev;
            h->size--;
            return v;
        }
        prev = e;
    }
    return Qnil;
}

static VALUE hash_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(BUILTIN_TYPE(argv[0]) == T_HASH);
}

static VALUE hash_dup(CTX *c, VALUE self, int argc, VALUE *argv) {
    return hash_merge(c, self, 0, NULL);
}

static VALUE hash_empty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(((struct korb_hash *)self)->size == 0);
}

static VALUE hash_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_ary_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(r, m);
    }
    return r;
}

static VALUE hash_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE r = korb_hash_new();
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE args[2] = { e->key, e->value };
        VALUE m = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_hash_aset(r, e->key, e->value);
    }
    return r;
}

static VALUE hash_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_hash *h = (struct korb_hash *)self;
    VALUE acc = argc > 0 ? argv[0] : Qnil;
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        VALUE pair = korb_ary_new_capa(2);
        korb_ary_push(pair, e->key);
        korb_ary_push(pair, e->value);
        VALUE args[2] = { acc, pair };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}

/* ---------- Object reflection ---------- */

static VALUE obj_send(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    return korb_funcall(c, self, name, argc - 1, argv + 1);
}

static VALUE obj_instance_variable_get(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    return korb_ivar_get(self, name);
}

static VALUE obj_instance_variable_set(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return Qnil;
    ID name;
    if (SYMBOL_P(argv[0])) name = korb_sym2id(argv[0]);
    else if (BUILTIN_TYPE(argv[0]) == T_STRING) name = korb_intern_n(((struct korb_string *)argv[0])->ptr, ((struct korb_string *)argv[0])->len);
    else return Qnil;
    korb_ivar_set(self, name, argv[1]);
    return argv[1];
}

static VALUE obj_method(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* return a Method-like object — for now just a string */
    return Qnil;
}

static VALUE obj_instance_of_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    return KORB_BOOL((VALUE)korb_class_of_class(self) == argv[0]);
}

static VALUE obj_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* default === is == */
    return KORB_BOOL(korb_eq(self, argv[0]));
}

/* ---------- Class === (for case/when class match) ---------- */
static VALUE class_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Class === obj  ⇔ obj.is_a?(self) */
    if (argc < 1) return Qfalse;
    if (BUILTIN_TYPE(self) != T_CLASS && BUILTIN_TYPE(self) != T_MODULE) return Qfalse;
    struct korb_class *target = (struct korb_class *)self;
    for (struct korb_class *k = korb_class_of_class(argv[0]); k; k = k->super) {
        if (k == target) return Qtrue;
    }
    return Qfalse;
}

/* ---------- Symbol#to_proc ---------- */
static VALUE sym_to_proc(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* return a proc that calls self method on its first arg */
    /* This is tricky to do without spawning AST.  We can't easily build a
     * proc that calls send on its first arg without runtime AST.  Skip for
     * now — optcarrot's hot path doesn't use &:method heavily. */
    return self;
}

/* ---------- Range methods (extended) ---------- */
static VALUE rng_step(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long step = argc >= 1 && FIXNUM_P(argv[0]) ? FIX2LONG(argv[0]) : 1;
    if (step == 0) return self;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    for (long i = b; i <= e; i += step) {
        VALUE v = INT2FIX(i);
        korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return self;
}

static VALUE rng_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    long sz = e - b + 1; if (r->exclude_end) sz--;
    if (sz < 0) sz = 0;
    return INT2FIX(sz);
}

static VALUE rng_include(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(argv[0])) return Qfalse;
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qfalse;
    long v = FIX2LONG(argv[0]);
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) return KORB_BOOL(v >= b && v < e);
    return KORB_BOOL(v >= b && v <= e);
}

static VALUE rng_map(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return korb_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new();
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        korb_ary_push(out, m);
    }
    return out;
}

static VALUE rng_select(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return korb_ary_new();
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE out = korb_ary_new();
    for (long i = b; i <= e; i++) {
        VALUE v = INT2FIX(i);
        VALUE m = korb_yield(c, 1, &v);
        if (c->state != KORB_NORMAL) return Qnil;
        if (RTEST(m)) korb_ary_push(out, v);
    }
    return out;
}

static VALUE rng_reduce(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct korb_range *r = (struct korb_range *)self;
    if (!FIXNUM_P(r->begin) || !FIXNUM_P(r->end)) return Qnil;
    long b = FIX2LONG(r->begin), e = FIX2LONG(r->end);
    if (r->exclude_end) e--;
    VALUE acc = argc > 0 ? argv[0] : INT2FIX(b++);
    for (long i = b; i <= e; i++) {
        VALUE args[2] = { acc, INT2FIX(i) };
        acc = korb_yield(c, 2, args);
        if (c->state != KORB_NORMAL) return Qnil;
    }
    return acc;
}

/* ---------- Integer methods (extended) ---------- */
static VALUE int_chr(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self)) return Qnil;
    char ch = (char)(FIX2LONG(self) & 0xff);
    return korb_str_new(&ch, 1);
}

static VALUE int_format(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Integer#to_s(base) */
    if (!FIXNUM_P(self)) return korb_to_s(self);
    long v = FIX2LONG(self);
    int base = argc >= 1 && FIXNUM_P(argv[0]) ? (int)FIX2LONG(argv[0]) : 10;
    char buf[80];
    if (base == 10) snprintf(buf, sizeof(buf), "%ld", v);
    else if (base == 16) snprintf(buf, sizeof(buf), "%lx", v);
    else if (base == 8) snprintf(buf, sizeof(buf), "%lo", v);
    else if (base == 2) {
        unsigned long uv = (unsigned long)v;
        char tmp[80]; int tl = 0;
        if (uv == 0) tmp[tl++] = '0';
        while (uv) { tmp[tl++] = '0' + (uv & 1); uv >>= 1; }
        for (int i = 0; i < tl/2; i++) { char t = tmp[i]; tmp[i] = tmp[tl-1-i]; tmp[tl-1-i] = t; }
        tmp[tl] = 0;
        return korb_str_new_cstr(tmp);
    }
    else snprintf(buf, sizeof(buf), "%ld", v);
    return korb_str_new_cstr(buf);
}

static VALUE int_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qfalse;
    if (FIXNUM_P(self) && FIXNUM_P(argv[0])) return KORB_BOOL(self == argv[0]);
    return KORB_BOOL(korb_eq(self, argv[0]));
}

static VALUE int_floor(CTX *c, VALUE self, int argc, VALUE *argv) {
    return self;
}

static VALUE int_abs(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (FIXNUM_P(self)) {
        long v = FIX2LONG(self);
        return INT2FIX(v < 0 ? -v : v);
    }
    return self;
}

static VALUE int_aref(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Integer#[i] — extract bit i */
    if (argc < 1 || !FIXNUM_P(argv[0])) return INT2FIX(0);
    if (!FIXNUM_P(self)) return INT2FIX(0);
    long n = FIX2LONG(self);
    long b = FIX2LONG(argv[0]);
    if (b < 0 || b >= 63) return INT2FIX(0);
    return INT2FIX((n >> b) & 1);
}

static VALUE int_bit_length(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (!FIXNUM_P(self)) return INT2FIX(0);
    long v = FIX2LONG(self);
    if (v < 0) v = ~v;
    int n = 0;
    while (v > 0) { n++; v >>= 1; }
    return INT2FIX(n);
}

static VALUE int_divmod(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return korb_ary_new();
    long a = FIX2LONG(self), b = FIX2LONG(argv[0]);
    if (b == 0) { korb_raise(c, NULL, "divided by 0"); return Qnil; }
    long q = a / b, m = a % b;
    if ((a ^ b) < 0 && m != 0) { q--; m += b; }
    VALUE r = korb_ary_new_capa(2);
    korb_ary_push(r, INT2FIX(q));
    korb_ary_push(r, INT2FIX(m));
    return r;
}

static VALUE int_pow(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || !FIXNUM_P(self) || !FIXNUM_P(argv[0])) return self;
    long b = FIX2LONG(self), e = FIX2LONG(argv[0]);
    long r = 1;
    while (e > 0) {
        if (e & 1) {
            long s;
            if (__builtin_mul_overflow(r, b, &s)) {
                /* fallback to bignum */
                return korb_int_mul(self, INT2FIX(1));  /* TODO */
            }
            r = s;
        }
        long s;
        if (__builtin_mul_overflow(b, b, &s)) break;
        b = s;
        e >>= 1;
    }
    return INT2FIX(r);
}

/* ---------- Float methods (extended) ---------- */
static VALUE flt_floor(CTX *c, VALUE self, int argc, VALUE *argv) {
    double v = ((struct korb_float *)self)->value;
    return INT2FIX((long)v);
}

static VALUE flt_eqq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(korb_eq(self, argv[0]));
}

/* ---------- File ---------- */
static VALUE file_read(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    const char *path = korb_str_cstr(argv[0]);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        korb_raise(c, NULL, "no such file -- %s", path);
        return Qnil;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = korb_xmalloc_atomic(sz + 1);
    fread(buf, 1, sz, fp);
    buf[sz] = 0;
    fclose(fp);
    return korb_str_new(buf, sz);
}

static VALUE file_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_str_new("", 0);
    for (int i = 0; i < argc; i++) {
        VALUE s = BUILTIN_TYPE(argv[i]) == T_STRING ? argv[i] : korb_to_s(argv[i]);
        if (i > 0) korb_str_concat(r, korb_str_new_cstr("/"));
        korb_str_concat(r, s);
    }
    return r;
}

static VALUE file_exist_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    return KORB_BOOL(korb_file_exists(korb_str_cstr(argv[0])));
}

static VALUE file_dirname(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(".", 1);
    return korb_str_new_cstr(korb_dirname(korb_str_cstr(argv[0])));
}

static VALUE file_basename(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *s = korb_str_cstr(argv[0]);
    const char *slash = strrchr(s, '/');
    return korb_str_new_cstr(slash ? slash + 1 : s);
}

static VALUE file_expand_path(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    /* simplistic: if absolute, return as-is; else prepend dir */
    const char *s = korb_str_cstr(argv[0]);
    if (s[0] == '/') return argv[0];
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        return korb_str_new_cstr(korb_join_path(korb_str_cstr(argv[1]), s));
    }
    return argv[0];
}

/* IO (stubbed via STDOUT / $stdout) */

/* ---------- Class.new etc ---------- */
static VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_CLASS) {
        korb_raise(c, NULL, "Class.new called on non-class");
        return Qnil;
    }
    struct korb_class *klass = (struct korb_class *)self;
    VALUE obj = korb_object_new(klass);
    /* call initialize if defined */
    struct korb_method *m = korb_class_find_method(klass, id_initialize);
    if (m) {
        korb_funcall(c, obj, id_initialize, argc, argv);
    }
    return obj;
}

static VALUE class_name(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(korb_id_name(((struct korb_class *)self)->name));
}

/* ---------- Symbol ---------- */
static VALUE sym_to_s(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_str_new_cstr(korb_id_name(korb_sym2id(self)));
}
static VALUE sym_eq(CTX *c, VALUE self, int argc, VALUE *argv) {
    return KORB_BOOL(self == argv[0]);
}

/* ---------- Boolean ---------- */
static VALUE true_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("true"); }
static VALUE false_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("false"); }
static VALUE nil_to_s(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr(""); }
static VALUE nil_inspect(CTX *c, VALUE self, int argc, VALUE *argv) { return korb_str_new_cstr("nil"); }

/* ---------- Proc ---------- */
extern VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv);

static VALUE proc_call(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (BUILTIN_TYPE(self) != T_PROC) return Qnil;
    struct korb_proc *p = (struct korb_proc *)self;
    VALUE *fp = p->env;
    VALUE prev_self = c->self;
    for (int i = 0; i < argc && (uint32_t)i < p->params_cnt; i++) {
        fp[p->param_base + i] = argv[i];
    }
    c->self = p->self;
    VALUE r = EVAL(c, p->body);
    c->self = prev_self;
    if (c->state == KORB_RETURN || c->state == KORB_BREAK) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

#define DEF(klass, name, fn, argc) \
    korb_class_add_method_cfunc((klass), korb_intern(name), (fn), (argc))

void korb_init_builtins(void) {
    /* Object methods */
    struct korb_class *cObj = korb_vm->object_class;
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
    DEF(cObj, "require_relative", kernel_require_relative, 1);
    DEF(cObj, "require", kernel_require, 1);
    DEF(cObj, "load", kernel_load, -1);
    DEF(cObj, "abort", kernel_abort, -1);
    DEF(cObj, "exit", kernel_exit, -1);

    /* Integer */
    struct korb_class *cInt = korb_vm->integer_class;
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
    struct korb_class *cFlt = korb_vm->float_class;
    DEF(cFlt, "+", flt_plus, 1);
    DEF(cFlt, "-", flt_minus, 1);
    DEF(cFlt, "*", flt_mul, 1);
    DEF(cFlt, "/", flt_div, 1);
    DEF(cFlt, "to_s", flt_to_s, 0);

    /* String */
    struct korb_class *cStr = korb_vm->string_class;
    DEF(cStr, "+", str_plus, 1);
    DEF(cStr, "<<", str_concat, 1);
    DEF(cStr, "concat", str_concat, 1);
    DEF(cStr, "size", str_size, 0);
    DEF(cStr, "length", str_size, 0);
    DEF(cStr, "==", str_eq, 1);
    DEF(cStr, "to_s", str_to_s, 0);
    DEF(cStr, "to_sym", str_to_sym, 0);

    /* Array */
    struct korb_class *cAry = korb_vm->array_class;
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
    struct korb_class *cHsh = korb_vm->hash_class;
    DEF(cHsh, "[]", hash_aref, 1);
    DEF(cHsh, "[]=", hash_aset, 2);
    DEF(cHsh, "size", hash_size, 0);
    DEF(cHsh, "length", hash_size, 0);
    DEF(cHsh, "each", hash_each, 0);

    /* Range */
    struct korb_class *cRng = korb_vm->range_class;
    DEF(cRng, "each", rng_each, 0);
    DEF(cRng, "first", rng_first, 0);
    DEF(cRng, "last", rng_last, 0);
    DEF(cRng, "to_a", rng_to_a, 0);

    /* Class */
    struct korb_class *cCls = korb_vm->class_class;
    DEF(cCls, "new", class_new, -1);
    DEF(cCls, "name", class_name, 0);

    /* Module — applies to both Class and Module */
    struct korb_class *cMod = korb_vm->module_class;
    DEF(cMod, "attr_reader",   module_attr_reader,   -1);
    DEF(cMod, "attr_writer",   module_attr_writer,   -1);
    DEF(cMod, "attr_accessor", module_attr_accessor, -1);
    DEF(cMod, "include",       module_include,       -1);
    DEF(cMod, "private",       module_private,       -1);
    DEF(cMod, "public",        module_public,        -1);
    DEF(cMod, "protected",     module_protected,     -1);
    DEF(cMod, "module_function", module_module_function, -1);
    DEF(cMod, "define_method", module_define_method, -1);
    DEF(cMod, "const_get",     module_const_get,     -1);
    DEF(cMod, "const_set",     module_const_set,     -1);
    /* mirror onto Class so class-level def works at top level */
    DEF(cCls, "attr_reader",   module_attr_reader,   -1);
    DEF(cCls, "attr_writer",   module_attr_writer,   -1);
    DEF(cCls, "attr_accessor", module_attr_accessor, -1);
    DEF(cCls, "include",       module_include,       -1);
    DEF(cCls, "private",       module_private,       -1);
    DEF(cCls, "public",        module_public,        -1);
    DEF(cCls, "protected",     module_protected,     -1);
    DEF(cCls, "===",           class_eqq,            1);
    DEF(cMod, "===",           class_eqq,            1);

    /* extra Object methods */
    DEF(cObj, "send",                  obj_send,                 -1);
    DEF(cObj, "__send__",              obj_send,                 -1);
    DEF(cObj, "public_send",           obj_send,                 -1);
    DEF(cObj, "instance_variable_get", obj_instance_variable_get, 1);
    DEF(cObj, "instance_variable_set", obj_instance_variable_set, 2);
    DEF(cObj, "method",                obj_method,                1);
    DEF(cObj, "instance_of?",          obj_instance_of_p,         1);
    DEF(cObj, "===",                   obj_eqq,                   1);
    DEF(cObj, "tap",                   kernel_inspect,            0); /* stub */
    DEF(cObj, "format",                kernel_format,            -1);
    DEF(cObj, "sprintf",               kernel_format,            -1);
    DEF(cObj, "printf",                kernel_print,             -1);

    /* extra Integer */
    DEF(cInt, "chr",   int_chr, 0);
    DEF(cInt, "===",   int_eqq, 1);
    DEF(cInt, "floor", int_floor, -1);
    DEF(cInt, "ceil",  int_floor, -1);
    DEF(cInt, "round", int_floor, -1);
    DEF(cInt, "abs",   int_abs, 0);
    DEF(cInt, "[]",    int_aref, -1);
    DEF(cInt, "bit_length", int_bit_length, 0);
    DEF(cInt, "divmod", int_divmod, 1);
    DEF(cInt, "**",    int_pow, 1);

    /* extra Float */
    DEF(cFlt, "floor", flt_floor, -1);
    DEF(cFlt, "===",   flt_eqq, 1);

    /* extra String */
    DEF(cStr, "split",       str_split,       -1);
    DEF(cStr, "chomp",       str_chomp,       -1);
    DEF(cStr, "strip",       str_strip,        0);
    DEF(cStr, "to_i",        str_to_i,        -1);
    DEF(cStr, "to_f",        str_to_f,         0);
    DEF(cStr, "[]",          str_aref,        -1);
    DEF(cStr, "[]=",         str_aset,        -1);
    DEF(cStr, "chars",       str_chars,        0);
    DEF(cStr, "bytes",       str_bytes,        0);
    DEF(cStr, "each_char",   str_each_char,    0);
    DEF(cStr, "each_line",   str_split,       -1); /* approximate */
    DEF(cStr, "start_with?", str_start_with,  -1);
    DEF(cStr, "end_with?",   str_end_with,    -1);
    DEF(cStr, "include?",    str_include,     -1);
    DEF(cStr, "replace",     str_replace,      1);
    DEF(cStr, "reverse",     str_reverse,      0);
    DEF(cStr, "upcase",      str_upcase,       0);
    DEF(cStr, "downcase",    str_downcase,     0);
    DEF(cStr, "empty?",      str_empty_p,      0);
    DEF(cStr, "*",           str_mul,          1);
    DEF(cStr, "hash",        str_hash,         0);
    DEF(cStr, "===",         str_eqq,          1);
    DEF(cStr, "gsub",        str_gsub,        -1);
    DEF(cStr, "sub",         str_sub,         -1);
    DEF(cStr, "tr",          str_tr,          -1);
    DEF(cStr, "tr_s",        str_tr,          -1);
    DEF(cStr, "%",           str_percent,     -1);
    DEF(cStr, "inspect",     kernel_inspect,   0);
    DEF(cStr, "dup",         str_replace,      0); /* stub: returns same not-quite-dup */
    DEF(cStr, "=~",          str_match_op, 1);
    DEF(cStr, "match?",      str_match_p, -1);
    DEF(cStr, "match",       str_match, -1);
    DEF(cStr, "scan",        str_scan, 1);

    /* extra Array */
    DEF(cAry, "sort",       ary_sort,       -1);
    DEF(cAry, "sort_by",    ary_sort_by,     0);
    DEF(cAry, "zip",        ary_zip,        -1);
    DEF(cAry, "flatten",    ary_flatten,    -1);
    DEF(cAry, "compact",    ary_compact,     0);
    DEF(cAry, "uniq",       ary_uniq,       -1);
    DEF(cAry, "include?",   ary_include,     1);
    DEF(cAry, "any?",       ary_any_p,      -1);
    DEF(cAry, "all?",       ary_all_p,      -1);
    DEF(cAry, "min",        ary_min,        -1);
    DEF(cAry, "max",        ary_max,        -1);
    DEF(cAry, "sum",        ary_sum,        -1);
    DEF(cAry, "each_slice", ary_each_slice,  1);
    DEF(cAry, "step",       ary_step,       -1);
    DEF(cAry, "===",        ary_eqq,         1);
    DEF(cAry, "pack",       ary_pack,       -1);
    DEF(cAry, "concat",     ary_concat,     -1);
    DEF(cAry, "-",          ary_minus,       1);
    DEF(cAry, "+",          ary_concat,     -1);
    DEF(cAry, "index",      ary_index,      -1);
    DEF(cAry, "find_index", ary_index,      -1);
    DEF(cAry, "reverse",    ary_reverse,     0);
    DEF(cAry, "clear",      ary_clear,       0);
    DEF(cAry, "unshift",    ary_unshift,    -1);
    DEF(cAry, "prepend",    ary_unshift,    -1);
    DEF(cAry, "shift",      ary_shift,      -1);
    DEF(cAry, "each_with_object", ary_each_with_object, 1);
    DEF(cAry, "transpose", ary_transpose, 0);
    DEF(cAry, "count",     ary_count, -1);
    DEF(cAry, "drop",      ary_drop,   1);
    DEF(cAry, "take",      ary_take,   1);
    DEF(cAry, "fill",      ary_fill,  -1);
    DEF(cAry, "sample",    ary_sample, -1);
    DEF(cAry, "empty?",    ary_empty_p, 0);
    DEF(cAry, "find",      ary_find, 0);
    DEF(cAry, "detect",    ary_find, 0);
    DEF(cAry, "min_by",    ary_min_by, 0);
    DEF(cAry, "max_by",    ary_max_by, 0);

    /* extra Hash */
    DEF(cHsh, "keys",       hash_keys,       0);
    DEF(cHsh, "values",     hash_values,     0);
    DEF(cHsh, "each_value", hash_each_value, 0);
    DEF(cHsh, "each_key",   hash_each_key,   0);
    DEF(cHsh, "each_pair",  hash_each,       0);
    DEF(cHsh, "key?",       hash_key_p,      1);
    DEF(cHsh, "has_key?",   hash_key_p,      1);
    DEF(cHsh, "include?",   hash_key_p,      1);
    DEF(cHsh, "merge",      hash_merge,     -1);
    DEF(cHsh, "merge!",     hash_merge,     -1);
    DEF(cHsh, "invert",     hash_invert,     0);
    DEF(cHsh, "to_a",       hash_to_a,       0);
    DEF(cHsh, "delete",     hash_delete,    -1);
    DEF(cHsh, "===",        hash_eqq,        1);
    DEF(cHsh, "dup",        hash_dup,        0);
    DEF(cHsh, "clone",      hash_dup,        0);
    DEF(cHsh, "empty?",     hash_empty_p,    0);
    DEF(cHsh, "map",        hash_map,        0);
    DEF(cHsh, "collect",    hash_map,        0);
    DEF(cHsh, "select",     hash_select,     0);
    DEF(cHsh, "filter",     hash_select,     0);
    DEF(cHsh, "reduce",     hash_reduce,    -1);
    DEF(cHsh, "inject",     hash_reduce,    -1);

    /* extra Range */
    DEF(cRng, "step",     rng_step,    -1);
    DEF(cRng, "size",     rng_size,     0);
    DEF(cRng, "length",   rng_size,     0);
    DEF(cRng, "include?", rng_include, -1);
    DEF(cRng, "===",      rng_include, -1);
    DEF(cRng, "map",      rng_map,      0);
    DEF(cRng, "collect",  rng_map,      0);
    DEF(cRng, "select",   rng_select,   0);
    DEF(cRng, "filter",   rng_select,   0);
    DEF(cRng, "reduce",   rng_reduce,  -1);
    DEF(cRng, "inject",   rng_reduce,  -1);

    /* extra Symbol additions later (cSym defined further down) */

    /* Struct.new */
    /* Create Struct class object */
    struct korb_class *cStruct = korb_class_new(korb_intern("Struct"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("Struct"), (VALUE)cStruct);
    /* Struct.new is a class-level cfunc — install on Class so any class can call .new */
    /* But only Struct itself should have this constructor.  We add it on the class itself's
     * method table; calling Struct.new dispatches to class_of(Struct) which is Class.
     * Workaround: install on the Struct class's "self class" which is Class — but that
     * makes ALL classes have struct_class_new.  Instead, install a static name like
     * "__new_struct__" and use a stub cfunc on Struct that detects Struct === self.
     *
     * Simpler: add Class#new to delegate to self.class_new if class is Struct.
     * Even simpler: replace Class#new with a wrapper that handles Struct specially. */
    /* For our purposes, just add Struct as a singleton-like method to cCls keyed by the
     * actual class identity check: we install struct_class_new on cCls under "new_struct"
     * and add a method on Struct that calls it. */
    /* ... easier: just inject a method on cStruct at the "class level" via metaclass —
     * but we don't model singleton classes.  Instead, the simplest hack: install a cfunc
     * on Class itself that checks if self == Struct and calls struct_class_new. */
    /* Actually even simpler: make Struct.new == Class.new + struct_class_new logic.
     * Approach: provide a builtin on Class that, when self is Struct, returns a new struct
     * class.  For other classes, falls back to normal new. */
    /* implementing as: replace class_new */
    /* We modify class_new defined above — but it's static.  Instead add a layered method. */
    {
        extern VALUE class_new(CTX *c, VALUE self, int argc, VALUE *argv);
        /* not exposed — we need a wrapper.  Define inline: */
    }
    /* Add struct_class_new under name "new" on Struct.  Since dispatch goes through
     * class_of(Struct) = Class, NOT Struct itself — we need to use a different approach.
     * For now, let users call Struct.new(...) and ensure that lookup goes to Struct's
     * own metaclass.  We create a special ko_class for Struct's metaclass with .new
     * pointing to struct_class_new. */
    {
        struct korb_class *cStructMeta = korb_class_new(korb_intern("StructMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cStructMeta, korb_intern("new"), struct_class_new, -1);
        cStruct->basic.klass = (VALUE)cStructMeta;
    }

    /* File class */
    struct korb_class *cFile = korb_class_new(korb_intern("File"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("File"), (VALUE)cFile);
    {
        struct korb_class *cFileMeta = korb_class_new(korb_intern("FileMeta"), korb_vm->class_class, T_CLASS);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("read"), file_read, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("join"), file_join, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exist?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("exists?"), file_exist_p, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("dirname"), file_dirname, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("basename"), file_basename, -1);
        korb_class_add_method_cfunc(cFileMeta, korb_intern("expand_path"), file_expand_path, -1);
        cFile->basic.klass = (VALUE)cFileMeta;
    }

    /* IO / STDOUT / $stdout */
    struct korb_class *cIO = korb_class_new(korb_intern("IO"), korb_vm->object_class, T_OBJECT);
    korb_const_set(korb_vm->object_class, korb_intern("IO"), (VALUE)cIO);
    /* dummy STDOUT/STDERR */
    VALUE stdout_obj = korb_object_new(cIO);
    VALUE stderr_obj = korb_object_new(cIO);
    korb_const_set(korb_vm->object_class, korb_intern("STDOUT"), stdout_obj);
    korb_const_set(korb_vm->object_class, korb_intern("STDERR"), stderr_obj);
    korb_const_set(korb_vm->object_class, korb_intern("STDIN"), korb_object_new(cIO));
    /* IO#puts / write methods */
    korb_class_add_method_cfunc(cIO, korb_intern("puts"), kernel_puts, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("print"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("write"), kernel_print, -1);
    korb_class_add_method_cfunc(cIO, korb_intern("flush"), kernel_inspect, 0);
    korb_class_add_method_cfunc(cIO, korb_intern("sync="), kernel_inspect, 1);

    /* gvars */
    korb_gvar_set(korb_intern("$stdout"), stdout_obj);
    korb_gvar_set(korb_intern("$stderr"), stderr_obj);

    /* Symbol */
    struct korb_class *cSym = korb_vm->symbol_class;
    DEF(cSym, "to_s", sym_to_s, 0);
    DEF(cSym, "==", sym_eq, 1);
    DEF(cSym, "to_proc", sym_to_proc, 0);
    DEF(cSym, "===", sym_eq, 1);
    DEF(cSym, "inspect", kernel_inspect, 0);

    /* Boolean / Nil */
    DEF(korb_vm->true_class, "to_s", true_to_s, 0);
    DEF(korb_vm->false_class, "to_s", false_to_s, 0);
    DEF(korb_vm->nil_class, "to_s", nil_to_s, 0);
    DEF(korb_vm->nil_class, "inspect", nil_inspect, 0);

    /* Proc */
    struct korb_class *cPrc = korb_vm->proc_class;
    DEF(cPrc, "call", proc_call, -1);
    DEF(cPrc, "[]", proc_call, -1);

    /* Make sure ARGV is at least an empty array; main.c will override */
    korb_const_set(korb_vm->object_class, korb_intern("ARGV"), korb_ary_new());
    /* ENV stub: empty hash */
    korb_const_set(korb_vm->object_class, korb_intern("ENV"), korb_hash_new());
}
