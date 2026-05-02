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

#include <ucontext.h>

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

/* Used by undef_method / remove_method.  Walks the bucket chain and
 * unlinks the entry; method records themselves stay live (cached
 * elsewhere — caches will miss on the next lookup and fall through). */
void korb_method_table_remove(struct korb_method_table *mt, ID name) {
    if (!mt->buckets) return;
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    struct korb_method_table_entry **slot = &mt->buckets[b];
    while (*slot) {
        if ((*slot)->name == name) {
            struct korb_method_table_entry *gone = *slot;
            *slot = gone->next;
            mt->size--;
            return;
        }
        slot = &(*slot)->next;
    }
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
    k->includes = NULL;
    k->includes_cnt = 0;
    k->includes_capa = 0;
    k->prepends = NULL;
    k->prepends_cnt = 0;
    k->prepends_capa = 0;
    k->default_visibility = KORB_VIS_PUBLIC;
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

/* Walk the body's textual dump (oneline) and decide whether the method
 * needs the heavy frame setup: yield, super, block_given?, const access,
 * or any block-passing call.  Method bodies without these can run with
 * a slim prologue (no current_block / cref / current_frame churn).  We
 * scan the dump string instead of writing a generic AST walker — the
 * scan happens once per method definition. */
static bool korb_method_body_is_simple_frame(struct Node *body) {
    if (!body) return true;
    char *buf = NULL;
    size_t sz = 0;
    FILE *fp = open_memstream(&buf, &sz);
    if (!fp) return false;
    DUMP(fp, body, true);
    fclose(fp);
    bool ok =
        strstr(buf, "(node_yield ")             == NULL &&
        strstr(buf, "(node_super")              == NULL &&
        strstr(buf, "(node_method_call_block ") == NULL &&
        strstr(buf, "(node_func_call_block ")   == NULL &&
        strstr(buf, "(node_const_get ")         == NULL &&
        strstr(buf, "(node_const_set ")         == NULL &&
        strstr(buf, "(node_const_path_get ")    == NULL &&
        strstr(buf, "(node_raise ")             == NULL &&
        strstr(buf, "block_given?")             == NULL &&
        /* __method__ / __callee__ / caller need a real frame so they
         * can find the enclosing method; skip the slim path. */
        strstr(buf, "__method__")               == NULL &&
        strstr(buf, "__callee__")               == NULL &&
        strstr(buf, "caller")                   == NULL;
    free(buf);
    return ok;
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
    m->is_simple_frame = korb_method_body_is_simple_frame(body);
    m->visibility = klass ? klass->default_visibility : KORB_VIS_PUBLIC;
    m->u.ast.body = body;
    m->u.ast.required_params_cnt = required_params;
    m->u.ast.total_params_cnt = total_params;
    m->u.ast.rest_slot = rest_slot;
    m->u.ast.block_slot = -1;
    m->u.ast.locals_cnt = locals_cnt;
    m->u.ast.post_params_cnt = 0;
    m->u.ast.kwh_save_slot = -1;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
}

/* Set the &blk parameter slot on the most-recently-added AST method
 * for `klass::name`.  Called from node_def_full when the def's
 * parameter list has a block parameter. */
void korb_class_set_method_block_slot(struct korb_class *klass, ID name, int slot) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.block_slot = slot;
}

void korb_class_set_method_post_params_cnt(struct korb_class *klass, ID name, uint32_t cnt) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.post_params_cnt = cnt;
}

void korb_class_set_method_kwh_save_slot(struct korb_class *klass, ID name, int slot) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.kwh_save_slot = slot;
}

void korb_class_add_method_cfunc(struct korb_class *klass, ID name,
                               VALUE (*func)(CTX *, VALUE, int, VALUE *), int argc) {
    struct korb_method *m = korb_xmalloc(sizeof(*m));
    m->type = KORB_METHOD_CFUNC;
    m->name = name;
    m->defining_class = klass;
    m->is_simple_frame = false;
    m->visibility = KORB_VIS_PUBLIC;
    m->u.cfunc.func = func;
    m->u.cfunc.argc = argc;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
}

/* Register a proc-bodied method (used by Module#define_method).
 *
 * Closure capture: the block's env field originally points into the
 * defining method's stack frame.  Once that method returns, those slots
 * get reused by the next call.  Snapshot the env onto the heap so the
 * closure values survive — this matches how Proc#call's env-snapshot
 * works, but baked at registration time.  (Live binding semantics —
 * where a later mutation of the defining method's lvar would be seen
 * — would require keeping the live fp pointer; not worth it for
 * define_method.)  */
void korb_class_add_method_proc(struct korb_class *klass, ID name, struct korb_proc *p) {
    struct korb_proc *snap = korb_xmalloc(sizeof(*snap));
    *snap = *p;
    if (p->env_size > 0 && p->env) {
        snap->env = korb_xmalloc(p->env_size * sizeof(VALUE));
        for (uint32_t i = 0; i < p->env_size; i++) snap->env[i] = p->env[i];
    } else {
        snap->env = NULL;
    }
    struct korb_method *m = korb_xmalloc(sizeof(*m));
    m->type = KORB_METHOD_PROC;
    m->name = name;
    m->defining_class = klass;
    m->is_simple_frame = false;
    m->visibility = klass ? klass->default_visibility : KORB_VIS_PUBLIC;
    m->u.proc.proc = snap;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
}

/* Register an existing method object under a new name on `klass`.
 * Both `alias` (keyword) and `Module#alias_method` lower to this. */
void korb_class_alias_method(struct korb_class *klass, ID new_name, struct korb_method *m) {
    method_table_set(&klass->methods, new_name, m);
    korb_check_basic_op_redef(klass, new_name);
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
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

/* Singleton class for an arbitrary value.  Same idea as
 * `korb_singleton_class_of` but works on T_OBJECT instances too —
 * lazily allocates a fresh class whose super = current class, then
 * rewires basic.klass.  Returns NULL for immediate values. */
struct korb_class *korb_singleton_class_of_value(VALUE v) {
    if (SPECIAL_CONST_P(v)) return NULL;
    if (BUILTIN_TYPE(v) == T_CLASS || BUILTIN_TYPE(v) == T_MODULE) {
        return korb_singleton_class_of((struct korb_class *)v);
    }
    /* Generic heap object: rewire klass to a private subclass. */
    struct korb_object *o = (struct korb_object *)v;
    struct korb_class *cur = (struct korb_class *)o->basic.klass;
    if (cur && cur->name == korb_intern("(singleton)")) return cur;
    struct korb_class *meta = korb_class_new(korb_intern("(singleton)"),
                                             cur, cur ? cur->instance_type : T_OBJECT);
    o->basic.klass = (VALUE)meta;
    return meta;
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
    /* Record the include for ancestors / is_a?.  Skip duplicates. */
    for (uint32_t i = 0; i < klass->includes_cnt; i++) {
        if (klass->includes[i] == mod) return;
    }
    if (klass->includes_cnt >= klass->includes_capa) {
        uint32_t nc = klass->includes_capa ? klass->includes_capa * 2 : 4;
        klass->includes = korb_xrealloc(klass->includes, nc * sizeof(*klass->includes));
        klass->includes_capa = nc;
    }
    klass->includes[klass->includes_cnt++] = mod;
}

struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name) {
    while (klass) {
        /* prepended modules win over the class's own methods.  Walk in
         * reverse — the most recently prepended module dispatches first. */
        for (int32_t i = (int32_t)klass->prepends_cnt - 1; i >= 0; i--) {
            struct korb_method *m = method_table_get(&klass->prepends[i]->methods, name);
            if (m) return m;
        }
        struct korb_method *m = method_table_get(&klass->methods, name);
        if (m) return m;
        klass = klass->super;
    }
    return NULL;
}

/* Find the next method in receiver's class MRO after `defining_class`.
 * Used for `super`: receiver_klass = class of `c->self`,
 * defining_class = current method's defining_class.
 *
 * MRO order at each class level: prepends (last-first), class itself,
 * then super class (recursively).  Includes are flattened into the
 * class's own table, so they aren't a separate MRO step here. */
