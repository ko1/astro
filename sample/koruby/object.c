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

struct korb_vm *korb_vm = NULL;

ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

/* ---- memory ---- */
void *korb_xmalloc(size_t s) { void *p = GC_MALLOC(s); if (!p) abort(); return p; }
void *korb_xmalloc_atomic(size_t s) { void *p = GC_MALLOC_ATOMIC(s); if (!p) abort(); return p; }
void *korb_xcalloc(size_t n, size_t sz) { return korb_xmalloc(n * sz); /* GC_MALLOC zero-inits */ }
void *korb_xrealloc(void *p, size_t newsize) {
    void *q = GC_REALLOC(p, newsize); if (!q && newsize) abort(); return q;
}
void  korb_xfree(void *p) { (void)p; /* GC handles */ }

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

ID korb_intern_n(const char *str, long len) {
    uint64_t h = fnv_hash(str, len);
    uint32_t b = (uint32_t)(h % ID_POOL_BUCKETS);
    for (struct id_pool_entry *e = id_pool[b]; e; e = e->next) {
        if (e->len == (size_t)len && memcmp(e->name, str, len) == 0) {
            return e->id;
        }
    }
    struct id_pool_entry *e = korb_xmalloc(sizeof(*e));
    e->name = korb_xmalloc_atomic(len + 1);
    memcpy(e->name, str, len);
    e->name[len] = 0;
    e->len = len;
    e->id = id_next++;
    e->next = id_pool[b];
    id_pool[b] = e;

    if (e->id >= id_index_capa) {
        uint32_t newcapa = id_index_capa == 0 ? 256 : id_index_capa * 2;
        while (e->id >= newcapa) newcapa *= 2;
        id_index = korb_xrealloc(id_index, newcapa * sizeof(*id_index));
        for (uint32_t i = id_index_capa; i < newcapa; i++) id_index[i] = NULL;
        id_index_capa = newcapa;
    }
    id_index[e->id] = e;
    return e->id;
}

ID korb_intern(const char *s) { return korb_intern_n(s, strlen(s)); }

const char *korb_id_name(ID id) {
    if (id == 0 || id >= id_index_capa || !id_index[id]) return "<bad-id>";
    return id_index[id]->name;
}

VALUE korb_id2sym(ID id) {
    /* encode id in upper bits, low byte = SYMBOL_FLAG */
    return ((VALUE)id << 8) | SYMBOL_FLAG;
}

ID korb_sym2id(VALUE sym) {
    return (ID)(sym >> 8);
}

VALUE korb_str_to_sym(VALUE s) {
    return korb_id2sym(korb_intern_n(((struct korb_string *)s)->ptr, ((struct korb_string *)s)->len));
}

/* ---- class system ---- */

static void method_table_init(struct korb_method_table *mt) {
    mt->bucket_cnt = 16;
    mt->buckets = korb_xcalloc(mt->bucket_cnt, sizeof(*mt->buckets));
    mt->size = 0;
}

static void method_table_resize(struct korb_method_table *mt) {
    uint32_t newcap = mt->bucket_cnt * 2;
    struct korb_method_table_entry **newbk = korb_xcalloc(newcap, sizeof(*newbk));
    for (uint32_t i = 0; i < mt->bucket_cnt; i++) {
        struct korb_method_table_entry *e = mt->buckets[i];
        while (e) {
            struct korb_method_table_entry *nx = e->next;
            uint32_t b = (uint32_t)(e->name % newcap);
            e->next = newbk[b];
            newbk[b] = e;
            e = nx;
        }
    }
    mt->buckets = newbk;
    mt->bucket_cnt = newcap;
}

static void method_table_set(struct korb_method_table *mt, ID name, struct korb_method *m) {
    if (mt->size * 2 > mt->bucket_cnt) method_table_resize(mt);
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    for (struct korb_method_table_entry *e = mt->buckets[b]; e; e = e->next) {
        if (e->name == name) { e->method = m; return; }
    }
    struct korb_method_table_entry *e = korb_xmalloc(sizeof(*e));
    e->name = name;
    e->method = m;
    e->next = mt->buckets[b];
    mt->buckets[b] = e;
    mt->size++;
}

static struct korb_method *method_table_get(const struct korb_method_table *mt, ID name) {
    if (!mt->buckets) return NULL;
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    for (struct korb_method_table_entry *e = mt->buckets[b]; e; e = e->next) {
        if (e->name == name) return e->method;
    }
    return NULL;
}

struct korb_class *korb_class_new(ID name, struct korb_class *super, enum korb_type instance_type) {
    struct korb_class *k = korb_xmalloc(sizeof(*k));
    k->basic.flags = T_CLASS;
    k->basic.klass = korb_vm ? (VALUE)korb_vm->class_class : 0;
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

struct korb_class *korb_module_new(ID name) {
    struct korb_class *k = korb_class_new(name, NULL, T_NONE);
    k->basic.flags = T_MODULE;
    k->basic.klass = korb_vm ? (VALUE)korb_vm->module_class : 0;
    return k;
}

void korb_class_add_method_ast(struct korb_class *klass, ID name, struct Node *body, uint32_t params_cnt, uint32_t locals_cnt) {
    korb_class_add_method_ast_full(klass, name, body, params_cnt, params_cnt, -1, locals_cnt);
}

void korb_class_add_method_ast_full(struct korb_class *klass, ID name, struct Node *body,
                                    uint32_t required_params, uint32_t total_params,
                                    int rest_slot, uint32_t locals_cnt) {
    korb_class_add_method_ast_full_cref(klass, name, body, required_params,
                                         total_params, rest_slot, locals_cnt, NULL);
}

struct korb_cref *korb_cref_dup(struct korb_cref *src) {
    /* Deep-copy a cref chain into the heap so it survives stack unwind. */
    if (!src) return NULL;
    struct korb_cref *head = NULL, *tail = NULL;
    for (; src; src = src->prev) {
        struct korb_cref *e = korb_xmalloc(sizeof(*e));
        e->klass = src->klass;
        e->prev = NULL;
        if (!head) head = tail = e;
        else { tail->prev = e; tail = e; }
    }
    return head;
}

void korb_class_add_method_ast_full_cref(struct korb_class *klass, ID name, struct Node *body,
                                          uint32_t required_params, uint32_t total_params,
                                          int rest_slot, uint32_t locals_cnt,
                                          struct korb_cref *def_cref) {
    struct korb_method *m = korb_xmalloc(sizeof(*m));
    m->type = KORB_METHOD_AST;
    m->name = name;
    m->defining_class = klass;
    m->def_cref = korb_cref_dup(def_cref);
    m->u.ast.body = body;
    m->u.ast.required_params_cnt = required_params;
    m->u.ast.total_params_cnt = total_params;
    m->u.ast.rest_slot = rest_slot;
    m->u.ast.locals_cnt = locals_cnt;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) korb_vm->method_serial++;
}

