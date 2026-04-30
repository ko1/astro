/*
 * koruby object/runtime support.
 * Memory comes from Boehm GC (libgc).  Bignum uses GMP.
 */

#include <gc.h>
#include <gmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "context.h"
#include "object.h"
#include "node.h"

struct ko_vm *ko_vm = NULL;

ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

/* ---- memory ---- */
void *ko_xmalloc(size_t s) { void *p = GC_MALLOC(s); if (!p) abort(); return p; }
void *ko_xmalloc_atomic(size_t s) { void *p = GC_MALLOC_ATOMIC(s); if (!p) abort(); return p; }
void *ko_xcalloc(size_t n, size_t sz) { return ko_xmalloc(n * sz); /* GC_MALLOC zero-inits */ }
void *ko_xrealloc(void *p, size_t newsize) {
    void *q = GC_REALLOC(p, newsize); if (!q && newsize) abort(); return q;
}
void  ko_xfree(void *p) { (void)p; /* GC handles */ }

/* ---- ID interning ---- */

struct id_pool_entry {
    char *name;
    size_t len;
    ID id;
    struct id_pool_entry *next;
};

#define ID_POOL_BUCKETS 1024
static struct id_pool_entry *id_pool[ID_POOL_BUCKETS];
static struct id_pool_entry **id_index;
static uint32_t id_next = 1;
static uint32_t id_index_capa = 0;

static uint64_t fnv_hash(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

ID ko_intern_n(const char *str, long len) {
    uint64_t h = fnv_hash(str, len);
    uint32_t b = (uint32_t)(h % ID_POOL_BUCKETS);
    for (struct id_pool_entry *e = id_pool[b]; e; e = e->next) {
        if (e->len == (size_t)len && memcmp(e->name, str, len) == 0) {
            return e->id;
        }
    }
    struct id_pool_entry *e = ko_xmalloc(sizeof(*e));
    e->name = ko_xmalloc_atomic(len + 1);
    memcpy(e->name, str, len);
    e->name[len] = 0;
    e->len = len;
    e->id = id_next++;
    e->next = id_pool[b];
    id_pool[b] = e;

    if (e->id >= id_index_capa) {
        uint32_t newcapa = id_index_capa == 0 ? 256 : id_index_capa * 2;
        while (e->id >= newcapa) newcapa *= 2;
        id_index = ko_xrealloc(id_index, newcapa * sizeof(*id_index));
        for (uint32_t i = id_index_capa; i < newcapa; i++) id_index[i] = NULL;
        id_index_capa = newcapa;
    }
    id_index[e->id] = e;
    return e->id;
}

ID ko_intern(const char *s) { return ko_intern_n(s, strlen(s)); }

const char *ko_id_name(ID id) {
    if (id == 0 || id >= id_index_capa || !id_index[id]) return "<bad-id>";
    return id_index[id]->name;
}

VALUE ko_id2sym(ID id) {
    /* encode id in upper bits, low byte = SYMBOL_FLAG */
    return ((VALUE)id << 8) | SYMBOL_FLAG;
}

ID ko_sym2id(VALUE sym) {
    return (ID)(sym >> 8);
}

VALUE ko_str_to_sym(VALUE s) {
    return ko_id2sym(ko_intern_n(((struct ko_string *)s)->ptr, ((struct ko_string *)s)->len));
}

/* ---- class system ---- */

static void method_table_init(struct ko_method_table *mt) {
    mt->bucket_cnt = 16;
    mt->buckets = ko_xcalloc(mt->bucket_cnt, sizeof(*mt->buckets));
    mt->size = 0;
}

static void method_table_resize(struct ko_method_table *mt) {
    uint32_t newcap = mt->bucket_cnt * 2;
    struct ko_method_table_entry **newbk = ko_xcalloc(newcap, sizeof(*newbk));
    for (uint32_t i = 0; i < mt->bucket_cnt; i++) {
        struct ko_method_table_entry *e = mt->buckets[i];
        while (e) {
            struct ko_method_table_entry *nx = e->next;
            uint32_t b = (uint32_t)(e->name % newcap);
            e->next = newbk[b];
            newbk[b] = e;
            e = nx;
        }
    }
    mt->buckets = newbk;
    mt->bucket_cnt = newcap;
}

static void method_table_set(struct ko_method_table *mt, ID name, struct ko_method *m) {
    if (mt->size * 2 > mt->bucket_cnt) method_table_resize(mt);
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    for (struct ko_method_table_entry *e = mt->buckets[b]; e; e = e->next) {
        if (e->name == name) { e->method = m; return; }
    }
    struct ko_method_table_entry *e = ko_xmalloc(sizeof(*e));
    e->name = name;
    e->method = m;
    e->next = mt->buckets[b];
    mt->buckets[b] = e;
    mt->size++;
}

static struct ko_method *method_table_get(const struct ko_method_table *mt, ID name) {
    if (!mt->buckets) return NULL;
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    for (struct ko_method_table_entry *e = mt->buckets[b]; e; e = e->next) {
        if (e->name == name) return e->method;
    }
    return NULL;
}

struct ko_class *ko_class_new(ID name, struct ko_class *super, enum ko_type instance_type) {
    struct ko_class *k = ko_xmalloc(sizeof(*k));
    k->basic.flags = T_CLASS;
    k->basic.klass = ko_vm ? (VALUE)ko_vm->class_class : 0;
    k->name = name;
    k->super = super;
    k->instance_type = instance_type;
    method_table_init(&k->methods);
    k->constants = NULL;
    k->ivar_names = NULL;
    k->ivar_count = 0;
    k->ivar_capa = 0;
    return k;
}

struct ko_class *ko_module_new(ID name) {
    struct ko_class *k = ko_class_new(name, NULL, T_NONE);
    k->basic.flags = T_MODULE;
    k->basic.klass = ko_vm ? (VALUE)ko_vm->module_class : 0;
    return k;
}

void ko_class_add_method_ast(struct ko_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt) {
    struct ko_method *m = ko_xmalloc(sizeof(*m));
    m->type = KO_METHOD_AST;
    m->name = name;
    m->defining_class = klass;
    m->u.ast.body = body;
    m->u.ast.required_params_cnt = params_cnt;
    m->u.ast.locals_cnt = locals_cnt;
    method_table_set(&klass->methods, name, m);
    if (ko_vm) ko_vm->method_serial++;
}

void ko_class_add_method_cfunc(struct ko_class *klass, ID name,
                               VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc) {
    struct ko_method *m = ko_xmalloc(sizeof(*m));
    m->type = KO_METHOD_CFUNC;
    m->name = name;
    m->defining_class = klass;
    m->u.cfunc.func = func;
    m->u.cfunc.argc = argc;
    method_table_set(&klass->methods, name, m);
    if (ko_vm) ko_vm->method_serial++;
}

struct ko_method *ko_class_find_method(const struct ko_class *klass, ID name) {
    while (klass) {
        struct ko_method *m = method_table_get(&klass->methods, name);
        if (m) return m;
        klass = klass->super;
    }
    return NULL;
}

/* ---- constants ---- */
void ko_const_set(struct ko_class *klass, ID name, VALUE value) {
    for (struct ko_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) { e->value = value; return; }
    }
    struct ko_const_entry *e = ko_xmalloc(sizeof(*e));
    e->name = name;
    e->value = value;
    e->next = klass->constants;
    klass->constants = e;
}

