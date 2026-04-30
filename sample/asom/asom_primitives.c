// asom: built-in primitives.
//
// Each primitive is a C function `(CTX *, VALUE self, [args...]) -> VALUE`
// installed on its class by `asom_install_primitives` at boot. We provide
// enough surface for typical SOM programs (Hello, Smoke, AreWeFastYet
// benchmarks) to run without auto-loading the SOM standard library.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "context.h"
#include "asom_runtime.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static struct asom_string *to_string(VALUE v) { return (struct asom_string *)ASOM_VAL2OBJ(v); }
static struct asom_array  *to_array (VALUE v) { return (struct asom_array  *)ASOM_VAL2OBJ(v); }
static struct asom_block  *to_block (VALUE v) { return (struct asom_block  *)ASOM_VAL2OBJ(v); }
static struct asom_class  *to_class (VALUE v) { return (struct asom_class  *)ASOM_VAL2OBJ(v); }
static struct asom_double *to_double(VALUE v) { return (struct asom_double *)ASOM_VAL2OBJ(v); }

static int
is_truthy(CTX *c, VALUE v) { return v == c->val_true; }

static double
to_double_value(CTX *c, VALUE v)
{
    if (ASOM_IS_FLO(v)) return asom_val2flo(v);
    if (ASOM_IS_INT(v)) return (double)ASOM_VAL2INT(v);
    if (ASOM_IS_OBJ(v)) {
        struct asom_object *o = ASOM_VAL2OBJ(v);
        if (o && o->klass == c->cls_double) return ((struct asom_double *)o)->value;
    }
    return 0.0;
}

// `to_double` (used by Double primitives that have already gated on the
// receiver's class) needs to read through the flonum tag too.
static inline double
asom_val_double(VALUE v)
{
    if (ASOM_IS_FLO(v)) return asom_val2flo(v);
    return ((struct asom_double *)ASOM_VAL2OBJ(v))->value;
}

static VALUE
make_cstr_string(CTX *c, const char *s) { return asom_string_new(c, s, strlen(s)); }

// True if `v` is any Block subclass (Block, Block1, Block2, Block3).
static int
is_block_obj(CTX *c, VALUE v)
{
    if (!ASOM_IS_OBJ(v)) return 0;
    struct asom_object *o = ASOM_VAL2OBJ(v);
    if (!o) return 0;
    for (struct asom_class *cls = o->klass; cls; cls = cls->superclass) {
        if (cls == c->cls_block) return 1;
    }
    return 0;
}

// Send `value` to v, transparently invoking it if it's a Block; otherwise
// delegate via a regular send. This matches SOM's polymorphic Block/Object
// `value` convention used by ifTrue:, deny:, etc.
static VALUE
send_value(CTX *c, VALUE v)
{
    if (is_block_obj(c, v)) {
        return asom_block_invoke(c, (struct asom_block *)ASOM_VAL2OBJ(v), NULL, 0);
    }
    return v; // Object>>value answers self in SOM; mimic that.
}

static void
def_prim(struct asom_class *cls, const char *sel, void *fn, uint32_t nargs)
{
    struct asom_method *m = calloc(1, sizeof(*m));
    m->selector = asom_intern_cstr(sel);
    m->primitive = fn;
    m->num_params = nargs;
    m->holder = cls;
    asom_class_define_method(cls, m);
}

static void
def_prim_kind(struct asom_class *cls, const char *sel, void *fn, uint32_t nargs,
              enum asom_prim_kind kind)
{
    struct asom_method *m = calloc(1, sizeof(*m));
    m->selector = asom_intern_cstr(sel);
    m->primitive = fn;
    m->num_params = nargs;
    m->holder = cls;
    m->prim_kind = kind;
    asom_class_define_method(cls, m);
}

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

static VALUE
obj_class(CTX *c, VALUE self)
{
    return ASOM_OBJ2VAL(asom_class_of(c, self));
}

static VALUE
obj_eq_eq(CTX *c, VALUE self, VALUE other) { return self == other ? c->val_true : c->val_false; }

static VALUE
obj_neq_neq(CTX *c, VALUE self, VALUE other) { return self != other ? c->val_true : c->val_false; }

static VALUE
obj_eq(CTX *c, VALUE self, VALUE other) { return obj_eq_eq(c, self, other); }

static VALUE
obj_neq(CTX *c, VALUE self, VALUE other) { return obj_neq_neq(c, self, other); }

static VALUE
obj_isnil(CTX *c, VALUE self) { return self == c->val_nil ? c->val_true : c->val_false; }

static VALUE
obj_notnil(CTX *c, VALUE self) { return self != c->val_nil ? c->val_true : c->val_false; }

static VALUE
obj_value(CTX *c, VALUE self) { (void)c; return self; }

static VALUE
obj_hashcode(CTX *c, VALUE self)
{
    (void)c;
    if (ASOM_IS_INT(self)) return self;
    return ASOM_INT2VAL((intptr_t)((uintptr_t)self >> 3));
}

static VALUE
obj_asString(CTX *c, VALUE self)
{
    struct asom_class *cls = asom_class_of(c, self);
    char buf[128];
    snprintf(buf, sizeof(buf), "instance of %s",
             cls && cls->name ? cls->name : "?");
    return make_cstr_string(c, buf);
}

static VALUE
obj_print(CTX *c, VALUE self)
{
    VALUE s = asom_send(c, self, asom_intern_cstr("asString"), 0, NULL, NULL);
    fputs(to_string(s)->bytes, stdout);
    return self;
}

static VALUE
obj_println(CTX *c, VALUE self)
{
    obj_print(c, self);
    putchar('\n');
    return self;
}

static VALUE
obj_inspect(CTX *c, VALUE self) { return obj_println(c, self); }

static VALUE
obj_ifnil_(CTX *c, VALUE self, VALUE block)
{
    return self == c->val_nil ? send_value(c, block) : self;
}
static VALUE
obj_ifnotnil_(CTX *c, VALUE self, VALUE block)
{
    return self != c->val_nil ? send_value(c, block) : c->val_nil;
}
static VALUE
obj_ifnil_ifnotnil_(CTX *c, VALUE self, VALUE bnil, VALUE bnot)
{
    return self == c->val_nil ? send_value(c, bnil) : send_value(c, bnot);
}
static VALUE
obj_ifnotnil_ifnil_(CTX *c, VALUE self, VALUE bnot, VALUE bnil)
{
    return self == c->val_nil ? send_value(c, bnil) : send_value(c, bnot);
}

static VALUE
obj_subclass_responsibility(CTX *c, VALUE self)
{
    (void)c; (void)self;
    fprintf(stderr, "asom: subclassResponsibility was not overridden\n");
    exit(1);
}

static VALUE
obj_error_(CTX *c, VALUE self, VALUE msg)
{
    (void)self;
    if (ASOM_IS_OBJ(msg)) {
        struct asom_object *o = ASOM_VAL2OBJ(msg);
        if (o && (o->klass == c->cls_string || o->klass == c->cls_symbol)) {
            fprintf(stderr, "asom: error: %s\n", to_string(msg)->bytes);
            exit(1);
        }
    }
    fprintf(stderr, "asom: error: <object>\n");
    exit(1);
}

// `obj perform: aSymbol`  /  `obj perform: aSymbol withArguments: anArray`
// — send the selector to self, with optional explicit args. The args array
// length must match the selector's arity.
static VALUE
obj_perform_(CTX *c, VALUE self, VALUE sel)
{
    if (!ASOM_IS_OBJ(sel)) {
        fprintf(stderr, "asom: perform: expects a Symbol\n"); exit(1);
    }
    struct asom_string *s = to_string(sel);
    return asom_send(c, self, asom_intern_cstr(s->bytes), 0, NULL, NULL);
}

static VALUE
obj_perform_with_args_(CTX *c, VALUE self, VALUE sel, VALUE argsv)
{
    struct asom_string *s = to_string(sel);
    struct asom_array *args = to_array(argsv);
    return asom_send(c, self, asom_intern_cstr(s->bytes), args->len, args->data, NULL);
}