struct korb_method *korb_class_find_super_method(const struct korb_class *receiver_klass,
                                                 const struct korb_class *defining_class,
                                                 ID name) {
    bool past = false;
    const struct korb_class *k = receiver_klass;
    while (k) {
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            const struct korb_class *p = k->prepends[i];
            if (past) {
                struct korb_method *m = method_table_get(&p->methods, name);
                if (m) return m;
            } else if (p == defining_class) {
                past = true;
            }
        }
        if (past) {
            struct korb_method *m = method_table_get(&k->methods, name);
            if (m) return m;
        } else if (k == defining_class) {
            past = true;
        }
        k = k->super;
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
    /* Preallocate ivar slots based on the class's known ivar shape, so
     * the inline ivar_set_ic fast path hits on the first write to each
     * @ivar (otherwise every fresh object pays korb_ivar_set_ic_slow
     * twice through initialize, which showed up as 6% on bm_object). */
    uint32_t n = klass->ivar_count;
    if (n) {
        o->ivar_cnt = n;
        o->ivar_capa = n;
        o->ivars = korb_xmalloc(n * sizeof(VALUE));
        for (uint32_t i = 0; i < n; i++) o->ivars[i] = Qnil;
    } else {
        o->ivar_cnt = 0;
        o->ivar_capa = 0;
        o->ivars = NULL;
    }
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

/* Out-of-line slow path for the inline korb_ivar_get_ic (object.h).
 * Reached on cache miss (different class or unset slot) or non-T_OBJECT
 * receiver. */
VALUE korb_ivar_get_ic_slow(VALUE obj, ID name, struct ivar_cache *cache) {
    if (SPECIAL_CONST_P(obj)) return Qnil;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return Qnil;
    struct korb_object *o = (struct korb_object *)obj;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    int s = ivar_slot(k, name);
    if (s >= 0) { cache->klass = k; cache->slot = s; }
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

/* Cached ivar set: same as get but with assign-on-miss semantics. */
/* Out-of-line slow path for the inline korb_ivar_set_ic in object.h.
 * Reached on cache miss (different class or unset slot), or when the
 * slot is past current ivar_capa / ivar_cnt and needs growth. */
void korb_ivar_set_ic_slow(VALUE obj, ID name, VALUE val, struct ivar_cache *cache) {
    if (SPECIAL_CONST_P(obj)) return;
    if (BUILTIN_TYPE(obj) != T_OBJECT) return;
    struct korb_object *o = (struct korb_object *)obj;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    int s;
    if (cache->klass == k && cache->slot >= 0) {
        s = cache->slot;
    } else {
        s = ivar_slot_assign(k, name);
        cache->klass = k;
        cache->slot = s;
    }
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

/* korb_ary_len, korb_ary_aref: now static inline in object.h. */

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
    /* Arrays hash by content so `h[[1, 2]]` works on a fresh literal. */
    if (BUILTIN_TYPE(v) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)v;
        uint64_t h = 0xcbf29ce484222325ULL;
        for (long i = 0; i < a->len; i++) {
            uint64_t eh = korb_hash_value(a->ptr[i]);
            h ^= eh;
            h *= 0x100000001b3ULL;
        }
        return h;
    }
    /* Custom class with user-defined #hash — call it through current
     * CTX (single-threaded global).  Falls back to identity if no
     * CTX is available (boot phase) or if the user method's result
     * isn't a Fixnum. */
    if (BUILTIN_TYPE(v) == T_OBJECT && korb_vm && korb_vm->current_ctx) {
        struct korb_class *k = korb_class_of_class(v);
        struct korb_method *m = korb_class_find_method(k, korb_intern("hash"));
        /* Skip Object#hash (our default identity-based version) — it
         * lives on the Object class.  Otherwise we'd recurse forever. */
        if (m && m->defining_class != korb_vm->object_class &&
            m->defining_class != korb_vm->kernel_module) {
            VALUE r = korb_funcall(korb_vm->current_ctx, v, korb_intern("hash"), 0, NULL);
            if (FIXNUM_P(r)) return (uint64_t)FIX2LONG(r) * 11400714819323198485ULL;
        }
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
    /* Arrays compared by content (eql? element-wise via ==).  Match
     * Hash's equality semantics so an Array key looks the same on
     * lookup as on insert. */
    if (BUILTIN_TYPE(a) == T_ARRAY && BUILTIN_TYPE(b) == T_ARRAY) {
        struct korb_array *x = (struct korb_array *)a;
        struct korb_array *y = (struct korb_array *)b;
        if (x->len != y->len) return false;
        for (long i = 0; i < x->len; i++) {
            if (!korb_eql(x->ptr[i], y->ptr[i])) return false;
        }
        return true;
    }
    /* Custom class with user-defined eql? — invoke it through
     * current CTX, falling back to identity if not overridden. */
    if (BUILTIN_TYPE(a) == T_OBJECT && korb_vm && korb_vm->current_ctx) {
        struct korb_class *k = korb_class_of_class(a);
        struct korb_method *m = korb_class_find_method(k, korb_intern("eql?"));
        if (m && m->defining_class != korb_vm->object_class &&
            m->defining_class != korb_vm->kernel_module) {
            VALUE arg = b;
            VALUE r = korb_funcall(korb_vm->current_ctx, a, korb_intern("eql?"), 1, &arg);
            return RTEST(r);
        }
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
    h->default_proc  = Qnil;
    h->compare_by_identity = false;
    return (VALUE)h;
}

static inline uint64_t korb_hash_key(const struct korb_hash *h, VALUE key) {
    return h->compare_by_identity ? (uint64_t)key : korb_hash_value(key);
}

static inline bool korb_hash_keys_match(const struct korb_hash *h, VALUE a, VALUE b) {
    return h->compare_by_identity ? (a == b) : korb_eql(a, b);
}

static void korb_hash_resize(struct korb_hash *h, uint32_t nc) {
    struct korb_hash_entry **newbk = korb_xcalloc(nc, sizeof(*newbk));
    /* re-insert each entry into the new bucket array via its bucket_next chain. */
    for (struct korb_hash_entry *e = h->first; e; e = e->next) {
        uint32_t b = (uint32_t)(e->hash % nc);
        e->bucket_next = newbk[b];
        newbk[b] = e;
    }
    h->buckets = newbk;
    h->bucket_cnt = nc;
}

VALUE korb_hash_aset(VALUE hv, VALUE key, VALUE val) {
    struct korb_hash *h = (struct korb_hash *)hv;
    uint64_t hh = korb_hash_key(h, key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    /* search existing within this bucket only — proper chained hash */
    for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
        if (e->hash == hh && korb_hash_keys_match(h, e->key, key)) {
            e->value = val;
            return val;
        }
    }
    struct korb_hash_entry *e = korb_xmalloc(sizeof(*e));
    e->key = key;
    e->value = val;
    e->hash = hh;
    e->next = NULL;
    e->bucket_next = h->buckets[b];
    h->buckets[b] = e;
    if (!h->first) h->first = e;
    else h->last->next = e;
    h->last = e;
    h->size++;
    /* grow when load factor passes 0.75 (or always above ~75% capacity) */
    if (h->size * 4 > h->bucket_cnt * 3) {
        korb_hash_resize(h, h->bucket_cnt * 2);
    }
    return val;
}

/* The inline fast path lives in object.h.  This handles all the
 * cases the inline can't (T_STRING keys, compare_by_identity tables). */
VALUE korb_hash_aref_slow(VALUE hv, VALUE key) {
    struct korb_hash *h = (struct korb_hash *)hv;
    uint64_t hh = korb_hash_key(h, key);
    uint32_t b = (uint32_t)(hh % h->bucket_cnt);
    for (struct korb_hash_entry *e = h->buckets[b]; e; e = e->bucket_next) {
        if (e->hash == hh && korb_hash_keys_match(h, e->key, key))
            return e->value;
    }
    /* Miss: return default_value.  default_proc is handled by Hash#[]
     * in builtins/hash.c which has a CTX to invoke the proc. */
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
/* The FLONUM-encode fast path lives inline in object.h.  This is the
 * heap-allocate fallback used when the double doesn't fit FLONUM
 * (NaN/Inf/0/denorm/very large/very small). */
VALUE korb_float_new_heap(double d) {
    struct korb_float *f = korb_xmalloc(sizeof(*f));
    f->basic.flags = T_FLOAT;
    f->basic.klass = korb_vm ? (VALUE)korb_vm->float_class : 0;
    f->value = d;
    return (VALUE)f;
}

/* Slow tail of korb_num2dbl (heap T_FLOAT, T_BIGNUM).  FLONUM and
 * FIXNUM are handled inline in object.h. */
double korb_num2dbl_slow(VALUE v) {
    if (KORB_IS_FLOAT(v)) return ((struct korb_float *)v)->value;
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
/* Snapshot a proc's env into a heap array if it currently points into
 * the about-to-be-deallocated stack frame `fp`.  Called from method
 * prologue exits: when a method returns a Proc whose env is the
 * caller's stack, we have to make a copy or the next stack push will
 * clobber the captured state.  Hot path is no-op (proc not in fp's
 * range), so cheap to call per return. */
void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi) {
    if (SPECIAL_CONST_P(v) || BUILTIN_TYPE(v) != T_PROC) return;
    struct korb_proc *p = (struct korb_proc *)v;
    if (!p->env) return;
    if (p->env < fp_lo || p->env >= fp_hi) return;
    VALUE *snap = korb_xmalloc(p->env_size * sizeof(VALUE));
    for (uint32_t i = 0; i < p->env_size; i++) snap[i] = p->env[i];
    p->env = snap;
}

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
    p->rest_slot = -1;
    p->kwh_save_slot = -1;
    p->enclosing_block = current_block;  /* capture enclosing-method's block */
    p->self = self;
    p->is_lambda = is_lambda;
    p->creates_proc = false;
    return (VALUE)p;
}

/* Block currently active for yield (set by dispatch_call). */
struct korb_proc *current_block = NULL;

bool korb_block_given(void) { return current_block != NULL; }

/* The single-arg/single-param fast path is inlined in object.h
 * (korb_yield).  This handles every other shape — auto-destructure,
 * argc/params mismatch, etc. */
VALUE korb_yield_slow(CTX *c, struct korb_proc *blk, uint32_t argc, VALUE *argv) {
    /* Symbol-proc shim (`&:method`): dispatch as argv[0].send(symbol, *rest). */
    if (blk->body == NULL && SYMBOL_P(blk->self)) {
        if (argc < 1) {
            korb_raise(c, NULL, "no receiver for symbol proc");
            return Qnil;
        }
        ID name = korb_sym2id(blk->self);
        return korb_funcall(c, argv[0], name, argc - 1, argv + 1);
    }
    /* Method-proc shim (`&obj.method(:m)`): dispatch as receiver.send(name, *args). */
    if (blk->body == NULL && !SPECIAL_CONST_P(blk->self) &&
        BUILTIN_TYPE(blk->self) == T_DATA &&
        ((struct RBasic *)blk->self)->klass == (VALUE)korb_vm->method_class) {
        struct korb_method_obj { struct RBasic basic; VALUE receiver; ID name; };
        struct korb_method_obj *mo = (struct korb_method_obj *)blk->self;
        return korb_funcall(c, mo->receiver, mo->name, argc, argv);
    }
    /* Shared-fp closure: block evaluates with env_fp's view of locals.
     * IMPORTANT: argv may point into the YIELDER's fp (e.g., a slot inside
     * the calling method's frame) and we're about to overwrite that slot
     * via blk->env[param_base + i] (which IS the caller's fp — same memory
     * if blk's outer is the yielder's caller).  Snapshot args first. */
    VALUE saved_args[16];  /* fast path for common case */
    VALUE *args_buf = saved_args;
    if (argc > 16) args_buf = korb_xmalloc(sizeof(VALUE) * argc);
    for (uint32_t i = 0; i < argc; i++) args_buf[i] = argv[i];

    /* Per-iteration capture path: when the block creates inner procs
     * (`(1..3).each { |i| procs << proc { i } }`), allocate a fresh env
     * for THIS yield's block-locals so the captured proc sees its own
     * `i`.  Outer slots are aliased via copy-in / copy-back.  Non-
     * creates_proc blocks share env directly (faster, and matches
     * shared-state semantics for `count += 1`-style accumulators). */
    bool fresh_env_path = blk->creates_proc;
    VALUE *fp;
    VALUE *outer_env_ptr = blk->env;
    if (fresh_env_path) {
        fp = (VALUE *)korb_xmalloc(blk->env_size * sizeof(VALUE));
        /* Copy ALL of env: outer slots so depth-walks/reads see their
         * current values; block-local slots are about to be overwritten
         * by params/destructure anyway. */
        for (uint32_t i = 0; i < blk->env_size; i++) fp[i] = blk->env[i];
    } else {
        fp = blk->env;
    }
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    /* Auto-destructure: block with N params yielded a single Array of size M
     * → assign array elements to params (Ruby block calling convention). */
    if (blk->params_cnt > 1 && argc == 1 &&
        !SPECIAL_CONST_P(args_buf[0]) && BUILTIN_TYPE(args_buf[0]) == T_ARRAY) {
        struct korb_array *a = (struct korb_array *)args_buf[0];
        for (uint32_t i = 0; i < blk->params_cnt; i++) {
            fp[blk->param_base + i] = (i < (uint32_t)a->len) ? a->ptr[i] : Qnil;
        }
    }
    /* Auto-pack: 1-param non-lambda block yielded with N>1 args — Ruby
     * packs them into an Array so `|x|` sees `[a, b, ...]`.  Hash#each
     * { |(k, v)| ... } and { |x| ... } both rely on this. */
    else if (blk->params_cnt == 1 && argc > 1 && !blk->is_lambda) {
        VALUE pack = korb_ary_new_capa((long)argc);
        for (uint32_t i = 0; i < argc; i++) korb_ary_push(pack, args_buf[i]);
        fp[blk->param_base] = pack;
    } else {
        for (uint32_t i = 0; i < blk->params_cnt && i < argc; i++) {
            fp[blk->param_base + i] = args_buf[i];
        }
        /* fill missing params with nil */
        for (uint32_t i = (argc < blk->params_cnt ? argc : blk->params_cnt); i < blk->params_cnt; i++) {
            fp[blk->param_base + i] = Qnil;
        }
    }
    c->self = blk->self;
    /* Switch fp so block body's lvar_get/set hit the captured frame's slots. */
    c->fp = fp;
    /* Lexical block target: yield inside block body refers to the
     * enclosing method's block, not back to this block. */
    struct korb_proc *prev_block = current_block;
    current_block = blk->enclosing_block;
    VALUE r;
redo_block:
    r = EVAL(c, blk->body);
    /* `redo` inside the block: re-evaluate the block body with the
     * same args (params keep their current bindings). */
    if (c->state == KORB_REDO) {
        c->state = KORB_NORMAL; c->state_value = Qnil;
        goto redo_block;
    }
    /* Copy outer-slot writes back to the shared env so updates like
     * `count += 1` propagate.  Only outer slots — block-local slots
     * stay in `fp` (the fresh env), captured by any procs created
     * during this iteration. */
    if (fresh_env_path) {
        for (uint32_t i = 0; i < blk->param_base; i++) outer_env_ptr[i] = fp[i];
    }
    c->fp = prev_fp;
    c->self = prev_self;
    current_block = prev_block;
    /* `next` inside a block: yield returns the next value, state cleared.
     * `break` should NOT be cleared here — it propagates to the yielding
     * method, where dispatch_call catches it as that method's return. */
    if (c->state == KORB_NEXT) {
        VALUE nv = c->state_value;
        c->state = KORB_NORMAL; c->state_value = Qnil;
        return nv;
    }
    return r;
}

/* ---- class lookup ---- */
/* Heap-object fast path is inline in object.h.  This handles the
 * immediate values: only reached when SPECIAL_CONST_P(v) is true. */
struct korb_class *korb_class_of_class_slow(VALUE v) {
    if (FIXNUM_P(v)) return korb_vm->integer_class;
    if (FLONUM_P(v)) return korb_vm->float_class;
    if (SYMBOL_P(v)) return korb_vm->symbol_class;
    if (NIL_P(v))   return korb_vm->nil_class;
    if (TRUE_P(v))  return korb_vm->true_class;
    if (FALSE_P(v)) return korb_vm->false_class;
    /* shouldn't happen for true SPECIAL_CONST_P, but be safe */
    return (struct korb_class *)((struct RBasic *)v)->klass;
}

VALUE korb_class_of(VALUE v) { return (VALUE)korb_class_of_class(v); }

/* ---- exceptions ----
 * Exception is a T_OBJECT — its message lives in the @message ivar
 * just like any other Ruby object.  This keeps user code (`e.message`,
 * `e.instance_variable_get(:@message)`) consistent with Exception.new
 * created instances and with raise-built ones. */
/* Build a backtrace array by walking c->current_frame.
 * Format: "FILE:LINE:in `METHOD'"  (matches CRuby).
 *
 * The first entry is for the raise site itself: we use raise_line if
 * non-zero, falling back to caller_node's line.  Subsequent entries
 * use each frame's caller_node->head.line. */
VALUE korb_build_backtrace(CTX *c, int raise_line) {
    VALUE arr = korb_ary_new();
    const char *default_file = c->current_file ? c->current_file : "(unknown)";
    char buf[512];
    struct korb_frame *f = c->current_frame;
    bool first = true;
    while (f) {
        const char *name = (f->method && f->method->name)
                             ? korb_id_name(f->method->name) : "<main>";
        /* Prefer the method body's source_file (set when the def was
         * parsed) so methods from required files report their own
         * file, not the entry-point's. */
        const char *file = default_file;
        if (f->method && f->method->type == KORB_METHOD_AST &&
            f->method->u.ast.body && f->method->u.ast.body->head.source_file) {
            file = f->method->u.ast.body->head.source_file;
        }
        int line;
        if (first) {
            line = raise_line;
            if (line == 0 && f->caller_node) line = f->caller_node->head.line;
            first = false;
        } else {
            line = f->caller_node ? f->caller_node->head.line : 0;
        }
        snprintf(buf, sizeof(buf), "%s:%d:in `%s'", file, line, name);
        korb_ary_push(arr, korb_str_new_cstr(buf));
        f = f->prev;
    }
    /* Always include a top-level entry so the trace is non-empty even
     * for raises from main. */
    snprintf(buf, sizeof(buf), "%s:%d:in `<main>'", default_file, raise_line);
    if (((struct korb_array *)arr)->len == 0) {
        korb_ary_push(arr, korb_str_new_cstr(buf));
    }
    return arr;
}

void korb_exc_set_backtrace(CTX *c, VALUE exc, int raise_line) {
    if (SPECIAL_CONST_P(exc)) return;
    ID id = korb_intern("@__backtrace__");
    VALUE existing = korb_ivar_get(exc, id);
    if (!UNDEF_P(existing) && !NIL_P(existing)) return;
    korb_ivar_set(exc, id, korb_build_backtrace(c, raise_line));
}

VALUE korb_exc_new(struct korb_class *klass, const char *msg) {
    if (!klass) {
        VALUE eRuntime = korb_const_get(korb_vm->object_class,
                                        korb_intern("RuntimeError"));
        if (eRuntime && !SPECIAL_CONST_P(eRuntime) &&
            BUILTIN_TYPE(eRuntime) == T_CLASS) {
            klass = (struct korb_class *)eRuntime;
        } else {
            klass = korb_vm->object_class;
        }
    }
    VALUE obj = korb_object_new(klass);
    if (msg) {
        korb_ivar_set(obj, korb_intern("@message"), korb_str_new_cstr(msg));
    }
    return obj;
}

void korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE e = korb_exc_new(klass, buf);
    int line = (c->last_cfunc_callsite ? c->last_cfunc_callsite->head.line : 0);
    korb_exc_set_backtrace(c, e, line);
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
    if (FLONUM_P(v)) {
        char b[64]; snprintf(b, 64, "%.17g", korb_flonum_to_double(v));
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
        /* Escape control chars so inspect output is re-evalable. */
        struct korb_string *s = (struct korb_string *)v;
        VALUE r = korb_str_new_cstr("\"");
        long start = 0;
        for (long i = 0; i < s->len; i++) {
            unsigned char ch = (unsigned char)s->ptr[i];
            const char *esc = NULL;
            char buf[8];
            switch (ch) {
                case '\\': esc = "\\\\"; break;
                case '"':  esc = "\\\""; break;
                case '\n': esc = "\\n";  break;
                case '\t': esc = "\\t";  break;
                case '\r': esc = "\\r";  break;
                case '\0': esc = "\\0";  break;
                case '\a': esc = "\\a";  break;
                case '\b': esc = "\\b";  break;
                case '\f': esc = "\\f";  break;
                case '\v': esc = "\\v";  break;
                case '\x1b': esc = "\\e"; break;
                default:
                    if (ch < 0x20 || ch == 0x7f) {
                        snprintf(buf, sizeof(buf), "\\x%02X", ch);
                        esc = buf;
                    }
                    break;
            }
            if (esc) {
                if (i > start) korb_str_concat(r, korb_str_new(s->ptr + start, i - start));
                korb_str_concat(r, korb_str_new_cstr(esc));
                start = i + 1;
            }
        }
        if (start < s->len) korb_str_concat(r, korb_str_new(s->ptr + start, s->len - start));
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
        struct korb_class *k = (struct korb_class *)((struct korb_object *)v)->basic.klass;
        /* If the class defines its own #inspect, delegate (so nested
         * Rational / Complex / user-class inside Array/Hash render
         * via their inspect rather than the default `#<Cls:0x...>`). */
        if (k && korb_vm && korb_vm->current_ctx) {
            struct korb_method *m = korb_class_find_method(k, korb_intern("inspect"));
            if (m && m->type == KORB_METHOD_AST) {
                VALUE r = korb_funcall(korb_vm->current_ctx, v,
                                       korb_intern("inspect"), 0, NULL);
                if (BUILTIN_TYPE(r) == T_STRING) return r;
            }
        }
        VALUE msg = korb_ivar_get(v, korb_intern("@message"));
        if (msg && !UNDEF_P(msg) && !SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) == T_STRING) {
            /* Exception-shaped: "#<ClassName: message>" */
            VALUE r = korb_str_new_cstr("#<");
            korb_str_concat(r, korb_str_new_cstr(k && k->name ? korb_id_name(k->name) : "Object"));
            korb_str_concat(r, korb_str_new_cstr(": "));
            korb_str_concat(r, msg);
            korb_str_concat(r, korb_str_new_cstr(">"));
            return r;
        }
        char b[64];
        snprintf(b, 64, "#<%s:%p>", k && k->name ? korb_id_name(k->name) : "Object", (void *)v);
        return korb_str_new_cstr(b);
    }
    if (t == T_DATA) {
        return korb_str_new_cstr("#<data>");
    }
    if (t == T_PROC) return korb_str_new_cstr("#<Proc>");
    if (t == T_SYMBOL) return korb_str_new_cstr(":?");
    return korb_str_new_cstr("#<?>");
}

VALUE korb_inspect(VALUE v) { return korb_inspect_inner(v, 0); }

/* CTX-aware inspect — dispatches a user-defined inspect if the
 * receiver's class has one (e.g., Rational defines `def inspect;
 * "(num/den)"; end`).  Used by kernel_p and #inspect-from-Ruby
 * so user objects render via their own inspect rather than the
 * default `#<Class:0x...>` form. */
VALUE korb_inspect_dispatch(CTX *c, VALUE v) {
    if (!c) return korb_inspect(v);
    if (!SPECIAL_CONST_P(v)) {
        struct korb_class *klass = korb_class_of_class(v);
        struct korb_method *m = korb_class_find_method(klass, korb_intern("inspect"));
        /* Skip the inherited Kernel#inspect cfunc (which would just
         * loop back here); only redirect when the user actually
         * overrode it as an AST method. */
        if (m && m->type == KORB_METHOD_AST) {
            VALUE r = korb_funcall(c, v, korb_intern("inspect"), 0, NULL);
            if (BUILTIN_TYPE(r) == T_STRING) return r;
        }
    }
    return korb_inspect(v);
}

/* CTX-aware to_s — dispatches a user-defined to_s if the receiver's
 * class has one (e.g., a class with `def to_s; "..."; end`).  Used
 * by kernel_puts / kernel_print so user objects render via their
 * own to_s instead of the default `#<Class:0x...>` inspect. */
VALUE korb_to_s_dispatch(CTX *c, VALUE v) {
    if (BUILTIN_TYPE(v) == T_STRING) return v;
    if (!SPECIAL_CONST_P(v)) {
        struct korb_class *klass = korb_class_of_class(v);
        struct korb_method *m = korb_class_find_method(klass, korb_intern("to_s"));
        if (m && m->type == KORB_METHOD_AST) {
            VALUE r = korb_funcall(c, v, korb_intern("to_s"), 0, NULL);
            /* If user's to_s returned something other than a String,
             * fall through to the default rendering rather than
             * crash inside korb_str_concat. */
            if (BUILTIN_TYPE(r) == T_STRING) return r;
        }
    }
    return korb_to_s(v);
}

VALUE korb_to_s(VALUE v) {
    if (BUILTIN_TYPE(v) == T_STRING) return v;
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return korb_str_new_cstr(b);
    }
    if (NIL_P(v)) return korb_str_new_cstr("");
    if (SYMBOL_P(v)) return korb_str_new_cstr(korb_id_name(korb_sym2id(v)));
    if (BUILTIN_TYPE(v) == T_OBJECT) {
        /* Exception-like: prefer @message ivar if it's a String. */
        VALUE msg = korb_ivar_get(v, korb_intern("@message"));
        if (msg && !SPECIAL_CONST_P(msg) && BUILTIN_TYPE(msg) == T_STRING) return msg;
    }
    return korb_inspect(v);
}