VALUE ko_const_get(struct ko_class *klass, ID name) {
    for (struct ko_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return e->value;
    }
    return Qundef;
}

bool ko_const_has(struct ko_class *klass, ID name) {
    for (struct ko_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return true;
    }
    return false;
}

VALUE ko_const_lookup(CTX *c, ID name) {
    struct ko_class *k = c->current_class;
    /* search current class and ancestors */
    for (struct ko_class *kk = k; kk; kk = kk->super) {
        VALUE v = ko_const_get(kk, name);
        if (!UNDEF_P(v)) return v;
    }
    /* search Object */
    if (k != ko_vm->object_class) {
        VALUE v = ko_const_get(ko_vm->object_class, name);
        if (!UNDEF_P(v)) return v;
    }
    ko_raise(c, NULL, "uninitialized constant %s", ko_id_name(name));
    return Qnil;
}

/* ---- gvars ---- */
static struct ko_method_table gvars_table_dummy; /* unused; we reuse a hash */
static struct {
    ID *keys;
    VALUE *vals;
    uint32_t size, capa;
} gvars;

VALUE ko_gvar_get(ID name) {
    for (uint32_t i = 0; i < gvars.size; i++) if (gvars.keys[i] == name) return gvars.vals[i];
    return Qnil;
}

void ko_gvar_set(ID name, VALUE v) {
    for (uint32_t i = 0; i < gvars.size; i++) if (gvars.keys[i] == name) { gvars.vals[i] = v; return; }
    if (gvars.size >= gvars.capa) {
        uint32_t nc = gvars.capa == 0 ? 8 : gvars.capa * 2;
        gvars.keys = ko_xrealloc(gvars.keys, nc * sizeof(ID));
        gvars.vals = ko_xrealloc(gvars.vals, nc * sizeof(VALUE));
        gvars.capa = nc;
    }
    gvars.keys[gvars.size] = name;
    gvars.vals[gvars.size] = v;
    gvars.size++;
}

/* ---- objects (with class-shape ivars) ---- */
VALUE ko_object_new(struct ko_class *klass) {
    struct ko_object *o = ko_xmalloc(sizeof(*o));
    o->basic.flags = klass->instance_type ? klass->instance_type : T_OBJECT;
    o->basic.klass = (VALUE)klass;
    o->ivar_cnt = 0;
    o->ivar_capa = 0;
    o->ivars = NULL;
    return (VALUE)o;
}

static int ivar_slot(struct ko_class *k, ID name) {
    for (uint32_t i = 0; i < k->ivar_count; i++) if (k->ivar_names[i] == name) return (int)i;
    return -1;
}

static int ivar_slot_assign(struct ko_class *k, ID name) {
    int s = ivar_slot(k, name);
    if (s >= 0) return s;
    if (k->ivar_count >= k->ivar_capa) {
        uint32_t nc = k->ivar_capa == 0 ? 4 : k->ivar_capa * 2;
        k->ivar_names = ko_xrealloc(k->ivar_names, nc * sizeof(ID));
        k->ivar_capa = nc;
    }
    s = k->ivar_count++;
    k->ivar_names[s] = name;
    return s;
}

VALUE ko_ivar_get(VALUE obj, ID name) {
    if (SPECIAL_CONST_P(obj)) return Qnil;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return Qnil;
    struct ko_object *o = (struct ko_object *)obj;
    struct ko_class *k = (struct ko_class *)o->basic.klass;
    int s = ivar_slot(k, name);
    if (s < 0 || (uint32_t)s >= o->ivar_cnt) return Qnil;
    return o->ivars[s];
}

void ko_ivar_set(VALUE obj, ID name, VALUE val) {
    if (SPECIAL_CONST_P(obj)) return;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return;
    struct ko_object *o = (struct ko_object *)obj;
    struct ko_class *k = (struct ko_class *)o->basic.klass;
    int s = ivar_slot_assign(k, name);
    if ((uint32_t)s >= o->ivar_capa) {
        uint32_t nc = o->ivar_capa == 0 ? 4 : o->ivar_capa * 2;
        while ((uint32_t)s >= nc) nc *= 2;
        o->ivars = ko_xrealloc(o->ivars, nc * sizeof(VALUE));
        for (uint32_t i = o->ivar_capa; i < nc; i++) o->ivars[i] = Qnil;
        o->ivar_capa = nc;
    }
    if ((uint32_t)s >= o->ivar_cnt) {
        for (uint32_t i = o->ivar_cnt; i <= (uint32_t)s; i++) o->ivars[i] = Qnil;
        o->ivar_cnt = s + 1;
    }
    o->ivars[s] = val;
}