static VALUE
obj_perform_in_super_(CTX *c, VALUE self, VALUE sel, VALUE klass)
{
    // Method lookup starts in the given class itself (not its superclass).
    // Mirrors the Smalltalk `perform:inSuperclass:` convention used by
    // ReflectionTest's super-send mirror.
    struct asom_string *s = to_string(sel);
    struct asom_class *start = to_class(klass);
    struct asom_method *m = asom_class_lookup(start, asom_intern_cstr(s->bytes));
    if (!m) {
        // Fall back to a regular send so we don't exit(1) on edge cases.
        return asom_send(c, self, asom_intern_cstr(s->bytes), 0, NULL, NULL);
    }
    return asom_invoke_method(c, m, self, NULL, 0);
}

// `,` (Object>>,) builds a Vector with self + element appended. We follow
// SOM's stdlib convention by lazy-loading Vector.som and dispatching its
// `new` and `append:` so Object>>, gives the same semantics other SOMs do
// (used heavily by TestHarness). Falls back to a 2-element Array if the
// classpath has no Vector.som.
static VALUE
obj_comma(CTX *c, VALUE self, VALUE other)
{
    struct asom_class *vec = asom_load_class(c, "Vector");
    if (!vec) {
        VALUE arr = asom_array_new(c, 2);
        to_array(arr)->data[0] = self;
        to_array(arr)->data[1] = other;
        return arr;
    }
    VALUE v = asom_send(c, ASOM_OBJ2VAL(vec), asom_intern_cstr("new"), 0, NULL, NULL);
    VALUE a1[1] = { self };
    v = asom_send(c, v, asom_intern_cstr("append:"), 1, a1, NULL);
    VALUE a2[1] = { other };
    return asom_send(c, v, asom_intern_cstr("append:"), 1, a2, NULL);
}

// ---------------------------------------------------------------------------
// Class (for `Foo new`, `Foo new: n`, `Foo name`, `Foo superclass`)
// ---------------------------------------------------------------------------

static VALUE
class_new(CTX *c, VALUE self)
{
    struct asom_class *cls = to_class(self);
    if (cls == c->cls_array) return asom_array_new(c, 0);
    return asom_object_new(c, cls);
}

static VALUE
class_new_size(CTX *c, VALUE self, VALUE size)
{
    struct asom_class *cls = to_class(self);
    intptr_t n = ASOM_VAL2INT(size);
    if (n < 0) n = 0;
    if (cls == c->cls_array) return asom_array_new(c, (uint32_t)n);
    fprintf(stderr, "asom: %s new: not supported\n", cls && cls->name ? cls->name : "?");
    exit(1);
}

static VALUE
class_new_size_withall_(CTX *c, VALUE self, VALUE size, VALUE init)
{
    struct asom_class *cls = to_class(self);
    intptr_t n = ASOM_VAL2INT(size);
    if (n < 0) n = 0;
    if (cls != c->cls_array) {
        fprintf(stderr, "asom: %s new:withAll: not supported\n", cls && cls->name ? cls->name : "?");
        exit(1);
    }
    VALUE arr = asom_array_new(c, (uint32_t)n);
    if (is_block_obj(c, init)) {
        // Reference SOM semantics: Array new:size withAll: aBlock fills
        // each slot by invoking aBlock once.
        struct asom_block *b = (struct asom_block *)ASOM_VAL2OBJ(init);
        if (true) {
            for (uint32_t i = 0; i < (uint32_t)n; i++) {
                to_array(arr)->data[i] = asom_block_invoke(c, b, NULL, 0);
            }
            return arr;
        }
    }
    for (uint32_t i = 0; i < (uint32_t)n; i++) to_array(arr)->data[i] = init;
    return arr;
}

static VALUE
class_name(CTX *c, VALUE self)
{
    struct asom_class *cls = to_class(self);
    return asom_intern_symbol(c, cls && cls->name ? cls->name : "?");
}

static VALUE
class_superclass(CTX *c, VALUE self)
{
    struct asom_class *cls = to_class(self);
    return cls && cls->superclass ? ASOM_OBJ2VAL(cls->superclass) : c->val_nil;
}

// Lightweight method-mirror object — wraps an asom_method so reflective
// queries like `m signature` can return the selector as a Symbol.
struct asom_method_mirror {
    struct asom_object hdr;
    struct asom_method *method;
};

static VALUE
class_methods(CTX *c, VALUE self)
{
    // Return method mirrors in *definition order* (source order for parsed
    // classes; primitive-install order for bootstrap classes), as expected
    // by reflective tests like `(Object methods at: 1) signature = #class`.
    struct asom_class *cls = to_class(self);
    VALUE arr = asom_array_new(c, cls->methods.order_cnt);
    for (uint32_t i = 0; i < cls->methods.order_cnt; i++) {
        struct asom_method *m = cls->methods.ordered[i];
        struct asom_method_mirror *mo = calloc(1, sizeof(*mo));
        mo->hdr.klass = c->cls_method;
        mo->method = m;
        to_array(arr)->data[i] = ASOM_OBJ2VAL(mo);
    }
    return arr;
}

static VALUE
method_signature(CTX *c, VALUE self)
{
    struct asom_method_mirror *mo = (struct asom_method_mirror *)ASOM_VAL2OBJ(self);
    return asom_intern_symbol(c, mo->method ? mo->method->selector : "?");
}

static VALUE
method_holder(CTX *c, VALUE self)
{
    struct asom_method_mirror *mo = (struct asom_method_mirror *)ASOM_VAL2OBJ(self);
    return mo->method && mo->method->holder ? ASOM_OBJ2VAL(mo->method->holder) : c->val_nil;
}

static VALUE
class_PositiveInfinity(CTX *c, VALUE self) { (void)self; return asom_double_new(c, INFINITY); }

// `Foo asString` returns the class name as a String. Without this, the generic
// Object>>asString would say "instance of Foo class".
static VALUE
class_asString(CTX *c, VALUE self)
{
    struct asom_class *cls = to_class(self);
    return make_cstr_string(c, cls && cls->name ? cls->name : "?");
}

static VALUE
class_fromString_(CTX *c, VALUE self, VALUE str)
{
    struct asom_class *cls = to_class(self);
    if (!ASOM_IS_OBJ(str)) return c->val_nil;
    const char *bytes = to_string(str)->bytes;
    if (cls == c->cls_integer) return ASOM_INT2VAL((intptr_t)strtoll(bytes, NULL, 10));
    if (cls == c->cls_double)  return asom_double_new(c, strtod(bytes, NULL));
    fprintf(stderr, "asom: %s fromString: not supported\n", cls && cls->name ? cls->name : "?");
    return c->val_nil;
}

static VALUE
class_fields(CTX *c, VALUE self)
{
    struct asom_class *cls = to_class(self);
    VALUE arr = asom_array_new(c, cls->num_instance_fields);
    for (uint32_t i = 0; i < cls->num_instance_fields; i++) {
        to_array(arr)->data[i] = asom_intern_symbol(c, cls->field_names[i]);
    }
    return arr;
}

// Object reflection: read/write instance fields by 1-based index or by
// symbol name. Used by ReflectionTest and a few stdlib helpers.
static VALUE
obj_instVarAt_(CTX *c, VALUE self, VALUE idx)
{
    if (!ASOM_IS_OBJ(self)) return c->val_nil;
    struct asom_object *o = ASOM_VAL2OBJ(self);
    intptr_t i = ASOM_VAL2INT(idx);
    if (i < 1 || (uint32_t)i > o->klass->num_instance_fields) return c->val_nil;
    VALUE *fields = (VALUE *)(o + 1);
    return fields[i - 1];
}

static VALUE
obj_instVarAt_put_(CTX *c, VALUE self, VALUE idx, VALUE val)
{
    if (!ASOM_IS_OBJ(self)) return val;
    struct asom_object *o = ASOM_VAL2OBJ(self);
    intptr_t i = ASOM_VAL2INT(idx);
    if (i < 1 || (uint32_t)i > o->klass->num_instance_fields) return val;
    VALUE *fields = (VALUE *)(o + 1);
    return fields[i - 1] = val;
}

static VALUE
obj_instVarNamed_(CTX *c, VALUE self, VALUE name)
{
    if (!ASOM_IS_OBJ(self) || !ASOM_IS_OBJ(name)) return c->val_nil;
    struct asom_object *o = ASOM_VAL2OBJ(self);
    struct asom_string *n = to_string(name);
    for (uint32_t i = 0; i < o->klass->num_instance_fields; i++) {
        if (strcmp(o->klass->field_names[i], n->bytes) == 0) {
            return ((VALUE *)(o + 1))[i];
        }
    }
    return c->val_nil;
}