void korb_class_add_method_cfunc(struct korb_class *klass, ID name,
                               VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc) {
    struct korb_method *m = korb_xmalloc(sizeof(*m));
    m->type = KORB_METHOD_CFUNC;
    m->name = name;
    m->defining_class = klass;
    m->u.cfunc.func = func;
    m->u.cfunc.argc = argc;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) korb_vm->method_serial++;
}

struct korb_class *korb_singleton_class_of(struct korb_class *klass) {
    /* If klass->basic.klass is the shared metaclass, create a per-instance
     * singleton class so per-class methods can be installed. */
    struct korb_class *current_meta = (struct korb_class *)klass->basic.klass;
    if (current_meta == korb_vm->class_class || current_meta == korb_vm->module_class) {
        struct korb_class *meta = korb_class_new(klass->name, current_meta, T_CLASS);
        meta->basic.flags = T_CLASS;
        klass->basic.klass = (VALUE)meta;
        return meta;
    }
    return current_meta;
}

void korb_module_include(struct korb_class *klass, struct korb_class *mod) {
    for (uint32_t b = 0; b < mod->methods.bucket_cnt; b++) {
        for (struct korb_method_table_entry *e = mod->methods.buckets[b]; e; e = e->next) {
            if (!method_table_get(&klass->methods, e->name)) {
                method_table_set(&klass->methods, e->name, e->method);
            }
        }
    }
    for (struct korb_const_entry *ce = mod->constants; ce; ce = ce->next) {
        if (!korb_const_has(klass, ce->name)) korb_const_set(klass, ce->name, ce->value);
    }
}

struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name) {
    while (klass) {
        struct korb_method *m = method_table_get(&klass->methods, name);
        if (m) return m;
        klass = klass->super;
    }
    return NULL;
}

/* ---- constants ---- */
void korb_const_set(struct korb_class *klass, ID name, VALUE value) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) { e->value = value; return; }
    }
    struct korb_const_entry *e = korb_xmalloc(sizeof(*e));
    e->name = name;
    e->value = value;
    e->next = klass->constants;
    klass->constants = e;
}

VALUE korb_const_get(struct korb_class *klass, ID name) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return e->value;
    }
    return Qundef;
}

bool korb_const_has(struct korb_class *klass, ID name) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return true;
    }
    return false;
}

VALUE korb_const_lookup(CTX *c, ID name) {
    /* Lexical lookup along cref chain, then ancestors of innermost cref. */
    for (struct korb_cref *cr = c->cref; cr; cr = cr->prev) {
        VALUE v = korb_const_get(cr->klass, name);
        if (!UNDEF_P(v)) return v;
    }
    /* Inheritance chain of innermost class */
    struct korb_class *k = c->cref ? c->cref->klass : c->current_class;
    for (struct korb_class *kk = k ? k->super : NULL; kk; kk = kk->super) {
        VALUE v = korb_const_get(kk, name);
        if (!UNDEF_P(v)) return v;
    }
    /* Object as global namespace */
    VALUE v = korb_const_get(korb_vm->object_class, name);
    if (!UNDEF_P(v)) return v;
    korb_raise(c, NULL, "uninitialized constant %s", korb_id_name(name));
    return Qnil;
}

/* ---- gvars ---- */
static struct korb_method_table gvars_table_dummy; /* unused; we reuse a hash */
static struct {
    ID *keys;
    VALUE *vals;
    uint32_t size, capa;
} gvars;

VALUE korb_gvar_get(ID name) {
    for (uint32_t i = 0; i < gvars.size; i++) if (gvars.keys[i] == name) return gvars.vals[i];
    return Qnil;
}

void korb_gvar_set(ID name, VALUE v) {
    for (uint32_t i = 0; i < gvars.size; i++) if (gvars.keys[i] == name) { gvars.vals[i] = v; return; }
    if (gvars.size >= gvars.capa) {
        uint32_t nc = gvars.capa == 0 ? 8 : gvars.capa * 2;
        gvars.keys = korb_xrealloc(gvars.keys, nc * sizeof(ID));
        gvars.vals = korb_xrealloc(gvars.vals, nc * sizeof(VALUE));
        gvars.capa = nc;
    }
    gvars.keys[gvars.size] = name;
    gvars.vals[gvars.size] = v;
    gvars.size++;
}

/* ---- objects (with class-shape ivars) ---- */
VALUE korb_object_new(struct korb_class *klass) {
    struct korb_object *o = korb_xmalloc(sizeof(*o));
    o->basic.flags = klass->instance_type ? klass->instance_type : T_OBJECT;
    o->basic.klass = (VALUE)klass;
    o->ivar_cnt = 0;
    o->ivar_capa = 0;
    o->ivars = NULL;
    return (VALUE)o;
}

static int ivar_slot(struct korb_class *k, ID name) {
    for (uint32_t i = 0; i < k->ivar_count; i++) if (k->ivar_names[i] == name) return (int)i;
    return -1;
}

static int ivar_slot_assign(struct korb_class *k, ID name) {
    int s = ivar_slot(k, name);
    if (s >= 0) return s;
    if (k->ivar_count >= k->ivar_capa) {
        uint32_t nc = k->ivar_capa == 0 ? 4 : k->ivar_capa * 2;
        k->ivar_names = korb_xrealloc(k->ivar_names, nc * sizeof(ID));
        k->ivar_capa = nc;
    }
    s = k->ivar_count++;
    k->ivar_names[s] = name;
    return s;
}

VALUE korb_ivar_get(VALUE obj, ID name) {
    if (SPECIAL_CONST_P(obj)) return Qnil;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return Qnil;
    struct korb_object *o = (struct korb_object *)obj;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    int s = ivar_slot(k, name);
    if (s < 0 || (uint32_t)s >= o->ivar_cnt) return Qnil;
    return o->ivars[s];
}