/* ---- string ---- */
VALUE ko_str_new(const char *p, long len) {
    struct ko_string *s = ko_xmalloc(sizeof(*s));
    s->basic.flags = T_STRING;
    s->basic.klass = ko_vm ? (VALUE)ko_vm->string_class : 0;
    s->ptr = ko_xmalloc_atomic(len + 1);
    if (p && len > 0) memcpy(s->ptr, p, len);
    s->ptr[len] = 0;
    s->len = len;
    s->capa = len;
    return (VALUE)s;
}

VALUE ko_str_new_cstr(const char *cstr) { return ko_str_new(cstr, (long)strlen(cstr)); }

VALUE ko_str_dup(VALUE s) {
    return ko_str_new(((struct ko_string *)s)->ptr, ((struct ko_string *)s)->len);
}

VALUE ko_str_concat(VALUE a, VALUE b) {
    struct ko_string *x = (struct ko_string *)a;
    struct ko_string *y = (struct ko_string *)b;
    long total = x->len + y->len;
    if (total > x->capa) {
        long nc = x->capa == 0 ? total : x->capa;
        while (nc < total) nc *= 2;
        char *np = ko_xmalloc_atomic(nc + 1);
        memcpy(np, x->ptr, x->len);
        x->ptr = np;
        x->capa = nc;
    }
    memcpy(x->ptr + x->len, y->ptr, y->len);
    x->len = total;
    x->ptr[x->len] = 0;
    return a;
}

const char *ko_str_cstr(VALUE s) {
    if (BUILTIN_TYPE(s) != T_STRING) return "<not-string>";
    return ((struct ko_string *)s)->ptr;
}

long ko_str_len(VALUE s) { return ((struct ko_string *)s)->len; }

/* ---- array ---- */
VALUE ko_ary_new_capa(long capa) {
    struct ko_array *a = ko_xmalloc(sizeof(*a));
    a->basic.flags = T_ARRAY;
    a->basic.klass = ko_vm ? (VALUE)ko_vm->array_class : 0;
    a->len = 0;
    a->capa = capa < 4 ? 4 : capa;
    a->ptr = ko_xmalloc(a->capa * sizeof(VALUE));
    for (long i = 0; i < a->capa; i++) a->ptr[i] = Qnil;
    return (VALUE)a;
}
VALUE ko_ary_new(void) { return ko_ary_new_capa(0); }

VALUE ko_ary_new_from_values(long n, const VALUE *vals) {
    VALUE a = ko_ary_new_capa(n);
    for (long i = 0; i < n; i++) ko_ary_push(a, vals[i]);
    return a;
}

void ko_ary_push(VALUE av, VALUE v) {
    struct ko_array *a = (struct ko_array *)av;
    if (a->len >= a->capa) {
        long nc = a->capa == 0 ? 4 : a->capa * 2;
        a->ptr = ko_xrealloc(a->ptr, nc * sizeof(VALUE));
        for (long i = a->capa; i < nc; i++) a->ptr[i] = Qnil;
        a->capa = nc;
    }
    a->ptr[a->len++] = v;
}

VALUE ko_ary_pop(VALUE av) {
    struct ko_array *a = (struct ko_array *)av;
    if (a->len == 0) return Qnil;
    return a->ptr[--a->len];
}

VALUE ko_ary_aref(VALUE av, long i) {
    struct ko_array *a = (struct ko_array *)av;
    if (i < 0) i += a->len;
    if (i < 0 || i >= a->len) return Qnil;
    return a->ptr[i];
}

void ko_ary_aset(VALUE av, long i, VALUE v) {
    struct ko_array *a = (struct ko_array *)av;
    if (i < 0) i += a->len;
    if (i < 0) return;
    while (a->len <= i) {
        if (a->len >= a->capa) {
            long nc = a->capa == 0 ? 4 : a->capa * 2;
            while (nc <= i) nc *= 2;
            a->ptr = ko_xrealloc(a->ptr, nc * sizeof(VALUE));
            for (long k = a->capa; k < nc; k++) a->ptr[k] = Qnil;
            a->capa = nc;
        }
        a->ptr[a->len++] = Qnil;
    }
    a->ptr[i] = v;
}

long ko_ary_len(VALUE av) { return ((struct ko_array *)av)->len; }

/* ---- hash ---- */
uint64_t ko_hash_value(VALUE v) {
    if (FIXNUM_P(v)) return (uint64_t)v * 11400714819323198485ULL;
    if (BUILTIN_TYPE(v) == T_STRING) {
        return fnv_hash(((struct ko_string *)v)->ptr, ((struct ko_string *)v)->len);
    }
    if (SYMBOL_P(v)) return (uint64_t)v * 2654435761ULL;
    if (NIL_P(v)) return 0;
    if (TRUE_P(v)) return 1;
    if (FALSE_P(v)) return 2;
    return (uint64_t)v;
}

bool ko_eql(VALUE a, VALUE b) {
    if (a == b) return true;
    if (FIXNUM_P(a) && FIXNUM_P(b)) return a == b;
    if (BUILTIN_TYPE(a) == T_STRING && BUILTIN_TYPE(b) == T_STRING) {
        struct ko_string *x = (struct ko_string *)a;
        struct ko_string *y = (struct ko_string *)b;
        return x->len == y->len && memcmp(x->ptr, y->ptr, x->len) == 0;
    }
    return false;
}