static VALUE
obj_instVarNamed_put_(CTX *c, VALUE self, VALUE name, VALUE val)
{
    if (!ASOM_IS_OBJ(self) || !ASOM_IS_OBJ(name)) return val;
    struct asom_object *o = ASOM_VAL2OBJ(self);
    struct asom_string *n = to_string(name);
    for (uint32_t i = 0; i < o->klass->num_instance_fields; i++) {
        if (strcmp(o->klass->field_names[i], n->bytes) == 0) {
            return ((VALUE *)(o + 1))[i] = val;
        }
    }
    return val;
}

// ---------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------

static VALUE
true_iftrue_(CTX *c, VALUE self, VALUE block) { (void)self; return send_value(c, block); }
static VALUE
true_iffalse_(CTX *c, VALUE self, VALUE block) { (void)self; (void)block; return c->val_nil; }
static VALUE
true_iftrue_iffalse_(CTX *c, VALUE self, VALUE bt, VALUE bf) { (void)self; (void)bf; return send_value(c, bt); }
static VALUE
true_iffalse_iftrue_(CTX *c, VALUE self, VALUE bf, VALUE bt) { (void)self; (void)bf; return send_value(c, bt); }
static VALUE
true_not(CTX *c, VALUE self) { (void)self; return c->val_false; }
static VALUE
true_and_(CTX *c, VALUE self, VALUE other) { (void)self; return send_value(c, other); }
static VALUE
true_or_(CTX *c, VALUE self, VALUE other) { (void)other; return self; }
static VALUE
true_asString(CTX *c, VALUE self) { (void)self; return make_cstr_string(c, "true"); }

static VALUE
false_iftrue_(CTX *c, VALUE self, VALUE block) { (void)self; (void)block; return c->val_nil; }
static VALUE
false_iffalse_(CTX *c, VALUE self, VALUE block) { (void)self; return send_value(c, block); }
static VALUE
false_iftrue_iffalse_(CTX *c, VALUE self, VALUE bt, VALUE bf) { (void)self; (void)bt; return send_value(c, bf); }
static VALUE
false_iffalse_iftrue_(CTX *c, VALUE self, VALUE bf, VALUE bt) { (void)self; (void)bt; return send_value(c, bf); }
static VALUE
false_not(CTX *c, VALUE self) { (void)self; return c->val_true; }
static VALUE
false_and_(CTX *c, VALUE self, VALUE other) { (void)other; return self; }
static VALUE
false_or_(CTX *c, VALUE self, VALUE other) { (void)self; return send_value(c, other); }
static VALUE
false_asString(CTX *c, VALUE self) { (void)self; return make_cstr_string(c, "false"); }

// ---------------------------------------------------------------------------
// Integer
// ---------------------------------------------------------------------------

#define INT_BIN_PRIM(name, op) \
    static VALUE int_##name(CTX *c, VALUE self, VALUE other) { \
        (void)c; \
        if (ASOM_IS_INT(other)) return ASOM_INT2VAL(ASOM_VAL2INT(self) op ASOM_VAL2INT(other)); \
        return asom_double_new(c, (double)ASOM_VAL2INT(self) op to_double_value(c, other)); \
    }

INT_BIN_PRIM(plus, +)
INT_BIN_PRIM(minus, -)
INT_BIN_PRIM(times, *)

static VALUE
int_div(CTX *c, VALUE self, VALUE other)
{
    if (ASOM_IS_INT(other)) {
        intptr_t r = ASOM_VAL2INT(other);
        if (r == 0) { fprintf(stderr, "asom: division by zero\n"); exit(1); }
        return ASOM_INT2VAL(ASOM_VAL2INT(self) / r);
    }
    double r = to_double_value(c, other);
    return asom_double_new(c, (double)ASOM_VAL2INT(self) / r);
}

// Euclidean modulo: result has the sign of the divisor.
static VALUE
int_mod(CTX *c, VALUE self, VALUE other)
{
    (void)c;
    intptr_t r = ASOM_VAL2INT(other);
    if (r == 0) { fprintf(stderr, "asom: modulo by zero\n"); exit(1); }
    intptr_t a = ASOM_VAL2INT(self);
    intptr_t res = a % r;
    if (res != 0 && ((res < 0) != (r < 0))) res += r;
    return ASOM_INT2VAL(res);
}

// Truncating remainder: result has the sign of the dividend (matches C's %).
static VALUE
int_rem_(CTX *c, VALUE self, VALUE other)
{
    (void)c;
    intptr_t r = ASOM_VAL2INT(other);
    if (r == 0) { fprintf(stderr, "asom: rem: by zero\n"); exit(1); }
    return ASOM_INT2VAL(ASOM_VAL2INT(self) % r);
}

// SOM convention: `//` on Integer is Double division, `/` is integer quotient.
// (PySOM/SOM++ both follow this; the SOM standard library tests rely on
// `1 // 2 = 0.5` and `(2.0 * x // size)` mapping to floating-point divide.)
static VALUE
int_div_div(CTX *c, VALUE self, VALUE other)
{
    double a = (double)ASOM_VAL2INT(self);
    double b = ASOM_IS_INT(other) ? (double)ASOM_VAL2INT(other) : to_double_value(c, other);
    return asom_double_new(c, a / b);
}

#define INT_CMP_PRIM(name, op) \
    static VALUE int_##name(CTX *c, VALUE self, VALUE other) { \
        if (ASOM_IS_INT(other)) \
            return ASOM_VAL2INT(self) op ASOM_VAL2INT(other) ? c->val_true : c->val_false; \
        return ((double)ASOM_VAL2INT(self) op to_double_value(c, other)) ? c->val_true : c->val_false; \
    }

INT_CMP_PRIM(lt,  <)
INT_CMP_PRIM(gt,  >)
INT_CMP_PRIM(le,  <=)
INT_CMP_PRIM(ge,  >=)
INT_CMP_PRIM(eq_,  ==)

static VALUE
int_neq(CTX *c, VALUE self, VALUE other)
{
    if (ASOM_IS_INT(other)) return ASOM_VAL2INT(self) != ASOM_VAL2INT(other) ? c->val_true : c->val_false;
    return ((double)ASOM_VAL2INT(self) != to_double_value(c, other)) ? c->val_true : c->val_false;
}

static VALUE
int_negated(CTX *c, VALUE self) { (void)c; return ASOM_INT2VAL(-ASOM_VAL2INT(self)); }

static VALUE
int_abs(CTX *c, VALUE self)
{
    (void)c;
    intptr_t v = ASOM_VAL2INT(self);
    return ASOM_INT2VAL(v < 0 ? -v : v);
}

static VALUE
int_max_(CTX *c, VALUE self, VALUE other)
{
    (void)c;
    intptr_t a = ASOM_VAL2INT(self), b = ASOM_VAL2INT(other);
    return ASOM_INT2VAL(a > b ? a : b);
}

static VALUE
int_min_(CTX *c, VALUE self, VALUE other)
{
    (void)c;
    intptr_t a = ASOM_VAL2INT(self), b = ASOM_VAL2INT(other);
    return ASOM_INT2VAL(a < b ? a : b);
}

static VALUE
int_asString(CTX *c, VALUE self)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIdPTR, (intptr_t)ASOM_VAL2INT(self));
    return make_cstr_string(c, buf);
}

static VALUE
int_print(CTX *c, VALUE self)
{
    printf("%" PRIdPTR, (intptr_t)ASOM_VAL2INT(self));
    (void)c;
    return self;
}

static VALUE
int_println(CTX *c, VALUE self)
{
    printf("%" PRIdPTR "\n", (intptr_t)ASOM_VAL2INT(self));
    (void)c;
    return self;
}

// `from to: end do: aBlock` — invoke aBlock with each integer in [from, end].
static VALUE
int_to_do_(CTX *c, VALUE self, VALUE end, VALUE block)
{
    intptr_t lo = ASOM_VAL2INT(self);
    intptr_t hi = ASOM_VAL2INT(end);
    if (!ASOM_IS_OBJ(block)) {
        fprintf(stderr, "asom: to:do: expects a Block\n"); exit(1);
    }
    struct asom_block *b = to_block(block);
    for (intptr_t i = lo; i <= hi; i++) {
        VALUE arg = ASOM_INT2VAL(i);
        asom_block_invoke(c, b, &arg, 1);
    }
    return self;
}