void korb_ivar_set(VALUE obj, ID name, VALUE val) {
    if (SPECIAL_CONST_P(obj)) return;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return;
    struct korb_object *o = (struct korb_object *)obj;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    int s = ivar_slot_assign(k, name);
    if ((uint32_t)s >= o->ivar_capa) {
        uint32_t nc = o->ivar_capa == 0 ? 4 : o->ivar_capa * 2;
        while ((uint32_t)s >= nc) nc *= 2;
        o->ivars = korb_xrealloc(o->ivars, nc * sizeof(VALUE));
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
VALUE korb_str_new(const char *p, long len) {
    struct korb_string *s = korb_xmalloc(sizeof(*s));
    s->basic.flags = T_STRING;
    s->basic.klass = korb_vm ? (VALUE)korb_vm->string_class : 0;
    s->ptr = korb_xmalloc_atomic(len + 1);
    if (p && len > 0) memcpy(s->ptr, p, len);
    s->ptr[len] = 0;
    s->len = len;
    s->capa = len;
    return (VALUE)s;
}

VALUE korb_str_new_cstr(const char *cstr) { return korb_str_new(cstr, (long)strlen(cstr)); }

VALUE korb_str_dup(VALUE s) {
    return korb_str_new(((struct korb_string *)s)->ptr, ((struct korb_string *)s)->len);
}

VALUE korb_str_concat(VALUE a, VALUE b) {
    struct korb_string *x = (struct korb_string *)a;
    struct korb_string *y = (struct korb_string *)b;
    long total = x->len + y->len;
    if (total > x->capa) {
        long nc = x->capa == 0 ? total : x->capa;
        while (nc < total) nc *= 2;
        char *np = korb_xmalloc_atomic(nc + 1);
        memcpy(np, x->ptr, x->len);
        x->ptr = np;
        x->capa = nc;
    }
    memcpy(x->ptr + x->len, y->ptr, y->len);
    x->len = total;
    x->ptr[x->len] = 0;
    return a;
}

const char *korb_str_cstr(VALUE s) {
    if (BUILTIN_TYPE(s) != T_STRING) return "<not-string>";
    return ((struct korb_string *)s)->ptr;
}

long korb_str_len(VALUE s) { return ((struct korb_string *)s)->len; }

/* ---- array ---- */
VALUE korb_ary_new_capa(long capa) {
    struct korb_array *a = korb_xmalloc(sizeof(*a));
    a->basic.flags = T_ARRAY;
    a->basic.klass = korb_vm ? (VALUE)korb_vm->array_class : 0;
    a->len = 0;
    a->capa = capa < 4 ? 4 : capa;
    a->ptr = korb_xmalloc(a->capa * sizeof(VALUE));
    for (long i = 0; i < a->capa; i++) a->ptr[i] = Qnil;
    return (VALUE)a;
}
VALUE korb_ary_new(void) { return korb_ary_new_capa(0); }

VALUE korb_ary_new_from_values(long n, const VALUE *vals) {
    VALUE a = korb_ary_new_capa(n);
    for (long i = 0; i < n; i++) korb_ary_push(a, vals[i]);
    return a;
}

void korb_ary_push(VALUE av, VALUE v) {
    struct korb_array *a = (struct korb_array *)av;
    if (a->len >= a->capa) {
        long nc = a->capa == 0 ? 4 : a->capa * 2;
        a->ptr = korb_xrealloc(a->ptr, nc * sizeof(VALUE));
        for (long i = a->capa; i < nc; i++) a->ptr[i] = Qnil;
        a->capa = nc;
    }
    a->ptr[a->len++] = v;
}

VALUE korb_ary_pop(VALUE av) {
    struct korb_array *a = (struct korb_array *)av;
    if (a->len == 0) return Qnil;
    return a->ptr[--a->len];
}

VALUE korb_ary_aref(VALUE av, long i) {
    struct korb_array *a = (struct korb_array *)av;
    if (i < 0) i += a->len;
    if (i < 0 || i >= a->len) return Qnil;
    return a->ptr[i];
}

void korb_ary_aset(VALUE av, long i, VALUE v) {
    struct korb_array *a = (struct korb_array *)av;
    if (i < 0) i += a->len;
    if (i < 0) return;
    while (a->len <= i) {
        if (a->len >= a->capa) {
            long nc = a->capa == 0 ? 4 : a->capa * 2;
            while (nc <= i) nc *= 2;
            a->ptr = korb_xrealloc(a->ptr, nc * sizeof(VALUE));
            for (long k = a->capa; k < nc; k++) a->ptr[k] = Qnil;
            a->capa = nc;
        }
        a->ptr[a->len++] = Qnil;
    }
    a->ptr[i] = v;
}

long korb_ary_len(VALUE av) { return ((struct korb_array *)av)->len; }

/* ---- hash ---- */
uint64_t korb_hash_value(VALUE v) {
    if (FIXNUM_P(v)) return (uint64_t)v * 11400714819323198485ULL;
    if (SYMBOL_P(v)) return (uint64_t)v * 2654435761ULL;
    if (NIL_P(v)) return 0;
    if (TRUE_P(v)) return 1;
    if (FALSE_P(v)) return 2;
    if (FLONUM_P(v)) return (uint64_t)v * 11400714819323198485ULL;
    if (BUILTIN_TYPE(v) == T_STRING) {
        return fnv_hash(((struct korb_string *)v)->ptr, ((struct korb_string *)v)->len);
    }
    return (uint64_t)v;
}

bool korb_eql(VALUE a, VALUE b) {
    if (a == b) return true;
    if (SPECIAL_CONST_P(a) || SPECIAL_CONST_P(b)) return false;
    if (BUILTIN_TYPE(a) == T_STRING && BUILTIN_TYPE(b) == T_STRING) {
        struct korb_string *x = (struct korb_string *)a;
        struct korb_string *y = (struct korb_string *)b;
        return x->len == y->len && memcmp(x->ptr, y->ptr, x->len) == 0;
    }
    return false;
}

VALUE korb_hash_new(void) {
    struct korb_hash *h = korb_xmalloc(sizeof(*h));
    h->basic.flags = T_HASH;
    h->basic.klass = korb_vm ? (VALUE)korb_vm->hash_class : 0;
    h->bucket_cnt = 8;
    h->buckets = korb_xcalloc(h->bucket_cnt, sizeof(*h->buckets));
    h->size = 0;
    h->first = h->last = NULL;
    h->default_value = Qnil;
    return (VALUE)h;
}

static void korb_hash_resize(struct korb_hash *h) {
    uint32_t nc = h->bucket_cnt * 2;
    struct korb_hash_entry **newbk = korb_xcalloc(nc, sizeof(*newbk));
    /* re-insert by iterating insertion order */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        uint32_t b = (uint32_t)(e->hash % nc);
        /* note: e->next is the insertion order chain, which we don't change */
        /* we need a separate per-bucket chain. Let's just rehash. */
        /* simpler: store via aset which uses bucket chain. */
    }
    /* simpler approach: rebuild from scratch via aset */
    struct korb_hash_entry *first = h->first;
    h->buckets = newbk;
    h->bucket_cnt = nc;
    h->size = 0;
    h->first = h->last = NULL;
    for (struct korb_hash_entry *e = first; e; ) {
        struct korb_hash_entry *nx = e->next;
        korb_hash_aset((VALUE)h, e->key, e->value);
        e = nx;
    }
}

VALUE korb_hash_aset(VALUE hv, VALUE key, VALUE val) {
    struct korb_hash *h = (struct korb_hash *)hv;
    if (h->size * 2 > h->bucket_cnt) korb_hash_resize(h);
    uint64_t hh = korb_hash_value(key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    /* search existing */
    for (struct korb_hash_entry *e = h->buckets[b]; e; ) {
        /* we use 'next' as insertion-order chain; need separate bucket chain.
         * Simplification: linear scan within bucket via insertion-order entries
         * filtered by hash. Not super efficient but correct. */
        if (e->hash == hh && korb_eql(e->key, key)) {
            e->value = val;
            return val;
        }
        /* break — bucket only holds first matching */
        break;
    }
    /* full linear scan to be safe */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, key)) {
            e->value = val;
            return val;
        }
    }
    struct korb_hash_entry *e = korb_xmalloc(sizeof(*e));
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