VALUE ko_hash_new(void) {
    struct ko_hash *h = ko_xmalloc(sizeof(*h));
    h->basic.flags = T_HASH;
    h->basic.klass = ko_vm ? (VALUE)ko_vm->hash_class : 0;
    h->bucket_cnt = 8;
    h->buckets = ko_xcalloc(h->bucket_cnt, sizeof(*h->buckets));
    h->size = 0;
    h->first = h->last = NULL;
    h->default_value = Qnil;
    return (VALUE)h;
}

static void ko_hash_resize(struct ko_hash *h) {
    uint32_t nc = h->bucket_cnt * 2;
    struct ko_hash_entry **newbk = ko_xcalloc(nc, sizeof(*newbk));
    /* re-insert by iterating insertion order */
    for (struct ko_hash_entry *e = h->first; e; e = e->next) {
        uint32_t b = (uint32_t)(e->hash % nc);
        /* note: e->next is the insertion order chain, which we don't change */
        /* we need a separate per-bucket chain. Let's just rehash. */
        /* simpler: store via aset which uses bucket chain. */
    }
    /* simpler approach: rebuild from scratch via aset */
    struct ko_hash_entry *first = h->first;
    h->buckets = newbk;
    h->bucket_cnt = nc;
    h->size = 0;
    h->first = h->last = NULL;
    for (struct ko_hash_entry *e = first; e; ) {
        struct ko_hash_entry *nx = e->next;
        ko_hash_aset((VALUE)h, e->key, e->value);
        e = nx;
    }
}

VALUE ko_hash_aset(VALUE hv, VALUE key, VALUE val) {
    struct ko_hash *h = (struct ko_hash *)hv;
    if (h->size * 2 > h->bucket_cnt) ko_hash_resize(h);
    uint64_t hh = ko_hash_value(key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    /* search existing */
    for (struct ko_hash_entry *e = h->buckets[b]; e; ) {
        /* we use 'next' as insertion-order chain; need separate bucket chain.
         * Simplification: linear scan within bucket via insertion-order entries
         * filtered by hash. Not super efficient but correct. */
        if (e->hash == hh && ko_eql(e->key, key)) {
            e->value = val;
            return val;
        }
        /* break — bucket only holds first matching */
        break;
    }
    /* full linear scan to be safe */
    for (struct ko_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && ko_eql(e->key, key)) {
            e->value = val;
            return val;
        }
    }
    struct ko_hash_entry *e = ko_xmalloc(sizeof(*e));
    e->key = key;
    e->value = val;
    e->hash = hh;
    e->next = NULL;
    if (!h->first) h->first = e;
    else h->last->next = e;
    h->last = e;
    h->buckets[b] = e; /* simplistic: just stash */
    h->size++;
    return val;
}

VALUE ko_hash_aref(VALUE hv, VALUE key) {
    struct ko_hash *h = (struct ko_hash *)hv;
    uint64_t hh = ko_hash_value(key);
    for (struct ko_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && ko_eql(e->key, key)) return e->value;
    }
    return h->default_value;
}

long ko_hash_size(VALUE hv) { return ((struct ko_hash *)hv)->size; }

/* ---- range ---- */
VALUE ko_range_new(VALUE b, VALUE e, bool excl) {
    struct ko_range *r = ko_xmalloc(sizeof(*r));
    r->basic.flags = T_RANGE;
    r->basic.klass = ko_vm ? (VALUE)ko_vm->range_class : 0;
    r->begin = b;
    r->end = e;
    r->exclude_end = excl;
    return (VALUE)r;
}

/* ---- float ---- */
VALUE ko_float_new(double d) {
    /* Use FLONUM encoding for typical doubles */
    /* CRuby flonum encoding: rotate exponent bits.  We just heap-box for safety. */
    struct ko_float *f = ko_xmalloc(sizeof(*f));
    f->basic.flags = T_FLOAT;
    f->basic.klass = ko_vm ? (VALUE)ko_vm->float_class : 0;
    f->value = d;
    return (VALUE)f;
}

double ko_num2dbl(VALUE v) {
    if (FIXNUM_P(v)) return (double)FIX2LONG(v);
    if (BUILTIN_TYPE(v) == T_FLOAT) return ((struct ko_float *)v)->value;
    if (BUILTIN_TYPE(v) == T_BIGNUM) return mpz_get_d((mpz_ptr)(((struct ko_bignum *)v)->mpz));
    return 0.0;
}

/* ---- bignum (GMP) ---- */
VALUE ko_bignum_new_str(const char *str, int base) {
    struct ko_bignum *b = ko_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = ko_vm ? (VALUE)ko_vm->integer_class : 0;
    mpz_t *z = ko_xmalloc(sizeof(mpz_t));
    mpz_init_set_str(*z, str, base);
    b->mpz = z;
    /* if it fits in fixnum, return fixnum */
    if (mpz_fits_slong_p(*z)) {
        long v = mpz_get_si(*z);
        if (FIXABLE(v)) return INT2FIX(v);
    }
    return (VALUE)b;
}

VALUE ko_bignum_new_long(long v) {
    if (FIXABLE(v)) return INT2FIX(v);
    struct ko_bignum *b = ko_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = ko_vm ? (VALUE)ko_vm->integer_class : 0;
    mpz_t *z = ko_xmalloc(sizeof(mpz_t));
    mpz_init_set_si(*z, v);
    b->mpz = z;
    return (VALUE)b;
}

static void to_mpz(VALUE v, mpz_t out) {
    if (FIXNUM_P(v)) mpz_init_set_si(out, FIX2LONG(v));
    else mpz_init_set(out, (mpz_ptr)((struct ko_bignum *)v)->mpz);
}

static VALUE from_mpz(mpz_t z) {
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (FIXABLE(v)) { mpz_clear(z); return INT2FIX(v); }
    }
    struct ko_bignum *b = ko_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = ko_vm ? (VALUE)ko_vm->integer_class : 0;
    mpz_t *bz = ko_xmalloc(sizeof(mpz_t));
    mpz_init_set(*bz, z);
    mpz_clear(z);
    b->mpz = bz;
    return (VALUE)b;
}