static VALUE
int_downto_do_(CTX *c, VALUE self, VALUE end, VALUE block)
{
    intptr_t hi = ASOM_VAL2INT(self);
    intptr_t lo = ASOM_VAL2INT(end);
    struct asom_block *b = to_block(block);
    for (intptr_t i = hi; i >= lo; i--) {
        VALUE arg = ASOM_INT2VAL(i);
        asom_block_invoke(c, b, &arg, 1);
    }
    return self;
}

static VALUE
int_times_repeat_(CTX *c, VALUE self, VALUE block)
{
    intptr_t n = ASOM_VAL2INT(self);
    struct asom_block *b = to_block(block);
    for (intptr_t i = 0; i < n; i++) asom_block_invoke(c, b, NULL, 0);
    return self;
}

static VALUE
int_sqrt(CTX *c, VALUE self)
{
    intptr_t v = ASOM_VAL2INT(self);
    double r = sqrt((double)v);
    intptr_t ri = (intptr_t)r;
    // SOM returns Integer when the result is a perfect square, otherwise Double.
    if (ri * ri == v) return ASOM_INT2VAL(ri);
    return asom_double_new(c, r);
}

static VALUE
int_as32_unsigned(CTX *c, VALUE self) { (void)c; return ASOM_INT2VAL((intptr_t)((uint32_t)ASOM_VAL2INT(self))); }
static VALUE
int_as32_signed(CTX *c, VALUE self) { (void)c; return ASOM_INT2VAL((intptr_t)((int32_t)ASOM_VAL2INT(self))); }

// `self to: upper` returns Array from self to upper inclusive.
static VALUE
int_to_(CTX *c, VALUE self, VALUE upper)
{
    intptr_t lo = ASOM_VAL2INT(self), hi = ASOM_VAL2INT(upper);
    intptr_t n = hi - lo + 1;
    if (n < 0) n = 0;
    VALUE arr = asom_array_new(c, (uint32_t)n);
    for (intptr_t i = 0; i < n; i++) to_array(arr)->data[i] = ASOM_INT2VAL(lo + i);
    return arr;
}

static VALUE
int_bitand(CTX *c, VALUE self, VALUE other) { (void)c; return ASOM_INT2VAL(ASOM_VAL2INT(self) & ASOM_VAL2INT(other)); }
static VALUE
int_bitor(CTX *c, VALUE self, VALUE other) { (void)c; return ASOM_INT2VAL(ASOM_VAL2INT(self) | ASOM_VAL2INT(other)); }
static VALUE
int_bitxor(CTX *c, VALUE self, VALUE other) { (void)c; return ASOM_INT2VAL(ASOM_VAL2INT(self) ^ ASOM_VAL2INT(other)); }
static VALUE
int_shl(CTX *c, VALUE self, VALUE other) { (void)c; return ASOM_INT2VAL(ASOM_VAL2INT(self) << ASOM_VAL2INT(other)); }
static VALUE
int_shr(CTX *c, VALUE self, VALUE other) { (void)c; return ASOM_INT2VAL(ASOM_VAL2INT(self) >> ASOM_VAL2INT(other)); }

// ---------------------------------------------------------------------------
// Double
// ---------------------------------------------------------------------------

#define DBL_BIN_PRIM(name, op) \
    static VALUE dbl_##name(CTX *c, VALUE self, VALUE other) { \
        return asom_double_new(c, asom_val_double(self) op to_double_value(c, other)); \
    }
DBL_BIN_PRIM(plus, +)
DBL_BIN_PRIM(minus, -)
DBL_BIN_PRIM(times, *)
DBL_BIN_PRIM(div, /)

static VALUE
dbl_floor_div(CTX *c, VALUE self, VALUE other)
{
    // In SOM, Double>>// is regular floating-point division (matching
    // PySOM/SOM++): only Integer>>// is the floor-truncating variant.
    double a = asom_val_double(self);
    double b = to_double_value(c, other);
    return asom_double_new(c, a / b);
}

static VALUE
dbl_round(CTX *c, VALUE self) { return ASOM_INT2VAL((intptr_t)round(asom_val_double(self))); }
static VALUE
dbl_floor(CTX *c, VALUE self) { return ASOM_INT2VAL((intptr_t)floor(asom_val_double(self))); }
static VALUE
dbl_ceiling(CTX *c, VALUE self) { return ASOM_INT2VAL((intptr_t)ceil(asom_val_double(self))); }
static VALUE
dbl_sqrt(CTX *c, VALUE self) { return asom_double_new(c, sqrt(asom_val_double(self))); }
static VALUE
dbl_negated(CTX *c, VALUE self) { return asom_double_new(c, -asom_val_double(self)); }
static VALUE
dbl_abs(CTX *c, VALUE self) { double v = asom_val_double(self); return asom_double_new(c, v < 0 ? -v : v); }
static VALUE
dbl_sin(CTX *c, VALUE self) { return asom_double_new(c, sin(asom_val_double(self))); }
static VALUE
dbl_cos(CTX *c, VALUE self) { return asom_double_new(c, cos(asom_val_double(self))); }
static VALUE
dbl_log(CTX *c, VALUE self) { return asom_double_new(c, log(asom_val_double(self))); }
static VALUE
dbl_exp(CTX *c, VALUE self) { return asom_double_new(c, exp(asom_val_double(self))); }
static VALUE
dbl_neq_(CTX *c, VALUE self, VALUE other) { return asom_val_double(self) != to_double_value(c, other) ? c->val_true : c->val_false; }
static VALUE
dbl_mod(CTX *c, VALUE self, VALUE other) { (void)c; return asom_double_new(c, fmod(asom_val_double(self), to_double_value(c, other))); }
static VALUE
dbl_rem(CTX *c, VALUE self, VALUE other) { return dbl_mod(c, self, other); }
static VALUE
dbl_asInteger(CTX *c, VALUE self) { (void)c; return ASOM_INT2VAL((intptr_t)asom_val_double(self)); }
static VALUE
int_asInteger(CTX *c, VALUE self) { (void)c; return self; }
static VALUE
int_asDouble(CTX *c, VALUE self) { return asom_double_new(c, (double)ASOM_VAL2INT(self)); }

#define DBL_CMP_PRIM(name, op) \
    static VALUE dbl_##name(CTX *c, VALUE self, VALUE other) { \
        return asom_val_double(self) op to_double_value(c, other) ? c->val_true : c->val_false; \
    }
DBL_CMP_PRIM(lt, <)
DBL_CMP_PRIM(gt, >)
DBL_CMP_PRIM(le, <=)
DBL_CMP_PRIM(ge, >=)
DBL_CMP_PRIM(eq_, ==)

static VALUE
dbl_asString(CTX *c, VALUE self)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", asom_val_double(self));
    return make_cstr_string(c, buf);
}

static VALUE
dbl_println(CTX *c, VALUE self)
{
    (void)c;
    printf("%g\n", asom_val_double(self));
    return self;
}

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------

static VALUE
str_print(CTX *c, VALUE self)
{
    (void)c;
    fputs(to_string(self)->bytes, stdout);
    return self;
}

static VALUE
str_println(CTX *c, VALUE self)
{
    (void)c;
    puts(to_string(self)->bytes);
    return self;
}

static VALUE
str_length(CTX *c, VALUE self)
{
    (void)c;
    return ASOM_INT2VAL((intptr_t)to_string(self)->len);
}

static VALUE
str_eq(CTX *c, VALUE self, VALUE other)
{
    if (!ASOM_IS_OBJ(other)) return c->val_false;
    struct asom_object *o = ASOM_VAL2OBJ(other);
    if (o->klass != c->cls_string && o->klass != c->cls_symbol) return c->val_false;
    struct asom_string *a = to_string(self), *b = to_string(other);
    if (a->len != b->len) return c->val_false;
    return memcmp(a->bytes, b->bytes, a->len) == 0 ? c->val_true : c->val_false;
}