void korb_p(VALUE v) {
    VALUE s = korb_inspect(v);
    fwrite(((struct korb_string *)s)->ptr, 1, ((struct korb_string *)s)->len, stdout);
    fputc('\n', stdout);
}

bool korb_eq(VALUE a, VALUE b) {
    /* Identity is normally enough — *except* for NaN (which is never
     * equal to anything, including itself).  Heap T_FLOAT might be NaN,
     * so fall through to numeric compare for that case. */
    if (a == b) {
        if (UNLIKELY(!FIXNUM_P(a) && !FLONUM_P(a) && !SPECIAL_CONST_P(a) &&
                     BUILTIN_TYPE(a) == T_FLOAT)) {
            double x = ((struct korb_float *)a)->value;
            return x == x; /* false only for NaN */
        }
        return true;
    }
    if (FIXNUM_P(a) || FIXNUM_P(b)) {
        if (FIXNUM_P(a) && FIXNUM_P(b)) return a == b;
        if (FIXNUM_P(a) && BUILTIN_TYPE(b) == T_BIGNUM) return korb_int_eq(a, b);
        if (FIXNUM_P(b) && BUILTIN_TYPE(a) == T_BIGNUM) return korb_int_eq(a, b);
        if (FIXNUM_P(a) && (FLONUM_P(b) || KORB_IS_FLOAT(b)))
            return (double)FIX2LONG(a) == korb_num2dbl(b);
        if (FIXNUM_P(b) && (FLONUM_P(a) || KORB_IS_FLOAT(a)))
            return korb_num2dbl(a) == (double)FIX2LONG(b);
        return false;
    }
    if (FLONUM_P(a) || FLONUM_P(b)) {
        if ((FLONUM_P(a) || KORB_IS_FLOAT(a)) &&
            (FLONUM_P(b) || KORB_IS_FLOAT(b)))
            return korb_num2dbl(a) == korb_num2dbl(b);
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
    if (KORB_IS_FLOAT(a) && KORB_IS_FLOAT(b)) return korb_num2dbl(a) == korb_num2dbl(b);
    if (ta == T_ARRAY && tb == T_ARRAY) {
        struct korb_array *ax = (struct korb_array *)a;
        struct korb_array *bx = (struct korb_array *)b;
        if (ax->len != bx->len) return false;
        for (long i = 0; i < ax->len; i++) {
            if (!korb_eq(ax->ptr[i], bx->ptr[i])) return false;
        }
        return true;
    }
    if (ta == T_HASH && tb == T_HASH) {
        struct korb_hash *ah = (struct korb_hash *)a;
        struct korb_hash *bh = (struct korb_hash *)b;
        if (ah->size != bh->size) return false;
        for (struct korb_hash_entry *e = ah->first; e; e = e->next) {
            VALUE bv = korb_hash_aref(b, e->key);
            if (!korb_eq(e->value, bv)) return false;
        }
        return true;
    }
    if (ta == T_RANGE && tb == T_RANGE) {
        struct korb_range *ar = (struct korb_range *)a;
        struct korb_range *br = (struct korb_range *)b;
        return ar->exclude_end == br->exclude_end &&
               korb_eq(ar->begin, br->begin) && korb_eq(ar->end, br->end);
    }
    return false;
}

/* Mirrored copy of korb_vm->method_serial — kept in sync by every site that
 * bumps the master serial.  Allows the inline cache check in object.h to
 * read this directly without seeing struct korb_vm's full definition. */
state_serial_t korb_g_method_serial = 0;

/* Set to true once user code redefines a method on Integer / Float /
 * Array / Hash / String / Symbol — the receiver classes that EVAL_node_*
 * fast paths assume are unmodified.  Each fast path includes an
 * UNLIKELY check; if the flag flips, the path falls through to slow
 * dispatch.  Coarse-grained (any redefinition flips it forever), but
 * common-case correct and zero-cost on the normal path. */
bool korb_g_basic_op_redefined = false;

/* Called from node_def_full when a Ruby-level `def` lands.  Flip the
 * basic-op fast-path flag only when both the target class AND the
 * method name are ones we actually shortcut.  Earlier this fired for
 * any def on Integer / Float / etc., so bootstrap.rb's `class Integer;
 * def gcd; ...` permanently disabled the FIXNUM fast path even though
 * gcd has nothing to do with `+`. */
static bool korb_is_basic_op_id(ID name) {
    static ID id_plus, id_minus, id_mul, id_div, id_mod, id_pow;
    static ID id_lt, id_le, id_gt, id_ge, id_eq, id_ne, id_cmp;
    static ID id_aref, id_aset, id_lshift, id_rshift, id_band, id_bor, id_bxor;
    static bool init = false;
    if (!init) {
        id_plus  = korb_intern("+");  id_minus = korb_intern("-");
        id_mul   = korb_intern("*");  id_div   = korb_intern("/");
        id_mod   = korb_intern("%");  id_pow   = korb_intern("**");
        id_lt    = korb_intern("<");  id_le    = korb_intern("<=");
        id_gt    = korb_intern(">");  id_ge    = korb_intern(">=");
        id_eq    = korb_intern("=="); id_ne    = korb_intern("!=");
        id_cmp   = korb_intern("<=>");
        id_aref  = korb_intern("[]"); id_aset  = korb_intern("[]=");
        id_lshift= korb_intern("<<"); id_rshift= korb_intern(">>");
        id_band  = korb_intern("&");  id_bor   = korb_intern("|");
        id_bxor  = korb_intern("^");
        init = true;
    }
    return name == id_plus || name == id_minus || name == id_mul ||
           name == id_div  || name == id_mod   || name == id_pow ||
           name == id_lt   || name == id_le    || name == id_gt  ||
           name == id_ge   || name == id_eq    || name == id_ne  ||
           name == id_cmp  || name == id_aref  || name == id_aset ||
           name == id_lshift || name == id_rshift ||
           name == id_band || name == id_bor   || name == id_bxor;
}

void korb_check_basic_op_redef(struct korb_class *target, ID name) {
    if (!korb_vm) return;
    if (target != korb_vm->integer_class &&
        target != korb_vm->float_class   &&
        target != korb_vm->array_class   &&
        target != korb_vm->hash_class    &&
        target != korb_vm->string_class  &&
        target != korb_vm->symbol_class  &&
        target != korb_vm->numeric_class &&
        target != korb_vm->true_class    &&
        target != korb_vm->false_class   &&
        target != korb_vm->nil_class) return;
    if (!korb_is_basic_op_id(name)) return;
    korb_g_basic_op_redefined = true;
}

/* ---- method dispatch ---- */

/* ---- specialized prologues -----------------------------------------------
 * method_cache_fill picks one of these based on the matched method's type
 * and parameter shape.  After fill, dispatch is a single indirect call —
 * no in-function branching for cfunc-vs-AST or rest-slot/opt-arg shapes. */

#include "prologues.h"

/* Out-of-line wrappers: the inline bodies live in prologues.h so each
 * TU (main + every SD .so) gets its own inlined copy.  These named
 * non-inline wrappers exist so method_cache.prologue can hold a stable
 * function pointer in the main koruby binary; inside an SD the call is
 * compared against these names and inlined directly when it matches. */
VALUE prologue_cfunc(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                     uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_cfunc_inl(c, cs, recv, argc, ai, bl, mc); }

VALUE prologue_ast_simple_0(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                            uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_ast_simple_inl(c, cs, recv, argc, ai, bl, mc, 0); }

VALUE prologue_ast_simple_1(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                            uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_ast_simple_inl(c, cs, recv, argc, ai, bl, mc, 1); }

VALUE prologue_ast_simple_2(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                            uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_ast_simple_inl(c, cs, recv, argc, ai, bl, mc, 2); }

VALUE prologue_ast_simple_3(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                            uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_ast_simple_inl(c, cs, recv, argc, ai, bl, mc, 3); }

static VALUE prologue_ast_simple(CTX *c, struct Node *cs, VALUE recv, uint32_t argc,
                                 uint32_t ai, struct korb_proc *bl, struct method_cache *mc)
{ return prologue_ast_simple_inl(c, cs, recv, argc, ai, bl, mc, -1); }

/* AST general: handles opt args, rest_slot, all the trimmings.  Same body
 * as the legacy korb_dispatch_call AST hot path. */
static VALUE prologue_ast_general(CTX *c, struct Node *callsite, VALUE recv,
                                  uint32_t argc, uint32_t arg_index,
                                  struct korb_proc *block, struct method_cache *mc)
{
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
    if (mc->def_cref) c->cref = mc->def_cref;

    /* Kwargs hash peel: when the method declares keyword params and the
     * caller's last positional is a Hash, treat that hash as kwargs.
     * We stash it into mc->kwh_save_slot (which the body prelude reads)
     * and decrement argc so the rest of arg processing ignores it.  If
     * the caller passed no hash, write {} into kwh_save_slot. */
    VALUE peeled_kwh = Qundef;
    if (mc->kwh_save_slot >= 0) {
        if (argc > 0 && !SPECIAL_CONST_P(c->fp[argc - 1]) &&
            BUILTIN_TYPE(c->fp[argc - 1]) == T_HASH) {
            peeled_kwh = c->fp[argc - 1];
            argc--;
        } else {
            peeled_kwh = korb_hash_new();
        }
    }

    if (UNLIKELY(mc->rest_slot < 0 && argc > mc->total_params_cnt)) {
        korb_raise(c, NULL, "wrong number of arguments (given %u, expected %u)",
                   argc, mc->required_params_cnt);
        c->fp = prev_fp;
        c->cref = prev_cref;
        current_block = prev_block;
        return Qnil;
    }

    if (mc->rest_slot >= 0) {
        long fixed_pre  = (long)mc->required_params_cnt;
        long fixed_post = (long)mc->post_params_cnt;
        /* total_params_cnt = required + optional + rest(=1) + post.
         * Solve for optional_cnt. */
        long optional_cnt = (long)mc->total_params_cnt - fixed_pre - 1 - fixed_post;
        if (optional_cnt < 0) optional_cnt = 0;
        long after_required = (long)argc - fixed_pre - fixed_post;
        if (after_required < 0) after_required = 0;
        long opt_filled = after_required < optional_cnt ? after_required : optional_cnt;
        long extra = after_required - opt_filled;  /* leftover for rest */
        /* Snapshot post args before overwriting fp[rest_slot]. */
        VALUE post_save[16];
        VALUE *post_buf = post_save;
        if (fixed_post > 16) post_buf = korb_xmalloc(sizeof(VALUE) * fixed_post);
        for (long i = 0; i < fixed_post; i++) {
            post_buf[i] = c->fp[fixed_pre + opt_filled + extra + i];
        }
        VALUE rest = korb_ary_new_capa(extra);
        for (long i = 0; i < extra; i++) {
            korb_ary_push(rest, c->fp[fixed_pre + opt_filled + i]);
        }
        c->fp[mc->rest_slot] = rest;
        for (long i = 0; i < fixed_post; i++) {
            c->fp[mc->rest_slot + 1 + i] = post_buf[i];
        }
    }

    uint32_t opt_start = argc;
    if (opt_start < mc->required_params_cnt) opt_start = mc->required_params_cnt;
    /* Don't clobber post-rest slots (just populated above). */
    uint32_t post_lo = (mc->rest_slot >= 0 && mc->post_params_cnt)
                         ? (uint32_t)(mc->rest_slot + 1) : 0;
    uint32_t post_hi = post_lo + mc->post_params_cnt;
    for (uint32_t i = opt_start; i < mc->total_params_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        if (mc->post_params_cnt && i >= post_lo && i < post_hi) continue;
        c->fp[i] = Qundef;
    }
    for (uint32_t i = mc->total_params_cnt; i < mc->locals_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        if ((int)i == mc->block_slot) continue;
        c->fp[i] = Qnil;
    }
    /* &blk parameter — store the incoming block as a Proc into its
     * slot.  block can be NULL (no block given), in which case the
     * local reads as nil. */
    if (mc->block_slot >= 0) {
        c->fp[mc->block_slot] = block ? (VALUE)block : Qnil;
    }
    /* Stash the peeled kwargs hash where the body prelude can read it. */
    if (mc->kwh_save_slot >= 0 && !UNDEF_P(peeled_kwh)) {
        c->fp[mc->kwh_save_slot] = peeled_kwh;
    }
    c->self = recv;

    /* Trimmed frame: only fields actually read elsewhere (.prev,
     * .method for super, .self for backtrace, .block for block_given?,
     * .caller_node for backtrace lines).  Skipping .fp / .locals_cnt
     * saves a couple stores per call. */
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    c->current_frame = &frame;
    VALUE *frame_lo = c->fp;
    VALUE *frame_hi = c->fp + mc->locals_cnt;
    VALUE r = mc->dispatcher(c, mc->body);
    c->current_frame = frame.prev;
    korb_proc_snapshot_env_if_in_frame(r, frame_lo, frame_hi);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_if_in_frame(c->state_value, frame_lo, frame_hi);
    }
    c->fp = prev_fp;
    c->self = prev_self;
    c->cref = prev_cref;
    current_block = prev_block;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
    }
    return r;
}