VALUE korb_hash_aref(VALUE hv, VALUE key) {
    struct korb_hash *h = (struct korb_hash *)hv;
    uint64_t hh = korb_hash_value(key);
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        if (e->hash == hh && korb_eql(e->key, key)) return e->value;
    }
    return h->default_value;
}

long korb_hash_size(VALUE hv) { return ((struct korb_hash *)hv)->size; }

/* ---- range ---- */
VALUE korb_range_new(VALUE b, VALUE e, bool excl) {
    struct korb_range *r = korb_xmalloc(sizeof(*r));
    r->basic.flags = T_RANGE;
    r->basic.klass = korb_vm ? (VALUE)korb_vm->range_class : 0;
    r->begin = b;
    r->end = e;
    r->exclude_end = excl;
    return (VALUE)r;
}

/* ---- float ---- */
VALUE korb_float_new(double d) {
    /* Use FLONUM encoding for typical doubles */
    /* CRuby flonum encoding: rotate exponent bits.  We just heap-box for safety. */
    struct korb_float *f = korb_xmalloc(sizeof(*f));
    f->basic.flags = T_FLOAT;
    f->basic.klass = korb_vm ? (VALUE)korb_vm->float_class : 0;
    f->value = d;
    return (VALUE)f;
}

double korb_num2dbl(VALUE v) {
    if (FIXNUM_P(v)) return (double)FIX2LONG(v);
    if (BUILTIN_TYPE(v) == T_FLOAT) return ((struct korb_float *)v)->value;
    if (BUILTIN_TYPE(v) == T_BIGNUM) return mpz_get_d((mpz_ptr)(((struct korb_bignum *)v)->mpz));
    return 0.0;
}

/* ---- bignum (GMP) ---- */
VALUE korb_bignum_new_str(const char *str, int base) {
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *z = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set_str(*z, str, base);
    b->mpz = z;
    /* if it fits in fixnum, return fixnum */
    if (mpz_fits_slong_p(*z)) {
        long v = mpz_get_si(*z);
        if (FIXABLE(v)) return INT2FIX(v);
    }
    return (VALUE)b;
}

VALUE korb_bignum_new_long(long v) {
    if (FIXABLE(v)) return INT2FIX(v);
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *z = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set_si(*z, v);
    b->mpz = z;
    return (VALUE)b;
}

static void to_mpz(VALUE v, mpz_t out) {
    if (FIXNUM_P(v)) mpz_init_set_si(out, FIX2LONG(v));
    else mpz_init_set(out, (mpz_ptr)((struct korb_bignum *)v)->mpz);
}