static VALUE
str_concat(CTX *c, VALUE self, VALUE other)
{
    const char *bs;
    size_t bl;
    char tmp[64];
    bool other_is_symbol = false;
    if (ASOM_IS_INT(other)) {
        snprintf(tmp, sizeof(tmp), "%" PRIdPTR, (intptr_t)ASOM_VAL2INT(other));
        bs = tmp; bl = strlen(tmp);
    } else if (ASOM_IS_OBJ(other)) {
        struct asom_object *o = ASOM_VAL2OBJ(other);
        if (o && o->klass == c->cls_symbol) {
            bs = to_string(other)->bytes; bl = to_string(other)->len; other_is_symbol = true;
        } else if (o && o->klass == c->cls_string) {
            bs = to_string(other)->bytes; bl = to_string(other)->len;
        } else {
            VALUE s = asom_send(c, other, asom_intern_cstr("asString"), 0, NULL, NULL);
            bs = to_string(s)->bytes; bl = to_string(s)->len;
        }
    } else {
        bs = "?"; bl = 1;
    }
    struct asom_string *a = to_string(self);
    bool self_is_symbol = ASOM_VAL2OBJ(self)->klass == c->cls_symbol;
    char *buf = malloc(a->len + bl + 1);
    memcpy(buf, a->bytes, a->len);
    memcpy(buf + a->len, bs, bl);
    buf[a->len + bl] = '\0';
    // SOM++ convention: when the receiver is a Symbol, the concatenation
    // result is also a Symbol regardless of the argument type. Other
    // implementations (PySOM) always return String — we follow SOM++ so
    // SymbolTest passes.
    (void)other_is_symbol;
    VALUE r;
    if (self_is_symbol) {
        r = asom_intern_symbol(c, buf);
    } else {
        r = asom_string_new(c, buf, a->len + bl);
    }
    free(buf);
    return r;
}

static VALUE
str_asString(CTX *c, VALUE self) { (void)c; return self; }

static VALUE
str_hash(CTX *c, VALUE self)
{
    (void)c;
    uint64_t h = 1469598103934665603ULL;
    struct asom_string *s = to_string(self);
    for (size_t i = 0; i < s->len; i++) { h ^= (unsigned char)s->bytes[i]; h *= 1099511628211ULL; }
    return ASOM_INT2VAL((intptr_t)(h & 0x3fffffff));
}

static VALUE
str_asInteger(CTX *c, VALUE self)
{
    (void)c;
    return ASOM_INT2VAL((intptr_t)strtoll(to_string(self)->bytes, NULL, 10));
}

static VALUE
str_asSymbol(CTX *c, VALUE self)
{
    return asom_intern_symbol(c, to_string(self)->bytes);
}

static VALUE
str_isDigits(CTX *c, VALUE self)
{
    struct asom_string *s = to_string(self);
    if (s->len == 0) return c->val_false;
    for (size_t i = 0; i < s->len; i++) {
        if (s->bytes[i] < '0' || s->bytes[i] > '9') return c->val_false;
    }
    return c->val_true;
}

static VALUE
str_isLetters(CTX *c, VALUE self)
{
    struct asom_string *s = to_string(self);
    if (s->len == 0) return c->val_false;
    for (size_t i = 0; i < s->len; i++) {
        char ch = s->bytes[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) return c->val_false;
    }
    return c->val_true;
}

static VALUE
str_isWhiteSpace(CTX *c, VALUE self)
{
    struct asom_string *s = to_string(self);
    if (s->len == 0) return c->val_false;
    for (size_t i = 0; i < s->len; i++) {
        char ch = s->bytes[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f') return c->val_false;
    }
    return c->val_true;
}

static VALUE
str_substring_from_to_(CTX *c, VALUE self, VALUE from, VALUE to)
{
    struct asom_string *s = to_string(self);
    intptr_t lo = ASOM_VAL2INT(from);
    intptr_t hi = ASOM_VAL2INT(to);
    if (lo < 1) lo = 1;
    if (hi > (intptr_t)s->len) hi = s->len;
    if (hi < lo) return asom_string_new(c, "", 0);
    return asom_string_new(c, s->bytes + (lo - 1), (size_t)(hi - lo + 1));
}

static VALUE
str_begins_with_(CTX *c, VALUE self, VALUE other)
{
    if (!ASOM_IS_OBJ(other)) return c->val_false;
    struct asom_object *o = ASOM_VAL2OBJ(other);
    if (o->klass != c->cls_string && o->klass != c->cls_symbol) return c->val_false;
    struct asom_string *a = to_string(self), *b = to_string(other);
    if (b->len > a->len) return c->val_false;
    return memcmp(a->bytes, b->bytes, b->len) == 0 ? c->val_true : c->val_false;
}

static VALUE
str_at_(CTX *c, VALUE self, VALUE idx)
{
    intptr_t i = ASOM_VAL2INT(idx);
    struct asom_string *s = to_string(self);
    if (i < 1 || i > (intptr_t)s->len) return c->val_nil;
    char buf[2] = { s->bytes[i - 1], '\0' };
    return make_cstr_string(c, buf);
}

// ---------------------------------------------------------------------------
// Symbol
// ---------------------------------------------------------------------------

static VALUE
sym_asString(CTX *c, VALUE self)
{
    return make_cstr_string(c, to_string(self)->bytes);
}

// ---------------------------------------------------------------------------
// Array
// ---------------------------------------------------------------------------

static VALUE
arr_at_(CTX *c, VALUE self, VALUE idx)
{
    intptr_t i = ASOM_VAL2INT(idx);
    struct asom_array *a = to_array(self);
    if (i < 1 || i > (intptr_t)a->len) {
        fprintf(stderr, "asom: Array at: out of bounds (%ld of %u)\n", (long)i, a->len);
        exit(1);
    }
    return a->data[i - 1];
}

static VALUE
arr_at_put_(CTX *c, VALUE self, VALUE idx, VALUE val)
{
    intptr_t i = ASOM_VAL2INT(idx);
    struct asom_array *a = to_array(self);
    if (i < 1 || i > (intptr_t)a->len) {
        fprintf(stderr, "asom: Array at:put: out of bounds (%ld of %u)\n", (long)i, a->len);
        exit(1);
    }
    a->data[i - 1] = val;
    return val;
}

static VALUE
arr_length(CTX *c, VALUE self) { (void)c; return ASOM_INT2VAL((intptr_t)to_array(self)->len); }

static VALUE
arr_size(CTX *c, VALUE self) { return arr_length(c, self); }

static VALUE
arr_putall_(CTX *c, VALUE self, VALUE val)
{
    // SOM semantics (from stdlib Array>>putAll:): if the argument is a
    // block, invoke `value` per slot; otherwise broadcast the value.
    struct asom_array *a = to_array(self);
    if (is_block_obj(c, val)) {
        struct asom_block *b = (struct asom_block *)ASOM_VAL2OBJ(val);
        for (uint32_t i = 0; i < a->len; i++) {
            a->data[i] = asom_block_invoke(c, b, NULL, 0);
        }
        return self;
    }
    for (uint32_t i = 0; i < a->len; i++) a->data[i] = val;
    return self;
}

static VALUE
arr_do_(CTX *c, VALUE self, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    for (uint32_t i = 0; i < a->len; i++) {
        VALUE arg = a->data[i];
        asom_block_invoke(c, b, &arg, 1);
    }
    return self;
}

static VALUE
arr_doindexes_(CTX *c, VALUE self, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    for (uint32_t i = 0; i < a->len; i++) {
        VALUE arg = ASOM_INT2VAL((intptr_t)i + 1);
        asom_block_invoke(c, b, &arg, 1);
    }
    return self;
}

// `from from: lo to: hi do: aBlock` — extension used by Echo.som etc.
static VALUE
arr_copy(CTX *c, VALUE self)
{
    struct asom_array *a = to_array(self);
    VALUE copy = asom_array_new(c, a->len);
    for (uint32_t i = 0; i < a->len; i++) to_array(copy)->data[i] = a->data[i];
    return copy;
}

static VALUE
arr_first(CTX *c, VALUE self) { struct asom_array *a = to_array(self); return a->len ? a->data[0] : c->val_nil; }
static VALUE
arr_last(CTX *c, VALUE self)  { struct asom_array *a = to_array(self); return a->len ? a->data[a->len - 1] : c->val_nil; }

static VALUE
arr_isEmpty(CTX *c, VALUE self) { return to_array(self)->len == 0 ? c->val_true : c->val_false; }

static VALUE
arr_collect_(CTX *c, VALUE self, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    VALUE out = asom_array_new(c, a->len);
    for (uint32_t i = 0; i < a->len; i++) {
        VALUE arg = a->data[i];
        to_array(out)->data[i] = asom_block_invoke(c, b, &arg, 1);
    }
    return out;
}

static VALUE
arr_select_(CTX *c, VALUE self, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    VALUE *tmp = malloc(a->len * sizeof(VALUE));
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->len; i++) {
        VALUE arg = a->data[i];
        VALUE r = asom_block_invoke(c, b, &arg, 1);
        if (r == c->val_true) tmp[k++] = arg;
    }
    VALUE out = asom_array_new(c, k);
    for (uint32_t i = 0; i < k; i++) to_array(out)->data[i] = tmp[i];
    free(tmp);
    return out;
}

