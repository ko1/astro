// Runtime value model for anlox (Lox): heap allocation, environment frames,
// hash tables (globals / fields / methods), function application with the
// `return` unwinding protocol, the `+` overload, equality, truthiness, and
// the native functions.  Heap objects are GC-managed (libgc).
#include <gc.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include "node.h"

// ---- allocation ------------------------------------------------------

struct lox_obj *
lox_alloc(int type)
{
    struct lox_obj *o = (struct lox_obj *)GC_MALLOC(sizeof(struct lox_obj));
    o->type = type;
    return o;
}

VALUE lox_number(double d) { struct lox_obj *o = lox_alloc(LOX_NUM); o->u.num = d; return LOX_OBJ(o); }

VALUE
lox_string(const char *s, int len)
{
    struct lox_obj *o = lox_alloc(LOX_STR);
    char *buf = GC_MALLOC_ATOMIC(len + 1);
    memcpy(buf, s, len); buf[len] = '\0';
    o->u.str.chars = buf; o->u.str.len = len;
    return LOX_OBJ(o);
}
VALUE lox_string_take(char *s, int len) {
    struct lox_obj *o = lox_alloc(LOX_STR);
    o->u.str.chars = s; o->u.str.len = len;
    return LOX_OBJ(o);
}

VALUE
lox_closure(struct lox_fundef *fn, struct lox_frame *env)
{
    struct lox_obj *o = lox_alloc(LOX_CLOSURE);
    o->u.closure.fn = fn; o->u.closure.env = env;
    return LOX_OBJ(o);
}

VALUE
lox_class(const char *name, struct lox_obj *superclass)
{
    struct lox_obj *o = lox_alloc(LOX_CLASS);
    o->u.klass.name = name; o->u.klass.superclass = superclass;
    lox_table_init(&o->u.klass.methods);
    return LOX_OBJ(o);
}

VALUE
lox_instance(struct lox_obj *klass)
{
    struct lox_obj *o = lox_alloc(LOX_INSTANCE);
    o->u.instance.klass = klass;
    lox_table_init(&o->u.instance.fields);
    return LOX_OBJ(o);
}

VALUE
lox_native(const char *name, lox_native_fn fn, int arity)
{
    struct lox_obj *o = lox_alloc(LOX_NATIVE);
    o->u.native.name = name; o->u.native.fn = fn; o->u.native.arity = arity;
    return LOX_OBJ(o);
}

struct lox_frame *
lox_new_frame(struct lox_frame *parent, int nslots)
{
    struct lox_frame *f = (struct lox_frame *)GC_MALLOC(sizeof(struct lox_frame) + sizeof(VALUE) * nslots);
    f->parent = parent; f->nslots = nslots;
    for (int i = 0; i < nslots; i++) f->slots[i] = LOX_NIL;
    return f;
}

// ---- hash tables -----------------------------------------------------

#define TABLE_NB 16

void
lox_table_init(struct lox_table *t)
{
    t->nbuckets = TABLE_NB;
    t->buckets = (struct lox_entry **)GC_MALLOC(sizeof(struct lox_entry *) * TABLE_NB);
    t->count = 0;
}

static size_t str_hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

bool
lox_table_get(struct lox_table *t, const char *key, VALUE *out)
{
    if (!t->buckets) return false;
    for (struct lox_entry *e = t->buckets[str_hash(key) % t->nbuckets]; e; e = e->next)
        if (strcmp(e->key, key) == 0) { if (out) *out = e->val; return true; }
    return false;
}

void
lox_table_set(struct lox_table *t, const char *key, VALUE v)
{
    if (!t->buckets) lox_table_init(t);
    size_t b = str_hash(key) % t->nbuckets;
    for (struct lox_entry *e = t->buckets[b]; e; e = e->next)
        if (strcmp(e->key, key) == 0) { e->val = v; return; }
    struct lox_entry *e = (struct lox_entry *)GC_MALLOC(sizeof(struct lox_entry));
    e->key = GC_strdup(key); e->val = v; e->next = t->buckets[b];
    t->buckets[b] = e; t->count++;
}

// ---- globals ---------------------------------------------------------

void lox_global_define(CTX *c, const char *name, VALUE v) { lox_table_set(c->globals, name, v); }

VALUE
lox_global_get(CTX *c, const char *name)
{
    VALUE v;
    if (lox_table_get(c->globals, name, &v)) return v;
    lox_runtime_error(c, "Undefined variable '%s'.", name);
}

void
lox_global_set(CTX *c, const char *name, VALUE v)
{
    VALUE tmp;
    if (!lox_table_get(c->globals, name, &tmp)) lox_runtime_error(c, "Undefined variable '%s'.", name);
    lox_table_set(c->globals, name, v);
}

// ---- truthiness / equality / + --------------------------------------

bool lox_truthy(VALUE v) { return v != LOX_NIL && v != LOX_FALSE; }   // only nil & false are falsey