static VALUE from_mpz(mpz_t z) {
    if (mpz_fits_slong_p(z)) {
        long v = mpz_get_si(z);
        if (FIXABLE(v)) { mpz_clear(z); return INT2FIX(v); }
    }
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *bz = korb_xmalloc(sizeof(mpz_t));
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

VALUE korb_int_plus(VALUE a, VALUE b)  { BIGOP(mpz_add); }
VALUE korb_int_minus(VALUE a, VALUE b) { BIGOP(mpz_sub); }
VALUE korb_int_mul(VALUE a, VALUE b)   { BIGOP(mpz_mul); }
VALUE korb_int_div(VALUE a, VALUE b) {
    mpz_t la, ra, q;
    mpz_init(q);
    to_mpz(a, la); to_mpz(b, ra);
    mpz_fdiv_q(q, la, ra);
    mpz_clear(la); mpz_clear(ra);
    return from_mpz(q);
}
VALUE korb_int_mod(VALUE a, VALUE b) {
    mpz_t la, ra, m;
    mpz_init(m);
    to_mpz(a, la); to_mpz(b, ra);
    mpz_fdiv_r(m, la, ra);
    mpz_clear(la); mpz_clear(ra);
    return from_mpz(m);
}
VALUE korb_int_lshift(VALUE a, VALUE b) {
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
VALUE korb_int_rshift(VALUE a, VALUE b) {
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
VALUE korb_int_and(VALUE a, VALUE b) { BIGOP(mpz_and); }
VALUE korb_int_or(VALUE a, VALUE b) { BIGOP(mpz_ior); }
VALUE korb_int_xor(VALUE a, VALUE b) { BIGOP(mpz_xor); }
int korb_int_cmp(VALUE a, VALUE b) {
    if (FIXNUM_P(a) && FIXNUM_P(b)) {
        long la = FIX2LONG(a), lb = FIX2LONG(b);
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    mpz_t la, ra; to_mpz(a, la); to_mpz(b, ra);
    int c = mpz_cmp(la, ra);
    mpz_clear(la); mpz_clear(ra);
    return c;
}
bool korb_int_eq(VALUE a, VALUE b) { return korb_int_cmp(a, b) == 0; }

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
VALUE korb_proc_new(struct Node *body, VALUE *fp, uint32_t env_size,
                  uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda) {
    struct korb_proc *p = korb_xmalloc(sizeof(*p));
    p->basic.flags = T_PROC;
    p->basic.klass = korb_vm ? (VALUE)korb_vm->proc_class : 0;
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
static __thread struct korb_proc *current_block = NULL;

VALUE korb_yield(CTX *c, uint32_t argc, VALUE *argv) {
    if (UNLIKELY(!current_block)) {
        korb_raise(c, NULL, "no block given (yield)");
        return Qnil;
    }
    struct korb_proc *blk = current_block;
    /* Shared-fp closure: block evaluates with env_fp's view of locals. */
    VALUE *fp = blk->env;
    VALUE prev_self = c->self;
    for (uint32_t i = 0; i < blk->params_cnt && i < argc; i++) {
        fp[blk->param_base + i] = argv[i];
    }
    c->self = blk->self;
    VALUE r = EVAL(c, blk->body);
    c->self = prev_self;
    if (c->state == KORB_BREAK) {
        VALUE bv = c->state_value;
        c->state = KORB_NORMAL; c->state_value = Qnil;
        return bv;
    }
    if (c->state == KORB_NEXT) {
        VALUE nv = c->state_value;
        c->state = KORB_NORMAL; c->state_value = Qnil;
        return nv;
    }
    return r;
}

/* ---- class lookup ---- */
struct korb_class *korb_class_of_class(VALUE v) {
    if (FIXNUM_P(v)) return korb_vm->integer_class;
    if (FLONUM_P(v)) return korb_vm->float_class;
    if (SYMBOL_P(v)) return korb_vm->symbol_class;
    if (NIL_P(v))   return korb_vm->nil_class;
    if (TRUE_P(v))  return korb_vm->true_class;
    if (FALSE_P(v)) return korb_vm->false_class;
    /* heap object */
    return (struct korb_class *)((struct RBasic *)v)->klass;
}

VALUE korb_class_of(VALUE v) { return (VALUE)korb_class_of_class(v); }

/* ---- exceptions ---- */
struct korb_exception {
    struct RBasic basic;
    VALUE message;
    struct korb_class *exc_class;
};

VALUE korb_exc_new(struct korb_class *klass, const char *msg) {
    struct korb_exception *e = korb_xmalloc(sizeof(*e));
    e->basic.flags = T_DATA;
    e->basic.klass = klass ? (VALUE)klass : (VALUE)korb_vm->object_class;
    e->message = korb_str_new_cstr(msg);
    e->exc_class = klass;
    return (VALUE)e;
}

void korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE e = korb_exc_new(klass, buf);
    c->state = KORB_RAISE;
    c->state_value = e;
}

/* ---- inspect / to_s ---- */
static void str_appendf(VALUE s, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE part = korb_str_new_cstr(buf);
    korb_str_concat(s, part);
}

static VALUE korb_inspect_inner(VALUE v, int depth);

static VALUE korb_inspect_inner(VALUE v, int depth) {
    if (depth > 32) return korb_str_new_cstr("...");
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return korb_str_new_cstr(b);
    }
    if (NIL_P(v)) return korb_str_new_cstr("nil");
    if (TRUE_P(v)) return korb_str_new_cstr("true");
    if (FALSE_P(v)) return korb_str_new_cstr("false");
    if (SYMBOL_P(v)) {
        VALUE s = korb_str_new_cstr(":");
        korb_str_concat(s, korb_str_new_cstr(korb_id_name(korb_sym2id(v))));
        return s;
    }
    enum korb_type t = BUILTIN_TYPE(v);
    if (t == T_STRING) {
        struct korb_string *s = (struct korb_string *)v;
        VALUE r = korb_str_new_cstr("\"");
        korb_str_concat(r, korb_str_new(s->ptr, s->len));
        korb_str_concat(r, korb_str_new_cstr("\""));
        return r;
    }
    if (t == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)v;
        VALUE r = korb_str_new_cstr("[");
        for (long i = 0; i < a->len; i++) {
            if (i) korb_str_concat(r, korb_str_new_cstr(", "));
            korb_str_concat(r, korb_inspect_inner(a->ptr[i], depth+1));
        }
        korb_str_concat(r, korb_str_new_cstr("]"));
        return r;
    }
    if (t == T_HASH) {
        struct korb_hash *h = (struct korb_hash *)v;
        VALUE r = korb_str_new_cstr("{");
        bool first = true;
        for (struct korb_hash_entry *e = h->first; e; e = e->next) {
            if (!first) korb_str_concat(r, korb_str_new_cstr(", "));
            first = false;
            korb_str_concat(r, korb_inspect_inner(e->key, depth+1));
            korb_str_concat(r, korb_str_new_cstr("=>"));
            korb_str_concat(r, korb_inspect_inner(e->value, depth+1));
        }
        korb_str_concat(r, korb_str_new_cstr("}"));
        return r;
    }
    if (t == T_RANGE) {
        struct korb_range *r = (struct korb_range *)v;
        VALUE s = korb_inspect_inner(r->begin, depth+1);
        korb_str_concat(s, korb_str_new_cstr(r->exclude_end ? "..." : ".."));
        korb_str_concat(s, korb_inspect_inner(r->end, depth+1));
        return s;
    }
    if (t == T_FLOAT) {
        char b[64]; snprintf(b, 64, "%.17g", ((struct korb_float *)v)->value);
        return korb_str_new_cstr(b);
    }
    if (t == T_BIGNUM) {
        struct korb_bignum *bn = (struct korb_bignum *)v;
        char *s = mpz_get_str(NULL, 10, (mpz_ptr)bn->mpz);
        VALUE r = korb_str_new_cstr(s);
        free(s);
        return r;
    }
    if (t == T_CLASS || t == T_MODULE) {
        return korb_str_new_cstr(korb_id_name(((struct korb_class *)v)->name));
    }
    if (t == T_OBJECT) {
        char b[64];
        struct korb_class *k = (struct korb_class *)((struct korb_object *)v)->basic.klass;
        snprintf(b, 64, "#<%s:%p>", korb_id_name(k->name), (void *)v);
        return korb_str_new_cstr(b);
    }
    if (t == T_DATA) {
        struct korb_exception *e = (struct korb_exception *)v;
        if (BUILTIN_TYPE(e->message) == T_STRING) {
            VALUE r = korb_str_new_cstr("#<Exception: ");
            korb_str_concat(r, e->message);
            korb_str_concat(r, korb_str_new_cstr(">"));
            return r;
        }
        return korb_str_new_cstr("#<data>");
    }
    if (t == T_PROC) return korb_str_new_cstr("#<Proc>");
    if (t == T_SYMBOL) return korb_str_new_cstr(":?");
    return korb_str_new_cstr("#<?>");
}

VALUE korb_inspect(VALUE v) { return korb_inspect_inner(v, 0); }

VALUE korb_to_s(VALUE v) {
    if (BUILTIN_TYPE(v) == T_STRING) return v;
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return korb_str_new_cstr(b);
    }
    if (NIL_P(v)) return korb_str_new_cstr("");
    if (SYMBOL_P(v)) return korb_str_new_cstr(korb_id_name(korb_sym2id(v)));
    return korb_inspect(v);
}

void korb_p(VALUE v) {
    VALUE s = korb_inspect(v);
    fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
    fputc('\n', stdout);
}

bool korb_eq(VALUE a, VALUE b) {
    if (a == b) return true;
    if (FIXNUM_P(a) || FIXNUM_P(b)) {
        if (FIXNUM_P(a) && FIXNUM_P(b)) return a == b;
        if (FIXNUM_P(a) && BUILTIN_TYPE(b) == T_BIGNUM) return korb_int_eq(a, b);
        if (FIXNUM_P(b) && BUILTIN_TYPE(a) == T_BIGNUM) return korb_int_eq(a, b);
        return false;
    }
    if (NIL_P(a) || NIL_P(b)) return a == b;
    if (TRUE_P(a) || TRUE_P(b) || FALSE_P(a) || FALSE_P(b)) return a == b;
    if (SYMBOL_P(a) || SYMBOL_P(b)) return a == b;
    enum korb_type ta = BUILTIN_TYPE(a), tb = BUILTIN_TYPE(b);
    if (ta == T_STRING && tb == T_STRING) {
        return korb_eql(a, b);
    }
    if (ta == T_BIGNUM && tb == T_BIGNUM) return korb_int_eq(a, b);
    if (ta == T_FLOAT && tb == T_FLOAT) return ((struct korb_float *)a)->value == ((struct korb_float *)b)->value;
    return false;
}

/* ---- method dispatch ---- */

static __attribute__((noinline)) void
korb_method_cache_fill(struct method_cache *mc, struct korb_class *klass, struct korb_method *m)
{
    mc->serial = korb_vm->method_serial;
    mc->klass = klass;
    mc->method = m;
    if (m->type == KORB_METHOD_AST) {
        mc->body = m->u.ast.body;
        mc->dispatcher = (korb_dispatcher_t)m->u.ast.body->head.dispatcher;
        mc->locals_cnt = m->u.ast.locals_cnt;
        mc->required_params_cnt = m->u.ast.required_params_cnt;
        mc->total_params_cnt = m->u.ast.total_params_cnt;
        mc->rest_slot = m->u.ast.rest_slot;
        mc->type = 0;
        mc->cfunc = NULL;
        mc->def_cref = m->def_cref;
    } else {
        mc->body = NULL;
        mc->dispatcher = NULL;
        mc->locals_cnt = 0;
        mc->required_params_cnt = 0;
        mc->total_params_cnt = 0;
        mc->rest_slot = -1;
        mc->type = 1;
        mc->cfunc = m->u.cfunc.func;
        mc->def_cref = NULL;
    }
}

VALUE korb_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name,
                       uint32_t argc, uint32_t arg_index, struct korb_proc *block,
                       struct method_cache *mc)
{
    struct korb_class *klass = korb_class_of_class(recv);