void
korb_method_cache_fill(struct method_cache *mc, struct korb_class *klass, struct korb_method *m)
{
    mc->serial = korb_vm->method_serial;
    mc->klass = klass;
    mc->method = m;
    mc->is_simple_frame = m->is_simple_frame;
    if (m->type == KORB_METHOD_AST) {
        mc->body = m->u.ast.body;
        mc->dispatcher = (korb_dispatcher_t)m->u.ast.body->head.dispatcher;
        mc->locals_cnt = m->u.ast.locals_cnt;
        mc->required_params_cnt = m->u.ast.required_params_cnt;
        mc->total_params_cnt = m->u.ast.total_params_cnt;
        mc->rest_slot = m->u.ast.rest_slot;
        mc->block_slot = m->u.ast.block_slot;
        mc->post_params_cnt = m->u.ast.post_params_cnt;
        mc->kwh_save_slot = m->u.ast.kwh_save_slot;
        mc->type = 0;
        mc->cfunc = NULL;
        mc->def_cref = m->def_cref;
        /* &blk reification needs a runtime store, so it goes through the
         * general prologue.  Pick simple vs general based on parameter
         * shape; for the simple case prefer an argc-specialized variant
         * so the C compiler can fold the argc check + unroll the Qnil
         * fill. */
        if (mc->rest_slot < 0 && mc->block_slot < 0 && mc->kwh_save_slot < 0 &&
            mc->total_params_cnt == mc->required_params_cnt) {
            switch (mc->required_params_cnt) {
                case 0:  mc->prologue = prologue_ast_simple_0; break;
                case 1:  mc->prologue = prologue_ast_simple_1; break;
                case 2:  mc->prologue = prologue_ast_simple_2; break;
                case 3:  mc->prologue = prologue_ast_simple_3; break;
                default: mc->prologue = prologue_ast_simple;   break;
            }
        } else {
            mc->prologue = prologue_ast_general;
        }
    } else if (m->type == KORB_METHOD_PROC) {
        /* define_method: dispatch via the proc-method prologue which
         * pulls the proc from mc->method->u.proc.proc and invokes it
         * via proc_call (so closure env is preserved). */
        extern VALUE prologue_proc_method(CTX *c, struct Node *callsite,
                                          VALUE recv, uint32_t argc,
                                          uint32_t arg_index,
                                          struct korb_proc *block,
                                          struct method_cache *mc);
        mc->body = NULL;
        mc->dispatcher = NULL;
        mc->locals_cnt = 0;
        mc->required_params_cnt = 0;
        mc->total_params_cnt = 0;
        mc->rest_slot = -1;
        mc->kwh_save_slot = -1;
        mc->type = 2;
        mc->cfunc = NULL;
        mc->def_cref = NULL;
        mc->prologue = prologue_proc_method;
    } else {
        mc->body = NULL;
        mc->dispatcher = NULL;
        mc->locals_cnt = 0;
        mc->required_params_cnt = 0;
        mc->total_params_cnt = 0;
        mc->rest_slot = -1;
        mc->kwh_save_slot = -1;
        mc->type = 1;
        mc->cfunc = m->u.cfunc.func;
        mc->def_cref = NULL;
        mc->prologue = prologue_cfunc;
    }
}