#define BIGOP(op) do { \
    mpz_t la, ra, ra_res; mpz_init(ra_res); \
    to_mpz(a, la); to_mpz(b, ra); \
    op(ra_res, la, ra); \
    mpz_clear(la); mpz_clear(ra); \
    return from_mpz(ra_res); \
} while (0)

VALUE ko_int_plus(VALUE a, VALUE b)  { BIGOP(mpz_add); }
VALUE ko_int_minus(VALUE a, VALUE b) { BIGOP(mpz_sub); }
VALUE ko_int_mul(VALUE a, VALUE b)   { BIGOP(mpz_mul); }
VALUE ko_int_div(VALUE a, VALUE b) {
    mpz_t la, ra, q;
    mpz_init(q);
    to_mpz(a, la); to_mpz(b, ra);
    mpz_fdiv_q(q, la, ra);
    mpz_clear(la); mpz_clear(ra);
    return from_mpz(q);
}
VALUE ko_int_mod(VALUE a, VALUE b) {
    mpz_t la, ra, m;
    mpz_init(m);
    to_mpz(a, la); to_mpz(b, ra);
    mpz_fdiv_r(m, la, ra);
    mpz_clear(la); mpz_clear(ra);
    return from_mpz(m);
}
VALUE ko_int_lshift(VALUE a, VALUE b) {
    if (!FIXNUM_P(b)) return INT2FIX(0);
    long s = FIX2LONG(b);
    mpz_t la, r;
    mpz_init(r);
    to_mpz(a, la);
    if (s >= 0) mpz_mul_2exp(r, la, s);
    else mpz_fdiv_q_2exp(r, la, -s);
    mpz_clear(la);
    return from_mpz(r);
}
VALUE ko_int_rshift(VALUE a, VALUE b) {
    if (!FIXNUM_P(b)) return INT2FIX(0);
    long s = FIX2LONG(b);
    mpz_t la, r;
    mpz_init(r);
    to_mpz(a, la);
    if (s >= 0) mpz_fdiv_q_2exp(r, la, s);
    else mpz_mul_2exp(r, la, -s);
    mpz_clear(la);
    return from_mpz(r);
}
VALUE ko_int_and(VALUE a, VALUE b) { BIGOP(mpz_and); }
VALUE ko_int_or(VALUE a, VALUE b) { BIGOP(mpz_ior); }
VALUE ko_int_xor(VALUE a, VALUE b) { BIGOP(mpz_xor); }
int ko_int_cmp(VALUE a, VALUE b) {
    if (FIXNUM_P(a) && FIXNUM_P(b)) {
        long la = FIX2LONG(a), lb = FIX2LONG(b);
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    mpz_t la, ra; to_mpz(a, la); to_mpz(b, ra);
    int c = mpz_cmp(la, ra);
    mpz_clear(la); mpz_clear(ra);
    return c;
}
bool ko_int_eq(VALUE a, VALUE b) { return ko_int_cmp(a, b) == 0; }

/* Proc structure stores:
 *  - body: AST to evaluate
 *  - env_fp: pointer into the lexical-parent frame's stack (shared closure)
 *  - env_size: number of slots accessible from the parent
 *  - param_base: absolute slot where block's own locals begin
 *  - params_cnt: number of block params
 *  - self: captured self
 * For non-escaping yields we just reuse the parent fp (no copy).  When a
 * proc escapes (returned/assigned), env_fp may dangle — for the subset we
 * support that won't happen in practice.
 */
VALUE ko_proc_new(struct Node *body, VALUE *fp, uint32_t env_size,
                  uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda) {
    struct ko_proc *p = ko_xmalloc(sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = ko_vm ? (VALUE)ko_vm->proc_class : 0;
    p->body = body;
    p->env_size = env_size;
    p->env = fp;
    p->params_cnt = params_cnt;
    p->param_base = param_base;
    p->self = self;
    p->is_lambda = is_lambda;
    return (VALUE)p;
}

/* Block currently active for yield (set by dispatch_call). */
static __thread struct ko_proc *current_block = NULL;

VALUE ko_yield(CTX *c, uint32_t argc, VALUE *argv) {
    if (UNLIKELY(!current_block)) {
        ko_raise(c, NULL, "no block given (yield)");
        return Qnil;
    }
    struct ko_proc *blk = current_block;
    /* Shared-fp closure: block evaluates with env_fp's view of locals. */
    VALUE *fp = blk->env;
    VALUE prev_self = c->self;
    for (uint32_t i = 0; i < blk->params_cnt && i < argc; i++) {
        fp[blk->param_base + i] = argv[i];
    }
    c->self = blk->self;
    VALUE r = EVAL(c, blk->body);
    c->self = prev_self;
    if (c->state == KO_BREAK) {
        VALUE bv = c->state_value;
        c->state = KO_NORMAL; c->state_value = Qnil;
        return bv;
    }
    if (c->state == KO_NEXT) {
        VALUE nv = c->state_value;
        c->state = KO_NORMAL; c->state_value = Qnil;
        return nv;
    }
    return r;
}

/* ---- class lookup ---- */
struct ko_class *ko_class_of_class(VALUE v) {
    if (FIXNUM_P(v)) return ko_vm->integer_class;
    if (FLONUM_P(v)) return ko_vm->float_class;
    if (SYMBOL_P(v)) return ko_vm->symbol_class;
    if (NIL_P(v))   return ko_vm->nil_class;
    if (TRUE_P(v))  return ko_vm->true_class;
    if (FALSE_P(v)) return ko_vm->false_class;
    /* heap object */
    return (struct ko_class *)((struct RBasic *)v)->klass;
}

VALUE ko_class_of(VALUE v) { return (VALUE)ko_class_of_class(v); }

/* ---- exceptions ---- */
struct ko_exception {
    struct RBasic basic;
    VALUE message;
    struct ko_class *exc_class;
};

VALUE ko_exc_new(struct ko_class *klass, const char *msg) {
    struct ko_exception *e = ko_xmalloc(sizeof(*e));
    e->basic.flags = T_DATA;
    e->basic.klass = klass ? (VALUE)klass : (VALUE)ko_vm->object_class;
    e->message = ko_str_new_cstr(msg);
    e->exc_class = klass;
    return (VALUE)e;
}

void ko_raise(CTX *c, struct ko_class *klass, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE e = ko_exc_new(klass, buf);
    c->state = KO_RAISE;
    c->state_value = e;
}

/* ---- inspect / to_s ---- */
static void str_appendf(VALUE s, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE part = ko_str_new_cstr(buf);
    ko_str_concat(s, part);
}

static VALUE ko_inspect_inner(VALUE v, int depth);

static VALUE ko_inspect_inner(VALUE v, int depth) {
    if (depth > 32) return ko_str_new_cstr("...");
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return ko_str_new_cstr(b);
    }
    if (NIL_P(v)) return ko_str_new_cstr("nil");
    if (TRUE_P(v)) return ko_str_new_cstr("true");
    if (FALSE_P(v)) return ko_str_new_cstr("false");
    if (SYMBOL_P(v)) {
        VALUE s = ko_str_new_cstr(":");
        ko_str_concat(s, ko_str_new_cstr(ko_id_name(ko_sym2id(v))));
        return s;
    }
    enum ko_type t = BUILTIN_TYPE(v);
    if (t == T_STRING) {
        struct ko_string *s = (struct ko_string *)v;
        VALUE r = ko_str_new_cstr("\"");
        ko_str_concat(r, ko_str_new(s->ptr, s->len));
        ko_str_concat(r, ko_str_new_cstr("\""));
        return r;
    }
    if (t == T_ARRAY) {
        struct ko_array *a = (struct ko_array *)v;
        VALUE r = ko_str_new_cstr("[");
        for (long i = 0; i < a->len; i++) {
            if (i) ko_str_concat(r, ko_str_new_cstr(", "));
            ko_str_concat(r, ko_inspect_inner(a->ptr[i], depth+1));
        }
        ko_str_concat(r, ko_str_new_cstr("]"));
        return r;
    }
    if (t == T_HASH) {
        struct ko_hash *h = (struct ko_hash *)v;
        VALUE r = ko_str_new_cstr("{");
        bool first = true;
        for (struct ko_hash_entry *e = h->first; e; e = e->next) {
            if (!first) ko_str_concat(r, ko_str_new_cstr(", "));
            first = false;
            ko_str_concat(r, ko_inspect_inner(e->key, depth+1));
            ko_str_concat(r, ko_str_new_cstr("=>"));
            ko_str_concat(r, ko_inspect_inner(e->value, depth+1));
        }
        ko_str_concat(r, ko_str_new_cstr("}"));
        return r;
    }
    if (t == T_RANGE) {
        struct ko_range *r = (struct ko_range *)v;
        VALUE s = ko_inspect_inner(r->begin, depth+1);
        ko_str_concat(s, ko_str_new_cstr(r->exclude_end ? "..." : ".."));
        ko_str_concat(s, ko_inspect_inner(r->end, depth+1));
        return s;
    }
    if (t == T_FLOAT) {
        char b[64]; snprintf(b, 64, "%.17g", ((struct ko_float *)v)->value);
        return ko_str_new_cstr(b);
    }
    if (t == T_BIGNUM) {
        struct ko_bignum *bn = (struct ko_bignum *)v;
        char *s = mpz_get_str(NULL, 10, (mpz_ptr)bn->mpz);
        VALUE r = ko_str_new_cstr(s);
        free(s);
        return r;
    }
    if (t == T_CLASS || t == T_MODULE) {
        return ko_str_new_cstr(ko_id_name(((struct ko_class *)v)->name));
    }
    if (t == T_OBJECT) {
        char b[64];
        struct ko_class *k = (struct ko_class *)((struct ko_object *)v)->basic.klass;
        snprintf(b, 64, "#<%s:%p>", ko_id_name(k->name), (void *)v);
        return ko_str_new_cstr(b);
    }
    if (t == T_DATA) {
        struct ko_exception *e = (struct ko_exception *)v;
        if (BUILTIN_TYPE(e->message) == T_STRING) {
            VALUE r = ko_str_new_cstr("#<Exception: ");
            ko_str_concat(r, e->message);
            ko_str_concat(r, ko_str_new_cstr(">"));
            return r;
        }
        return ko_str_new_cstr("#<data>");
    }
    if (t == T_PROC) return ko_str_new_cstr("#<Proc>");
    if (t == T_SYMBOL) return ko_str_new_cstr(":?");
    return ko_str_new_cstr("#<?>");
}

VALUE ko_inspect(VALUE v) { return ko_inspect_inner(v, 0); }

VALUE ko_to_s(VALUE v) {
    if (BUILTIN_TYPE(v) == T_STRING) return v;
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return ko_str_new_cstr(b);
    }
    if (NIL_P(v)) return ko_str_new_cstr("");
    if (SYMBOL_P(v)) return ko_str_new_cstr(ko_id_name(ko_sym2id(v)));
    return ko_inspect(v);
}