static VALUE
arr_inject_into_(CTX *c, VALUE self, VALUE acc, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    for (uint32_t i = 0; i < a->len; i++) {
        VALUE args[2] = { acc, a->data[i] };
        acc = asom_block_invoke(c, b, args, 2);
    }
    return acc;
}

static VALUE
arr_contains_(CTX *c, VALUE self, VALUE el)
{
    struct asom_array *a = to_array(self);
    for (uint32_t i = 0; i < a->len; i++) {
        if (a->data[i] == el) return c->val_true;
    }
    return c->val_false;
}

static VALUE
arr_indexOf_(CTX *c, VALUE self, VALUE el)
{
    struct asom_array *a = to_array(self);
    for (uint32_t i = 0; i < a->len; i++) {
        if (a->data[i] == el) return ASOM_INT2VAL((intptr_t)(i + 1));
    }
    return c->val_nil;
}

static VALUE
arr_from_to_do_(CTX *c, VALUE self, VALUE from, VALUE to, VALUE block)
{
    struct asom_array *a = to_array(self);
    struct asom_block *b = to_block(block);
    intptr_t lo = ASOM_VAL2INT(from);
    intptr_t hi = ASOM_VAL2INT(to);
    if (lo < 1) lo = 1;
    if (hi > (intptr_t)a->len) hi = (intptr_t)a->len;
    for (intptr_t i = lo; i <= hi; i++) {
        VALUE arg = a->data[i - 1];
        asom_block_invoke(c, b, &arg, 1);
    }
    return self;
}

// ---------------------------------------------------------------------------
// Block
// ---------------------------------------------------------------------------

static VALUE
blk_value(CTX *c, VALUE self) { return asom_block_invoke(c, to_block(self), NULL, 0); }

static VALUE
blk_value_(CTX *c, VALUE self, VALUE a)
{
    return asom_block_invoke(c, to_block(self), &a, 1);
}

static VALUE
blk_value_with_(CTX *c, VALUE self, VALUE a, VALUE b)
{
    VALUE args[2] = { a, b };
    return asom_block_invoke(c, to_block(self), args, 2);
}

static VALUE
blk_value_with_with_(CTX *c, VALUE self, VALUE a, VALUE b, VALUE x)
{
    VALUE args[3] = { a, b, x };
    return asom_block_invoke(c, to_block(self), args, 3);
}

static VALUE
blk_while_true_(CTX *c, VALUE self, VALUE body)
{
    struct asom_block *cond = to_block(self);
    for (;;) {
        VALUE r = asom_block_invoke(c, cond, NULL, 0);
        if (!is_truthy(c, r)) break;
        send_value(c, body);
    }
    return c->val_nil;
}

static VALUE
blk_while_false_(CTX *c, VALUE self, VALUE body)
{
    struct asom_block *cond = to_block(self);
    for (;;) {
        VALUE r = asom_block_invoke(c, cond, NULL, 0);
        if (is_truthy(c, r)) break;
        send_value(c, body);
    }
    return c->val_nil;
}

// ---------------------------------------------------------------------------
// Nil
// ---------------------------------------------------------------------------

static VALUE
nil_isnil(CTX *c, VALUE self) { (void)self; return c->val_true; }
static VALUE
nil_notnil(CTX *c, VALUE self) { (void)self; return c->val_false; }
static VALUE
nil_asString(CTX *c, VALUE self) { (void)self; return make_cstr_string(c, "nil"); }
static VALUE
nil_println(CTX *c, VALUE self) { (void)c; (void)self; puts("nil"); return self; }
static VALUE
nil_ifnil_(CTX *c, VALUE self, VALUE block) { (void)self; return send_value(c, block); }
static VALUE
nil_ifnotnil_(CTX *c, VALUE self, VALUE block) { (void)self; (void)block; return c->val_nil; }
static VALUE
nil_ifnil_ifnotnil_(CTX *c, VALUE self, VALUE bnil, VALUE bnot) { (void)self; (void)bnot; return send_value(c, bnil); }
static VALUE
nil_ifnotnil_ifnil_(CTX *c, VALUE self, VALUE bnot, VALUE bnil) { (void)self; (void)bnot; return send_value(c, bnil); }

// ---------------------------------------------------------------------------
// Random — singleton-style class compatible with SOM's Random.som API
// (initialize / next, both as class-side messages).
// ---------------------------------------------------------------------------

static int64_t g_random_seed = 74755;

static VALUE
rand_initialize(CTX *c, VALUE self) { (void)c; (void)self; g_random_seed = 74755; return self; }

static VALUE
rand_next(CTX *c, VALUE self)
{
    (void)c;
    g_random_seed = ((g_random_seed * 1309) + 13849) & 65535;
    return ASOM_INT2VAL((intptr_t)g_random_seed);
}

static VALUE
rand_seed_(CTX *c, VALUE self, VALUE s) { (void)c; (void)self; g_random_seed = ASOM_VAL2INT(s); return self; }

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

static VALUE
sys_print_newline(CTX *c, VALUE self) { (void)c; (void)self; putchar('\n'); return self; }

static VALUE
sys_print_string_(CTX *c, VALUE self, VALUE arg)
{
    (void)self;
    if (ASOM_IS_OBJ(arg)) {
        struct asom_object *o = ASOM_VAL2OBJ(arg);
        if (o && (o->klass == c->cls_string || o->klass == c->cls_symbol)) {
            fputs(to_string(arg)->bytes, stdout);
            return self;
        }
    }
    VALUE s = asom_send(c, arg, asom_intern_cstr("asString"), 0, NULL, NULL);
    fputs(to_string(s)->bytes, stdout);
    return self;
}

static VALUE
sys_println_(CTX *c, VALUE self, VALUE arg)
{
    sys_print_string_(c, self, arg);
    putchar('\n');
    return self;
}

static VALUE
sys_global_(CTX *c, VALUE self, VALUE arg)
{
    (void)self;
    if (!ASOM_IS_OBJ(arg)) return c->val_nil;
    struct asom_object *o = ASOM_VAL2OBJ(arg);
    if (o->klass != c->cls_symbol && o->klass != c->cls_string) return c->val_nil;
    return asom_global_get(c, to_string(arg)->bytes);
}

static VALUE
sys_global_put_(CTX *c, VALUE self, VALUE name, VALUE value)
{
    if (!ASOM_IS_OBJ(name)) return c->val_nil;
    struct asom_object *o = ASOM_VAL2OBJ(name);
    if (o->klass != c->cls_symbol && o->klass != c->cls_string) return c->val_nil;
    asom_global_set(c, to_string(name)->bytes, value);
    (void)self;
    return value;
}

static VALUE
sys_load_(CTX *c, VALUE self, VALUE arg)
{
    (void)self;
    if (!ASOM_IS_OBJ(arg)) return c->val_nil;
    struct asom_object *o = ASOM_VAL2OBJ(arg);
    if (o->klass != c->cls_symbol && o->klass != c->cls_string) return c->val_nil;
    struct asom_class *cls = asom_load_class(c, to_string(arg)->bytes);
    return cls ? ASOM_OBJ2VAL(cls) : c->val_nil;
}

static VALUE
sys_exit_(CTX *c, VALUE self, VALUE arg) { (void)c; (void)self; exit((int)ASOM_VAL2INT(arg)); }

static VALUE
sys_full_gc(CTX *c, VALUE self) { (void)self; return c->val_false; } // no GC yet -- report no-effect

static VALUE
sys_gc_stats(CTX *c, VALUE self) { (void)self; return asom_array_new(c, 0); }

static VALUE
sys_total_compilation_time(CTX *c, VALUE self) { (void)c; (void)self; return ASOM_INT2VAL(0); }