bool
lox_equals(VALUE a, VALUE b)
{
    if (a == b) return true;   // same constant or same object identity
    if (LOX_IS_PTR(a) && LOX_IS_PTR(b)) {
        struct lox_obj *oa = LOX_PTR(a), *ob = LOX_PTR(b);
        if (oa->type != ob->type) return false;
        if (oa->type == LOX_NUM) return oa->u.num == ob->u.num;
        if (oa->type == LOX_STR) return oa->u.str.len == ob->u.str.len &&
                                        memcmp(oa->u.str.chars, ob->u.str.chars, oa->u.str.len) == 0;
        return false;   // closures/classes/instances: identity (already checked a==b)
    }
    return false;
}

double
lox_as_num(CTX *c, VALUE v, const char *what)
{
    if (LIKELY(LOX_IS_PTR(v) && LOX_PTR(v)->type == LOX_NUM)) return LOX_PTR(v)->u.num;
    lox_runtime_error(c, "%s", what);
}

VALUE
lox_add(CTX *c, VALUE a, VALUE b)
{
    if (LOX_IS_PTR(a) && LOX_IS_PTR(b)) {
        struct lox_obj *oa = LOX_PTR(a), *ob = LOX_PTR(b);
        if (oa->type == LOX_NUM && ob->type == LOX_NUM) return lox_number(oa->u.num + ob->u.num);
        if (oa->type == LOX_STR && ob->type == LOX_STR) {
            int len = oa->u.str.len + ob->u.str.len;
            char *buf = GC_MALLOC_ATOMIC(len + 1);
            memcpy(buf, oa->u.str.chars, oa->u.str.len);
            memcpy(buf + oa->u.str.len, ob->u.str.chars, ob->u.str.len);
            buf[len] = '\0';
            return lox_string_take(buf, len);
        }
    }
    lox_runtime_error(c, "Operands must be two numbers or two strings.");
}

// ---- function application -------------------------------------------

static VALUE bind_method(VALUE method, VALUE instance);
// Look up `name` in klass and its superclasses; returns true and sets *out.
static bool find_method(struct lox_obj *klass, const char *name, VALUE *out) {
    for (struct lox_obj *k = klass; k; k = k->u.klass.superclass)
        if (lox_table_get(&k->u.klass.methods, name, out)) return true;
    return false;
}

VALUE
lox_call(CTX *c, VALUE callee, int argc, VALUE *argv)
{
    if (UNLIKELY(!LOX_IS_PTR(callee))) lox_runtime_error(c, "Can only call functions and classes.");
    struct lox_obj *o = LOX_PTR(callee);

    if (o->type == LOX_CLOSURE) {
        struct lox_fundef *fn = o->u.closure.fn;
        if (UNLIKELY(argc != fn->arity))
            lox_runtime_error(c, "Expected %d arguments but got %d.", fn->arity, argc);
        struct lox_frame *frame = lox_new_frame(o->u.closure.env, fn->nslots);
        for (int i = 0; i < argc; i++) frame->slots[i] = argv[i];

        struct lox_frame *saved_env = c->env;
        bool saved_ret = c->returning; VALUE saved_rv = c->retval;
        c->env = frame; c->returning = false;

        NODE *body = fn->body;
        (*body->head.dispatcher)(c, body);
        VALUE result = c->returning ? c->retval : LOX_NIL;
        if (fn->is_init) result = frame->slots[0];   // initializer returns `this` (slot 0)

        c->env = saved_env; c->returning = saved_ret; c->retval = saved_rv;
        return result;
    }

    if (o->type == LOX_NATIVE) {
        if (UNLIKELY(o->u.native.arity >= 0 && argc != o->u.native.arity))
            lox_runtime_error(c, "Expected %d arguments but got %d.", o->u.native.arity, argc);
        return o->u.native.fn(c, argc, argv);
    }

    if (o->type == LOX_CLASS) {
        VALUE inst = lox_instance(o);
        VALUE init;
        if (find_method(o, "init", &init)) {        // walks the superclass chain
            lox_call(c, bind_method(init, inst), argc, argv);
        } else if (argc != 0) {
            lox_runtime_error(c, "Expected 0 arguments but got %d.", argc);
        }
        return inst;
    }

    lox_runtime_error(c, "Can only call functions and classes.");
}

// ---- properties (classes) -------------------------------------------
// `this`-binding: accessing a method returns a fresh closure whose captured
// env is a one-slot frame holding the receiver, parented on the method's
// definition env.  The resolver places `this` at (depth into that frame).

// Bind `this` to a method closure: return a fresh closure whose captured env
// is a one-slot frame holding the receiver, parented on the method's env.
static VALUE
bind_method(VALUE method, VALUE instance)
{
    struct lox_obj *mc = LOX_PTR(method);
    struct lox_frame *bf = lox_new_frame(mc->u.closure.env, 1);
    bf->slots[0] = instance;     // `this`
    return lox_closure(mc->u.closure.fn, bf);
}

VALUE
lox_get_property(CTX *c, VALUE obj, const char *name)
{
    if (UNLIKELY(!LOX_IS_PTR(obj) || LOX_PTR(obj)->type != LOX_INSTANCE))
        lox_runtime_error(c, "Only instances have properties.");
    struct lox_obj *inst = LOX_PTR(obj);
    VALUE v;
    if (lox_table_get(&inst->u.instance.fields, name, &v)) return v;

    // method lookup up the superclass chain
    for (struct lox_obj *k = inst->u.instance.klass; k; k = k->u.klass.superclass) {
        VALUE m;
        if (lox_table_get(&k->u.klass.methods, name, &m)) return bind_method(m, obj);
    }
    lox_runtime_error(c, "Undefined property '%s'.", name);
}