void ko_p(VALUE v) {
    VALUE s = ko_inspect(v);
    fwrite(((struct ko_string *)s)->ptr, 1, ((struct ko_string *)s)->len, stdout);
    fputc('\n', stdout);
}

bool ko_eq(VALUE a, VALUE b) {
    if (a == b) return true;
    if (FIXNUM_P(a) || FIXNUM_P(b)) {
        if (FIXNUM_P(a) && FIXNUM_P(b)) return a == b;
        if (FIXNUM_P(a) && BUILTIN_TYPE(b) == T_BIGNUM) return ko_int_eq(a, b);
        if (FIXNUM_P(b) && BUILTIN_TYPE(a) == T_BIGNUM) return ko_int_eq(a, b);
        return false;
    }
    if (NIL_P(a) || NIL_P(b)) return a == b;
    if (TRUE_P(a) || TRUE_P(b) || FALSE_P(a) || FALSE_P(b)) return a == b;
    if (SYMBOL_P(a) || SYMBOL_P(b)) return a == b;
    enum ko_type ta = BUILTIN_TYPE(a), tb = BUILTIN_TYPE(b);
    if (ta == T_STRING && tb == T_STRING) {
        return ko_eql(a, b);
    }
    if (ta == T_BIGNUM && tb == T_BIGNUM) return ko_int_eq(a, b);
    if (ta == T_FLOAT && tb == T_FLOAT) return ((struct ko_float *)a)->value == ((struct ko_float *)b)->value;
    return false;
}