/* Shim cfunc for proc-bodied methods (define_method).  We need to look
 * up the method again from (current_frame's caller side) — simpler: walk
 * receiver's class chain for a method named __method__ matching the call
 * site.  Hack: store the proc on a thread-local before dispatch.  But
 * the cfunc receives self/argc/argv with no method-name handle...
 * Workaround: walk class for a KORB_METHOD_PROC entry whose name matches
 * the most recent ID we resolved.  The cleanest C-level approach is to
 * keep a tiny cache; we put the (klass, proc) pair into the method
 * itself and reach it through `c->current_callsite` if present.  For
 * the common case we walk the class methods and find any PROC entry —
 * not great if multiple PROC methods exist, but our tests have one
 * recipient at a time.  TODO: real solution is a per-method cfunc
 * trampoline (one per define_method).  See todo.md. */
/* Prologue for define_method-defined methods: dispatch the captured
 * proc via proc_call so its env (closure) is preserved. */
VALUE prologue_proc_method(CTX *c, struct Node *callsite, VALUE recv,
                           uint32_t argc, uint32_t arg_index,
                           struct korb_proc *block, struct method_cache *mc)
{
    (void)callsite; (void)block;
    extern VALUE proc_call(CTX *c, VALUE self, int argc, VALUE *argv);
    if (!mc || !mc->method || mc->method->type != KORB_METHOD_PROC) return Qnil;
    struct korb_proc *p = mc->method->u.proc.proc;
    if (!p) return Qnil;
    /* args live at fp[arg_index..arg_index+argc-1]; pass that view. */
    VALUE *argv = &c->fp[arg_index];
    VALUE prev_self = c->self;
    c->self = recv;
    VALUE r = proc_call(c, (VALUE)p, (int)argc, argv);
    c->self = prev_self;
    return r;
}