    if (UNLIKELY(!mc || mc->serial != korb_vm->method_serial || mc->klass != klass)) {
        struct korb_method *m = korb_class_find_method(klass, name);
        if (UNLIKELY(!m)) {
            korb_raise(c, NULL, "undefined method '%s' for %s",
                     korb_id_name(name), korb_id_name(klass->name));
            return Qnil;
        }
        if (mc) korb_method_cache_fill(mc, klass, m);
        if (m->type == KORB_METHOD_CFUNC) {
            VALUE *argv = &c->fp[arg_index];
            struct korb_proc *prev_block = current_block;
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
        struct korb_proc *prev_block = current_block;
        current_block = block;
        VALUE prev_self = c->self;
        c->self = recv;
        VALUE r = mc->cfunc(c, recv, argc, argv);
        c->self = prev_self;
        current_block = prev_block;
        return r;
    }

    /* AST hot path */
    /* Argument matching:
     *   argc < required          → fill missing positions with Qnil (loose)
     *   argc <= total            → optional slots beyond argc → Qnil
     *   rest_slot >= 0           → leftover args go into rest_slot as Array
     *   else                     → too many args, raise
     */
    if (UNLIKELY(mc->rest_slot < 0 && argc > mc->total_params_cnt)) {
        korb_raise(c, NULL, "wrong number of arguments (given %u, expected %u) for %s",
                 argc, mc->required_params_cnt, korb_id_name(name));
        return Qnil;
    }

    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    struct korb_proc *prev_block = current_block;
    struct korb_cref *prev_cref = c->cref;
    current_block = block;

    c->fp = prev_fp + arg_index;
    if (UNLIKELY(c->fp + mc->locals_cnt >= c->stack_end)) {
        c->fp = prev_fp;
        korb_raise(c, NULL, "stack overflow");
        current_block = prev_block;
        return Qnil;
    }
    if (c->fp + mc->locals_cnt > c->sp) c->sp = c->fp + mc->locals_cnt;
    /* Switch to the method's lexical cref so const lookup sees its
     * enclosing classes.  Use the captured chain. */
    if (mc->def_cref) c->cref = mc->def_cref;

    /* If there's a *rest slot, gather extras (or empty array). */
    if (mc->rest_slot >= 0) {
        long extra = (long)argc - (long)(mc->total_params_cnt - 1);
        if (extra < 0) extra = 0;
        VALUE rest = korb_ary_new_capa(extra);
        for (long i = 0; i < extra; i++) {
            korb_ary_push(rest, c->fp[mc->total_params_cnt - 1 + i]);
        }
        c->fp[mc->rest_slot] = rest;
    }

    /* Initialize slots between argc and total_params with Qundef so that
     * node_default_init can tell "supplied" from "use default".  All other
     * locals (above total_params, except rest_slot) get Qnil. */
    uint32_t opt_start = argc;
    if (opt_start < mc->required_params_cnt) opt_start = mc->required_params_cnt;
    for (uint32_t i = opt_start; i < mc->total_params_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        c->fp[i] = Qundef;
    }
    /* Locals beyond all params */
    for (uint32_t i = mc->total_params_cnt; i < mc->locals_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        c->fp[i] = Qnil;
    }
    /* Args between required and argc: leave as supplied (already in fp[i]) */
    c->self = recv;
    /* push frame for super() / backtrace */
    struct korb_frame frame = {
        .prev = c->current_frame,
        .caller_node = callsite,
        .method = mc->method,
        .self = recv,
        .fp = c->fp,
        .locals_cnt = mc->locals_cnt,
    };
    c->current_frame = &frame;
    /* Direct dispatcher call — avoids one level of indirection compared to EVAL */
    VALUE r = mc->dispatcher(c, mc->body);
    c->current_frame = frame.prev;
    c->fp = prev_fp;
    c->self = prev_self;
    c->cref = prev_cref;
    current_block = prev_block;

    if (UNLIKELY(c->state == KORB_RETURN)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

VALUE korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv) {
    struct korb_class *klass = korb_class_of_class(recv);
    struct korb_method *m = korb_class_find_method(klass, name);
    if (!m) {
        korb_raise(c, NULL, "undefined method '%s' for %s",
                 korb_id_name(name), korb_id_name(klass->name));
        return Qnil;
    }
    if (m->type == KORB_METHOD_CFUNC) {
        VALUE prev_self = c->self;
        c->self = recv;
        VALUE r = m->u.cfunc.func(c, recv, argc, argv);
        c->self = prev_self;
        return r;
    }
    /* AST: same as korb_dispatch_call but argv is ad-hoc */
    if (m->u.ast.rest_slot < 0 && (unsigned)argc > m->u.ast.total_params_cnt) {
        korb_raise(c, NULL, "wrong arg count for %s", korb_id_name(name));
        return Qnil;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    /* push frame after all current locals; we don't know exactly the boundary,
       so use sp as upper bound */
    VALUE *new_fp = c->sp + 1;
    if (new_fp + m->u.ast.locals_cnt >= c->stack_end) {
        korb_raise(c, NULL, "stack overflow");
        return Qnil;
    }
    for (int i = 0; i < argc; i++) new_fp[i] = argv[i];
    /* rest_slot collection */
    if (m->u.ast.rest_slot >= 0) {
        long extra = (long)argc - (long)(m->u.ast.total_params_cnt - 1);
        if (extra < 0) extra = 0;
        VALUE rest = korb_ary_new_capa(extra);
        for (long i = 0; i < extra; i++) {
            korb_ary_push(rest, new_fp[m->u.ast.total_params_cnt - 1 + i]);
        }
        new_fp[m->u.ast.rest_slot] = rest;
    }
    /* Qundef for missing optionals */
    uint32_t opt_start = (unsigned)argc;
    if (opt_start < m->u.ast.required_params_cnt) opt_start = m->u.ast.required_params_cnt;
    for (uint32_t i = opt_start; i < m->u.ast.total_params_cnt; i++) {
        if ((int)i == m->u.ast.rest_slot) continue;
        new_fp[i] = Qundef;
    }
    for (uint32_t i = m->u.ast.total_params_cnt; i < m->u.ast.locals_cnt; i++) {
        if ((int)i == m->u.ast.rest_slot) continue;
        new_fp[i] = Qnil;
    }
    c->fp = new_fp;
    if (c->fp + m->u.ast.locals_cnt > c->sp) c->sp = c->fp + m->u.ast.locals_cnt;
    c->self = recv;
    struct korb_cref *prev_cref2 = c->cref;
    if (m->def_cref) c->cref = m->def_cref;
    /* push frame for super() / cref  */
    struct korb_frame frame2 = {
        .prev = c->current_frame,
        .caller_node = NULL,
        .method = m,
        .self = recv,
        .fp = c->fp,
        .locals_cnt = m->u.ast.locals_cnt,
    };
    c->current_frame = &frame2;
    VALUE r = EVAL(c, m->u.ast.body);
    c->current_frame = frame2.prev;
    c->fp = prev_fp;
    c->self = prev_self;
    c->cref = prev_cref2;
    if (c->state == KORB_RETURN) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

VALUE korb_funcall(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv) {
    return korb_dispatch_binop(c, recv, mid, argc, argv);
}

/* ---- runtime init ---- */

static void init_well_known_ids(void) {
    id_initialize = korb_intern("initialize");
    id_to_s = korb_intern("to_s");
    id_inspect = korb_intern("inspect");
    id_call = korb_intern("call");
    id_each = korb_intern("each");
    id_new = korb_intern("new");
    id_op_plus  = korb_intern("+");
    id_op_minus = korb_intern("-");
    id_op_mul   = korb_intern("*");
    id_op_div   = korb_intern("/");
    id_op_mod   = korb_intern("%");
    id_op_eq    = korb_intern("==");
    id_op_neq   = korb_intern("!=");
    id_op_lt    = korb_intern("<");
    id_op_le    = korb_intern("<=");
    id_op_gt    = korb_intern(">");
    id_op_ge    = korb_intern(">=");
    id_op_aref  = korb_intern("[]");
    id_op_aset  = korb_intern("[]=");
    id_op_lshift= korb_intern("<<");
    id_op_rshift= korb_intern(">>");
    id_op_and   = korb_intern("&");
    id_op_or    = korb_intern("|");
    id_op_xor   = korb_intern("^");
}

void korb_init_builtins(void); /* defined in builtins.c */

void korb_runtime_init(void) {
    GC_INIT();
    init_well_known_ids();

    korb_vm = korb_xmalloc(sizeof(*korb_vm));
    memset(korb_vm, 0, sizeof(*korb_vm));
    korb_vm->method_serial = 1;

    /* bootstrap classes (forward refs) */
    struct korb_class *cObject = korb_class_new(korb_intern("Object"), NULL, T_OBJECT);
    struct korb_class *cClass  = korb_class_new(korb_intern("Class"), cObject, T_CLASS);
    struct korb_class *cModule = korb_class_new(korb_intern("Module"), cObject, T_MODULE);
    cObject->basic.klass = (VALUE)cClass;
    cClass->basic.klass  = (VALUE)cClass;
    cModule->basic.klass = (VALUE)cClass;

    korb_vm->object_class = cObject;
    korb_vm->class_class  = cClass;
    korb_vm->module_class = cModule;

    korb_vm->numeric_class = korb_class_new(korb_intern("Numeric"), cObject, T_OBJECT);
    korb_vm->integer_class = korb_class_new(korb_intern("Integer"), korb_vm->numeric_class, T_BIGNUM);
    korb_vm->float_class   = korb_class_new(korb_intern("Float"),   korb_vm->numeric_class, T_FLOAT);
    korb_vm->string_class  = korb_class_new(korb_intern("String"),  cObject, T_STRING);
    korb_vm->array_class   = korb_class_new(korb_intern("Array"),   cObject, T_ARRAY);
    korb_vm->hash_class    = korb_class_new(korb_intern("Hash"),    cObject, T_HASH);
    korb_vm->symbol_class  = korb_class_new(korb_intern("Symbol"),  cObject, T_SYMBOL);
    korb_vm->true_class    = korb_class_new(korb_intern("TrueClass"),  cObject, T_NONE);
    korb_vm->false_class   = korb_class_new(korb_intern("FalseClass"), cObject, T_NONE);
    korb_vm->nil_class     = korb_class_new(korb_intern("NilClass"),   cObject, T_NONE);
    korb_vm->proc_class    = korb_class_new(korb_intern("Proc"),       cObject, T_PROC);
    korb_vm->range_class   = korb_class_new(korb_intern("Range"),      cObject, T_RANGE);
    korb_vm->kernel_module = korb_module_new(korb_intern("Kernel"));
    korb_vm->comparable_module = korb_module_new(korb_intern("Comparable"));
    korb_vm->enumerable_module = korb_module_new(korb_intern("Enumerable"));

    /* register top-level constants */
    korb_const_set(cObject, korb_vm->object_class->name,  (VALUE)cObject);
    korb_const_set(cObject, korb_vm->class_class->name,   (VALUE)cClass);
    korb_const_set(cObject, korb_vm->module_class->name,  (VALUE)cModule);
    korb_const_set(cObject, korb_vm->integer_class->name, (VALUE)korb_vm->integer_class);
    korb_const_set(cObject, korb_vm->float_class->name,   (VALUE)korb_vm->float_class);
    korb_const_set(cObject, korb_vm->string_class->name,  (VALUE)korb_vm->string_class);
    korb_const_set(cObject, korb_vm->array_class->name,   (VALUE)korb_vm->array_class);
    korb_const_set(cObject, korb_vm->hash_class->name,    (VALUE)korb_vm->hash_class);
    korb_const_set(cObject, korb_vm->symbol_class->name,  (VALUE)korb_vm->symbol_class);
    korb_const_set(cObject, korb_vm->numeric_class->name, (VALUE)korb_vm->numeric_class);
    korb_const_set(cObject, korb_vm->range_class->name,   (VALUE)korb_vm->range_class);
    korb_const_set(cObject, korb_vm->proc_class->name,    (VALUE)korb_vm->proc_class);

    /* main object */
    korb_vm->main_obj_class = korb_class_new(korb_intern("Main"), cObject, T_OBJECT);
    korb_vm->main_obj = korb_object_new(korb_vm->main_obj_class);

    /* Exception class hierarchy.  We don't model the full chain — just add
     * common classes so `rescue StandardError`, etc., parse successfully. */
    struct korb_class *cException = korb_class_new(korb_intern("Exception"), cObject, T_DATA);
    korb_const_set(cObject, korb_intern("Exception"), (VALUE)cException);
    static const char *exc_classes[] = {
        "StandardError", "RuntimeError", "ArgumentError", "TypeError",
        "NameError", "NoMethodError", "IndexError", "KeyError",
        "RangeError", "FloatDomainError", "ZeroDivisionError",
        "IOError", "Errno", "NotImplementedError", "LoadError",
        "FrozenError", "StopIteration", "LocalJumpError", "SystemCallError",
        "ScriptError", "SyntaxError", NULL,
    };
    for (int i = 0; exc_classes[i]; i++) {
        struct korb_class *k = korb_class_new(korb_intern(exc_classes[i]), cException, T_DATA);
        korb_const_set(cObject, korb_intern(exc_classes[i]), (VALUE)k);
    }

    /* Common stub classes */
    static const char *stub_classes[] = {
        "Comparable", "Enumerable", "Numeric", NULL,
    };
    (void)stub_classes;

    korb_init_builtins();
}

/* ---- file load / eval ---- */

NODE *koruby_parse(const char *src, size_t len, const char *filename); /* parse.c */

char *korb_dirname(const char *path) {
    if (!path) return korb_xmalloc_atomic(2), NULL;
    const char *slash = strrchr(path, '/');
    if (!slash) {
        char *r = korb_xmalloc_atomic(2);
        r[0] = '.'; r[1] = 0;
        return r;
    }
    long len = slash - path;
    char *r = korb_xmalloc_atomic(len + 1);
    memcpy(r, path, len);
    r[len] = 0;
    return r;
}

char *korb_join_path(const char *dir, const char *name) {
    long dl = strlen(dir), nl = strlen(name);
    char *r = korb_xmalloc_atomic(dl + nl + 2);
    memcpy(r, dir, dl);
    r[dl] = '/';
    memcpy(r + dl + 1, name, nl);
    r[dl + nl + 1] = 0;
    return r;
}

bool korb_file_exists(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    fclose(fp);
    return true;
}

char *korb_resolve_relative(const char *current_file, const char *name) {
    /* Try dirname(current_file)/name and add .rb if missing */
    const char *dir = current_file ? korb_dirname(current_file) : ".";
    long nl = strlen(name);
    bool has_rb = nl >= 3 && strcmp(name + nl - 3, ".rb") == 0;
    char *base = korb_join_path(dir, name);
    if (!has_rb) {
        char *with = korb_xmalloc_atomic(strlen(base) + 4);
        sprintf(with, "%s.rb", base);
        if (korb_file_exists(with)) return with;
    }
    if (korb_file_exists(base)) return base;
    return NULL;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = korb_xmalloc_atomic(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = korb_xrealloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    fclose(fp);
    *out_len = len;
    return buf;
}

VALUE korb_eval_string(CTX *c, const char *src, size_t len, const char *filename) {
    NODE *ast = koruby_parse(src, len, filename ? filename : "(eval)");
    if (!ast) return Qnil;

    /* Save / push fresh top-level state for the loaded file */
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    struct korb_class *prev_class = c->current_class;
    struct korb_cref *prev_cref = c->cref;
    const char *prev_file = c->current_file;

    /* Top-level frame for the new file: stack just past current sp */
    c->fp = c->sp + 1;
    c->self = korb_vm->main_obj;
    c->current_class = korb_vm->object_class;

    /* Reset cref to [Object] for top-level execution */
    struct korb_cref top_cref = { .klass = korb_vm->object_class, .prev = NULL };
    c->cref = &top_cref;
    c->current_file = filename;

    OPTIMIZE(ast);
    VALUE r = EVAL(c, ast);

    c->fp = prev_fp;
    c->self = prev_self;
    c->current_class = prev_class;
    c->cref = prev_cref;
    c->current_file = prev_file;
    return r;
}

/* loaded file tracker (for require) */
static struct {
    char **paths;
    uint32_t size, capa;
} loaded_files;

static bool already_loaded(const char *path) {
    for (uint32_t i = 0; i < loaded_files.size; i++) {
        if (strcmp(loaded_files.paths[i], path) == 0) return true;
    }
    return false;
}

static void mark_loaded(const char *path) {
    if (loaded_files.size >= loaded_files.capa) {
        uint32_t nc = loaded_files.capa ? loaded_files.capa * 2 : 16;
        loaded_files.paths = korb_xrealloc(loaded_files.paths, nc * sizeof(char *));
        loaded_files.capa = nc;
    }
    char *cp = korb_xmalloc_atomic(strlen(path) + 1);
    strcpy(cp, path);
    loaded_files.paths[loaded_files.size++] = cp;
}

VALUE korb_load_file(CTX *c, const char *path) {
    if (already_loaded(path)) return Qfalse;
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        korb_raise(c, NULL, "no such file: %s", path);
        return Qnil;
    }
    mark_loaded(path);
    korb_eval_string(c, src, len, path);
    return Qtrue;
}