/* ---- method dispatch ---- */

static __attribute__((noinline)) void
ko_method_cache_fill(struct method_cache *mc, struct ko_class *klass, struct ko_method *m)
{
    mc->serial = ko_vm->method_serial;
    mc->klass = klass;
    mc->method = m;
    if (m->type == KO_METHOD_AST) {
        mc->body = m->u.ast.body;
        mc->dispatcher = (ko_dispatcher_t)m->u.ast.body->head.dispatcher;
        mc->locals_cnt = m->u.ast.locals_cnt;
        mc->required_params_cnt = m->u.ast.required_params_cnt;
        mc->type = 0;
        mc->cfunc = NULL;
    } else {
        mc->body = NULL;
        mc->dispatcher = NULL;
        mc->locals_cnt = 0;
        mc->required_params_cnt = 0;
        mc->type = 1;
        mc->cfunc = m->u.cfunc.func;
    }
}

VALUE ko_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name,
                       uint32_t argc, uint32_t arg_index, struct ko_proc *block,
                       struct method_cache *mc)
{
    struct ko_class *klass = ko_class_of_class(recv);

    if (UNLIKELY(!mc || mc->serial != ko_vm->method_serial || mc->klass != klass)) {
        struct ko_method *m = ko_class_find_method(klass, name);
        if (UNLIKELY(!m)) {
            ko_raise(c, NULL, "undefined method '%s' for %s",
                     ko_id_name(name), ko_id_name(klass->name));
            return Qnil;
        }
        if (mc) ko_method_cache_fill(mc, klass, m);
        if (m->type == KO_METHOD_CFUNC) {
            VALUE *argv = &c->fp[arg_index];
            struct ko_proc *prev_block = current_block;
            current_block = block;
            VALUE prev_self = c->self;
            c->self = recv;
            VALUE r = m->u.cfunc.func(c, recv, argc, argv);
            c->self = prev_self;
            current_block = prev_block;
            return r;
        }
        /* fallthrough to AST path with mc filled in */
    }

    /* CFUNC fast path via cache */
    if (UNLIKELY(mc->type)) {
        VALUE *argv = &c->fp[arg_index];
        struct ko_proc *prev_block = current_block;
        current_block = block;
        VALUE prev_self = c->self;
        c->self = recv;
        VALUE r = mc->cfunc(c, recv, argc, argv);
        c->self = prev_self;
        current_block = prev_block;
        return r;
    }