VALUE korb_dispatch_visibility_raise(CTX *c, struct korb_method *m, ID name,
                                     struct korb_class *klass, VALUE recv) {
    (void)recv;
    const char *kind = (m->visibility == KORB_VIS_PRIVATE) ? "private" : "protected";
    VALUE eNoMethodError = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
    struct korb_class *exc_class = NULL;
    if (eNoMethodError && !SPECIAL_CONST_P(eNoMethodError) &&
        (BUILTIN_TYPE(eNoMethodError) == T_CLASS || BUILTIN_TYPE(eNoMethodError) == T_MODULE)) {
        exc_class = (struct korb_class *)eNoMethodError;
    }
    korb_raise(c, exc_class, "%s method '%s' called for %s",
               kind, korb_id_name(name),
               klass && klass->name ? korb_id_name(klass->name) : "?");
    return Qnil;
}

VALUE korb_dispatch_call(CTX *c, struct Node *callsite, VALUE recv, ID name,
                       uint32_t argc, uint32_t arg_index, struct korb_proc *block,
                       struct method_cache *mc)
{
    struct korb_class *klass = korb_class_of_class(recv);

    if (UNLIKELY(!mc || mc->serial != korb_vm->method_serial || mc->klass != klass)) {
        struct korb_method *m = NULL;
        if (BUILTIN_TYPE(recv) == T_MODULE) {
            m = korb_class_find_method((struct korb_class *)recv, name);
        }
        if (!m) m = korb_class_find_method(klass, name);
        if (UNLIKELY(!m)) {
            /* method_missing fallback: prepend the missing name to the
             * argv and dispatch :method_missing if defined. */
            struct korb_method *mm = korb_class_find_method(klass, korb_intern("method_missing"));
            if (mm) {
                /* Shift args right by one; insert :name in front. */
                VALUE *fp = c->fp;
                if (fp + arg_index + argc + 1 < c->stack_end) {
                    for (int i = (int)argc - 1; i >= 0; i--) {
                        fp[arg_index + 1 + i] = fp[arg_index + i];
                    }
                    fp[arg_index] = korb_id2sym(name);
                    struct method_cache tmp = {0};
                    korb_method_cache_fill(&tmp, klass, mm);
                    return tmp.prologue(c, callsite, recv, argc + 1, arg_index, block, &tmp);
                }
            }
            VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
            korb_raise(c, (struct korb_class *)eNo, "undefined method '%s' for %s",
                     korb_id_name(name), korb_id_name(klass->name));
            return Qnil;
        }
        if (mc) {
            korb_method_cache_fill(mc, klass, m);
        } else {
            /* No mc — slow one-shot path.  Synthesize a temp cache and dispatch. */
            struct method_cache tmp = {0};
            korb_method_cache_fill(&tmp, klass, m);
            if (m->visibility == KORB_VIS_PRIVATE && recv != c->self) {
                return korb_dispatch_visibility_raise(c, m, name, klass, recv);
            }
            if (m->visibility == KORB_VIS_PROTECTED) {
                struct korb_class *caller_klass = korb_class_of_class(c->self);
                bool ok = false;
                for (struct korb_class *k = caller_klass; k; k = k->super) {
                    if (k == m->defining_class) { ok = true; break; }
                }
                if (!ok) return korb_dispatch_visibility_raise(c, m, name, klass, recv);
            }
            return tmp.prologue(c, callsite, recv, argc, arg_index, block, &tmp);
        }
    }
    /* Visibility check on the freshly-filled mc (cache miss path) and
     * for cache hits — same logic as the inline fast path. */
    if (UNLIKELY(mc->method && mc->method->visibility != KORB_VIS_PUBLIC)) {
        if (mc->method->visibility == KORB_VIS_PRIVATE && recv != c->self) {
            return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
        }
        if (mc->method->visibility == KORB_VIS_PROTECTED) {
            struct korb_class *caller_klass = korb_class_of_class(c->self);
            bool ok = false;
            for (struct korb_class *k = caller_klass; k; k = k->super) {
                if (k == mc->method->defining_class) { ok = true; break; }
            }
            if (!ok) return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
        }
    }
    return mc->prologue(c, callsite, recv, argc, arg_index, block, mc);
}

/* Cold tails for fast-path NODEs (declared in object.h).  Each body
 * matches the original cold tail that used to be inlined into every
 * SD that contained the corresponding fast-path node.  Hoisting them
 * out shrinks all.so by ~10% and cuts AOT compile time. */

#define COLD_BINOP_DEFAULT(OP_ID) do {            \
    c->fp[arg_index+1] = r;                       \
    return korb_dispatch_binop(c, l, OP_ID, 1,    \
                               &c->fp[arg_index+1]); \
} while (0)

__attribute__((noinline,cold)) VALUE
korb_node_plus_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    if (BUILTIN_TYPE(l) == T_STRING && BUILTIN_TYPE(r) == T_STRING) {
        return korb_str_concat(l, r);
    }
    if (BUILTIN_TYPE(l) == T_ARRAY && BUILTIN_TYPE(r) == T_ARRAY) {
        VALUE a = korb_ary_new_capa(korb_ary_len(l) + korb_ary_len(r));
        for (long i = 0, n2 = korb_ary_len(l); i < n2; i++) korb_ary_push(a, korb_ary_aref(l, i));
        for (long i = 0, n2 = korb_ary_len(r); i < n2; i++) korb_ary_push(a, korb_ary_aref(r, i));
        return a;
    }
    COLD_BINOP_DEFAULT(id_op_plus);
}

__attribute__((noinline,cold)) VALUE
korb_node_minus_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_minus);
}
__attribute__((noinline,cold)) VALUE
korb_node_mul_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_mul);
}
__attribute__((noinline,cold)) VALUE
korb_node_div_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_div);
}
__attribute__((noinline,cold)) VALUE
korb_node_mod_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_mod);
}
__attribute__((noinline,cold)) VALUE
korb_node_uminus_slow(CTX *c, VALUE v) {
    return korb_dispatch_binop(c, v, korb_intern("-@"), 0, NULL);
}
__attribute__((noinline,cold)) VALUE
korb_node_band_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_and);
}
__attribute__((noinline,cold)) VALUE
korb_node_bor_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_or);
}
__attribute__((noinline,cold)) VALUE
korb_node_bxor_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_xor);
}
__attribute__((noinline,cold)) VALUE
korb_node_lshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_lshift);
}
__attribute__((noinline,cold)) VALUE
korb_node_rshift_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_rshift);
}
__attribute__((noinline,cold)) VALUE
korb_node_lt_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_lt);
}
__attribute__((noinline,cold)) VALUE
korb_node_le_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_le);
}
__attribute__((noinline,cold)) VALUE
korb_node_gt_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_gt);
}
__attribute__((noinline,cold)) VALUE
korb_node_ge_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    COLD_BINOP_DEFAULT(id_op_ge);
}

__attribute__((noinline,cold)) VALUE
korb_node_aref_slow(CTX *c, VALUE r, VALUE i, uint32_t arg_index) {
    if (UNLIKELY(SPECIAL_CONST_P(r))) {
        if (NIL_P(r)) return Qnil;
        c->fp[arg_index+1] = i;
        return korb_dispatch_binop(c, r, id_op_aref, 1, &c->fp[arg_index+1]);
    }
    c->fp[arg_index+1] = i;
    return korb_dispatch_binop(c, r, id_op_aref, 1, &c->fp[arg_index+1]);
}