static VALUE
sys_ticks(CTX *c, VALUE self)
{
    (void)c; (void)self;
    extern long asom_get_ticks_us(void);
    return ASOM_INT2VAL(asom_get_ticks_us());
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

void
asom_install_primitives(CTX *c)
{
    // Object
    def_prim(c->cls_object, "class",         obj_class,            0);
    def_prim(c->cls_object, "==",            obj_eq_eq,            1);
    def_prim(c->cls_object, "~~",            obj_neq_neq,          1);
    def_prim(c->cls_object, "=",             obj_eq,               1);
    def_prim(c->cls_object, "~=",            obj_neq,              1);
    def_prim(c->cls_object, "<>",            obj_neq,              1);
    def_prim(c->cls_object, "isNil",         obj_isnil,            0);
    def_prim(c->cls_object, "notNil",        obj_notnil,           0);
    def_prim(c->cls_object, "value",         obj_value,            0);
    def_prim(c->cls_object, "hashcode",      obj_hashcode,         0);
    def_prim(c->cls_object, "hash",          obj_hashcode,         0);
    def_prim(c->cls_object, "asString",      obj_asString,         0);
    def_prim(c->cls_object, "print",         obj_print,            0);
    def_prim(c->cls_object, "println",       obj_println,          0);
    def_prim(c->cls_object, "inspect",       obj_inspect,          0);
    def_prim(c->cls_object, "ifNil:",                obj_ifnil_,           1);
    def_prim(c->cls_object, "ifNotNil:",             obj_ifnotnil_,        1);
    def_prim(c->cls_object, "ifNil:ifNotNil:",       obj_ifnil_ifnotnil_,  2);
    def_prim(c->cls_object, "ifNotNil:ifNil:",       obj_ifnotnil_ifnil_,  2);
    def_prim(c->cls_object, "subclassResponsibility", obj_subclass_responsibility, 0);
    def_prim(c->cls_object, "error:",        obj_error_,           1);
    def_prim(c->cls_object, ",",             obj_comma,            1);
    def_prim(c->cls_object, "perform:",      obj_perform_,         1);
    def_prim(c->cls_object, "perform:withArguments:",   obj_perform_with_args_,   2);
    def_prim(c->cls_object, "perform:inSuperclass:",    obj_perform_in_super_,    2);

    // Class methods (installed on cls_class so they apply to every class).
    def_prim(c->cls_class, "new",            class_new,            0);
    def_prim(c->cls_class, "new:",           class_new_size,       1);
    def_prim(c->cls_class, "new:withAll:",   class_new_size_withall_, 2);
    def_prim(c->cls_class, "name",           class_name,           0);
    def_prim(c->cls_class, "asString",       class_asString,       0);
    def_prim(c->cls_class, "superclass",     class_superclass,     0);
    def_prim(c->cls_class, "methods",        class_methods,        0);
    def_prim(c->cls_class, "fields",         class_fields,         0);
    def_prim(c->cls_class, "fromString:",    class_fromString_,    1);
    def_prim(c->cls_class, "PositiveInfinity", class_PositiveInfinity, 0);

    def_prim(c->cls_method, "signature",     method_signature,     0);
    def_prim(c->cls_method, "holder",        method_holder,        0);

    def_prim(c->cls_object, "instVarAt:",        obj_instVarAt_,         1);
    def_prim(c->cls_object, "instVarAt:put:",    obj_instVarAt_put_,     2);
    def_prim(c->cls_object, "instVarNamed:",     obj_instVarNamed_,      1);
    def_prim(c->cls_object, "instVarNamed:put:", obj_instVarNamed_put_,  2);

    // Booleans
    def_prim(c->cls_true,  "ifTrue:",        true_iftrue_,         1);
    def_prim(c->cls_true,  "ifFalse:",       true_iffalse_,        1);
    def_prim(c->cls_true,  "ifTrue:ifFalse:", true_iftrue_iffalse_, 2);
    def_prim(c->cls_true,  "ifFalse:ifTrue:", true_iffalse_iftrue_, 2);
    def_prim(c->cls_true,  "not",            true_not,             0);
    def_prim(c->cls_true,  "and:",           true_and_,            1);
    def_prim(c->cls_true,  "or:",            true_or_,             1);
    def_prim(c->cls_true,  "&",              true_and_,            1);
    def_prim(c->cls_true,  "|",              true_or_,             1);
    def_prim(c->cls_true,  "&&",             true_and_,            1);
    def_prim(c->cls_true,  "||",             true_or_,             1);
    def_prim(c->cls_true,  "asString",       true_asString,        0);

    def_prim(c->cls_false, "ifTrue:",        false_iftrue_,        1);
    def_prim(c->cls_false, "ifFalse:",       false_iffalse_,       1);
    def_prim(c->cls_false, "ifTrue:ifFalse:", false_iftrue_iffalse_, 2);
    def_prim(c->cls_false, "ifFalse:ifTrue:", false_iffalse_iftrue_, 2);
    def_prim(c->cls_false, "not",            false_not,            0);
    def_prim(c->cls_false, "and:",           false_and_,           1);
    def_prim(c->cls_false, "or:",            false_or_,            1);
    def_prim(c->cls_false, "&",              false_and_,           1);
    def_prim(c->cls_false, "|",              false_or_,            1);
    def_prim(c->cls_false, "&&",             false_and_,           1);
    def_prim(c->cls_false, "||",             false_or_,            1);
    def_prim(c->cls_false, "asString",       false_asString,       0);

    // Integer
    def_prim_kind(c->cls_integer, "+",       int_plus,             1, ASOM_PRIM_INT_PLUS);
    def_prim_kind(c->cls_integer, "-",       int_minus,            1, ASOM_PRIM_INT_MINUS);
    def_prim_kind(c->cls_integer, "*",       int_times,            1, ASOM_PRIM_INT_TIMES);
    def_prim(c->cls_integer, "/",            int_div,              1);
    def_prim(c->cls_integer, "//",           int_div_div,          1);
    def_prim(c->cls_integer, "%",            int_mod,              1);
    def_prim(c->cls_integer, "rem:",         int_rem_,             1);
    def_prim_kind(c->cls_integer, "<",       int_lt,               1, ASOM_PRIM_INT_LT);
    def_prim_kind(c->cls_integer, ">",       int_gt,               1, ASOM_PRIM_INT_GT);
    def_prim_kind(c->cls_integer, "<=",      int_le,               1, ASOM_PRIM_INT_LE);
    def_prim_kind(c->cls_integer, ">=",      int_ge,               1, ASOM_PRIM_INT_GE);
    def_prim_kind(c->cls_integer, "=",       int_eq_,              1, ASOM_PRIM_INT_EQ);
    def_prim(c->cls_integer, "<>",           int_neq,              1);
    def_prim(c->cls_integer, "negated",      int_negated,          0);
    def_prim(c->cls_integer, "abs",          int_abs,              0);
    def_prim(c->cls_integer, "max:",         int_max_,             1);
    def_prim(c->cls_integer, "min:",         int_min_,             1);
    def_prim(c->cls_integer, "asString",     int_asString,         0);
    def_prim(c->cls_integer, "print",        int_print,            0);
    def_prim(c->cls_integer, "println",      int_println,          0);
    def_prim(c->cls_integer, "to:",          int_to_,              1);
    def_prim(c->cls_integer, "to:do:",       int_to_do_,           2);
    def_prim(c->cls_integer, "downTo:do:",   int_downto_do_,       2);
    def_prim(c->cls_integer, "timesRepeat:", int_times_repeat_,    1);
    def_prim(c->cls_integer, "sqrt",         int_sqrt,             0);
    def_prim(c->cls_integer, "as32BitUnsignedValue", int_as32_unsigned, 0);
    def_prim(c->cls_integer, "as32BitSignedValue",   int_as32_signed,   0);
    def_prim(c->cls_integer, "&",            int_bitand,           1);
    def_prim(c->cls_integer, "|",            int_bitor,            1);
    def_prim(c->cls_integer, "bitXor:",      int_bitxor,           1);
    def_prim(c->cls_integer, "<<",           int_shl,              1);
    def_prim(c->cls_integer, ">>",           int_shr,              1);
    def_prim(c->cls_integer, ">>>",          int_shr,              1);

    // Double — arith / compare get prim_kind tags so node_send1 can rewrite
    // hot call sites to the flonum-fast send1_dbl* variants.
    def_prim_kind(c->cls_double, "+",        dbl_plus,             1, ASOM_PRIM_DBL_PLUS);
    def_prim_kind(c->cls_double, "-",        dbl_minus,            1, ASOM_PRIM_DBL_MINUS);
    def_prim_kind(c->cls_double, "*",        dbl_times,            1, ASOM_PRIM_DBL_TIMES);
    def_prim     (c->cls_double, "/",        dbl_div,              1);
    def_prim_kind(c->cls_double, "<",        dbl_lt,               1, ASOM_PRIM_DBL_LT);
    def_prim_kind(c->cls_double, ">",        dbl_gt,               1, ASOM_PRIM_DBL_GT);
    def_prim_kind(c->cls_double, "<=",       dbl_le,               1, ASOM_PRIM_DBL_LE);
    def_prim_kind(c->cls_double, ">=",       dbl_ge,               1, ASOM_PRIM_DBL_GE);
    def_prim_kind(c->cls_double, "=",        dbl_eq_,              1, ASOM_PRIM_DBL_EQ);
    def_prim(c->cls_double, "//",            dbl_floor_div,        1);
    def_prim(c->cls_double, "round",         dbl_round,            0);
    def_prim(c->cls_double, "floor",         dbl_floor,            0);
    def_prim(c->cls_double, "ceiling",       dbl_ceiling,          0);
    def_prim(c->cls_double, "sqrt",          dbl_sqrt,             0);
    def_prim(c->cls_double, "negated",       dbl_negated,          0);
    def_prim(c->cls_double, "abs",           dbl_abs,              0);
    def_prim(c->cls_double, "sin",           dbl_sin,              0);
    def_prim(c->cls_double, "cos",           dbl_cos,              0);
    def_prim(c->cls_double, "log",           dbl_log,              0);
    def_prim(c->cls_double, "exp",           dbl_exp,              0);
    def_prim(c->cls_double, "<>",            dbl_neq_,             1);
    def_prim(c->cls_double, "~=",            dbl_neq_,             1);
    def_prim(c->cls_double, "%",             dbl_mod,              1);
    def_prim(c->cls_double, "rem:",          dbl_rem,              1);
    def_prim(c->cls_double, "asInteger",     dbl_asInteger,        0);
    def_prim(c->cls_double, "asString",      dbl_asString,         0);
    def_prim(c->cls_double, "println",       dbl_println,          0);

    def_prim(c->cls_integer, "asInteger",    int_asInteger,        0);
    def_prim(c->cls_integer, "asDouble",     int_asDouble,         0);
    def_prim(c->cls_integer, "round",        int_asInteger,        0); // self
    def_prim(c->cls_integer, "floor",        int_asInteger,        0);
    def_prim(c->cls_integer, "ceiling",      int_asInteger,        0);

    // String / Symbol
    def_prim(c->cls_string, "print",         str_print,            0);
    def_prim(c->cls_string, "println",       str_println,          0);
    def_prim(c->cls_string, "length",        str_length,           0);
    def_prim(c->cls_string, "size",          str_length,           0);
    def_prim(c->cls_string, "=",             str_eq,               1);
    def_prim(c->cls_string, "+",             str_concat,           1);
    def_prim(c->cls_string, ",",             str_concat,           1);
    def_prim(c->cls_string, "concatenate:",  str_concat,           1);
    def_prim(c->cls_string, "asString",      str_asString,         0);
    def_prim(c->cls_string, "hashcode",      str_hash,             0);
    def_prim(c->cls_string, "hash",          str_hash,             0);
    def_prim(c->cls_string, "at:",           str_at_,              1);
    def_prim(c->cls_string, "beginsWith:",   str_begins_with_,     1);
    def_prim(c->cls_string, "primSubstringFrom:to:", str_substring_from_to_, 2);
    def_prim(c->cls_string, "isDigits",      str_isDigits,         0);
    def_prim(c->cls_string, "isLetters",     str_isLetters,        0);
    def_prim(c->cls_string, "isWhiteSpace",  str_isWhiteSpace,     0);
    def_prim(c->cls_symbol, "beginsWith:",   str_begins_with_,     1);
    def_prim(c->cls_symbol, "primSubstringFrom:to:", str_substring_from_to_, 2);
    def_prim(c->cls_symbol, "=",             str_eq,               1);
    def_prim(c->cls_string, "asInteger",     str_asInteger,        0);
    def_prim(c->cls_string, "asSymbol",      str_asSymbol,         0);

    def_prim(c->cls_symbol, "print",         str_print,            0);
    def_prim(c->cls_symbol, "println",       str_println,          0);
    def_prim(c->cls_symbol, "asString",      sym_asString,         0);

    // Array
    def_prim(c->cls_array, "at:",            arr_at_,              1);
    def_prim(c->cls_array, "at:put:",        arr_at_put_,          2);
    def_prim(c->cls_array, "length",         arr_length,           0);
    def_prim(c->cls_array, "size",           arr_size,             0);
    def_prim(c->cls_array, "putAll:",        arr_putall_,          1);
    def_prim(c->cls_array, "do:",            arr_do_,              1);
    def_prim(c->cls_array, "doIndexes:",     arr_doindexes_,       1);
    def_prim(c->cls_array, "from:to:do:",    arr_from_to_do_,      3);
    def_prim(c->cls_array, "copy",           arr_copy,             0);
    def_prim(c->cls_array, "first",          arr_first,            0);
    def_prim(c->cls_array, "last",           arr_last,             0);
    def_prim(c->cls_array, "isEmpty",        arr_isEmpty,          0);
    def_prim(c->cls_array, "collect:",       arr_collect_,         1);
    def_prim(c->cls_array, "select:",        arr_select_,          1);
    def_prim(c->cls_array, "inject:into:",   arr_inject_into_,     2);
    def_prim(c->cls_array, "contains:",      arr_contains_,        1);
    def_prim(c->cls_array, "indexOf:",       arr_indexOf_,         1);

    // Block
    def_prim(c->cls_block, "value",          blk_value,            0);
    def_prim(c->cls_block, "value:",         blk_value_,           1);
    def_prim(c->cls_block, "value:with:",    blk_value_with_,      2);
    def_prim(c->cls_block, "value:with:with:", blk_value_with_with_, 3);
    def_prim(c->cls_block, "whileTrue:",     blk_while_true_,      1);
    def_prim(c->cls_block, "whileFalse:",    blk_while_false_,     1);

    // Nil
    def_prim(c->cls_nil,   "isNil",          nil_isnil,            0);
    def_prim(c->cls_nil,   "notNil",         nil_notnil,           0);
    def_prim(c->cls_nil,   "asString",       nil_asString,         0);
    def_prim(c->cls_nil,   "println",        nil_println,          0);
    def_prim(c->cls_nil,   "ifNil:",         nil_ifnil_,           1);
    def_prim(c->cls_nil,   "ifNotNil:",      nil_ifnotnil_,        1);
    def_prim(c->cls_nil,   "ifNil:ifNotNil:", nil_ifnil_ifnotnil_, 2);
    def_prim(c->cls_nil,   "ifNotNil:ifNil:", nil_ifnotnil_ifnil_, 2);

    // Random (the *class* is the singleton — initialize/next are class side
    // primitives. We install them on cls_class for now so `Random initialize`
    // and `Random next` resolve via metaclass dispatch.)
    def_prim(c->cls_class, "initialize",     rand_initialize,      0);
    def_prim(c->cls_class, "next",           rand_next,            0);
    def_prim(c->cls_class, "seed:",          rand_seed_,           1);

    // System
    def_prim(c->cls_system, "printNewline",  sys_print_newline,    0);
    def_prim(c->cls_system, "printString:",  sys_print_string_,    1);
    def_prim(c->cls_system, "print:",        sys_print_string_,    1);
    def_prim(c->cls_system, "println:",      sys_println_,         1);
    def_prim(c->cls_system, "global:",       sys_global_,          1);
    def_prim(c->cls_system, "global:put:",   sys_global_put_,      2);
    def_prim(c->cls_system, "load:",         sys_load_,            1);
    def_prim(c->cls_system, "exit:",         sys_exit_,            1);
    def_prim(c->cls_system, "ticks",         sys_ticks,            0);
    def_prim(c->cls_system, "time",          sys_ticks,            0);
    def_prim(c->cls_system, "fullGC",        sys_full_gc,          0);
    def_prim(c->cls_system, "gcStats",       sys_gc_stats,         0);
    def_prim(c->cls_system, "totalCompilationTime", sys_total_compilation_time, 0);
}