    /* AST hot path */
    if (UNLIKELY(argc != mc->required_params_cnt)) {
        ko_raise(c, NULL, "wrong number of arguments (given %u, expected %u) for %s",
                 argc, mc->required_params_cnt, ko_id_name(name));
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    struct ko_proc *prev_block = current_block;
    current_block = block;

    c->fp = prev_fp + arg_index;
    if (UNLIKELY(c->fp + mc->locals_cnt >= c->stack_end)) {
        c->fp = prev_fp;
        ko_raise(c, NULL, "stack overflow");
        current_block = prev_block;
        return Qnil;
    }
    if (c->fp + mc->locals_cnt > c->sp) c->sp = c->fp + mc->locals_cnt;

    /* zero locals beyond args */
    for (uint32_t i = argc; i < mc->locals_cnt; i++) c->fp[i] = Qnil;
    c->self = recv;
    /* Direct dispatcher call — avoids one level of indirection compared to EVAL */
    VALUE r = mc->dispatcher(c, mc->body);
    c->fp = prev_fp;
    c->self = prev_self;
    current_block = prev_block;

    if (UNLIKELY(c->state == KO_RETURN)) {
        r = c->state_value;
        c->state = KO_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

VALUE ko_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv) {
    struct ko_class *klass = ko_class_of_class(recv);
    struct ko_method *m = ko_class_find_method(klass, name);
    if (!m) {
        ko_raise(c, NULL, "undefined method '%s' for %s",
                 ko_id_name(name), ko_id_name(klass->name));
        return Qnil;
    }
    if (m->type == KO_METHOD_CFUNC) {
        VALUE prev_self = c->self;
        c->self = recv;
        VALUE r = m->u.cfunc.func(c, recv, argc, argv);
        c->self = prev_self;
        return r;
    }
    /* AST: same as ko_dispatch_call but argv is ad-hoc */
    if ((unsigned)argc != m->u.ast.required_params_cnt) {
        ko_raise(c, NULL, "wrong arg count");
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    /* push frame after all current locals; we don't know exactly the boundary,
       so use sp as upper bound */
    VALUE *new_fp = c->sp + 1;
    if (new_fp + m->u.ast.locals_cnt >= c->stack_end) {
        ko_raise(c, NULL, "stack overflow");
        return Qnil;
    }
    for (int i = 0; i < argc; i++) new_fp[i] = argv[i];
    for (uint32_t i = argc; i < m->u.ast.locals_cnt; i++) new_fp[i] = Qnil;
    c->fp = new_fp;
    if (c->fp + m->u.ast.locals_cnt > c->sp) c->sp = c->fp + m->u.ast.locals_cnt;
    c->self = recv;
    VALUE r = EVAL(c, m->u.ast.body);
    c->fp = prev_fp;
    c->self = prev_self;
    if (c->state == KO_RETURN) {
        r = c->state_value;
        c->state = KO_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

VALUE ko_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv) {
    return ko_dispatch_binop(c, recv, mid, argc, argv);
}

/* ---- runtime init ---- */

static void init_well_known_ids(void) {
    id_initialize = ko_intern("initialize");
    id_to_s = ko_intern("to_s");
    id_inspect = ko_intern("inspect");
    id_call = ko_intern("call");
    id_each = ko_intern("each");
    id_new = ko_intern("new");
    id_op_plus  = ko_intern("+");
    id_op_minus = ko_intern("-");
    id_op_mul   = ko_intern("*");
    id_op_div   = ko_intern("/");
    id_op_mod   = ko_intern("%");
    id_op_eq    = ko_intern("==");
    id_op_neq   = ko_intern("!=");
    id_op_lt    = ko_intern("<");
    id_op_le    = ko_intern("<=");
    id_op_gt    = ko_intern(">");
    id_op_ge    = ko_intern(">=");
    id_op_aref  = ko_intern("[]");
    id_op_aset  = ko_intern("[]=");
    id_op_lshift= ko_intern("<<");
    id_op_rshift= ko_intern(">>");
    id_op_and   = ko_intern("&");
    id_op_or    = ko_intern("|");
    id_op_xor   = ko_intern("^");
}

void ko_init_builtins(void); /* defined in builtins.c */

void ko_runtime_init(void) {
    GC_INIT();
    init_well_known_ids();

    ko_vm = ko_xmalloc(sizeof(*ko_vm));
    memset(ko_vm, 0, sizeof(*ko_vm));
    ko_vm->method_serial = 1;

    /* bootstrap classes (forward refs) */
    struct ko_class *cObject = ko_class_new(ko_intern("Object"), NULL, T_OBJECT);
    struct ko_class *cClass  = ko_class_new(ko_intern("Class"), cObject, T_CLASS);
    struct ko_class *cModule = ko_class_new(ko_intern("Module"), cObject, T_MODULE);
    cObject->basic.klass = (VALUE)cClass;
    cClass->basic.klass  = (VALUE)cClass;
    cModule->basic.klass = (VALUE)cClass;

    ko_vm->object_class = cObject;
    ko_vm->class_class  = cClass;
    ko_vm->module_class = cModule;

    ko_vm->numeric_class = ko_class_new(ko_intern("Numeric"), cObject, T_OBJECT);
    ko_vm->integer_class = ko_class_new(ko_intern("Integer"), ko_vm->numeric_class, T_BIGNUM);
    ko_vm->float_class   = ko_class_new(ko_intern("Float"),   ko_vm->numeric_class, T_FLOAT);
    ko_vm->string_class  = ko_class_new(ko_intern("String"),  cObject, T_STRING);
    ko_vm->array_class   = ko_class_new(ko_intern("Array"),   cObject, T_ARRAY);
    ko_vm->hash_class    = ko_class_new(ko_intern("Hash"),    cObject, T_HASH);
    ko_vm->symbol_class  = ko_class_new(ko_intern("Symbol"),  cObject, T_SYMBOL);
    ko_vm->true_class    = ko_class_new(ko_intern("TrueClass"),  cObject, T_NONE);
    ko_vm->false_class   = ko_class_new(ko_intern("FalseClass"), cObject, T_NONE);
    ko_vm->nil_class     = ko_class_new(ko_intern("NilClass"),   cObject, T_NONE);
    ko_vm->proc_class    = ko_class_new(ko_intern("Proc"),       cObject, T_PROC);
    ko_vm->range_class   = ko_class_new(ko_intern("Range"),      cObject, T_RANGE);
    ko_vm->kernel_module = ko_module_new(ko_intern("Kernel"));
    ko_vm->comparable_module = ko_module_new(ko_intern("Comparable"));
    ko_vm->enumerable_module = ko_module_new(ko_intern("Enumerable"));

    /* register top-level constants */
    ko_const_set(cObject, ko_vm->object_class->name,  (VALUE)cObject);
    ko_const_set(cObject, ko_vm->class_class->name,   (VALUE)cClass);
    ko_const_set(cObject, ko_vm->module_class->name,  (VALUE)cModule);
    ko_const_set(cObject, ko_vm->integer_class->name, (VALUE)ko_vm->integer_class);
    ko_const_set(cObject, ko_vm->float_class->name,   (VALUE)ko_vm->float_class);
    ko_const_set(cObject, ko_vm->string_class->name,  (VALUE)ko_vm->string_class);
    ko_const_set(cObject, ko_vm->array_class->name,   (VALUE)ko_vm->array_class);
    ko_const_set(cObject, ko_vm->hash_class->name,    (VALUE)ko_vm->hash_class);
    ko_const_set(cObject, ko_vm->symbol_class->name,  (VALUE)ko_vm->symbol_class);
    ko_const_set(cObject, ko_vm->numeric_class->name, (VALUE)ko_vm->numeric_class);
    ko_const_set(cObject, ko_vm->range_class->name,   (VALUE)ko_vm->range_class);
    ko_const_set(cObject, ko_vm->proc_class->name,    (VALUE)ko_vm->proc_class);

    /* main object */
    ko_vm->main_obj_class = ko_class_new(ko_intern("Main"), cObject, T_OBJECT);
    ko_vm->main_obj = ko_object_new(ko_vm->main_obj_class);

    ko_init_builtins();
}