__attribute__((noinline,cold)) VALUE
korb_node_aset_slow(CTX *c, VALUE r, VALUE i, VALUE v, uint32_t arg_index) {
    if (UNLIKELY(SPECIAL_CONST_P(r))) {
        if (NIL_P(r)) return v;
    }
    VALUE *args = &c->fp[arg_index+1];
    args[0] = i; args[1] = v;
    korb_dispatch_binop(c, r, id_op_aset, 2, args);
    return v;
}

#undef COLD_BINOP_DEFAULT

VALUE korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv) {
    struct korb_class *klass = korb_class_of_class(recv);
    struct korb_method *m = korb_class_find_method(klass, name);
    if (!m) {
        VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
        korb_raise(c, (struct korb_class *)eNo, "undefined method '%s' for %s",
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
    VALUE *prev_sp = c->sp;
    VALUE prev_self = c->self;
    /* push frame after all current locals; we don't know exactly the boundary,
     * so use sp as upper bound.  Restore sp at end so repeated send-style
     * dispatches don't leak high-water mark. */
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
    c->sp = prev_sp;
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
    korb_vm->method_serial = 1; korb_g_method_serial = 1;

    /* bootstrap classes — CRuby: BasicObject ← Object ← Module ← Class.
     * Each class's own metaclass is Class itself. */
    struct korb_class *cBasic  = korb_class_new(korb_intern("BasicObject"), NULL,    T_OBJECT);
    struct korb_class *cObject = korb_class_new(korb_intern("Object"),      cBasic,  T_OBJECT);
    struct korb_class *cModule = korb_class_new(korb_intern("Module"),      cObject, T_MODULE);
    struct korb_class *cClass  = korb_class_new(korb_intern("Class"),       cModule, T_CLASS);
    cBasic->basic.klass  = (VALUE)cClass;
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
    /* ObjectSpace — stub module for the API surface.  Real
     * each_object enumeration would need a weak-ref registry on top
     * of Boehm GC (which doesn't expose object iteration); for now
     * each_object yields nothing and count_objects returns a small
     * Hash with :TOTAL = 0 so callers don't crash. */
    {
        struct korb_class *cOS = korb_module_new(korb_intern("ObjectSpace"));
        korb_const_set(cObject, cOS->name, (VALUE)cOS);
        struct korb_class *cOSMeta = korb_class_new(korb_intern("ObjectSpaceMeta"),
                                                     cClass, T_CLASS);
        VALUE objspace_each_object(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE objspace_count_objects(CTX *c, VALUE self, int argc, VALUE *argv);
        VALUE objspace_garbage_collect(CTX *c, VALUE self, int argc, VALUE *argv);
        korb_class_add_method_cfunc(cOSMeta, korb_intern("each_object"),     objspace_each_object,     -1);
        korb_class_add_method_cfunc(cOSMeta, korb_intern("count_objects"),   objspace_count_objects,   -1);
        korb_class_add_method_cfunc(cOSMeta, korb_intern("garbage_collect"), objspace_garbage_collect,  0);
        cOS->basic.klass = (VALUE)cOSMeta;
    }
    korb_vm->comparable_module = korb_module_new(korb_intern("Comparable"));
    korb_vm->enumerable_module = korb_module_new(korb_intern("Enumerable"));

    /* CRuby's hierarchy has Object include Kernel — that's how every
     * object gets `puts` / `nil?` / `is_a?` etc.  Hook the include here
     * so `Object.ancestors` reports `[Object, Kernel, BasicObject]`. */
    {
        struct korb_class *o = cObject;
        if (o->includes_capa == 0) {
            o->includes_capa = 4;
            o->includes = korb_xmalloc(o->includes_capa * sizeof(*o->includes));
        }
        o->includes[o->includes_cnt++] = korb_vm->kernel_module;
    }

    /* register top-level constants */
    korb_const_set(cObject, cBasic->name,                 (VALUE)cBasic);
    korb_const_set(cObject, korb_vm->kernel_module->name, (VALUE)korb_vm->kernel_module);
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

    /* Exception class hierarchy.  Real CRuby tree:
     *   Exception
     *     StandardError
     *       RuntimeError, ArgumentError, TypeError, NameError,
     *       NoMethodError (< NameError), IndexError,
     *       KeyError (< IndexError), RangeError, FloatDomainError (< RangeError),
     *       ZeroDivisionError, IOError, FrozenError (< RuntimeError),
     *       NotImplementedError, StopIteration, LocalJumpError,
     *       SystemCallError, Errno
     *     ScriptError
     *       LoadError, SyntaxError
     * `rescue StandardError` only matches StandardError descendants —
     * the parent/child relationships need to reflect that. */
    struct korb_class *cException = korb_class_new(korb_intern("Exception"), cObject, T_OBJECT);
    korb_const_set(cObject, korb_intern("Exception"), (VALUE)cException);
    struct korb_class *cStandardError = korb_class_new(korb_intern("StandardError"), cException, T_OBJECT);
    korb_const_set(cObject, korb_intern("StandardError"), (VALUE)cStandardError);
    struct korb_class *cScriptError = korb_class_new(korb_intern("ScriptError"), cException, T_OBJECT);
    korb_const_set(cObject, korb_intern("ScriptError"), (VALUE)cScriptError);
    struct korb_class *cRuntimeError = korb_class_new(korb_intern("RuntimeError"), cStandardError, T_OBJECT);
    korb_const_set(cObject, korb_intern("RuntimeError"), (VALUE)cRuntimeError);
    struct korb_class *cIndexError = korb_class_new(korb_intern("IndexError"), cStandardError, T_OBJECT);
    korb_const_set(cObject, korb_intern("IndexError"), (VALUE)cIndexError);
    struct korb_class *cNameError = korb_class_new(korb_intern("NameError"), cStandardError, T_OBJECT);
    korb_const_set(cObject, korb_intern("NameError"), (VALUE)cNameError);
    struct korb_class *cRangeError = korb_class_new(korb_intern("RangeError"), cStandardError, T_OBJECT);
    korb_const_set(cObject, korb_intern("RangeError"), (VALUE)cRangeError);
    /* Direct StandardError children. */
    static const char *std_subs[] = {
        "ArgumentError", "TypeError",
        "ZeroDivisionError", "IOError", "Errno",
        "NotImplementedError", "StopIteration", "LocalJumpError",
        "SystemCallError",
        NULL,
    };
    for (int i = 0; std_subs[i]; i++) {
        struct korb_class *k = korb_class_new(korb_intern(std_subs[i]), cStandardError, T_OBJECT);
        korb_const_set(cObject, korb_intern(std_subs[i]), (VALUE)k);
    }
    /* Children of more-specific classes. */
    {
        struct korb_class *k = korb_class_new(korb_intern("NoMethodError"), cNameError, T_OBJECT);
        korb_const_set(cObject, korb_intern("NoMethodError"), (VALUE)k);
        k = korb_class_new(korb_intern("KeyError"), cIndexError, T_OBJECT);
        korb_const_set(cObject, korb_intern("KeyError"), (VALUE)k);
        k = korb_class_new(korb_intern("FloatDomainError"), cRangeError, T_OBJECT);
        korb_const_set(cObject, korb_intern("FloatDomainError"), (VALUE)k);
        k = korb_class_new(korb_intern("FrozenError"), cRuntimeError, T_OBJECT);
        korb_const_set(cObject, korb_intern("FrozenError"), (VALUE)k);
    }
    /* ScriptError children. */
    {
        struct korb_class *k = korb_class_new(korb_intern("LoadError"), cScriptError, T_OBJECT);
        korb_const_set(cObject, korb_intern("LoadError"), (VALUE)k);
        k = korb_class_new(korb_intern("SyntaxError"), cScriptError, T_OBJECT);
        korb_const_set(cObject, korb_intern("SyntaxError"), (VALUE)k);
    }

    /* Register Comparable / Enumerable / Numeric so user code can
     * `include Comparable` etc.  Comparable's instance methods are
     * installed in builtins.c. */
    korb_const_set(cObject, korb_intern("Comparable"), (VALUE)korb_vm->comparable_module);
    korb_const_set(cObject, korb_intern("Enumerable"), (VALUE)korb_vm->enumerable_module);

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
    /* If name is absolute, do not join with current_file's dir */
    long nl = strlen(name);
    bool has_rb = nl >= 3 && strcmp(name + nl - 3, ".rb") == 0;
    if (name[0] == '/') {
        if (!has_rb) {
            char *with = korb_xmalloc_atomic(nl + 4);
            sprintf(with, "%s.rb", name);
            if (korb_file_exists(with)) return with;
        }
        if (korb_file_exists(name)) {
            char *r = korb_xmalloc_atomic(nl + 1);
            strcpy(r, name);
            return r;
        }
        return NULL;
    }
    const char *dir = current_file ? korb_dirname(current_file) : ".";
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

/* Like korb_eval_string but uses the caller's `self` and the receiver's
 * class for cref — used by Object#instance_eval(string). */
VALUE korb_eval_string_in_self(CTX *c, const char *src, size_t len,
                                const char *filename, VALUE recv) {
    NODE *ast = koruby_parse(src, len, filename ? filename : "(eval)");
    if (!ast) return Qnil;
    VALUE *prev_fp = c->fp;
    VALUE prev_self = c->self;
    struct korb_class *prev_class = c->current_class;
    struct korb_cref *prev_cref = c->cref;
    const char *prev_file = c->current_file;
    c->fp = c->sp + 1;
    c->self = recv;
    struct korb_class *recv_klass = korb_class_of_class(recv);
    c->current_class = recv_klass;
    struct korb_cref top_cref = { .klass = recv_klass, .prev = NULL };
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

/* ---- Fiber ---- */
struct korb_fiber {
    struct RBasic basic;
    ucontext_t ctx;
    ucontext_t prev_ctx;
    struct korb_proc *block;
    char *stack;
    size_t stack_size;
    enum { KF_INIT, KF_RUNNING, KF_SUSPENDED, KF_DEAD } state;
    /* args/return values */
    VALUE *args;
    int argc;
    VALUE result;
    CTX *c;

    /* Resumer-side save: stashed on resume, restored on yield. */
    VALUE *resumer_fp;
    VALUE *resumer_sp;
    VALUE *resumer_stack_base;
    VALUE *resumer_stack_end;

    /* Fiber-side save: stashed on yield, restored on resume.
     * Initialized at fiber creation (or first resume) to point into
     * the fiber's heap frame so the body's slots don't overlap the
     * resumer's value-stack. */
    VALUE *fiber_fp;
    VALUE *fiber_sp;

    /* Per-fiber value-stack area: heap-allocated, lives as long as the
     * fiber, used for the block's frame and for any method calls made
     * from within the fiber. */
    VALUE *frame;
    size_t frame_size;
};

static __thread struct korb_fiber *current_fiber = NULL;

static void korb_fiber_entry(unsigned int hi, unsigned int lo) {
    uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    struct korb_fiber *fib = (struct korb_fiber *)p;
    if (fib->block) {
        struct korb_proc *blk = fib->block;
        CTX *c = fib->c;
        /* Place initial args into the fiber's heap frame at the block's
         * param slots (env was pre-copied at fiber creation). */
        for (uint32_t i = 0; i < blk->params_cnt && i < (uint32_t)fib->argc; i++) {
            fib->frame[blk->param_base + i] = fib->args[i];
        }
        VALUE prev_self = c->self;
        c->self = blk->self;
        struct korb_proc *prev_block = current_block;
        current_block = NULL;
        VALUE result = EVAL(c, blk->body);
        c->self = prev_self;
        current_block = prev_block;
        fib->result = result;
    }
    fib->state = KF_DEAD;
    /* Match the GC_disable resume() did before swapping to us — we're
     * exiting the fiber for the last time, so re-enable GC. */
    GC_enable();
    swapcontext(&fib->ctx, &fib->prev_ctx);
}

VALUE korb_fiber_new(struct korb_proc *block) {
    struct korb_fiber *fib = korb_xmalloc(sizeof(*fib));
    fib->basic.flags = T_DATA;
    fib->basic.klass = korb_vm->fiber_class
                         ? (VALUE)korb_vm->fiber_class
                         : (VALUE)korb_vm->object_class;
    fib->block = block;
    fib->stack_size = 4 * 1024 * 1024;  /* 4 MB — PPU pixel pipeline can be deep */
    fib->stack = korb_xmalloc(fib->stack_size);
    /* Register the fiber's C stack as a permanent GC root.  When the
     * fiber is running, rsp is in this region; without it being a
     * root, Boehm's GC walks toward the resumer's stack bottom and
     * SEGVs in unmapped memory.  We could swap stackbottom on each
     * resume/yield instead — but that adds a libgc-locked PLT call
     * per fiber switch, which costs ~50% on optcarrot.  Adding the
     * range as a root means Boehm scans the whole 4 MB on every
     * GC pass (a few µs) but the per-switch path stays free. */
    GC_add_roots(fib->stack, fib->stack + fib->stack_size);
    fib->state = KF_INIT;
    fib->args = NULL;
    fib->argc = 0;
    fib->result = Qnil;
    fib->c = NULL;
    /* Allocate a heap value-frame for the fiber's locals so they don't
     * share the resumer's stack slots.  Optcarrot's PPU pipeline can
     * have deep call chains (rendering helpers calling block-yields
     * down several levels), so size generously. */
    fib->frame_size = 64 * 1024;
    fib->frame = korb_xmalloc(sizeof(VALUE) * fib->frame_size);
    for (size_t i = 0; i < fib->frame_size; i++) fib->frame[i] = Qnil;
    /* Pre-fill from env so closure captured locals are visible. */
    uint32_t env_size = 0;
    if (block) {
        env_size = block->env_size;
        for (uint32_t i = 0; i < env_size && i < fib->frame_size; i++) {
            fib->frame[i] = block->env[i];
        }
    }
    /* Initial fiber c->fp/sp: fp at frame base, sp just past block's
     * env (so method calls inside the block don't overlap its locals). */
    fib->fiber_fp = fib->frame;
    fib->fiber_sp = fib->frame + env_size;
    fib->resumer_fp = NULL;
    fib->resumer_sp = NULL;
    fib->resumer_stack_base = NULL;
    fib->resumer_stack_end = NULL;
    return (VALUE)fib;
}

VALUE korb_fiber_resume(CTX *c, VALUE fibv, int argc, VALUE *argv) {
    struct korb_fiber *fib = (struct korb_fiber *)fibv;
    if (fib->state == KF_DEAD) {
        korb_raise(c, NULL, "dead fiber called");
        return Qnil;
    }
    if (fib->state == KF_RUNNING) {
        korb_raise(c, NULL, "double resume");
        return Qnil;
    }
    fib->args = argv;
    fib->argc = argc;
    fib->c = c;

    if (fib->state == KF_INIT) {
        getcontext(&fib->ctx);
        fib->ctx.uc_stack.ss_sp = fib->stack;
        fib->ctx.uc_stack.ss_size = fib->stack_size;
        fib->ctx.uc_link = &fib->prev_ctx;
        uintptr_t p = (uintptr_t)fib;
        unsigned int hi = (unsigned int)(p >> 32);
        unsigned int lo = (unsigned int)(p & 0xffffffff);
        makecontext(&fib->ctx, (void (*)(void))korb_fiber_entry, 2, hi, lo);
    }

    /* Save resumer's c->fp/sp/stack_base/stack_end into the fiber, swap
     * in the fiber's saved fp/sp + heap-frame extents, then swapcontext.
     * Yield will reverse this. */
    struct korb_fiber *prev = current_fiber;
    current_fiber = fib;
    fib->resumer_fp = c->fp;
    fib->resumer_sp = c->sp;
    fib->resumer_stack_base = c->stack_base;
    fib->resumer_stack_end = c->stack_end;
    c->fp = fib->fiber_fp;
    c->sp = fib->fiber_sp;
    c->stack_base = fib->frame;
    c->stack_end = fib->frame + fib->frame_size;
    fib->state = KF_RUNNING;

    /* Boehm walks the current thread's C stack during GC.  Inside a
     * fiber, rsp is in the fiber's malloc'd stack — Boehm walking
     * toward the resumer's stack bottom crosses unmapped memory and
     * SEGVs.  Two ways to avoid it:
     *   (a) GC_set_stackbottom around each swap — but it acquires a
     *       GC lock, ~50% perf hit for fiber-heavy workloads.
     *   (b) GC_disable while in the fiber so no GC fires there.
     *       Live data in the fiber stack stays reachable via the
     *       earlier GC_add_roots(fib->stack, ...) registration; we
     *       just skip running collection itself.
     * (b) is dramatically faster for optcarrot.  Memory pressure is
     * bounded since fibers in optcarrot yield often (per scanline).
     *
     * GC_disable / GC_enable are reference-counted in Boehm.  Pair
     * each disable here with exactly one enable — done by either the
     * yield path's swap-out or the entry function's terminal swap. */
    GC_disable();
    swapcontext(&fib->prev_ctx, &fib->ctx);

    /* Returned from yield/end — restore resumer's fp/sp from where the
     * yield path stashed them. */
    c->fp = fib->resumer_fp;
    c->sp = fib->resumer_sp;
    c->stack_base = fib->resumer_stack_base;
    c->stack_end = fib->resumer_stack_end;
    current_fiber = prev;
    if (fib->state != KF_DEAD) fib->state = KF_SUSPENDED;
    return fib->result;
}

VALUE korb_fiber_yield(CTX *c, int argc, VALUE *argv) {
    struct korb_fiber *fib = current_fiber;
    if (!fib) {
        korb_raise(c, NULL, "Fiber.yield called outside a fiber");
        return Qnil;
    }
    fib->result = argc > 0 ? argv[0] : Qnil;
    fib->state = KF_SUSPENDED;
    /* Save fiber's c->fp/sp so the next resume can pick up where we
     * yielded; restore the resumer's fp/sp/stack so it sees its own
     * value-stack. */
    fib->fiber_fp = c->fp;
    fib->fiber_sp = c->sp;
    c->fp = fib->resumer_fp;
    c->sp = fib->resumer_sp;
    c->stack_base = fib->resumer_stack_base;
    c->stack_end = fib->resumer_stack_end;

    /* Mirror of resume's GC_disable: re-enable on yield back, disable
     * again when the fiber resumes.  See resume for the full rationale. */
    GC_enable();
    swapcontext(&fib->ctx, &fib->prev_ctx);
    GC_disable();

    /* Resumed — restore the fiber's heap-frame extents so subsequent
     * method calls inside the fiber check against the right bounds. */
    c->stack_base = fib->frame;
    c->stack_end = fib->frame + fib->frame_size;
    /* Resumed: restore the fiber's fp/sp (resume already did this from
     * its side, but in a chain of resume->yield->resume the inner ctx
     * comes back here and the resumer's wrapper has overwritten c->fp
     * to its own; resume sets fp again before swapcontext, so by the
     * time we land here, c->fp is fib->fiber_fp). */
    if (fib->argc > 0) return fib->args[0];
    return Qnil;
}

VALUE korb_fiber_new_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Block is the current_block when Fiber.new is called */
    if (!current_block) {
        korb_raise(c, NULL, "Fiber.new requires a block");
        return Qnil;
    }
    return korb_fiber_new(current_block);
}
VALUE korb_fiber_yield_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_fiber_yield(c, argc, argv);
}
VALUE korb_fiber_resume_cfunc(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_fiber_resume(c, self, argc, argv);
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