// Build a class value from its method fundef indices (LOX_CLASS_METHODS).
// When there is a superclass, a one-slot `super` frame is pushed so the
// method closures capture it (matching the resolver's `super` scope).
VALUE
lox_make_class(CTX *c, const char *name, VALUE superclass, uint32_t methods_idx, uint32_t cnt)
{
    struct lox_obj *superk = NULL;
    if (superclass != LOX_NIL) {
        if (UNLIKELY(!LOX_IS_PTR(superclass) || LOX_PTR(superclass)->type != LOX_CLASS))
            lox_runtime_error(c, "Superclass must be a class.");
        superk = LOX_PTR(superclass);
    }
    struct lox_frame *saved = c->env;
    if (superk) {
        struct lox_frame *sf = lox_new_frame(c->env, 1);
        sf->slots[0] = superclass;
        c->env = sf;
    }
    VALUE cls = lox_class(name, superk);
    struct lox_obj *ko = LOX_PTR(cls);
    for (uint32_t i = 0; i < cnt; i++) {
        struct lox_fundef *fn = LOX_FUNDEFS[LOX_CLASS_METHODS[methods_idx + i]];
        lox_table_set(&ko->u.klass.methods, fn->name, lox_closure(fn, c->env));
    }
    c->env = saved;
    return cls;
}

// `super.method`: super is at frame `super_depth`, `this` one frame closer.
VALUE
lox_super_get(CTX *c, uint32_t super_depth, const char *method)
{
    struct lox_frame *f = c->env;
    for (uint32_t i = 0; i < super_depth; i++) f = f->parent;
    VALUE superv = f->slots[0];
    struct lox_frame *tf = c->env;
    for (uint32_t i = 0; i + 1 < super_depth; i++) tf = tf->parent;   // depth super_depth-1
    VALUE thisv = tf->slots[0];

    for (struct lox_obj *k = LOX_PTR(superv); k; k = k->u.klass.superclass) {
        VALUE m;
        if (lox_table_get(&k->u.klass.methods, method, &m)) return bind_method(m, thisv);
    }
    lox_runtime_error(c, "Undefined property '%s'.", method);
}

void
lox_set_property(CTX *c, VALUE obj, const char *name, VALUE v)
{
    if (UNLIKELY(!LOX_IS_PTR(obj) || LOX_PTR(obj)->type != LOX_INSTANCE))
        lox_runtime_error(c, "Only instances have fields.");
    lox_table_set(&LOX_PTR(obj)->u.instance.fields, name, v);
}

// ---- display ---------------------------------------------------------

static void
print_number(FILE *fp, double num)
{
    if (isfinite(num) && fabs(num) < 9e18 && num == (double)(long long)num)
        fprintf(fp, "%lld", (long long)num);
    else
        fprintf(fp, "%.10g", num);
}

void
lox_print(FILE *fp, VALUE v)
{
    if (v == LOX_NIL)   { fputs("nil", fp); return; }
    if (v == LOX_TRUE)  { fputs("true", fp); return; }
    if (v == LOX_FALSE) { fputs("false", fp); return; }
    struct lox_obj *o = LOX_PTR(v);
    switch (o->type) {
      case LOX_NUM:      print_number(fp, o->u.num); return;
      case LOX_STR:      fwrite(o->u.str.chars, 1, o->u.str.len, fp); return;
      case LOX_CLOSURE:  fprintf(fp, "<fn %s>", o->u.closure.fn->name); return;
      case LOX_NATIVE:   fputs("<native fn>", fp); return;
      case LOX_CLASS:    fprintf(fp, "%s", o->u.klass.name); return;
      case LOX_INSTANCE: fprintf(fp, "%s instance", o->u.instance.klass->u.klass.name); return;
    }
}

// ---- native functions -----------------------------------------------

static VALUE native_clock(CTX *c, int argc, VALUE *argv) {
    (void)c; (void)argc; (void)argv;
    return lox_number((double)clock() / CLOCKS_PER_SEC);
}

void
lox_register_natives(CTX *c)
{
    lox_global_define(c, "clock", lox_native("clock", native_clock, 0));
}

// ---- errors / context ------------------------------------------------

void
lox_runtime_error(CTX *c, const char *fmt, ...)
{
    fflush(stdout);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (c && c->err_active) longjmp(c->err_jmp, 1);
    exit(70);   // Lox runtime-error exit code
}

CTX *
lox_make_context(void)
{
    CTX *c = (CTX *)GC_MALLOC(sizeof(CTX));
    c->env = NULL;
    c->globals = (struct lox_table *)GC_MALLOC(sizeof(struct lox_table));
    lox_table_init(c->globals);
    c->returning = false;
    c->retval = LOX_NIL;
    c->err_active = 0;
    return c;
}
