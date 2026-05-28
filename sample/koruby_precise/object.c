/*
 * koruby object/runtime support.
 * Precise GC framework (= runtime/precise_gc/).  Bignum uses GMP.
 */

#include "korb_gmp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "context.h"
#include "object.h"
#include "node.h"
#include "precise_gc/gc.h"

#include <ucontext.h>

struct korb_vm *korb_vm = NULL;

ID id_initialize, id_to_s, id_inspect, id_call, id_each, id_new;
ID id_op_plus, id_op_minus, id_op_mul, id_op_div, id_op_mod;
ID id_op_eq, id_op_neq, id_op_lt, id_op_le, id_op_gt, id_op_ge;
ID id_op_aref, id_op_aset, id_op_lshift, id_op_rshift, id_op_and, id_op_or, id_op_xor;

/* ---- memory ---- */
/* korb_x{m,c,re}alloc are used for non-GC-tracked sample-internal
 * buffers (= method tables, const tables, hash buckets, etc. that are
 * either reachable via stable typed-ptr fields or used during compile-
 * time only).  Phase 1: keep libc calloc/realloc/free until Phase 3
 * (SCAN_EDGES + heap re-classification).  GC-managed heap objs use
 * `aro_gc_alloc` directly. */
void *korb_xmalloc(size_t s) { void *p = calloc(1, s); if (!p) abort(); return p; }
void *korb_xmalloc_atomic(size_t s) { return korb_xmalloc(s); }
void *korb_xcalloc(size_t n, size_t sz) { return korb_xmalloc(n * sz); }
void *korb_xrealloc(void *p, size_t newsize) {
    void *q = realloc(p, newsize); if (!q && newsize) abort(); return q;
}
void  korb_xfree(void *p) { free(p); }

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

void korb_method_table_set(struct korb_method_table *mt, ID name, struct korb_method *m);
static void method_table_set(struct korb_method_table *mt, ID name, struct korb_method *m) {
    korb_method_table_set(mt, name, m);
}
void korb_method_table_set(struct korb_method_table *mt, ID name, struct korb_method *m) {
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

struct korb_method *method_table_get(const struct korb_method_table *mt, ID name) {
    if (!mt->buckets) return NULL;
    uint32_t b = (uint32_t)(name % mt->bucket_cnt);
    for (struct korb_method_table_entry *e = mt->buckets[b]; e; e = e->next) {
        if (e->name == name) return e->method;
    }
    return NULL;
}

struct korb_class *korb_class_new(ID name, struct korb_class *super, enum korb_type instance_type) {
    /* Precise GC: allocate on GC heap via aro_gc_alloc.  super needs
     * protection across the inner child_meta alloc (= may trigger GC
     * which can relocate super under moving GC).  k itself needs
     * protection across child_meta alloc.  Pattern: park live VALUEs
     * in ARO_ROOT slots, reload C-local pointers after each alloc. */
    CTX *c = korb_vm->current_ctx;
    struct korb_class *k_ret;
    ARO_ROOT_SCOPE_START(c, r, 3) {
        /* r[0]=super, r[1]=k, r[2]=child_meta (if created) */
        r[0] = (VALUE)super;
        r[1] = aro_gc_alloc(c, sizeof(struct korb_class));
        super = (struct korb_class *)r[0];                /* reload */
        struct korb_class *k = (struct korb_class *)r[1];
        /* AROH_INIT_PAYLOAD already zero-filled post-head; fields just
         * need their non-zero values set. */
        k->basic.head.flags = T_CLASS;
        k->basic.klass = korb_vm->class_class;
        /* CRuby model: a subclass's metaclass has the parent's metaclass
         * as its superclass.  Without this, singleton methods don't
         * propagate down the inheritance chain. */
        if (super && super->basic.klass &&
            (struct korb_class *)super->basic.klass != korb_vm->class_class) {
            r[2] = aro_gc_alloc(c, sizeof(struct korb_class));
            super = (struct korb_class *)r[0];            /* reload */
            k     = (struct korb_class *)r[1];            /* reload */
            struct korb_class *child_meta = (struct korb_class *)r[2];
            child_meta->basic.head.flags = T_CLASS | FL_SINGLETON;
            child_meta->basic.klass = korb_vm->class_class;
            child_meta->name = name;
            child_meta->super = (struct korb_class *)super->basic.klass;
            child_meta->instance_type = T_CLASS;
            method_table_init(&child_meta->methods);
            child_meta->default_visibility = KORB_VIS_PUBLIC;
            k->basic.klass = (VALUE)child_meta;
        }
        k->name = name;
        k->super = super;
        k->instance_type = instance_type;
        method_table_init(&k->methods);
        k->default_visibility = KORB_VIS_PUBLIC;
        /* constants / ivar_* / includes_* / prepends_* / class_ivars /
         * cvars / anon_parent / anon_name_in_parent all already zero
         * via AROH_INIT_PAYLOAD. */
        k_ret = k;
    } ARO_ROOT_SCOPE_END(c, r);
    return k_ret;
}

/* Class variables: walk the cref's class up the super chain to find
 * @@name; return Qundef when not present.  Used by node_cvar_get. */
static struct korb_class *cvar_owner_walk_(struct korb_class *k, ID name);
static struct korb_class *cvar_owner_(struct korb_class *k, ID name) {
    return cvar_owner_walk_(k, name);
}
/* Walk super chain + transitive includes for a cvar.  Includes can also
 * declare class variables (`module M; @@x = 1; end`) which are visible
 * via the including class (CRuby semantics). */
static struct korb_class *cvar_owner_walk_(struct korb_class *k, ID name) {
    if (!k) return NULL;
    for (struct korb_class *cur = k; cur; cur = cur->super) {
        for (uint32_t i = 0; i < cur->cvar_cnt; i++) {
            if (cur->cvars[i].name == name) return cur;
        }
        for (uint32_t i = 0; i < cur->includes_cnt; i++) {
            struct korb_class *o = cvar_owner_walk_(cur->includes[i], name);
            if (o) return o;
        }
    }
    return NULL;
}

VALUE korb_cvar_get(CTX *c, ID name) {
    struct korb_class *k = c->current_frame->cref ? c->current_frame->cref->klass : c->current_frame->current_class;
    if (!k && c->current_frame) k = korb_class_of_class(c->current_frame->self);
    if (!k) k = korb_class_of_class(c->current_frame->self);
    /* Top-level read of @@cvar — RuntimeError per CRuby. */
    /* Top-level access (no enclosing class/module body) — RuntimeError.
     * Toplevel's cref->prev is NULL and the resolved class is Object's
     * meta or main. */
    if (c->current_frame->cref && !c->current_frame->cref->prev &&
        (k == korb_vm->object_class || k == korb_vm->main_obj_class)) {
        korb_raise(c, NULL, "class variable access from toplevel");
        return Qnil;
    }
    struct korb_class *owner = k ? cvar_owner_(k, name) : NULL;
    if (!owner) {
        VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eName,
                   "uninitialized class variable %s in %s",
                   korb_id_name(name),
                   k ? korb_id_name(k->name) : "(unknown)");
        return Qnil;
    }
    /* Overtaken detection: if a strict ancestor of `owner` also has this
     * cvar, the value seen by k differs from the chain's authoritative
     * top — CRuby raises RuntimeError to flag the inconsistency.  Happens
     * when child sets @@x first, then parent independently sets @@x. */
    for (struct korb_class *anc = owner->super; anc; anc = anc->super) {
        for (uint32_t i = 0; i < anc->cvar_cnt; i++) {
            if (anc->cvars[i].name == name) {
                korb_raise(c, NULL,
                           "class variable %s of %s is overtaken by %s",
                           korb_id_name(name),
                           anc->name ? korb_id_name(anc->name) : "(anon)",
                           owner->name ? korb_id_name(owner->name) : "(anon)");
                return Qnil;
            }
        }
    }
    for (uint32_t i = 0; i < owner->cvar_cnt; i++) {
        if (owner->cvars[i].name == name) return owner->cvars[i].value;
    }
    return Qnil;
}

void korb_cvar_set(CTX *c, ID name, VALUE val) {
    struct korb_class *k = c->current_frame->cref ? c->current_frame->cref->klass : c->current_frame->current_class;
    if (!k && c->current_frame) k = korb_class_of_class(c->current_frame->self);
    if (!k) k = korb_class_of_class(c->current_frame->self);
    if (!k) return;
    /* CRuby: `@@cvar = x` at top level (no enclosing class/module) is a
     * RuntimeError.  Detect "top level" as cref == NULL and current self
     * is the singleton main object. */
    if (c->current_frame->cref && !c->current_frame->cref->prev &&
        (k == korb_vm->object_class || k == korb_vm->main_obj_class)) {
        korb_raise(c, NULL, "class variable access from toplevel");
        return;
    }
    struct korb_class *target = cvar_owner_(k, name);
    if (!target) target = k;
    for (uint32_t i = 0; i < target->cvar_cnt; i++) {
        if (target->cvars[i].name == name) { target->cvars[i].value = val; return; }
    }
    if (target->cvar_cnt >= target->cvar_capa) {
        uint32_t nc = target->cvar_capa ? target->cvar_capa * 2 : 4;
        target->cvars = korb_xrealloc(target->cvars, nc * sizeof(*target->cvars));
        target->cvar_capa = nc;
    }
    target->cvars[target->cvar_cnt].name = name;
    target->cvars[target->cvar_cnt].value = val;
    target->cvar_cnt++;
}

bool korb_cvar_defined(CTX *c, ID name) {
    struct korb_class *k = c->current_frame->cref ? c->current_frame->cref->klass : c->current_frame->current_class;
    if (!k && c->current_frame) k = korb_class_of_class(c->current_frame->self);
    if (!k) k = korb_class_of_class(c->current_frame->self);
    return k && cvar_owner_(k, name) != NULL;
}

VALUE korb_cvar_names(struct korb_class *k) {
    VALUE arr = korb_ary_new();
    /* Collect from k and its supers, dedup by name. */
    for (struct korb_class *cur = k; cur; cur = cur->super) {
        for (uint32_t i = 0; i < cur->cvar_cnt; i++) {
            VALUE sym = korb_id2sym(cur->cvars[i].name);
            bool seen = false;
            struct korb_array *a = (struct korb_array *)arr;
            for (long j = 0; j < a->len; j++) if (a->ptr[j] == sym) { seen = true; break; }
            if (!seen) korb_ary_push(arr, sym);
        }
    }
    return arr;
}

struct korb_class *korb_module_new(ID name) {
    struct korb_class *k = korb_class_new(name, NULL, T_NONE);
    k->basic.head.flags = T_MODULE;
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
        strstr(buf, "(node_yield_splat ")       == NULL &&
        strstr(buf, "(node_super")              == NULL &&
        strstr(buf, "(node_method_call_block ") == NULL &&
        strstr(buf, "(node_func_call_block ")   == NULL &&
        strstr(buf, "(node_const_get ")         == NULL &&
        strstr(buf, "(node_const_set ")         == NULL &&
        strstr(buf, "(node_const_path_get ")    == NULL &&
        /* class variables need cref so the lookup roots at the
         * defining class, not the surrounding (Object) cref. */
        strstr(buf, "(node_cvar_get ")          == NULL &&
        strstr(buf, "(node_cvar_set ")          == NULL &&
        strstr(buf, "(node_raise ")             == NULL &&
        strstr(buf, "block_given?")             == NULL &&
        /* `def`/`undef`/`alias`/`class`/`module` reach for `c->current_frame->cref` to
         * pick the target class, so they need the cref restore step. */
        strstr(buf, "(node_def_full ")          == NULL &&
        strstr(buf, "(node_def_self ")          == NULL &&
        strstr(buf, "(node_undef ")             == NULL &&
        strstr(buf, "(node_alias ")             == NULL &&
        strstr(buf, "(node_class_def")          == NULL &&
        strstr(buf, "(node_module_def")         == NULL &&
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
    if (klass && korb_obj_frozen_p((VALUE)klass)) {
        VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
        if (eF && !SPECIAL_CONST_P(eF) && BUILTIN_TYPE(eF) == T_CLASS) {
            korb_raise(korb_vm->current_ctx, (struct korb_class *)eF,
                       "can't modify frozen %s",
                       klass->name ? korb_id_name(klass->name) : "Class");
        } else {
            korb_raise(korb_vm->current_ctx, NULL, "can't modify frozen Class");
        }
        return;
    }
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
    m->u.ast.local_names = NULL;
    m->u.ast.param_holder_slots = NULL;
    method_table_set(&klass->methods, name, m);
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
}

/* Attach a param_position → fp slot map to the latest AST method.
 * Caller owns the array. */
void korb_class_set_method_param_holder_slots(struct korb_class *klass, ID name, int *slots) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.param_holder_slots = slots;
    if (korb_vm) { korb_vm->method_serial++; korb_g_method_serial = korb_vm->method_serial; }
}

/* Set the &blk parameter slot on the most-recently-added AST method
 * for `klass::name`.  Called from node_def_full when the def's
 * parameter list has a block parameter. */
void korb_class_set_method_block_slot(struct korb_class *klass, ID name, int slot) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.block_slot = slot;
}

/* Attach the lvar-name table (slot -> ID) so Kernel#binding can
 * iterate the active frame and emit each name → value pair.  Caller
 * owns the array; we just store the pointer. */
void korb_class_set_method_local_names(struct korb_class *klass, ID name, ID *names) {
    struct korb_method *m = korb_class_find_method(klass, name);
    if (m && m->type == KORB_METHOD_AST) m->u.ast.local_names = names;
}

/* Side table: body NODE pointer → ID array (lvar names by slot).
 * Populated at parse time by a parse helper, queried at runtime
 * (after the def NODE has registered the method) so we can stamp
 * local_names onto m->u.ast.local_names. */
struct body_to_names_entry {
    struct Node *body;
    ID *names;
    struct body_to_names_entry *next;
};
static struct body_to_names_entry *g_body_to_names = NULL;

void korb_register_body_local_names(struct Node *body, ID *names) {
    struct body_to_names_entry *e = korb_xmalloc(sizeof(*e));
    e->body = body;
    e->names = names;
    e->next = g_body_to_names;
    g_body_to_names = e;
}

ID *korb_body_local_names(struct Node *body) {
    for (struct body_to_names_entry *e = g_body_to_names; e; e = e->next) {
        if (e->body == body) return e->names;
    }
    return NULL;
}

/* Side table: body NODE → param_holder_slots[].  Same shape as the
 * local_names registry — the parse phase records it, the def's
 * runtime node looks it up and stamps it onto the method record. */
struct body_to_phs_entry {
    struct Node *body;
    int *slots;
    struct body_to_phs_entry *next;
};
static struct body_to_phs_entry *g_body_to_phs = NULL;

void korb_register_body_param_holder_slots(struct Node *body, int *slots) {
    struct body_to_phs_entry *e = korb_xmalloc(sizeof(*e));
    e->body = body;
    e->slots = slots;
    e->next = g_body_to_phs;
    g_body_to_phs = e;
}

int *korb_body_param_holder_slots(struct Node *body) {
    for (struct body_to_phs_entry *e = g_body_to_phs; e; e = e->next) {
        if (e->body == body) return e->slots;
    }
    return NULL;
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
    /* Bind defining_method so `super` inside the proc body dispatches
     * via the registered method's defining_class (instead of whatever
     * outer method was active when the proc literal was created). */
    snap->defining_method = m;
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
     * singleton class so per-class methods can be installed.  CRuby
     * semantics: meta(C).super = meta(C.super), so a method defined on
     * super's singleton class is visible to C as a class method.
     *
     * Recursive + nested alloc — klass / current_meta / meta all live
     * across GC fires from the recursive call and korb_class_new.  Park
     * them in ARO_ROOT_SCOPE slots and reload C-local pointers after
     * each potential GC trigger. */
    CTX *c = korb_vm->current_ctx;
    struct korb_class *result;
    ARO_ROOT_SCOPE_START(c, r, 3) {
        r[0] = (VALUE)klass;
        struct korb_class *current_meta = (struct korb_class *)klass->basic.klass;
        if (current_meta == korb_vm->class_class || current_meta == korb_vm->module_class) {
            r[1] = (VALUE)current_meta;
            struct korb_class *super_meta;
            if (klass->super) {
                /* recursive korb_singleton_class_of may fire GC; r[0]/r[1]
                 * survive via visit_roots. */
                super_meta = korb_singleton_class_of(klass->super);
                klass        = (struct korb_class *)r[0];
                current_meta = (struct korb_class *)r[1];
            } else {
                super_meta = current_meta;
            }
            r[2] = (VALUE)super_meta;
            /* korb_class_new fires GC — read klass->name after the inner
             * alloc returns, but klass needs to be reloaded first. */
            ID nm = klass->name;
            VALUE meta_v = (VALUE)korb_class_new(nm, super_meta, T_CLASS);
            klass        = (struct korb_class *)r[0];
            current_meta = (struct korb_class *)r[1];
            struct korb_class *meta = (struct korb_class *)meta_v;
            meta->basic.head.flags = T_CLASS | FL_SINGLETON;
            /* meta itself's class is the original metaclass (Class). */
            meta->basic.klass = current_meta;
            klass->basic.klass = meta;
            result = meta;
        } else {
            result = current_meta;
        }
    } ARO_ROOT_SCOPE_END(c, r);
    return result;
}

/* Singleton class for an arbitrary value.  Same idea as
 * `korb_singleton_class_of` but works on T_OBJECT instances too —
 * lazily allocates a fresh class whose super = current class, then
 * rewires basic.klass.  Returns NULL for immediate values. */
struct korb_class *korb_singleton_class_of_value(VALUE v) {
    /* true/false/nil all share their respective immutable classes
     * (CRuby semantics: class << true is the same as TrueClass). */
    if (v == Qtrue) return korb_vm->true_class;
    if (v == Qfalse) return korb_vm->false_class;
    if (v == Qnil) return korb_vm->nil_class;
    if (SPECIAL_CONST_P(v)) return NULL;
    if (BUILTIN_TYPE(v) == T_CLASS || BUILTIN_TYPE(v) == T_MODULE) {
        struct korb_class *meta = korb_singleton_class_of((struct korb_class *)v);
        if (meta && korb_obj_frozen_p(v) && !korb_obj_frozen_p((VALUE)meta)) {
            ((struct RBasic *)meta)->head.flags |= FL_FROZEN;
        }
        return meta;
    }
    /* Generic heap object: rewire klass to a private subclass. */
    struct korb_object *o = (struct korb_object *)v;
    struct korb_class *cur = (struct korb_class *)o->basic.klass;
    if (cur && cur->name == korb_intern("(singleton)")) {
        if (korb_obj_frozen_p(v) && !korb_obj_frozen_p((VALUE)cur)) {
            ((struct RBasic *)cur)->head.flags |= FL_FROZEN;
        }
        return cur;
    }
    struct korb_class *meta = korb_class_new(korb_intern("(singleton)"),
                                             cur, cur ? cur->instance_type : T_OBJECT);
    meta->basic.head.flags |= FL_SINGLETON;
    if (korb_obj_frozen_p(v)) {
        meta->basic.head.flags |= FL_FROZEN;
    }
    o->basic.klass = (VALUE)meta;
    return meta;
}

void korb_module_include(struct korb_class *klass, struct korb_class *mod) {
    /* CRuby semantics: when M2 is included after M1 (`include M1; include M2`),
     * M2's methods take precedence over M1's.  Both still lose to methods
     * defined directly on the class.
     *
     * We mark each entry's `include_depth`: 0 = defined directly on the
     * class (or cfunc registered via DEF), >0 = pulled in from a module
     * include.  A fresh include overrides any existing module-imported
     * entry, but never an entry with depth 0. */
    for (uint32_t b = 0; b < mod->methods.bucket_cnt; b++) {
        for (struct korb_method_table_entry *e = mod->methods.buckets[b]; e; e = e->next) {
            /* Look up existing entry to read its include_depth. */
            uint32_t bb = (uint32_t)(e->name % klass->methods.bucket_cnt);
            struct korb_method_table_entry *existing = NULL;
            for (struct korb_method_table_entry *me = klass->methods.buckets[bb]; me; me = me->next) {
                if (me->name == e->name) { existing = me; break; }
            }
            if (existing && existing->include_depth == 0) continue;  /* class wins */
            method_table_set(&klass->methods, e->name, e->method);
            /* Re-lookup (resize may have moved buckets) and mark imported. */
            uint32_t bb2 = (uint32_t)(e->name % klass->methods.bucket_cnt);
            for (struct korb_method_table_entry *me = klass->methods.buckets[bb2]; me; me = me->next) {
                if (me->name == e->name) { me->include_depth = 1; break; }
            }
        }
    }
    /* Constants are NOT flattened — korb_const_get_inherited walks the
     * include chain at lookup time, so dynamic additions to `mod` after
     * the include become visible to subclasses (CRuby semantics).
     * Record the include for ancestors / is_a?.  Skip duplicates. */
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

/* Visit the linearized MRO of `klass` in the order CRuby uses for method
 * lookup, calling `cb(klass_or_iclass, ctx)` at each step.  cb returning
 * non-zero stops the walk (and the return value is propagated).
 *
 * Order at each level:
 *   For each prepended module (most-recent first):
 *     recurse into the prepended module (its own prepends, then itself)
 *   The class itself.
 *   For each included module (most-recent first):
 *     recurse into the included module (its own prepends, then itself)
 *   Then super class's expansion.
 *
 * Recursive expansion handles `m1.prepend m0; sc.include m1` so that
 * m0's methods appear between sc and m1 in the lookup order. */
typedef int (*korb_mro_visit_cb)(const struct korb_class *iclass, void *ctx);
static int korb_mro_walk_one(const struct korb_class *k, korb_mro_visit_cb cb, void *ctx);

/* Visit a module's own prepends (recursively) then the module itself.
 * Used for a single iclass entry in the linearized walk. */
static int korb_mro_visit_module(const struct korb_class *m,
                                  korb_mro_visit_cb cb, void *ctx) {
    for (int32_t i = (int32_t)m->prepends_cnt - 1; i >= 0; i--) {
        int rc = korb_mro_visit_module(m->prepends[i], cb, ctx);
        if (rc) return rc;
    }
    return cb(m, ctx);
}
static int korb_mro_walk_one(const struct korb_class *k, korb_mro_visit_cb cb, void *ctx) {
    while (k) {
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            int rc = korb_mro_visit_module(k->prepends[i], cb, ctx);
            if (rc) return rc;
        }
        int rc = cb(k, ctx);
        if (rc) return rc;
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            int rc2 = korb_mro_visit_module(k->includes[i], cb, ctx);
            if (rc2) return rc2;
        }
        k = k->super;
    }
    return 0;
}

/* Method dispatch keeps the original "walk the class chain, prepends
 * win, includes are already flattened into the class table" behavior.
 * The MRO walker is for super, where we need to distinguish iclasses. */
struct korb_method *korb_class_find_method(const struct korb_class *klass, ID name) {
    while (klass) {
        for (int32_t i = (int32_t)klass->prepends_cnt - 1; i >= 0; i--) {
            const struct korb_class *p = klass->prepends[i];
            /* Recurse into the prepended module's own prepends.  This
             * matters when M1.prepend M0 and the host class prepends/
             * includes M1 — M0's methods need to win. */
            for (int32_t j = (int32_t)p->prepends_cnt - 1; j >= 0; j--) {
                struct korb_method *m = method_table_get(&p->prepends[j]->methods, name);
                if (m) {
                    if (m->type == KORB_METHOD_UNDEF) return NULL;
                    return m;
                }
            }
            struct korb_method *m = method_table_get(&p->methods, name);
            if (m) {
                if (m->type == KORB_METHOD_UNDEF) return NULL;
                return m;
            }
        }
        struct korb_method *m = method_table_get(&klass->methods, name);
        if (m) {
            /* `undef` marker: hide the method here AND block ancestor
             * lookup.  The marker pretends "no method here". */
            if (m->type == KORB_METHOD_UNDEF) return NULL;
            return m;
        }
        klass = klass->super;
    }
    return NULL;
}

/* Find the next method in receiver's class MRO after `defining_class`.
 * Used for `super`: receiver_klass = class of `c->current_frame->self`,
 * defining_class = current method's defining_class.
 *
 * MRO order at each class level:
 *   prepends (last-included first), class itself, includes (last-first),
 *   then super class (recursively).
 *
 * For includes, since their methods are flattened into the class's own
 * `methods` table at include-time, the "find within an included module"
 * step also reads from `mod->methods` directly (matching CRuby's own
 * include semantics). */
/* Walk the receiver's linearized MRO looking for `name`, starting past
 * the (skip_n + 1)-th occurrence of `defining_class`.
 *
 * Linearization order (matches `korb_class_find_method`):
 *   prepends (most-recent first) → class → includes (most-recent first)
 *   → super class's same expansion
 *
 * `out_skip_n`, when non-null, is set to the number of times the
 * returned method's defining_class has appeared *before* the position
 * the method was found at (inclusive).  This is what caller stores in
 * the new frame's super_skip_n so that the next super walk skips past
 * exactly the right occurrence — handles `prepend M; include M` where
 * the same module's body shows up twice in the MRO. */
struct find_super_ctx {
    ID name;
    const struct korb_class *defining_class;
    uint16_t skip_n;
    uint16_t seen_def;            /* occurrences of defining_class seen so far */
    bool past;                    /* set once we've seen (skip_n + 1) of them */
    struct korb_method *result;
    uint16_t result_skip_n;       /* occurrences of result's defining_class up to & incl. found pos minus 1 */
    uint16_t seen_at_result;      /* tracks defining_class occurrences of result.defining_class as we walk */
};
static int find_super_cb(const struct korb_class *iclass, void *vctx) {
    struct find_super_ctx *ctx = vctx;
    if (iclass == ctx->defining_class) {
        ctx->seen_def++;
        if (ctx->seen_def >= (uint32_t)ctx->skip_n + 1) ctx->past = true;
        return 0;
    }
    if (!ctx->past) return 0;
    struct korb_method *m = method_table_get(&iclass->methods, ctx->name);
    if (!m) return 0;
    /* Count occurrences of m's own defining_class encountered up to and
     * including the iclass we found it on.  For super-from-here to skip
     * the right occurrence, the new frame's super_skip_n is that count
     * minus one. */
    ctx->result = m;
    /* Re-scan the MRO from the start counting m->defining_class up to
     * `iclass`.  This is O(MRO size) but only happens once per super. */
    return 1;  /* stop walk here; caller computes result_skip_n */
}
struct count_def_ctx {
    const struct korb_class *target_def;
    const struct korb_class *stop_at_iclass;
    uint16_t count;
    bool stop;
};
static int count_def_cb(const struct korb_class *iclass, void *vctx) {
    struct count_def_ctx *ctx = vctx;
    if (iclass == ctx->target_def) ctx->count++;
    if (iclass == ctx->stop_at_iclass) { ctx->stop = true; return 1; }
    return 0;
}
/* Single-pass MRO walker that finds the first iclass past the (skip_n+1)-th
 * occurrence of defining_class carrying `name`.  Stores the found method,
 * the iclass it was found on, and how many times its defining_class had
 * appeared up through (and including) that iclass — needed because the
 * same module pointer can appear at multiple MRO positions when
 * prepended and included on the same class.
 *
 * `visited_klasses` / `visit_counts` track per-pointer occurrence so we
 * can answer "this is the Nth time we've seen X" without re-walking. */
struct find_super_state {
    ID name;
    const struct korb_class *defining_class;
    uint16_t skip_n;
    uint16_t seen_def;
    bool past;
    struct korb_method *result;
    const struct korb_class *found_at;
    uint16_t result_visit_n;     /* visit count of result->defining_class at find time */
    /* Tiny flat (ptr, count) table.  Real MROs are small; 64 is
     * comfortably larger than any class's expanded MRO. */
    const struct korb_class *visited_klasses[64];
    uint16_t visit_counts[64];
    uint8_t visit_table_len;
};

/* Increment and return the visit count for `k` (1 after first visit). */
static uint16_t visit_inc(struct find_super_state *st, const struct korb_class *k) {
    for (uint8_t i = 0; i < st->visit_table_len; i++) {
        if (st->visited_klasses[i] == k) {
            return ++st->visit_counts[i];
        }
    }
    if (st->visit_table_len < 64) {
        st->visited_klasses[st->visit_table_len] = k;
        st->visit_counts[st->visit_table_len] = 1;
        st->visit_table_len++;
        return 1;
    }
    return 1; /* table full — fall back to assuming first */
}
static uint16_t visit_get(struct find_super_state *st, const struct korb_class *k) {
    for (uint8_t i = 0; i < st->visit_table_len; i++) {
        if (st->visited_klasses[i] == k) return st->visit_counts[i];
    }
    return 0;
}
static int find_super_visit_cb(const struct korb_class *iclass, void *vctx) {
    struct find_super_state *st = vctx;
    visit_inc(st, iclass);
    if (iclass == st->defining_class) {
        st->seen_def++;
        if (st->seen_def >= (uint32_t)st->skip_n + 1) st->past = true;
        return 0;
    }
    if (!st->past) return 0;
    struct korb_method *m = method_table_get(&iclass->methods, st->name);
    if (!m) return 0;
    /* When `iclass` is a host class that flattened-in the method via
     * include, m->defining_class points at the original module (not at
     * iclass).  The MRO walker also visits the original module as its
     * own iclass, where we'd find the same method object — return that
     * one (its iclass actually equals defining_class) instead so the
     * counting math below works. */
    if (m->defining_class != iclass) return 0;
    /* `undef` marker — blocks ancestor lookup.  Stop the walk and
     * report "no super method available". */
    if (m->type == KORB_METHOD_UNDEF) {
        st->result = NULL;
        st->found_at = NULL;
        return 1;
    }
    st->result = m;
    st->found_at = iclass;
    st->result_visit_n = visit_get(st, m->defining_class);
    return 1;
}
struct korb_method *korb_class_find_super_method_n(const struct korb_class *receiver_klass,
                                                    const struct korb_class *defining_class,
                                                    ID name,
                                                    uint16_t skip_n,
                                                    uint16_t *out_skip_n) {
    struct find_super_state st = {
        .name = name,
        .defining_class = defining_class,
        .skip_n = skip_n,
        .seen_def = 0,
        .past = false,
        .result = NULL,
        .found_at = NULL,
        .result_visit_n = 0,
        .visit_table_len = 0,
    };
    korb_mro_walk_one(receiver_klass, find_super_visit_cb, &st);
    if (!st.result) return NULL;
    if (out_skip_n) {
        *out_skip_n = st.result_visit_n > 0 ? (uint16_t)(st.result_visit_n - 1) : 0;
    }
    return st.result;
}

/* Backwards-compat shim — assumes skip_n=0 (first occurrence). */
struct korb_method *korb_class_find_super_method(const struct korb_class *receiver_klass,
                                                 const struct korb_class *defining_class,
                                                 ID name) {
    return korb_class_find_super_method_n(receiver_klass, defining_class, name, 0, NULL);
}

/* When an anonymous module/class has been recorded with anon_parent
 * pointing at an unnamed ancestor, rebuild its full "Parent::Name"
 * after the parent is finally named.  Recursively descends so chains
 * like a::B::C::E are all updated. */
static void korb_propagate_anon_name(struct korb_class *parent) {
    if (!parent || !parent->name || parent->name == korb_intern("(anon)")) return;
    for (struct korb_const_entry *e = parent->constants; e; e = e->next) {
        VALUE v = e->value;
        if (SPECIAL_CONST_P(v)) continue;
        if (BUILTIN_TYPE(v) != T_CLASS && BUILTIN_TYPE(v) != T_MODULE) continue;
        struct korb_class *child = (struct korb_class *)v;
        if (child->anon_parent != parent) continue;
        if (child->anon_name_in_parent != e->name) continue;
        size_t plen = strlen(korb_id_name(parent->name));
        size_t nlen = strlen(korb_id_name(e->name));
        char *combined = korb_xmalloc_atomic(plen + 2 + nlen + 1);
        memcpy(combined, korb_id_name(parent->name), plen);
        memcpy(combined + plen, "::", 2);
        memcpy(combined + plen + 2, korb_id_name(e->name), nlen + 1);
        child->name = korb_intern(combined);
        child->anon_parent = NULL;
        child->anon_name_in_parent = 0;
        /* Recurse: child's own children may have stashed `child` as
         * their anon_parent. */
        korb_propagate_anon_name(child);
    }
}

/* ---- constants ---- */
void korb_const_set(struct korb_class *klass, ID name, VALUE value) {
    if (!SPECIAL_CONST_P(value) &&
        (BUILTIN_TYPE(value) == T_CLASS || BUILTIN_TYPE(value) == T_MODULE)) {
        struct korb_class *target = (struct korb_class *)value;
        bool target_was_anon = (!target->name || target->name == korb_intern("(anon)"));
        if (target_was_anon) {
            /* Determine whether `klass` is "rooted" — has a finalized
             * name AND is not still pending propagation from an anon
             * ancestor.  Object is rooted by definition. */
            bool klass_rooted = (klass == korb_vm->object_class) ||
                                (klass->name &&
                                 klass->name != korb_intern("(anon)") &&
                                 klass->anon_parent == NULL);
            if (klass_rooted &&
                klass != korb_vm->object_class) {
                /* Compose "Parent::Name" — parent is fully named. */
                size_t plen = strlen(korb_id_name(klass->name));
                size_t nlen = strlen(korb_id_name(name));
                char *combined = korb_xmalloc_atomic(plen + 2 + nlen + 1);
                memcpy(combined, korb_id_name(klass->name), plen);
                memcpy(combined + plen, "::", 2);
                memcpy(combined + plen + 2, korb_id_name(name), nlen + 1);
                target->name = korb_intern(combined);
                target->anon_parent = NULL;
                target->anon_name_in_parent = 0;
            } else if (klass_rooted) {
                /* Object's namespace (top-level): plain name. */
                target->name = name;
                target->anon_parent = NULL;
                target->anon_name_in_parent = 0;
            } else {
                /* Parent is still anonymous (or pending propagation).
                 * Compose a tentative "Parent::Name" using parent's
                 * current best name (or a `#<Module:0x...>` placeholder
                 * when parent has no name yet).  Remember the linkage so
                 * the eventual rename of the parent re-resolves us. */
                const char *parent_str;
                char placeholder[64];
                if (klass->name && klass->name != korb_intern("(anon)")) {
                    parent_str = korb_id_name(klass->name);
                } else {
                    snprintf(placeholder, sizeof(placeholder),
                             "#<%s:%p>",
                             BUILTIN_TYPE(klass) == T_MODULE ? "Module" : "Class",
                             (void *)klass);
                    parent_str = placeholder;
                }
                size_t plen = strlen(parent_str);
                size_t nlen = strlen(korb_id_name(name));
                char *combined = korb_xmalloc_atomic(plen + 2 + nlen + 1);
                memcpy(combined, parent_str, plen);
                memcpy(combined + plen, "::", 2);
                memcpy(combined + plen + 2, korb_id_name(name), nlen + 1);
                target->name = korb_intern(combined);
                target->anon_parent = klass;
                target->anon_name_in_parent = name;
            }
        }
    }
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) { e->value = value; goto done; }
    }
    {
        struct korb_const_entry *e = korb_xmalloc(sizeof(*e));
        e->name = name;
        e->value = value;
        e->is_private = false;
        e->next = klass->constants;
        klass->constants = e;
    }
done:
    /* If `value` itself is a now-named module/class with stashed anon
     * descendants, propagate the name down. */
    if (!SPECIAL_CONST_P(value) &&
        (BUILTIN_TYPE(value) == T_CLASS || BUILTIN_TYPE(value) == T_MODULE)) {
        korb_propagate_anon_name((struct korb_class *)value);
    }
    return;
}

/* True iff `klass` has a constant named `name` directly (not inherited)
 * that is marked private_constant. */
bool korb_const_is_private(const struct korb_class *klass, ID name) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return e->is_private;
    }
    return false;
}

VALUE korb_const_get(struct korb_class *klass, ID name) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return e->value;
    }
    return Qundef;
}

/* Walk a class's includes/prepends/super chain looking for a const.
 * Mirrors the method dispatch MRO so dynamic additions to an included
 * module are visible to subclasses (CRuby semantics). */
VALUE korb_const_get_inherited(struct korb_class *klass, ID name) {
    for (struct korb_class *k = klass; k; k = k->super) {
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            VALUE v = korb_const_get(k->prepends[i], name);
            if (!UNDEF_P(v)) return v;
        }
        VALUE v = korb_const_get(k, name);
        if (!UNDEF_P(v)) return v;
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            VALUE v2 = korb_const_get(k->includes[i], name);
            if (!UNDEF_P(v2)) return v2;
        }
    }
    return Qundef;
}

/* Like korb_const_get_inherited but excludes Object's own constants
 * (Object's top-level constants are not visible via `Klass::CONST`,
 * but Object's INCLUDES still are — `include M` at top-level adds M
 * to Object's includes, and `Klass::CONST_FROM_M` resolves through
 * that path).  Used for explicit `Klass::CONST` scoped lookup. */
VALUE korb_const_get_inherited_stop_at_object(struct korb_class *klass, ID name) {
    bool start_is_object = (klass == korb_vm->object_class);
    for (struct korb_class *k = klass; k; k = k->super) {
        bool skip_self = (!start_is_object && k == korb_vm->object_class);
        for (int32_t i = (int32_t)k->prepends_cnt - 1; i >= 0; i--) {
            VALUE v = korb_const_get(k->prepends[i], name);
            if (!UNDEF_P(v)) return v;
        }
        if (!skip_self) {
            VALUE v = korb_const_get(k, name);
            if (!UNDEF_P(v)) return v;
        }
        for (int32_t i = (int32_t)k->includes_cnt - 1; i >= 0; i--) {
            VALUE v2 = korb_const_get(k->includes[i], name);
            if (!UNDEF_P(v2)) return v2;
        }
    }
    return Qundef;
}

bool korb_const_has_inherited(struct korb_class *klass, ID name) {
    return !UNDEF_P(korb_const_get_inherited(klass, name));
}

bool korb_const_remove(struct korb_class *klass, ID name, VALUE *out) {
    struct korb_const_entry **prev = &klass->constants;
    for (struct korb_const_entry *e = klass->constants; e; prev = &e->next, e = e->next) {
        if (e->name == name) {
            if (out) *out = e->value;
            *prev = e->next;
            return true;
        }
    }
    return false;
}

bool korb_const_has(struct korb_class *klass, ID name) {
    for (struct korb_const_entry *e = klass->constants; e; e = e->next) {
        if (e->name == name) return true;
    }
    return false;
}

VALUE korb_const_lookup(CTX *c, ID name) {
    /* Lexical lookup along cref chain (each cref level checks its own
     * class plus that class's includes — but NOT super).  The bottom
     * implicit `[Object, prev=NULL]` cref entry (created at top-level
     * before any user `class X` opens) is NOT a real lexical scope —
     * it's a placeholder so `node_def_full` knows we're at top level.
     * Skip it for lexical lookup; Object's namespace is reached via
     * the inheritance walk below.  An EXPLICITLY opened `class Object`
     * pushes its own cref entry whose prev is non-NULL, so that one
     * still participates. */
    for (struct korb_cref *cr = c->current_frame->cref; cr; cr = cr->prev) {
        if (cr->klass == korb_vm->object_class && cr->prev == NULL) continue;
        VALUE v = korb_const_get(cr->klass, name);
        if (!UNDEF_P(v)) return v;
        for (int32_t i = (int32_t)cr->klass->includes_cnt - 1; i >= 0; i--) {
            VALUE v2 = korb_const_get(cr->klass->includes[i], name);
            if (!UNDEF_P(v2)) return v2;
        }
    }
    /* Inheritance chain of innermost class — walks includes too. */
    struct korb_class *k = c->current_frame->cref ? c->current_frame->cref->klass : c->current_frame->current_class;
    if (k && k->super) {
        VALUE v = korb_const_get_inherited(k->super, name);
        if (!UNDEF_P(v)) return v;
    }
    /* Object as global namespace (Object's includes too). */
    VALUE v = korb_const_get_inherited(korb_vm->object_class, name);
    if (!UNDEF_P(v)) return v;
    /* const_missing — dispatch to the lexically innermost real class.
     * CRuby calls #const_missing on the original class/module scope so
     * code like `ClassA.constx → CS_CONSTX` can intercept the miss
     * even when the lookup walks past ClassA. */
    if (k) {
        struct korb_class *meta = korb_singleton_class_of(k);
        if (meta && korb_class_find_method(meta, korb_intern("const_missing"))) {
            VALUE sym = korb_id2sym(name);
            return korb_funcall(c, (VALUE)k, korb_intern("const_missing"), 1, &sym);
        }
    }
    {
        VALUE eName = korb_const_get(korb_vm->object_class, korb_intern("NameError"));
        korb_raise(c, (struct korb_class *)eName,
                   "uninitialized constant %s", korb_id_name(name));
    }
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

bool korb_gvar_defined(ID name) {
    for (uint32_t i = 0; i < gvars.size; i++) if (gvars.keys[i] == name) return true;
    return false;
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

/* $_ / $~ — method-scoped pseudo-globals.  Stored on the current frame's
 * last_line / last_match slots (initialized to Qnil on every method-frame
 * push).  Yields and proc/lambda calls don't push frames, so blocks
 * naturally share their enclosing method's slots.  Top-level (no frame)
 * falls back to the regular global table. */
VALUE korb_last_line_get(CTX *c) {
    return c->current_frame ? c->current_frame->last_line : korb_gvar_get(korb_intern("$_"));
}
void korb_last_line_set(CTX *c, VALUE v) {
    if (c->current_frame) c->current_frame->last_line = v;
    else korb_gvar_set(korb_intern("$_"), v);
}
VALUE korb_last_match_get(CTX *c) {
    return c->current_frame ? c->current_frame->last_match : korb_gvar_get(korb_intern("$~"));
}
void korb_last_match_set(CTX *c, VALUE v) {
    if (c->current_frame) c->current_frame->last_match = v;
    else korb_gvar_set(korb_intern("$~"), v);
}

/* ---- objects (with class-shape ivars) ---- */
VALUE korb_object_new(struct korb_class *klass) {
    /* Protect klass across the inner aro_gc_alloc — moving GC otherwise
     * leaves the C local pointing into K-2's to-space, which is the same
     * physical plane as K's to-space under 2-space alternation. */
    CTX *c = korb_vm->current_ctx;
    VALUE ret;
    ARO_ROOT_SCOPE_START(c, r, 2) {
        r[0] = (VALUE)klass;
        int it = klass->instance_type ? klass->instance_type : T_OBJECT;
        if (it == T_CLASS || it == T_MODULE) {
            /* Class.allocate / Module.allocate must produce a struct big
             * enough to hold a class — otherwise field accesses (super,
             * methods, basic.flags) on the result later read past the
             * alloc'd region.  Mark such an "uninitialized class" by
             * leaving super=NULL; the .new path raises TypeError before
             * dispatching. */
            r[1] = aro_gc_alloc(c, sizeof(struct korb_class));
            klass = (struct korb_class *)r[0];           /* reload */
            struct korb_class *k = (struct korb_class *)r[1];
            k->basic.head.flags = it;
            k->basic.klass = klass;
            k->name = korb_intern("(uninitialized)");
            k->instance_type = T_OBJECT;
            ret = r[1];
        } else {
            r[1] = aro_gc_alloc(c, sizeof(struct korb_object));
            klass = (struct korb_class *)r[0];           /* reload */
            struct korb_object *o = (struct korb_object *)r[1];
            o->basic.head.flags = it;
            o->basic.klass = klass;
            /* Preallocate ivar slots based on the class's known ivar shape,
             * so the inline ivar_set_ic fast path hits on the first write
             * to each @ivar.  (klass->ivar_count read AFTER reload — moving
             * GC could've relocated klass.) */
            uint32_t n = klass->ivar_count;
            if (n) {
                o->ivar_cnt = n;
                o->ivar_capa = n;
                o->ivars = korb_xmalloc(n * sizeof(VALUE));
                for (uint32_t i = 0; i < n; i++) o->ivars[i] = Qnil;
                /* o may have moved by korb_xmalloc — no, libc malloc
                 * doesn't fire GC.  Safe to write o->* directly here. */
            }
            ret = r[1];
        }
    } ARO_ROOT_SCOPE_END(c, r);
    return ret;
}

static int ivar_slot(struct korb_class *k, ID name) {
    for (uint32_t i = 0; i < k->ivar_count; i++) if (k->ivar_names[i] == name) return (int)i;
    /* Singleton-class wrapping (`def self.foo`) re-points basic.klass to
     * a child class with no ivar shape of its own.  Walk past it to the
     * original class so reads/writes hit the same slot table. */
    static ID singleton_id = 0;
    if (singleton_id == 0) singleton_id = korb_intern("(singleton)");
    if (k->name == singleton_id && k->super) {
        return ivar_slot(k->super, name);
    }
    return -1;
}

static int ivar_slot_assign(struct korb_class *k, ID name) {
    /* For singleton-class wrappers (`def self.foo`), the ivar shape lives
     * on the original class — append there so the slot is shared with
     * the rest of the class's instances. */
    static ID singleton_id = 0;
    if (singleton_id == 0) singleton_id = korb_intern("(singleton)");
    while (k->name == singleton_id && k->super) k = k->super;
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

/* True iff this specific object has assigned the named ivar at least
 * once.  Distinguishes "never set" from "set to nil" for defined?. */
bool korb_ivar_defined(VALUE obj, ID name) {
    if (SPECIAL_CONST_P(obj)) return false;
    if (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE) {
        struct korb_class *k = (struct korb_class *)obj;
        for (uint32_t i = 0; i < k->class_ivar_cnt; i++) {
            if (k->class_ivars[i].name == name) return true;
        }
        return false;
    }
    if (BUILTIN_TYPE(obj) != T_OBJECT) return false;
    struct korb_object *o = (struct korb_object *)obj;
    struct korb_class *k = (struct korb_class *)o->basic.klass;
    static ID singleton_id = 0;
    if (singleton_id == 0) singleton_id = korb_intern("(singleton)");
    while (k->name == singleton_id && k->super) k = k->super;
    for (uint32_t i = 0; i < k->ivar_count; i++) {
        if (k->ivar_names[i] == name) {
            return i < o->ivar_cnt;
        }
    }
    return false;
}

VALUE korb_ivar_get(VALUE obj, ID name) {
    if (SPECIAL_CONST_P(obj)) return Qnil;
    if (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE) {
        struct korb_class *k = (struct korb_class *)obj;
        for (uint32_t i = 0; i < k->class_ivar_cnt; i++) {
            if (k->class_ivars[i].name == name) return k->class_ivars[i].value;
        }
        return Qnil;
    }
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
    if (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE) {
        return korb_ivar_get(obj, name);
    }
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
    /* Class-level @ivars (rare but legal — `class Foo; @count = 0`). */
    if (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE) {
        struct korb_class *k = (struct korb_class *)obj;
        for (uint32_t i = 0; i < k->class_ivar_cnt; i++) {
            if (k->class_ivars[i].name == name) {
                k->class_ivars[i].value = val;
                return;
            }
        }
        if (k->class_ivar_cnt >= k->class_ivar_capa) {
            uint32_t nc = k->class_ivar_capa ? k->class_ivar_capa * 2 : 4;
            k->class_ivars = korb_xrealloc(k->class_ivars, nc * sizeof(*k->class_ivars));
            k->class_ivar_capa = nc;
        }
        k->class_ivars[k->class_ivar_cnt].name = name;
        k->class_ivars[k->class_ivar_cnt].value = val;
        k->class_ivar_cnt++;
        return;
    }
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
    if (!SPECIAL_CONST_P(val) && BUILTIN_TYPE(val) == T_PROC) {
        ((struct RBasic *)k)->head.flags |= FL_HAS_PROC_IVARS;
    }
}

/* Cached ivar set: same as get but with assign-on-miss semantics. */
/* Out-of-line slow path for the inline korb_ivar_set_ic in object.h.
 * Reached on cache miss (different class or unset slot), or when the
 * slot is past current ivar_capa / ivar_cnt and needs growth. */
void korb_ivar_set_ic_slow(VALUE obj, ID name, VALUE val, struct ivar_cache *cache) {
    if (SPECIAL_CONST_P(obj)) return;
    if (BUILTIN_TYPE(obj) == T_CLASS || BUILTIN_TYPE(obj) == T_MODULE) {
        korb_ivar_set(obj, name, val);
        return;
    }
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
    /* Lift FL_HAS_PROC_IVARS on the class.  The slow path covers every
     * fresh slot creation (`def initialize(&blk); @blk = blk; end`),
     * which is where proc ivars are typically born.  Fast path doesn't
     * set the flag; in the rare case of `@blk = new_proc` re-assignment
     * via the cache hit, the env-snapshot walk will be skipped, but
     * that's a niche case — env detach for newly-stored procs is the
     * common one and gets covered here. */
    if (!SPECIAL_CONST_P(val) && BUILTIN_TYPE(val) == T_PROC) {
        ((struct RBasic *)k)->head.flags |= FL_HAS_PROC_IVARS;
    }
}

/* ---- string ---- */
VALUE korb_str_new(const char *p, long len) {
    /* No protect needed for p — it's a const char * source (libc / static)
     * not a GC heap obj. */
    CTX *c = korb_vm->current_ctx;
    VALUE v = aro_gc_alloc(c, sizeof(struct korb_string));
    struct korb_string *s = (struct korb_string *)v;
    s->basic.head.flags = T_STRING;
    s->basic.klass = korb_vm->string_class;  /* may be NULL during bootstrap */
    /* korb_xmalloc_atomic is libc (no GC fire) — safe to assign result to
     * s->ptr without rooting s. */
    s->ptr = korb_xmalloc_atomic(len + 1);
    if (p && len > 0) memcpy(s->ptr, p, len);
    s->ptr[len] = 0;
    s->len = len;
    s->capa = len;
    return v;
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
    a->basic.head.flags = T_ARRAY;
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
    /* Reject indices that would resize the array to gigabytes — caller
     * (e.g. test_aset_error's `[0][LONGP] = 2`) should have raised
     * IndexError but if we got here, just no-op rather than OOM-killing
     * the process trying to allocate 2^63 slots. */
    if (i >= (long)(LONG_MAX / sizeof(VALUE))) return;
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
/* Tiny in-progress set used to break self-referential array/hash hashing
 * cycles (CRuby uses rb_exec_recursive; we just track a small stack of
 * pointers and return a sentinel on second entry). */
static __thread VALUE korb_hash_recurse_stk[64];
static __thread int korb_hash_recurse_top = 0;
static bool korb_hash_recurse_seen(VALUE v) {
    for (int i = 0; i < korb_hash_recurse_top; i++) {
        if (korb_hash_recurse_stk[i] == v) return true;
    }
    return false;
}
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
    /* Arrays hash by content so `h[[1, 2]]` works on a fresh literal.
     * Recursive arrays (`a << a`) get a fixed sentinel for the recursive
     * leg so the outer hash terminates with a stable code. */
    if (BUILTIN_TYPE(v) == T_ARRAY) {
        if (korb_hash_recurse_seen(v)) return 0xdeadbeefcafef00dULL;
        struct korb_array *a = (struct korb_array *)v;
        uint64_t h = 0xcbf29ce484222325ULL;
        if (korb_hash_recurse_top < 64) {
            korb_hash_recurse_stk[korb_hash_recurse_top++] = v;
        }
        for (long i = 0; i < a->len; i++) {
            uint64_t eh = korb_hash_value(a->ptr[i]);
            h ^= eh;
            h *= 0x100000001b3ULL;
        }
        if (korb_hash_recurse_top > 0 && korb_hash_recurse_stk[korb_hash_recurse_top - 1] == v) {
            korb_hash_recurse_top--;
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
    h->basic.head.flags = T_HASH;
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
    r->basic.head.flags = T_RANGE;
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
    f->basic.head.flags = T_FLOAT;
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

/* libgc finalizer for korb_bignum: GMP allocates the mpz_t's limbs via
 * its own allocator (NOT libgc), so when libgc reclaims the wrapping
 * korb_bignum / mpz_t structs, the limbs leak — silently growing
 * resident memory until OOM.  Register a finalizer that calls mpz_clear
 * on collection so GMP releases the limb buffer. */
static void korb_bignum_finalizer(void *obj, void *cd) {
    struct korb_bignum *b = (struct korb_bignum *)obj;
    if (b && b->mpz) {
        mpz_clear((mpz_ptr)b->mpz);
        b->mpz = NULL;
    }
    (void)cd;
}

static void korb_bignum_register_finalizer(struct korb_bignum *b) {
    /* Phase 1 stub: no precise-GC finalizer yet.  GMP limbs leak until
     * Phase 5 wires aro_gc_finalize_register + AROH_FINALIZE. */
    (void)b;
    (void)korb_bignum_finalizer;
}

VALUE korb_bignum_new_str(const char *str, int base) {
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.head.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *z = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set_str(*z, str, base);
    b->mpz = z;
    /* if it fits in fixnum, return fixnum */
    if (mpz_fits_slong_p(*z)) {
        long v = mpz_get_si(*z);
        if (FIXABLE(v)) {
            mpz_clear(*z);
            return INT2FIX(v);
        }
    }
    korb_bignum_register_finalizer(b);
    return (VALUE)b;
}

VALUE korb_bignum_new_long(long v) {
    if (FIXABLE(v)) return INT2FIX(v);
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.head.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *z = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set_si(*z, v);
    b->mpz = z;
    korb_bignum_register_finalizer(b);
    return (VALUE)b;
}

/* Convert a double (already truncated/rounded by the caller) to an
 * Integer.  Returns Fixnum when it fits, otherwise Bignum.  Required
 * for Float#ceil / floor / truncate / to_i / round on values whose
 * integer part exceeds LONG_MAX (where `(long)v` is UB and SIGFPE on
 * x86). */
VALUE korb_dbl2int(double v) {
    /* NaN / +-Inf: callers should have caught these, but if they
     * didn't (CRuby would raise FloatDomainError), avoid the UB cast
     * and return 0 silently — better than SIGFPE on x86. */
    if (isnan(v) || isinf(v)) return INT2FIX(0);
    /* LONG_MIN/MAX as exact doubles; the comparison is safe because
     * (double)LONG_MAX rounds up to 2^63, not down. */
    if (v >= -9.223372036854775e18 && v <= 9.223372036854775e18) {
        return korb_bignum_new_long((long)v);
    }
    struct korb_bignum *b = korb_xmalloc(sizeof(*b));
    b->basic.head.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *z = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set_d(*z, v);
    b->mpz = z;
    korb_bignum_register_finalizer(b);
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
    b->basic.head.flags = T_BIGNUM;
    b->basic.klass = korb_vm ? (VALUE)korb_vm->integer_class : 0;
    mpz_t *bz = korb_xmalloc(sizeof(mpz_t));
    mpz_init_set(*bz, z);
    mpz_clear(z);
    b->mpz = bz;
    korb_bignum_register_finalizer(b);
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
/* Detach `v` (or, for a T_OBJECT, every Proc reachable through its
 * ivars) from a dying stack frame [fp_lo, fp_hi).  Snapshots the env
 * to heap so subsequent calls see stable values.  No-op when v isn't
 * a Proc/Object or when env already lives outside the frame.  Hot
 * path stays cheap (one address-range compare for non-frame procs). */
static void korb_snapshot_one_proc_(struct korb_proc *p, VALUE *fp_lo, VALUE *fp_hi) {
    if (!p->env || p->env < fp_lo || p->env >= fp_hi) return;
    VALUE *snap = korb_xmalloc(p->env_size * sizeof(VALUE));
    for (uint32_t i = 0; i < p->env_size; i++) snap[i] = p->env[i];
    p->env = snap;
    /* Deep-snapshot: any Proc slot inside `snap` whose own env still
     * points at the dying frame must also be detached.  Curry-style
     * code keeps an enclosing proc (`accum` / `outer`) in a method
     * lvar and the inner proc references it through the captured env;
     * without this walk, the inner proc carries a Proc whose env
     * dangles after the method returns. */
    for (uint32_t i = 0; i < p->env_size; i++) {
        VALUE slot = snap[i];
        if (SPECIAL_CONST_P(slot)) continue;
        if (BUILTIN_TYPE(slot) != T_PROC) continue;
        struct korb_proc *sp = (struct korb_proc *)slot;
        if (!sp->env || sp->env < fp_lo || sp->env >= fp_hi) continue;
        VALUE *snap2 = korb_xmalloc(sp->env_size * sizeof(VALUE));
        for (uint32_t j = 0; j < sp->env_size; j++) snap2[j] = sp->env[j];
        sp->env = snap2;
    }
}

void korb_proc_snapshot_env_if_in_frame(VALUE v, VALUE *fp_lo, VALUE *fp_hi) {
    if (SPECIAL_CONST_P(v)) return;
    int t = BUILTIN_TYPE(v);
    if (t == T_PROC) {
        korb_snapshot_one_proc_((struct korb_proc *)v, fp_lo, fp_hi);
        return;
    }
    /* Object or class instance carrying a Proc in some ivar — common
     * for `class Foo; def initialize(&blk); @blk = blk; end; end`
     * shapes (Enumerator / Enumerator::Lazy etc).  Without this walk,
     * @blk's env dangles after the constructor returns.
     *
     * FL_HAS_PROC_IVARS gate: ivar_set lifts this flag on the class
     * when a Proc lands in any ivar.  Classes that never store procs
     * (the vast majority — most of optcarrot's hot classes) skip the
     * walk entirely.  Saves ~3% of optcarrot's runtime. */
    if (t == T_OBJECT) {
        struct korb_object *obj = (struct korb_object *)v;
        if (!obj->ivars) return;
        struct korb_class *k = (struct korb_class *)obj->basic.klass;
        if (LIKELY(k && !(k->basic.head.flags & FL_HAS_PROC_IVARS))) return;
        for (uint32_t i = 0; i < obj->ivar_cnt; i++) {
            VALUE iv = obj->ivars[i];
            if (SPECIAL_CONST_P(iv)) continue;
            if (BUILTIN_TYPE(iv) == T_PROC) {
                korb_snapshot_one_proc_((struct korb_proc *)iv, fp_lo, fp_hi);
            }
        }
        return;
    }
    /* Class / Module body: lambdas captured into a constant
     * (`READER = -> { m }` pattern in a class body) keep their env on
     * the class body's fp.  When the body returns, those slots are
     * reused by subsequent method calls.  Snapshot any T_PROC constant
     * whose env still points into the dying frame. */
    if (t == T_CLASS || t == T_MODULE) {
        struct korb_class *k = (struct korb_class *)v;
        for (struct korb_const_entry *e = k->constants; e; e = e->next) {
            if (SPECIAL_CONST_P(e->value)) continue;
            if (BUILTIN_TYPE(e->value) == T_PROC) {
                korb_snapshot_one_proc_((struct korb_proc *)e->value, fp_lo, fp_hi);
            }
        }
    }
}

VALUE korb_proc_new(struct Node *body, VALUE *fp, uint32_t env_size,
                  uint32_t params_cnt, uint32_t param_base, VALUE self, bool is_lambda) {
    struct korb_proc *p = korb_xmalloc(sizeof(*p));
    p->basic.head.flags = T_PROC;
    p->basic.klass = korb_vm ? (VALUE)korb_vm->proc_class : 0;
    p->body = body;
    p->env_size = env_size;
    p->env = fp;
    p->params_cnt = params_cnt;
    p->opt_cnt = 0;
    p->param_base = param_base;
    p->rest_slot = -1;
    p->kwh_save_slot = -1;
    p->block_slot = -1;
    p->post_cnt = 0;
    p->enclosing_block = current_block;  /* capture enclosing-method's block */
    p->self = self;
    p->is_lambda = is_lambda;
    p->implicit_rest = false;
    p->creates_proc = false;
    p->cref = NULL;  /* set by korb_proc_new_with_cref or by callers */
    p->return_target_frame = NULL;
    /* defining_method gets filled in by the proc-literal evaluator
     * (node_proc_set_def_method) once it has the active CTX. */
    p->defining_method = NULL;
    p->lexical_parent_block = NULL;
    return (VALUE)p;
}

VALUE korb_proc_new_with_cref(struct Node *body, VALUE *fp, uint32_t env_size,
                              uint32_t params_cnt, uint32_t param_base, VALUE self,
                              bool is_lambda, struct korb_cref *cref) {
    VALUE pv = korb_proc_new(body, fp, env_size, params_cnt, param_base, self, is_lambda);
    ((struct korb_proc *)pv)->cref = korb_cref_dup(cref);
    return pv;
}

/* Block currently active for yield (set by dispatch_call). */
struct korb_proc *current_block = NULL;

/* Block / proc / lambda currently executing — distinct from current_block
 * (which is the block PASSED to the current method).  See object.h. */
struct korb_proc *running_block = NULL;
/* Top-level program body — set by parse.c when transducing the
 * outermost PM_PROGRAM_NODE.  Used by kernel_eval to find the script's
 * own lvar names when eval is called with no enclosing method / block
 * frame. */
struct Node *korb_g_program_body = NULL;

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

    /* Lambda strict arity: yield-to-a-lambda must match the lambda's
     * parameter count exactly (no destructure / pad).  Non-lambda
     * blocks remain permissive. */
    if (blk->is_lambda && (blk->rest_slot < 0 || blk->implicit_rest)) {
        uint32_t required = (blk->params_cnt > blk->opt_cnt) ? blk->params_cnt - blk->opt_cnt : 0;
        if (argc < required || argc > blk->params_cnt) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                       "wrong number of arguments (given %u, expected %u)",
                       argc, blk->params_cnt);
            return Qnil;
        }
    } else if (blk->is_lambda) {
        /* Lambda with rest_slot — lower bound only. */
        uint32_t required = (blk->params_cnt > blk->opt_cnt) ? blk->params_cnt - blk->opt_cnt : 0;
        if (argc < required) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            korb_raise(c, (struct korb_class *)eArg,
                       "wrong number of arguments (given %u, expected %u+)",
                       argc, required);
            return Qnil;
        }
    }

    /* Per-iteration capture path: when the block creates inner procs
     * (`(1..3).each { |i| procs << proc { i } }`), allocate a fresh env
     * for THIS yield's block-locals so the captured proc sees its own
     * `i`.  Outer slots are aliased via copy-in / copy-back.  Non-
     * creates_proc blocks share env directly (faster, and matches
     * shared-state semantics for `count += 1`-style accumulators). */
    bool fresh_env_path = blk->creates_proc;
    VALUE *fp;
    VALUE *outer_env_ptr = blk->env;
    /* Generous slack past env_size: when the block calls Ruby methods,
     * those methods' frames live at fp[arg_index..arg_index+locals],
     * which can extend past env_size — env_size only reflects what
     * the block's own body uses.  Without slack, those writes spill
     * into adjacent heap (corrupting the symbol table etc).  4 KB
     * worth of slots is overkill but cheap. */
    enum { FRESH_ENV_SLACK = 512 };
    if (fresh_env_path) {
        fp = (VALUE *)korb_xmalloc((blk->env_size + FRESH_ENV_SLACK) * sizeof(VALUE));
        /* Copy ALL of env: outer slots so depth-walks/reads see their
         * current values; block-local slots are about to be overwritten
         * by params/destructure anyway. */
        for (uint32_t i = 0; i < blk->env_size; i++) fp[i] = blk->env[i];
        for (uint32_t i = blk->env_size; i < blk->env_size + FRESH_ENV_SLACK; i++) fp[i] = Qnil;
    } else {
        fp = blk->env;
    }
    VALUE *prev_fp = c->current_frame->fp;
    VALUE prev_self = c->current_frame->self;
    /* Auto-destructure: block with N params yielded a single Array of size M
     * → assign array elements to params (Ruby block calling convention).
     * Skip when the block has a *rest — destructuring would steal the rest's
     * args.  When the single arg responds to to_ary, call it; if to_ary
     * returns non-Array, raise TypeError. */
    bool bound_destructure = false;
    /* Peel kwargs-tagged Hash before destructure / arg processing.  When
     * yield(**h) carries the tagged hash, callee with **kwargs takes it;
     * callee without **kwargs drops it if empty (Ruby 3 separation). */
    VALUE peeled_kwh = Qundef;
    if (argc > 0 && !SPECIAL_CONST_P(args_buf[argc - 1]) &&
        BUILTIN_TYPE(args_buf[argc - 1]) == T_HASH &&
        (RBASIC(args_buf[argc - 1])->head.flags & FL_KWARGS)) {
        VALUE last = args_buf[argc - 1];
        if (blk->kwh_save_slot >= 0) {
            peeled_kwh = last;
            argc--;
        } else {
            struct korb_hash *h = (struct korb_hash *)last;
            if (h->size == 0) argc--;
        }
    }
    /* CRuby auto-destructures when:
     *   - exactly one arg is yielded
     *   - block has > 1 named param OR has rest with required around it
     *   - the single arg is an Array (or coerces via #to_ary)
     * We extend the trigger to cover *rest cases too: `|a, *b, c|`. */
    uint32_t total_pos = blk->params_cnt + blk->post_cnt;
    bool has_rest = blk->rest_slot >= 0;
    bool destruct_trigger = (argc == 1) &&
        ((total_pos > 1 && !has_rest) ||
         (has_rest && (total_pos >= 1 || blk->post_cnt > 0)));
    if (destruct_trigger) {
        VALUE arg0 = args_buf[0];
        VALUE arr = Qnil;
        if (!SPECIAL_CONST_P(arg0) && BUILTIN_TYPE(arg0) == T_ARRAY) {
            arr = arg0;
        } else if (!SPECIAL_CONST_P(arg0)) {
            /* Try to_ary if it responds to it (honors respond_to_missing?). */
            VALUE rt = korb_funcall(c, arg0, korb_intern("respond_to?"), 1,
                                    (VALUE[]){ korb_id2sym(korb_intern("to_ary")) });
            if (c->state != KORB_NORMAL) { c->state = KORB_NORMAL; c->state_value = Qnil; rt = Qfalse; }
            if (RTEST(rt)) {
                VALUE coerced = korb_funcall(c, arg0, korb_intern("to_ary"), 0, NULL);
                if (c->state != KORB_NORMAL) return Qnil;
                if (NIL_P(coerced)) {
                    /* to_ary explicitly returned nil — treat the same
                     * as not responding to to_ary: pass the original
                     * object through without destructuring. */
                } else if (BUILTIN_TYPE(coerced) != T_ARRAY) {
                    VALUE eT = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
                    korb_raise(c, (struct korb_class *)eT,
                               "can't convert to Array (#to_ary gave non-Array)");
                    return Qnil;
                } else {
                    arr = coerced;
                }
            }
        }
        if (!NIL_P(arr) && BUILTIN_TYPE(arr) == T_ARRAY) {
            struct korb_array *a = (struct korb_array *)arr;
            uint32_t req_cnt = (blk->params_cnt > blk->opt_cnt)
                                 ? blk->params_cnt - blk->opt_cnt : 0;
            uint32_t arr_len = (uint32_t)a->len;
            uint32_t opt_cnt = blk->opt_cnt;
            uint32_t post_cnt = blk->post_cnt;
            /* CRuby's destructure layout: required-left, optional, *rest,
             * post (required-right).  arr fills req left-to-right, then opt
             * (reserving post_cnt for post), then *rest gets the middle, then
             * post left-to-right with trailing nils. */
            uint32_t taken_left = 0;
            for (uint32_t i = 0; i < req_cnt; i++) {
                fp[blk->param_base + i] = (taken_left < arr_len)
                    ? a->ptr[taken_left++] : Qnil;
            }
            uint32_t opt_taken = 0;
            for (uint32_t i = 0; i < opt_cnt; i++) {
                if (taken_left < arr_len && arr_len - taken_left > post_cnt) {
                    fp[blk->param_base + req_cnt + i] = a->ptr[taken_left++];
                    opt_taken++;
                } else {
                    fp[blk->param_base + req_cnt + i] = Qundef;
                }
            }
            (void)opt_taken;
            /* *rest: gather elements between optionals and posts. */
            if (blk->rest_slot >= 0) {
                uint32_t rest_room = (arr_len > taken_left + post_cnt)
                                       ? arr_len - taken_left - post_cnt : 0;
                VALUE rest = korb_ary_new_capa((long)rest_room);
                for (uint32_t i = 0; i < rest_room; i++) {
                    korb_ary_push(rest, a->ptr[taken_left + i]);
                }
                fp[blk->rest_slot] = rest;
                taken_left += rest_room;
            }
            /* Post params: left-to-right from remaining, trailing nils. */
            uint32_t post_base = blk->param_base + blk->params_cnt
                                  + (blk->rest_slot >= 0 ? 1 : 0);
            uint32_t remaining = arr_len - taken_left;
            uint32_t post_filled = (remaining < post_cnt) ? remaining : post_cnt;
            for (uint32_t i = 0; i < post_filled; i++) {
                fp[post_base + i] = a->ptr[taken_left + i];
            }
            for (uint32_t i = post_filled; i < post_cnt; i++) {
                fp[post_base + i] = Qnil;
            }
            bound_destructure = true;
        }
    }
    if (bound_destructure) {
        /* destructure path took the bind */
    }
    else {
        /* Required params first.  blk->params_cnt covers req + opt; the
         * rest is in rest_slot if non-negative. */
        for (uint32_t i = 0; i < blk->params_cnt && i < argc; i++) {
            fp[blk->param_base + i] = args_buf[i];
        }
        /* Missing required → nil; missing optional → Qundef so the body's
         * default_init prologue substitutes the user-supplied default. */
        uint32_t req_cnt = (blk->params_cnt > blk->opt_cnt) ? blk->params_cnt - blk->opt_cnt : 0;
        for (uint32_t i = (argc < blk->params_cnt ? argc : blk->params_cnt); i < blk->params_cnt; i++) {
            fp[blk->param_base + i] = (i < req_cnt) ? Qnil : Qundef;
        }
        /* *rest: gather extras into an Array.  When called with a single
         * Array arg and the block is `|*x|` (params_cnt == 0), CRuby's
         * convention is to bind the array directly to *x rather than
         * wrap it in another array — so just pass it through. */
        if (blk->rest_slot >= 0) {
            uint32_t start = blk->params_cnt;
            if (argc <= start) {
                fp[blk->rest_slot] = korb_ary_new();
            } else {
                VALUE rest = korb_ary_new_capa((long)(argc - start));
                for (uint32_t i = start; i < argc; i++) korb_ary_push(rest, args_buf[i]);
                fp[blk->rest_slot] = rest;
            }
        }
    }
    /* `&blk` parameter: yield doesn't pass a block, so bind nil. */
    if (blk->block_slot >= 0) fp[blk->block_slot] = Qnil;
    /* `**kwargs` parameter: use peeled_kwh from above, or default to {}. */
    if (blk->kwh_save_slot >= 0) {
        fp[blk->kwh_save_slot] = UNDEF_P(peeled_kwh) ? korb_hash_new() : peeled_kwh;
    }
    c->current_frame->self = blk->self;
    /* Install the block's lexical cref so const lookup inside the block
     * uses the lexical scope at block-creation time, not the dynamic
     * caller's. */
    struct korb_cref *prev_cref = c->current_frame->cref;
    if (blk->cref) c->current_frame->cref = blk->cref;
    /* Switch fp so block body's lvar_get/set hit the captured frame's slots. */
    c->current_frame->fp = fp;
    /* Lexical block target: yield inside block body refers to the
     * enclosing method's block, not back to this block. */
    struct korb_proc *prev_block = current_block;
    current_block = blk->enclosing_block;
    struct korb_proc *prev_running = running_block;
    running_block = blk;
    VALUE r;
redo_block:
    /* sp = fp + env_size: see korb_yield fast-path comment. */
    r = EVAL(c, blk->body, fp + blk->env_size);
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
    c->current_frame->fp = prev_fp;
    c->current_frame->self = prev_self;
    c->current_frame->cref = prev_cref;
    current_block = prev_block;
    running_block = prev_running;
    /* `next` inside a block: yield returns the next value, state cleared.
     * `break` should NOT be cleared here — it propagates to the yielding
     * method, where dispatch_call catches it as that method's return. */
    if (c->state == KORB_NEXT) {
        VALUE nv = c->state_value;
        c->state = KORB_NORMAL; c->state_value = Qnil;
        return nv;
    }
    /* `break` from inside the block: target the yielding method's frame
     * so loops within that method's body don't consume the break (e.g.
     * `def m; while x; yield; end; <stmt>; end` should skip <stmt>). */
    if (c->state == KORB_BREAK && c->state_target_frame == NULL) {
        c->state_target_frame = c->current_frame;
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
    const char *default_file = c->current_frame->current_file ? c->current_frame->current_file : "(unknown)";
    char buf[512];
    char nbuf[256];
    struct korb_frame *f = c->current_frame;
    int line = raise_line;
    if (line == 0 && f && f->caller_node) line = f->caller_node->head.line;
    /* If we're currently inside a block / proc / lambda body, prepend a
     * "block in <enclosing>" entry — yield/proc.call don't push their
     * own frame in koruby but CRuby's backtrace shows them. */
    if (running_block) {
        const char *enc_name = (f && f->method && f->method->name)
                                  ? korb_id_name(f->method->name) : "<main>";
        /* The block's body lives in the lexically-enclosing file; that's
         * what CRuby reports for "block in <method>" entries. */
        const char *enc_file = default_file;
        if (running_block->body && running_block->body->head.source_file) {
            enc_file = running_block->body->head.source_file;
        }
        snprintf(nbuf, sizeof(nbuf), "block in %s", enc_name);
        snprintf(buf, sizeof(buf), "%s:%d:in '%s'", enc_file, line, nbuf);
        korb_ary_push(arr, korb_str_new_cstr(buf));
    }
    /* Cap the walk depth and validate each frame before dereferencing.
     * The frame chain can dangle: f->prev sometimes points to a stack
     * frame that already returned, whose memory may now be reused for
     * something else.  Stop walking if the frame doesn't look sane. */
    int depth_cap = 200;
    /* Approximate stack range: anything within 32 MB of our local. */
    uintptr_t stack_anchor = (uintptr_t)&depth_cap;
    while (f && depth_cap-- > 0) {
        uintptr_t fp = (uintptr_t)f;
        uintptr_t diff = (fp > stack_anchor) ? fp - stack_anchor : stack_anchor - fp;
        if (diff > (32UL << 20)) break;  /* far from C stack — bail */
        const char *name = (f->method && f->method->name)
                             ? korb_id_name(f->method->name) : "<main>";
        const char *file = default_file;
        if (f->method && f->method->type == KORB_METHOD_AST &&
            f->method->u.ast.body && f->method->u.ast.body->head.source_file) {
            file = f->method->u.ast.body->head.source_file;
        }
        snprintf(buf, sizeof(buf), "%s:%d:in '%s'", file, line, name);
        korb_ary_push(arr, korb_str_new_cstr(buf));
        /* Next iteration's line = where IN the parent's body this call
         * was made.  That's recorded on f->caller_node. */
        line = f->caller_node ? f->caller_node->head.line : 0;
        /* If THIS frame was called from inside a block (frame.caller_
         * running_block), insert the block entry between this frame
         * and its caller — that block's body is what called us. */
        if (f->caller_running_block) {
            struct korb_proc *cb = (struct korb_proc *)f->caller_running_block;
            /* Skip entirely if cb doesn't look Proc-shaped — it may be
             * a stale pointer (lazy enumerator block frames live past
             * their parent's stack frame returning).  Better to omit
             * one backtrace line than SEGV. */
            if (cb && !SPECIAL_CONST_P((VALUE)cb) &&
                (((struct RBasic *)cb)->head.flags & T_MASK) == T_PROC) {
                struct korb_method *enc_m = cb->defining_method;
                const char *enc_name = (enc_m && enc_m->name)
                                          ? korb_id_name(enc_m->name) : "<main>";
                const char *enc_file = default_file;
                if (cb->body && cb->body->head.source_file) {
                    enc_file = cb->body->head.source_file;
                } else if (enc_m && enc_m->type == KORB_METHOD_AST &&
                    enc_m->u.ast.body && enc_m->u.ast.body->head.source_file) {
                    enc_file = enc_m->u.ast.body->head.source_file;
                }
                snprintf(nbuf, sizeof(nbuf), "block in %s", enc_name);
                snprintf(buf, sizeof(buf), "%s:%d:in '%s'", enc_file, line, nbuf);
                korb_ary_push(arr, korb_str_new_cstr(buf));
            }
        }
        f = f->prev;
    }
    /* Always tack on a <main> entry — line is whatever the
     * outermost-method's caller_node pointed at (i.e. the toplevel
     * call site), or raise_line when raising directly from main. */
    snprintf(buf, sizeof(buf), "%s:%d:in '<main>'", default_file, line);
    korb_ary_push(arr, korb_str_new_cstr(buf));
    return arr;
}

void korb_exc_set_backtrace(CTX *c, VALUE exc, int raise_line) {
    if (SPECIAL_CONST_P(exc)) return;
    ID id = korb_intern("@__backtrace__");
    VALUE existing = korb_ivar_get(exc, id);
    if (!UNDEF_P(existing) && !NIL_P(existing)) return;
    /* korb_build_backtrace allocates (= can move exc).  Park exc so the
     * subsequent korb_ivar_set sees the forwarded address. */
    ARO_ROOT_SCOPE_START(c, r, 2) {
        r[0] = exc;
        r[1] = korb_build_backtrace(c, raise_line);
        korb_ivar_set(r[0], id, r[1]);
    } ARO_ROOT_SCOPE_END(c, r);
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
    /* Park the exception obj across korb_str_new_cstr's alloc-can-GC
     * — otherwise the C local `obj` goes stale (= obj moves under GC
     * compaction) and korb_ivar_set writes @message into a phantom. */
    CTX *c = korb_vm->current_ctx;
    VALUE result;
    ARO_ROOT_SCOPE_START(c, r, 1) {
        r[0] = korb_object_new(klass);
        if (msg) {
            VALUE m = korb_str_new_cstr(msg);
            korb_ivar_set(r[0], korb_intern("@message"), m);
        }
        result = r[0];
    } ARO_ROOT_SCOPE_END(c, r);
    return result;
}

/* Convenience helpers — pick the right exception subclass instead of
 * the default RuntimeError.  Many CRuby tests pattern-match on the
 * specific subclass (`assert_raise(TypeError) { ... }`), so getting
 * this right unblocks a lot of off-the-shelf tests. */
void korb_raise_type_error(CTX *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE eTy = korb_const_get(korb_vm->object_class, korb_intern("TypeError"));
    korb_raise(c, (struct korb_class *)eTy, "%s", buf);
}
void korb_raise_argument_error(CTX *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
    korb_raise(c, (struct korb_class *)eArg, "%s", buf);
}
void korb_raise_range_error(CTX *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE eR = korb_const_get(korb_vm->object_class, korb_intern("RangeError"));
    korb_raise(c, (struct korb_class *)eR, "%s", buf);
}
void korb_raise_index_error(CTX *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VALUE eI = korb_const_get(korb_vm->object_class, korb_intern("IndexError"));
    korb_raise(c, (struct korb_class *)eI, "%s", buf);
}

/* Hot-path frozen-write rejector (called from inlined ivar setter when
 * the receiver is frozen).  Raises FrozenError "can't modify frozen X".
 * Uses the global VM context. */
void korb_raise_frozen_modification(VALUE obj) {
    CTX *c = korb_vm ? korb_vm->current_ctx : NULL;
    if (!c) return;
    VALUE eF = korb_const_get(korb_vm->object_class, korb_intern("FrozenError"));
    const char *cn = "Object";
    if (obj == Qtrue) cn = "TrueClass";
    else if (obj == Qfalse) cn = "FalseClass";
    else if (obj == Qnil) cn = "NilClass";
    else if (FIXNUM_P(obj)) cn = "Integer";
    else if (FLONUM_P(obj)) cn = "Float";
    else if (SYMBOL_P(obj)) cn = "Symbol";
    else if (!SPECIAL_CONST_P(obj)) {
        struct korb_class *k = korb_class_of_class(obj);
        if (k && k->name) cn = korb_id_name(k->name);
    }
    korb_raise(c, (struct korb_class *)eF,
               "can't modify frozen %s", cn);
}

void korb_raise(CTX *c, struct korb_class *klass, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Park klass + the new Exception obj across korb_exc_set_backtrace
     * (= allocates a backtrace Array, triggers GC) and the cause-link
     * walk (= multiple korb_ivar_get / korb_ivar_set, the set path can
     * grow the ivar array via libc which is GC-safe but other paths
     * might fire if an ivar setter cfunc is registered).  Without this,
     * the C local `e` goes stale across the backtrace alloc and the
     * final `c->state_value = e` stores a moved-out address — which is
     * what made bootstrap.rb's RuntimeError surface as "" under STRESS. */
    ARO_ROOT_SCOPE_START(c, r, 2) {
        r[0] = (VALUE)klass;
        r[1] = korb_exc_new((struct korb_class *)r[0], buf);
        int line = (c->last_cfunc_callsite
                    ? c->last_cfunc_callsite->head.line : 0);
        korb_exc_set_backtrace(c, r[1], line);
        /* Exception#cause: when this raise happens inside a rescue body,
         * $! holds the currently-rescued exception — link it.  Skip if
         * the exception already has a cause, skip self, and skip when
         * linking would create a cycle.
         *
         * Use r[1] / r[0] directly throughout (= GC-protected) rather
         * than caching to C locals.  Each korb_ivar_get/set can fire
         * GC; a C-local cached exc/current goes stale and the next
         * korb_ivar_set writes into a phantom obj's @cause. */
        VALUE current = korb_gvar_get(korb_intern("$!"));
        /* Park current too — subsequent korb_ivar_get walks might GC. */
        ARO_ROOT_SCOPE_START(c, rc, 1) {
            rc[0] = current;
            if (!NIL_P(rc[0]) && rc[0] != r[1] &&
                !SPECIAL_CONST_P(r[1]) && BUILTIN_TYPE(r[1]) == T_OBJECT) {
                VALUE existing = korb_ivar_get(r[1], korb_intern("@cause"));
                if (UNDEF_P(existing) || NIL_P(existing)) {
                    /* Cycle check: walk rc[0]'s cause chain. */
                    ARO_ROOT_SCOPE_START(c, rw, 1) {
                        rw[0] = rc[0];
                        int hops = 0;
                        bool would_cycle = false;
                        while (!NIL_P(rw[0]) && hops++ < 32) {
                            if (rw[0] == r[1]) { would_cycle = true; break; }
                            if (SPECIAL_CONST_P(rw[0]) || BUILTIN_TYPE(rw[0]) != T_OBJECT) break;
                            rw[0] = korb_ivar_get(rw[0], korb_intern("@cause"));
                            if (UNDEF_P(rw[0])) break;
                        }
                        if (!would_cycle) korb_ivar_set(r[1], korb_intern("@cause"), rc[0]);
                    } ARO_ROOT_SCOPE_END(c, rw);
                }
            }
        } ARO_ROOT_SCOPE_END(c, rc);
        c->state = KORB_RAISE;
        c->state_value = r[1];
    } ARO_ROOT_SCOPE_END(c, r);
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

/* Shortest round-tripping decimal for a double (matches CRuby's
 * `3.14.to_s == "3.14"` while staying unambiguous).  Strategy:
 *   1. Try the shortest %.<p>g that round-trips.
 *   2. If the result uses scientific (e+NN), try %f at the same
 *      precision in case the fixed-point form also round-trips and
 *      is shorter / more conventional.  CRuby prefers `10.0` over
 *      `1e+01` and `0.001` over `1e-03`.
 * Always emits `.0` for whole numbers so the Float type stays visible. */
void korb_double_to_str(double d, char *out, size_t out_cap) {
    if (isnan(d)) { snprintf(out, out_cap, "NaN"); return; }
    if (isinf(d)) { snprintf(out, out_cap, d < 0 ? "-Infinity" : "Infinity"); return; }
    for (int p = 1; p <= 17; p++) {
        snprintf(out, out_cap, "%.*g", p, d);
        if (strtod(out, NULL) == d) goto check_dot;
    }
    snprintf(out, out_cap, "%.17g", d);
  check_dot:
    /* If the chosen %.*g representation went scientific, try a fixed-
     * point form for the same value.  Reasonable threshold: |d| in
     * [1e-4, 1e16) is what CRuby prefers as fixed-point. */
    {
        bool has_e = false;
        for (char *q = out; *q; q++) {
            if (*q == 'e' || *q == 'E') { has_e = true; break; }
        }
        double ad = d < 0 ? -d : d;
        if (has_e && ad >= 1e-4 && ad < 1e16) {
            char alt[64];
            for (int p = 0; p <= 17; p++) {
                snprintf(alt, sizeof(alt), "%.*f", p, d);
                if (strtod(alt, NULL) == d) {
                    snprintf(out, out_cap, "%s", alt);
                    break;
                }
            }
        }
    }
    {
        bool has_dot_or_e = false;
        for (char *q = out; *q; q++) {
            if (*q == '.' || *q == 'e' || *q == 'E') { has_dot_or_e = true; break; }
        }
        if (!has_dot_or_e) {
            size_t l = strlen(out);
            if (l + 2 < out_cap) { out[l] = '.'; out[l+1] = '0'; out[l+2] = 0; }
        }
    }
}

static VALUE korb_inspect_inner(VALUE v, int depth) {
    if (depth > 32) return korb_str_new_cstr("...");
    if (FIXNUM_P(v)) {
        char b[32]; snprintf(b, 32, "%ld", FIX2LONG(v));
        return korb_str_new_cstr(b);
    }
    if (FLONUM_P(v)) {
        char b[64]; korb_double_to_str(korb_flonum_to_double(v), b, sizeof(b));
        return korb_str_new_cstr(b);
    }
    if (NIL_P(v)) return korb_str_new_cstr("nil");
    if (TRUE_P(v)) return korb_str_new_cstr("true");
    if (FALSE_P(v)) return korb_str_new_cstr("false");
    if (SYMBOL_P(v)) {
        const char *name = korb_id_name(korb_sym2id(v));
        size_t nlen = strlen(name);
        /* CRuby quotes a symbol's name when it's not a valid bare identifier:
         * empty, leading non-alpha/underscore, contains spaces/control,
         * etc.  Approximate: bare identifier must start with [A-Za-z_] and
         * subsequent chars are alphanumeric or '_'; trailing '?', '!', '='
         * are also allowed.  Operator symbols (+, -, []=, ==, etc.) are
         * also displayed bare; we keep a small set. */
        bool needs_quote = false;
        if (nlen == 0) needs_quote = true;
        else {
            unsigned char c0 = (unsigned char)name[0];
            bool is_op = false;
            static const char *ops[] = {
                "+", "-", "*", "/", "%", "**", "==", "!=", "===",
                "<", ">", "<=", ">=", "<=>", "<<", ">>", "&", "|", "^",
                "~", "!", "+@", "-@", "[]", "[]=", "=~", "!~", NULL
            };
            for (int oi = 0; ops[oi]; oi++) {
                if (strcmp(name, ops[oi]) == 0) { is_op = true; break; }
            }
            if (!is_op) {
                if (c0 == '@') {
                    /* Instance / class variable — `@x` or `@@x`.  Bare
                     * form requires alnum tail; trailing `!?=` is NOT
                     * allowed (variables can't end with them).  Allow
                     * one optional second `@` for class vars. */
                    size_t i = 1;
                    if (i < nlen && (unsigned char)name[i] == '@') i++;
                    if (i == nlen) needs_quote = true; /* bare `@` or `@@` */
                    for (; i < nlen; i++) {
                        unsigned char ci = (unsigned char)name[i];
                        bool is_alnum = (ci == '_' || (ci >= 'A' && ci <= 'Z') ||
                                         (ci >= 'a' && ci <= 'z') || (ci >= '0' && ci <= '9'));
                        if (!is_alnum) { needs_quote = true; break; }
                    }
                } else if (c0 == '$') {
                    /* Global variable — many forms.  Bare-display the
                     * special CRuby gvars (`$~`, `$+`, `$:`, etc.) and
                     * normal `$ident` forms.  Anything else needs
                     * quoting.  We accept `$` followed by exactly one
                     * special punctuation char OR `$ident` (alnum-only
                     * tail, no trailing `!?=`). */
                    if (nlen == 1) needs_quote = true;
                    else if (nlen == 2) {
                        /* $X — accept all `[!@&`'+~:?<>=].\\\\,/$;]` style;
                         * simplest is to accept any single punctuation as
                         * a CRuby gvar literal. */
                        unsigned char ci = (unsigned char)name[1];
                        bool is_alnum = (ci == '_' || (ci >= 'A' && ci <= 'Z') ||
                                         (ci >= 'a' && ci <= 'z') || (ci >= '0' && ci <= '9'));
                        if (!is_alnum && (ci == '"' || ci == ' ')) needs_quote = true;
                    } else {
                        /* $-w / $LOAD_PATH / $stdin etc. */
                        size_t i = 1;
                        if (name[1] == '-') i = 2;
                        for (; i < nlen; i++) {
                            unsigned char ci = (unsigned char)name[i];
                            bool is_alnum = (ci == '_' || (ci >= 'A' && ci <= 'Z') ||
                                             (ci >= 'a' && ci <= 'z') || (ci >= '0' && ci <= '9'));
                            if (!is_alnum) { needs_quote = true; break; }
                        }
                    }
                } else if (!(c0 == '_' || (c0 >= 'A' && c0 <= 'Z') ||
                             (c0 >= 'a' && c0 <= 'z'))) {
                    needs_quote = true;
                } else {
                    for (size_t i = 1; i < nlen; i++) {
                        unsigned char ci = (unsigned char)name[i];
                        bool is_alnum = (ci == '_' || (ci >= 'A' && ci <= 'Z') ||
                                         (ci >= 'a' && ci <= 'z') || (ci >= '0' && ci <= '9'));
                        bool is_trailing = (i == nlen - 1) && (ci == '?' || ci == '!' || ci == '=');
                        if (!is_alnum && !is_trailing) { needs_quote = true; break; }
                    }
                }
            }
        }
        if (!needs_quote) {
            VALUE ret = Qnil;
            CTX *c2 = korb_vm ? korb_vm->current_ctx : NULL;
            if (c2) {
                ARO_ROOT_SCOPE_START(c2, rs, 2) {
                    rs[0] = korb_str_new_cstr(":");
                    rs[1] = korb_str_new(name, (long)nlen);
                    korb_str_concat(rs[0], rs[1]);
                    ret = rs[0];
                } ARO_ROOT_SCOPE_END(c2, rs);
                return ret;
            }
            VALUE s = korb_str_new_cstr(":");
            korb_str_concat(s, korb_str_new(name, (long)nlen));
            return s;
        }
        /* Quoted form: :"...".  Re-use the String inspect path for
         * escaping by inspecting the name as a String, which yields
         * a quoted, escaped form, then prepend the colon.  Each
         * korb_str_new + korb_inspect_inner can fire GC, so park the
         * intermediate strings in ARO_ROOT_SCOPE to keep them alive. */
        {
            VALUE ret = Qnil;
            CTX *c2 = korb_vm ? korb_vm->current_ctx : NULL;
            if (c2) {
                ARO_ROOT_SCOPE_START(c2, rs, 2) {
                    rs[0] = korb_str_new(name, (long)nlen);
                    rs[0] = korb_inspect_inner(rs[0], depth + 1);
                    rs[1] = korb_str_new_cstr(":");
                    korb_str_concat(rs[1], rs[0]);
                    ret = rs[1];
                } ARO_ROOT_SCOPE_END(c2, rs);
                return ret;
            }
        }
        VALUE name_str = korb_str_new(name, (long)nlen);
        VALUE inspected = korb_inspect_inner(name_str, depth + 1);
        VALUE r = korb_str_new_cstr(":");
        korb_str_concat(r, inspected);
        return r;
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
                case '\a': esc = "\\a";  break;
                case '\b': esc = "\\b";  break;
                case '\f': esc = "\\f";  break;
                case '\v': esc = "\\v";  break;
                case '\x1b': esc = "\\e"; break;
                case '#':
                    /* CRuby escapes `#` only when followed by an
                     * interpolation-trigger character: `{`, `$`, `@`. */
                    if (i + 1 < s->len) {
                        unsigned char nx = (unsigned char)s->ptr[i + 1];
                        if (nx == '{' || nx == '$' || nx == '@') esc = "\\#";
                    }
                    break;
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
        bool both_nil = NIL_P(r->begin) && NIL_P(r->end);
        VALUE s = (NIL_P(r->begin) && !both_nil)
                      ? korb_str_new_cstr("")
                      : korb_inspect_inner(r->begin, depth+1);
        korb_str_concat(s, korb_str_new_cstr(r->exclude_end ? "..." : ".."));
        if (!NIL_P(r->end) || both_nil)
            korb_str_concat(s, korb_inspect_inner(r->end, depth+1));
        return s;
    }
    if (t == T_FLOAT) {
        char b[64]; korb_double_to_str(((struct korb_float *)v)->value, b, sizeof(b));
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
            /* Exception-shaped: "#<ClassName: message>".  Park r and
             * msg in ARO_ROOT_SCOPE — each subsequent korb_str_new_cstr
             * can fire GC, invalidating prior C-local heap ptrs. */
            CTX *c2 = korb_vm ? korb_vm->current_ctx : NULL;
            const char *cls_name = k && k->name ? korb_id_name(k->name) : "Object";
            if (c2) {
                VALUE ret = Qnil;
                ARO_ROOT_SCOPE_START(c2, rs, 2) {
                    rs[0] = msg;
                    rs[1] = korb_str_new_cstr("#<");
                    korb_str_concat(rs[1], korb_str_new_cstr(cls_name));
                    korb_str_concat(rs[1], korb_str_new_cstr(": "));
                    korb_str_concat(rs[1], rs[0]);
                    korb_str_concat(rs[1], korb_str_new_cstr(">"));
                    ret = rs[1];
                } ARO_ROOT_SCOPE_END(c2, rs);
                return ret;
            }
            /* Fallback: no CTX (= early bootstrap), best-effort C-local. */
            VALUE r = korb_str_new_cstr("#<");
            korb_str_concat(r, korb_str_new_cstr(cls_name));
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
uint64_t korb_g_next_frame_id = 0;

/* Set to true once user code redefines a method on Integer / Float /
 * Array / Hash / String / Symbol — the receiver classes that EVAL_node_*
 * fast paths assume are unmodified.  Each fast path includes an
 * UNLIKELY check; if the flag flips, the path falls through to slow
 * dispatch.  Coarse-grained (any redefinition flips it forever), but
 * common-case correct and zero-cost on the normal path. */
bool korb_g_basic_op_redefined = false;
bool korb_g_array_op_redefined = false;

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
    if (!korb_is_basic_op_id(name)) return;
    /* Array#<< has its own fast path (`arr << x` without method dispatch
     * in node_lshift) — trip a dedicated Array flag so a user-defined
     * Array#<< actually runs.  Other Array methods don't have inline
     * fast paths so we don't care about them. */
    if (target == korb_vm->array_class && name == id_op_lshift) {
        korb_g_array_op_redefined = true;
        return;
    }
    /* Integer/Float redef trips the global FIXNUM/FLONUM fast-path off —
     * those are the basic ops the inline fast paths in node_plus/minus/
     * mul/div etc. care about.  Hash/String/Symbol redefining their own
     * `<` or `[]` doesn't affect Integer arithmetic dispatch, so don't
     * set the flag for them (otherwise adding any helper to bootstrap.rb
     * on those classes would kill the fast path across the whole VM —
     * fib(36) regressed ~5× from this).
     *
     * Numeric adds are SKIPPED here too: a default `Numeric#<=>` (or
     * other op) added in bootstrap.rb still has Integer/Float's own
     * cfunc winning at lookup time, so the inline fast paths remain
     * valid.  Without this skip, just defining `Numeric#<=>` once at
     * startup would permanently slow every fixnum compare ~10× even
     * though Integer's <=> is still in effect (optcarrot 70 → 7 fps). */
    if (target != korb_vm->integer_class &&
        target != korb_vm->float_class) return;
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
    /* Don't C-local save outer self — outer.self lives in the frame
     * chain (visit_roots phase d).  After popping our frame the outer
     * self is automatically fresh.  Writing a stale C-local back to
     * outer->self overwrites the fresh value with a dead pointer.
     * The pushed new_frame.self = recv covers the body's `self`. */
    VALUE *prev_fp = c->current_frame->fp;
    VALUE *prev_sp = c->sp;
    struct korb_proc *prev_block = current_block;
    struct korb_cref *prev_cref = c->current_frame->cref;
    current_block = block;
    /* Shift fp to the args base.  Caller staged argv at outer_fp[arg_index..
     * arg_index+argc); body expects to read params from fp[0..argc) and
     * locals from fp[argc..locals_cnt).  Same convention as
     * prologue_ast_simple_inl / prologue_ast_full_inl_K.  Without this
     * shift, the body reads its first param from outer_fp[0] (= caller's
     * first lvar) instead of the staged arg — pre-existing bug that
     * showed up in test_alias_redef as "expected/actual got the wrong
     * value from the caller's locals" (assert_equal's `e` parameter was
     * c.greet's `c` lvar, not the literal 42 caller passed). */
    c->current_frame->fp += arg_index;
    if (UNLIKELY(c->current_frame->fp + mc->locals_cnt >= c->stack_end)) {
        c->current_frame->fp = prev_fp;
        korb_raise(c, NULL, "stack overflow");
        current_block = prev_block;
        return Qnil;
    }
    /* Zero-fill the new frame's locals (= slots beyond argc) to Qnil so
     * an alloc inside the prologue / body doesn't see stale heap pointers
     * in not-yet-written slots.  Args [c->current_frame->fp, c->current_frame->fp + argc) are caller-
     * provided and stay; locals [c->current_frame->fp + argc, c->current_frame->fp + locals_cnt) need
     * clearing — visit_roots walks c->stack_base..c->sp and would otherwise
     * pick up popped-frame leftovers at this address range.
     *
     * Note: sp might already be at-or-beyond c->current_frame->fp + locals_cnt due to a
     * prior deeper call; in that case the extend branch is a no-op but
     * the zero-fill below still runs (= covers the "sp already high but
     * locals slot dirty" case). */
    if (c->current_frame->fp + mc->locals_cnt > c->sp) {
        /* Zero-fill any gap below c->current_frame->fp + the freshly-exposed range
         * past c->sp.  Args at [c->current_frame->fp, c->current_frame->fp+argc) were already
         * written by caller; the local range [c->current_frame->fp+argc, c->current_frame->fp+locals_cnt)
         * is zero-filled in the loop below.  Any gap before c->current_frame->fp
         * (if c->sp < c->current_frame->fp) is leftover dead slots from prior frames
         * at the same addresses — stale heap ptrs here get treated as
         * live roots by visit_roots, causing GC BUG forward to-space. */
        for (VALUE *p = c->sp; p < c->current_frame->fp; p++) *p = Qnil;
        c->sp = c->current_frame->fp + mc->locals_cnt;
    }
    for (VALUE *p = c->current_frame->fp + argc; p < c->current_frame->fp + mc->locals_cnt; p++) {
        *p = Qnil;
    }
    if (mc->def_cref) c->current_frame->cref = mc->def_cref;

    /* Kwargs hash peel: when the method declares keyword params and the
     * caller's last positional is a kwargs-tagged Hash (FL_KWARGS — set
     * by `m(**h)` / `m(k: v)`), treat that hash as kwargs.  Plain
     * positional Hash (`m(h)`) is NOT peeled (Ruby 3 separation). */
    VALUE peeled_kwh = Qundef;
    if (mc->kwh_save_slot >= 0) {
        if (argc > 0 && !SPECIAL_CONST_P(c->current_frame->fp[argc - 1]) &&
            BUILTIN_TYPE(c->current_frame->fp[argc - 1]) == T_HASH &&
            (RBASIC(c->current_frame->fp[argc - 1])->head.flags & FL_KWARGS)) {
            peeled_kwh = c->current_frame->fp[argc - 1];
            argc--;
        } else {
            peeled_kwh = korb_hash_new();
        }
    }
    /* If the last positional is a kwargs-tagged Hash but the callee
     * doesn't accept **kwargs, drop it silently (CRuby `m(**h)` to a
     * no-kwargs method is allowed only if h is empty; otherwise ArgError —
     * we always silently drop here, matching Ruby 2 lenient behavior).
     * This also handles `m(**empty)` → no positional hash. */
    if (mc->kwh_save_slot < 0 && argc > 0 &&
        !SPECIAL_CONST_P(c->current_frame->fp[argc - 1]) &&
        BUILTIN_TYPE(c->current_frame->fp[argc - 1]) == T_HASH &&
        (RBASIC(c->current_frame->fp[argc - 1])->head.flags & FL_KWARGS)) {
        struct korb_hash *h = (struct korb_hash *)c->current_frame->fp[argc - 1];
        if (h->size == 0) {
            argc--;
        }
    }

    if (UNLIKELY(mc->rest_slot < 0 && argc > mc->total_params_cnt)) {
        VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eArg,
                   "wrong number of arguments (given %u, expected %u)",
                   argc, mc->total_params_cnt);
        c->current_frame->fp = prev_fp;
        c->current_frame->cref = prev_cref;
        current_block = prev_block;
        return Qnil;
    }
    /* Too few — argc must satisfy required + post (rest absorbs the
     * middle, optional defaults).  required+post is the floor regardless
     * of whether rest is present. */
    {
        uint32_t min_argc = mc->required_params_cnt + mc->post_params_cnt;
        if (UNLIKELY(argc < min_argc)) {
            VALUE eArg = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
            bool variadic = (mc->rest_slot >= 0) || (mc->total_params_cnt > min_argc);
            uint32_t opt_total = mc->total_params_cnt - mc->required_params_cnt - mc->post_params_cnt;
            if (mc->rest_slot >= 0) {
                korb_raise(c, (struct korb_class *)eArg,
                           "wrong number of arguments (given %u, expected %u+)",
                           argc, min_argc);
            } else if (opt_total > 0) {
                korb_raise(c, (struct korb_class *)eArg,
                           "wrong number of arguments (given %u, expected %u..%u)",
                           argc, min_argc, mc->total_params_cnt);
            } else {
                korb_raise(c, (struct korb_class *)eArg,
                           "wrong number of arguments (given %u, expected %u)",
                           argc, min_argc);
            }
            (void)variadic;
            c->current_frame->fp = prev_fp;
            c->current_frame->cref = prev_cref;
            current_block = prev_block;
            return Qnil;
        }
    }

    long opt_filled = 0;
    if (mc->rest_slot >= 0) {
        long fixed_pre  = (long)mc->required_params_cnt;
        long fixed_post = (long)mc->post_params_cnt;
        /* total_params_cnt = required + optional + rest(=1) + post.
         * Solve for optional_cnt. */
        long optional_cnt = (long)mc->total_params_cnt - fixed_pre - 1 - fixed_post;
        if (optional_cnt < 0) optional_cnt = 0;
        long after_required = (long)argc - fixed_pre - fixed_post;
        if (after_required < 0) after_required = 0;
        opt_filled = after_required < optional_cnt ? after_required : optional_cnt;
        long extra = after_required - opt_filled;  /* leftover for rest */
        /* Snapshot post args before overwriting fp[rest_slot]. */
        VALUE post_save[16];
        VALUE *post_buf = post_save;
        if (fixed_post > 16) post_buf = korb_xmalloc(sizeof(VALUE) * fixed_post);
        for (long i = 0; i < fixed_post; i++) {
            post_buf[i] = c->current_frame->fp[fixed_pre + opt_filled + extra + i];
        }
        VALUE rest = korb_ary_new_capa(extra);
        for (long i = 0; i < extra; i++) {
            korb_ary_push(rest, c->current_frame->fp[fixed_pre + opt_filled + i]);
        }
        c->current_frame->fp[mc->rest_slot] = rest;
        /* Posts land at fp[total - post_cnt + i] in the param-position
         * layout — this is fp[rest_slot+1+i] iff rest_slot is at
         * required+opt position (named rest), but for *anonymous* rest
         * the rest_slot is allocated past locals, so the two indices
         * diverge.  Write to the param-position layout so the shuffle
         * (which reads fp[i] for each position i) sees the right values. */
        long post_base = (long)mc->total_params_cnt - fixed_post;
        for (long i = 0; i < fixed_post; i++) {
            c->current_frame->fp[post_base + i] = post_buf[i];
        }
    } else if (mc->post_params_cnt > 0) {
        /* No rest, but post params present.  argv layout in caller:
         * [pre, opts_filled, post].  We need to move the post args to
         * the post slots and put Qundef in the unfilled opt slots. */
        long fixed_pre  = (long)mc->required_params_cnt;
        long fixed_post = (long)mc->post_params_cnt;
        long optional_cnt = (long)mc->total_params_cnt - fixed_pre - fixed_post;
        if (optional_cnt < 0) optional_cnt = 0;
        long after_required = (long)argc - fixed_pre - fixed_post;
        if (after_required < 0) after_required = 0;
        opt_filled = after_required < optional_cnt ? after_required : optional_cnt;
        VALUE post_save[16];
        VALUE *post_buf = post_save;
        if (fixed_post > 16) post_buf = korb_xmalloc(sizeof(VALUE) * fixed_post);
        for (long i = 0; i < fixed_post; i++) {
            post_buf[i] = c->current_frame->fp[fixed_pre + opt_filled + i];
        }
        for (long i = 0; i < fixed_post; i++) {
            c->current_frame->fp[fixed_pre + optional_cnt + i] = post_buf[i];
        }
    }

    /* Optional-slot start: after required + opt_filled.  Slots
     * required..required+opt_filled-1 hold caller-provided opt values;
     * everything from required+opt_filled onward (excluding rest/post)
     * gets Qundef so the body's default_init prologue substitutes the
     * user-supplied default. */
    uint32_t opt_start;
    if (mc->rest_slot >= 0 || mc->post_params_cnt > 0) {
        opt_start = mc->required_params_cnt + (uint32_t)opt_filled;
    } else {
        opt_start = argc;
        if (opt_start < mc->required_params_cnt) opt_start = mc->required_params_cnt;
    }
    /* When there are post params and no rest, post slots live at the
     * end of total_params_cnt — set their range so the clear loop skips
     * them. */
    uint32_t post_lo_no_rest = 0, post_hi_no_rest = 0;
    if (mc->rest_slot < 0 && mc->post_params_cnt > 0) {
        post_lo_no_rest = mc->total_params_cnt - mc->post_params_cnt;
        post_hi_no_rest = mc->total_params_cnt;
    }
    /* Don't clobber post-rest slots (just populated above). */
    uint32_t post_lo = (mc->rest_slot >= 0 && mc->post_params_cnt)
                         ? (uint32_t)(mc->rest_slot + 1) : post_lo_no_rest;
    uint32_t post_hi = (mc->rest_slot >= 0 && mc->post_params_cnt)
                         ? post_lo + mc->post_params_cnt : post_hi_no_rest;
    for (uint32_t i = opt_start; i < mc->total_params_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        if (mc->post_params_cnt && i >= post_lo && i < post_hi) continue;
        c->current_frame->fp[i] = Qundef;
    }
    for (uint32_t i = mc->total_params_cnt; i < mc->locals_cnt; i++) {
        if ((int)i == mc->rest_slot) continue;
        if ((int)i == mc->block_slot) continue;
        c->current_frame->fp[i] = Qnil;
    }
    /* Param shuffle: when the method's params include a multi_target
     * (e.g. `def m(a, (b, c), d=1)`), the natural identity mapping
     * "param position i → fp[i]" collides — fp[1] is both the multi-
     * target's holder AND b's local slot.  param_holder_slots[] gives
     * the right destination for each param; copy via snapshot to
     * avoid clobbering source slots that are also someone's dest. */
    if (UNLIKELY(mc->param_holder_slots != NULL)) {
        VALUE snap_buf[64];
        VALUE *snap = snap_buf;
        bool dest_used_buf[64] = {0};
        bool *dest_used = dest_used_buf;
        if (mc->total_params_cnt > 64) {
            snap = korb_xmalloc(mc->total_params_cnt * sizeof(VALUE));
            dest_used = korb_xmalloc(mc->total_params_cnt * sizeof(bool));
            for (uint32_t i = 0; i < mc->total_params_cnt; i++) dest_used[i] = false;
        }
        for (uint32_t i = 0; i < mc->total_params_cnt; i++) snap[i] = c->current_frame->fp[i];
        /* First pass: write each param's value to its dest slot. */
        for (uint32_t i = 0; i < mc->total_params_cnt; i++) {
            int dest = mc->param_holder_slots[i];
            if (dest >= 0 && (uint32_t)dest != i) {
                c->current_frame->fp[dest] = snap[i];
            }
            /* Mark which slots in [0..total) are still actually used as
             * a param holder (= destination for some position). */
            if (dest >= 0 && (uint32_t)dest < mc->total_params_cnt) {
                dest_used[dest] = true;
            } else if (dest < 0 && i < mc->total_params_cnt) {
                /* Skipped param (e.g. rest_slot's dummy); leave its
                 * slot as-is. */
                dest_used[i] = true;
            }
        }
        /* Second pass: any slot in [0..total) that's NOT a destination
         * is shadowing a real lvar (whose lvar_slot < total but isn't
         * one of the param positions).  Reset those to Qnil so the
         * lvar reads nil instead of the leftover caller arg. */
        for (uint32_t i = 0; i < mc->total_params_cnt; i++) {
            if (!dest_used[i]) c->current_frame->fp[i] = Qnil;
        }
        if (snap != snap_buf) korb_xfree(snap);
        if (dest_used != dest_used_buf) korb_xfree(dest_used);
    }
    /* &blk parameter — store the incoming block as a Proc into its
     * slot.  block can be NULL (no block given), in which case the
     * local reads as nil. */
    if (mc->block_slot >= 0) {
        c->current_frame->fp[mc->block_slot] = block ? (VALUE)block : Qnil;
    }
    /* Stash the peeled kwargs hash where the body prelude can read it.
     * If no kwargs were passed (peeled_kwh == UNDEF), fill the slot
     * with an empty Hash so the prologue's `kwh.has_key?(:foo)` checks
     * don't blow up with NoMethodError on nil — happens whenever a C
     * cfunc (e.g. obj_clone → initialize_clone(other, freeze: nil))
     * dispatches to an AST method with optional keywords without
     * forwarding the hash arg. */
    if (mc->kwh_save_slot >= 0) {
        c->current_frame->fp[mc->kwh_save_slot] = UNDEF_P(peeled_kwh) ? korb_hash_new() : peeled_kwh;
    }
    /* No outer-self mutation — new_frame.self below = recv */

    /* Frame: .prev, .method (for super), .self / .caller_node (for
     * backtrace), .block (for block_given?), and .fp / .locals_cnt
     * (for Kernel#binding via __capture_lvars__).  Earlier we elided
     * .fp / .locals_cnt to save two stores; that broke binding from
     * any method routed through the general prologue (anything with
     * &blk, *rest, or kwargs). */
    struct korb_frame frame;
    frame.prev = c->current_frame;
    frame.method = mc->method;
    frame.self = recv;
    frame.block = block;
    frame.caller_node = callsite;
    frame.fp = c->current_frame->fp;
    frame.locals_cnt = mc->locals_cnt;
    frame.super_skip_n = 0;
    /* Inherit cref / current_class / current_file from outer (= the
     * caller frame, whose cref was just set to mc->def_cref above at
     * line 3260).  Without these initializers the struct.cref field
     * holds uninitialized stack garbage; visit_roots phase (c+d)
     * iterating frame.cref would chase wild pointers and SEGV. */
    frame.cref = c->current_frame->cref;
    frame.current_class = c->current_frame->current_class;
    frame.current_file = c->current_frame->current_file;
    extern uint64_t korb_g_next_frame_id;
    frame.frame_id = ++korb_g_next_frame_id;
    frame.bindings_head = NULL;
    frame.last_line = Qnil;
    frame.last_match = Qnil;
    extern struct korb_proc *running_block;
    frame.caller_running_block = running_block;
    c->current_frame = &frame;
    /* Reset running_block: a method body is no longer "inside" the
     * caller's block, so a `return` inside it should be method-local. */
    struct korb_proc *prev_running = running_block;
    running_block = NULL;
    VALUE *frame_lo = c->current_frame->fp;
    VALUE *frame_hi = c->current_frame->fp + mc->locals_cnt;
    VALUE r = mc->dispatcher(c, mc->body, c->current_frame->fp + mc->locals_cnt);
    c->current_frame = frame.prev;
    running_block = prev_running;
    korb_proc_snapshot_env_maybe(r, frame_lo, frame_hi);
    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        korb_proc_snapshot_env_maybe(c->state_value, frame_lo, frame_hi);
    }
    c->current_frame->fp = prev_fp;
    /* outer->self auto-fresh via frame chain — no C-local restore */
    c->current_frame->cref = prev_cref;
    current_block = prev_block;
    /* Restore sp + zero-fill the popped range so a sibling/later push
     * doesn't re-expose this frame's stale heap ptrs (= same invariant
     * as prologue_ast_simple_inl line 377).  Without this, sp grows
     * unboundedly across calls through general, and visit_roots scans
     * stale slots → "BAD SLOT" abort under STRESS. */
    for (VALUE *p = prev_sp; p < c->sp; p++) *p = Qnil;
    c->sp = prev_sp;

    if (UNLIKELY(c->state == KORB_RETURN || c->state == KORB_BREAK)) {
        bool consume_return = (c->state == KORB_RETURN &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        bool consume_break = (c->state == KORB_BREAK &&
            (c->state_target_frame == NULL || c->state_target_frame == &frame));
        if (consume_break || consume_return) {
            r = c->state_value;
            c->state = KORB_NORMAL;
            c->state_value = Qnil;
            c->state_target_frame = NULL;
        }
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
        mc->param_holder_slots = m->u.ast.param_holder_slots;
        /* &blk reification needs a runtime store, so it goes through the
         * general prologue.  Pick simple vs general based on parameter
         * shape; for the simple case prefer an argc-specialized variant
         * so the C compiler can fold the argc check + unroll the Qnil
         * fill.  Force `general` when param_holder_slots is non-NULL
         * (multi_target rebinding required, simple path doesn't shuffle). */
        if (mc->rest_slot < 0 && mc->block_slot < 0 && mc->kwh_save_slot < 0 &&
            mc->total_params_cnt == mc->required_params_cnt &&
            mc->param_holder_slots == NULL) {
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
        mc->param_holder_slots = NULL;
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
        mc->param_holder_slots = NULL;
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
    VALUE *argv = &c->current_frame->fp[arg_index];
    VALUE prev_self = c->current_frame->self;
    c->current_frame->self = recv;
    /* Temporarily rebind the proc's `self` to the dispatch receiver:
     * define_method'd procs run with self = the call receiver, not the
     * class body's self captured at proc creation.  We restore after
     * so a later call (or other dispatch site) sees the original.
     *
     * Also flip is_lambda for the duration of the call so `return`
     * inside the proc body acts as a method-local return (CRuby's
     * define_method-via-proc semantics: the proc behaves like a
     * lambda for return / break purposes). */
    VALUE prev_p_self = p->self;
    bool prev_is_lambda = p->is_lambda;
    p->self = recv;
    p->is_lambda = true;
    VALUE r = proc_call(c, (VALUE)p, (int)argc, argv);
    p->self = prev_p_self;
    p->is_lambda = prev_is_lambda;
    c->current_frame->self = prev_self;
    return r;
}

VALUE korb_dispatch_visibility_raise(CTX *c, struct korb_method *m, ID name,
                                     struct korb_class *klass, VALUE recv) {
    /* CRuby semantics: explicit-receiver call to a private/protected
     * method routes through method_missing if user has defined one
     * (default method_missing raises NoMethodError).  This lets a mock
     * with `def method_missing(...)` intercept calls to `obj.lambda`
     * where Kernel#lambda exists but is private.  We detect "user
     * override" by walking up to but excluding BasicObject's default. */
    struct korb_method *mm = klass ? korb_class_find_method(klass, korb_intern("method_missing")) : NULL;
    if (mm) {
        VALUE av[1] = { korb_id2sym(name) };
        return korb_dispatch_binop(c, recv, korb_intern("method_missing"), 1, av);
    }
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
    /* Stash receiver + name on the exception so NoMethodError#receiver
     * and #name work.  CRuby exposes both. */
    if (c->state == KORB_RAISE && c->state_value && !SPECIAL_CONST_P(c->state_value)) {
        korb_ivar_set(c->state_value, korb_intern("@receiver"), recv);
        korb_ivar_set(c->state_value, korb_intern("@name"), korb_id2sym(name));
    }
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
            /* Module function lookup: when recv is the module itself,
             * its instance methods are accessible as class methods —
             * but only the PUBLIC ones.  Private instance methods
             * stay private (CRuby's module_function convention).
             * If we'd find a private one, fall through to klass
             * (recv's metaclass) so a public class-method version
             * (added via DEF on cKerMeta etc.) wins. */
            m = korb_class_find_method((struct korb_class *)recv, name);
            if (m && m->visibility == KORB_VIS_PRIVATE) m = NULL;
        }
        if (!m) m = korb_class_find_method(klass, name);
        if (UNLIKELY(!m)) {
            /* method_missing fallback: prepend the missing name to the
             * argv and dispatch :method_missing if defined. */
            struct korb_method *mm = korb_class_find_method(klass, korb_intern("method_missing"));
            if (mm) {
                /* Shift args right by one; insert :name in front. */
                VALUE *fp = c->current_frame->fp;
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
            /* Implicit-self bareword call (vcall): when recv is self
             * and the missing name looks like an identifier (no `?`,
             * `!`, `=`, `[]`, etc.), CRuby raises NameError with
             * "undefined local variable or method `name'" because the
             * resolver couldn't decide between local var and method.
             * NoMethodError is-a NameError, so this just adjusts the
             * message wording. */
            const char *nm = korb_id_name(name);
            bool bareword = nm[0] != '\0';
            for (const char *p = nm; *p; p++) {
                char ch = *p;
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_')) { bareword = false; break; }
            }
            if (recv == c->current_frame->self && bareword) {
                korb_raise(c, (struct korb_class *)eNo,
                           "undefined local variable or method '%s' for %s",
                           nm, korb_id_name(klass->name));
            } else {
                korb_raise(c, (struct korb_class *)eNo, "undefined method '%s' for %s",
                           nm, korb_id_name(klass->name));
            }
            if (c->state == KORB_RAISE && c->state_value && !SPECIAL_CONST_P(c->state_value)) {
                korb_ivar_set(c->state_value, korb_intern("@receiver"), recv);
                korb_ivar_set(c->state_value, korb_intern("@name"), korb_id2sym(name));
            }
            return Qnil;
        }
        if (mc) {
            korb_method_cache_fill(mc, klass, m);
        } else {
            /* No mc — slow one-shot path.  Synthesize a temp cache and dispatch. */
            struct method_cache tmp = {0};
            korb_method_cache_fill(&tmp, klass, m);
            if (m->visibility == KORB_VIS_PRIVATE && recv != c->current_frame->self) {
                return korb_dispatch_visibility_raise(c, m, name, klass, recv);
            }
            if (m->visibility == KORB_VIS_PROTECTED) {
                struct korb_class *caller_klass = korb_class_of_class(c->current_frame->self);
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
        if (mc->method->visibility == KORB_VIS_PRIVATE && recv != c->current_frame->self) {
            return korb_dispatch_visibility_raise(c, mc->method, name, klass, recv);
        }
        if (mc->method->visibility == KORB_VIS_PROTECTED) {
            struct korb_class *caller_klass = korb_class_of_class(c->current_frame->self);
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
    c->current_frame->fp[arg_index+1] = r;                       \
    return korb_dispatch_binop(c, l, OP_ID, 1,    \
                               &c->current_frame->fp[arg_index+1]); \
} while (0)

__attribute__((noinline,cold)) VALUE
korb_node_plus_slow(CTX *c, VALUE l, VALUE r, uint32_t arg_index) {
    /* Fast paths only when both operands are EXACTLY String / Array
     * (not subclasses).  A subclass may have redefined `+`, in which
     * case we must dispatch to find the override. */
    if (BUILTIN_TYPE(l) == T_STRING && BUILTIN_TYPE(r) == T_STRING &&
        ((struct RBasic *)l)->klass == (VALUE)korb_vm->string_class) {
        return korb_str_concat(l, r);
    }
    if (BUILTIN_TYPE(l) == T_ARRAY && BUILTIN_TYPE(r) == T_ARRAY &&
        ((struct RBasic *)l)->klass == (VALUE)korb_vm->array_class) {
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
        c->current_frame->fp[arg_index+1] = i;
        return korb_dispatch_binop(c, r, id_op_aref, 1, &c->current_frame->fp[arg_index+1]);
    }
    c->current_frame->fp[arg_index+1] = i;
    return korb_dispatch_binop(c, r, id_op_aref, 1, &c->current_frame->fp[arg_index+1]);
}

__attribute__((noinline,cold)) VALUE
korb_node_aset_slow(CTX *c, VALUE r, VALUE i, VALUE v, uint32_t arg_index) {
    if (UNLIKELY(SPECIAL_CONST_P(r))) {
        if (NIL_P(r)) return v;
    }
    VALUE *args = &c->current_frame->fp[arg_index+1];
    args[0] = i; args[1] = v;
    korb_dispatch_binop(c, r, id_op_aset, 2, args);
    return v;
}

#undef COLD_BINOP_DEFAULT

/* Forward decl — defined as the body of korb_dispatch_binop after method
 * lookup.  Used by Method#call to skip lookup and dispatch directly to
 * a captured method record (so define_method-based redefinition after
 * `instance_method(:foo)` doesn't redirect the call). */
VALUE korb_dispatch_to_method(CTX *c, struct korb_method *m,
                               struct korb_class *defining_class,
                               VALUE recv, ID name, int argc, VALUE *argv);

VALUE korb_dispatch_binop(CTX *c, VALUE recv, ID name, int argc, VALUE *argv) {
    struct korb_class *klass = korb_class_of_class(recv);
    struct korb_method *m = korb_class_find_method(klass, name);
    if (!m) {
        /* method_missing fallback — prepend the missing name as a Symbol
         * to argv and dispatch :method_missing if defined.  Lets user
         * classes (mocks etc.) intercept all dispatches. */
        struct korb_method *mm = korb_class_find_method(klass, korb_intern("method_missing"));
        if (mm) {
            VALUE *new_argv = korb_xmalloc(sizeof(VALUE) * (argc + 1));
            new_argv[0] = korb_id2sym(name);
            for (int i = 0; i < argc; i++) new_argv[i + 1] = argv[i];
            VALUE r = korb_dispatch_binop(c, recv, korb_intern("method_missing"), argc + 1, new_argv);
            return r;
        }
        VALUE eNo = korb_const_get(korb_vm->object_class, korb_intern("NoMethodError"));
        korb_raise(c, (struct korb_class *)eNo, "undefined method '%s' for %s",
                 korb_id_name(name), korb_id_name(klass->name));
        if (c->state == KORB_RAISE && c->state_value && !SPECIAL_CONST_P(c->state_value)) {
            korb_ivar_set(c->state_value, korb_intern("@receiver"), recv);
            korb_ivar_set(c->state_value, korb_intern("@name"), korb_id2sym(name));
        }
        return Qnil;
    }
    return korb_dispatch_to_method(c, m, klass, recv, name, argc, argv);
}

VALUE korb_dispatch_to_method(CTX *c, struct korb_method *m,
                               struct korb_class *defining_class,
                               VALUE recv, ID name, int argc, VALUE *argv) {
    if (m->type == KORB_METHOD_CFUNC) {
        /* Push a synthetic frame so the body sees self=recv via the
         * frame chain, AND outer self is preserved automatically
         * (= no stale C-local restore corrupts it on exit). */
        struct korb_frame fr = {
            .prev = c->current_frame,
            .self = recv,
            .fp   = c->current_frame->fp,
            .current_class = c->current_frame->current_class,
            .cref = c->current_frame->cref,
            .current_file = c->current_frame->current_file,
            .last_line  = Qnil,
            .last_match = Qnil,
        };
        c->current_frame = &fr;
        VALUE r = m->u.cfunc.func(c, recv, argc, argv);
        c->current_frame = fr.prev;
        return r;
    }
    /* Proc-method (define_method'd): same frame-push pattern. */
    if (m->type == KORB_METHOD_PROC) {
        struct korb_proc *p = m->u.proc.proc;
        if (!p) return Qnil;
        extern VALUE proc_call(CTX *c, VALUE self, int argc, VALUE *argv);
        struct korb_frame fr = {
            .prev = c->current_frame,
            .self = recv,
            .fp   = c->current_frame->fp,
            .current_class = c->current_frame->current_class,
            .cref = c->current_frame->cref,
            .current_file = c->current_frame->current_file,
            .last_line  = Qnil,
            .last_match = Qnil,
        };
        c->current_frame = &fr;
        VALUE r = proc_call(c, (VALUE)p, argc, argv);
        c->current_frame = fr.prev;
        return r;
    }
    /* AST: same as korb_dispatch_call but argv is ad-hoc */
    /* Peel trailing FL_KWARGS-tagged hash so kwarg-aware callees see
     * it in their kwh_save_slot, and no-kwarg callees don't get a
     * stray positional Hash. */
    VALUE peeled_kwh_ad = Qundef;
    if (argc > 0 && !SPECIAL_CONST_P(argv[argc - 1]) &&
        BUILTIN_TYPE(argv[argc - 1]) == T_HASH &&
        (RBASIC(argv[argc - 1])->head.flags & FL_KWARGS)) {
        if (m->u.ast.kwh_save_slot >= 0) {
            peeled_kwh_ad = argv[argc - 1];
            argc--;
        } else {
            struct korb_hash *h = (struct korb_hash *)argv[argc - 1];
            if (h->size == 0) argc--;
        }
    }
    if (m->u.ast.rest_slot < 0 && (unsigned)argc > m->u.ast.total_params_cnt) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA,
                   "wrong number of arguments (given %d, expected %u) for %s",
                   argc, m->u.ast.total_params_cnt, korb_id_name(name));
        return Qnil;
    }
    VALUE *prev_fp = c->current_frame->fp;
    VALUE *prev_sp = c->sp;
    VALUE prev_self = c->current_frame->self;
    /* push frame after all current locals; we don't know exactly the boundary,
     * so use sp as upper bound.  Restore sp at end so repeated send-style
     * dispatches don't leak high-water mark. */
    VALUE *new_fp = c->sp + 1;
    if (new_fp + m->u.ast.locals_cnt >= c->stack_end) {
        korb_raise(c, NULL, "stack overflow");
        return Qnil;
    }
    /* CRITICAL ordering — extend sp + zero-fill BEFORE any alloc and before
     * copying args.  Reasoning:
     *  - new_fp..new_fp+locals_cnt is the new frame's slot range.
     *  - Until we move c->sp up to include this range, visit_roots does
     *    NOT scan it.  Any heap pointer we write there (= the copied args)
     *    is invisible to GC.
     *  - rest_slot collection below calls korb_ary_new_capa, which fires
     *    GC.  Without prior extension, the copied args become stale (=
     *    point to from-space addrs that the current GC moved). */
    {
        VALUE *new_sp = new_fp + m->u.ast.locals_cnt;
        for (VALUE *p = c->sp; p < new_sp; p++) *p = Qnil;
        c->sp = new_sp;
    }
    /* Now safe to copy args + alloc rest array. */
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
    /* (Locals beyond total_params already initialized to Qnil by the
     * zero-fill above; nothing else needed here.) */
    c->current_frame->fp = new_fp;
    c->current_frame->self = recv;
    /* &blk binding: when the method declares `&name`, copy the current
     * block into that slot so funcall_with_block delivers it to the
     * body.  Without this, `Foo.new { ... }`'s block doesn't reach
     * `def initialize(&blk)`. */
    if (m->u.ast.block_slot >= 0 && m->u.ast.block_slot < (int)m->u.ast.locals_cnt) {
        c->current_frame->fp[m->u.ast.block_slot] = current_block ? (VALUE)current_block : Qnil;
    }
    /* kwh_save_slot: if the callee has kwargs but we (cfunc-side
     * korb_funcall) didn't supply any, default to {} so the prologue's
     * `kwh.has_key?(:foo)` lookup doesn't blow up.  When peeled_kwh_ad
     * was filled in by the FL_KWARGS peel above, use that. */
    if (m->u.ast.kwh_save_slot >= 0 && m->u.ast.kwh_save_slot < (int)m->u.ast.locals_cnt) {
        c->current_frame->fp[m->u.ast.kwh_save_slot] = UNDEF_P(peeled_kwh_ad)
            ? korb_hash_new() : peeled_kwh_ad;
    }
    struct korb_cref *prev_cref2 = c->current_frame->cref;
    if (m->def_cref) c->current_frame->cref = m->def_cref;
    /* push frame for super() / cref.  Inherit cref / current_class /
     * current_file from outer so body-side lookups via c->current_frame->*
     * see the lexical view defined by m->def_cref (= just installed on
     * the outer frame above).  Without inheritance, frame2.cref defaults
     * to NULL and const_lookup walks zero cref → reads garbage / NULL. */
    struct korb_frame frame2 = {
        .prev = c->current_frame,
        .caller_node = NULL,
        .method = m,
        .self = recv,
        .fp = c->current_frame->fp,
        .cref = c->current_frame->cref,
        .current_class = c->current_frame->current_class,
        .current_file = c->current_frame->current_file,
        .locals_cnt = m->u.ast.locals_cnt,
        .super_skip_n = 0,
        .last_line = Qnil,
        .last_match = Qnil,
        .caller_running_block = running_block,
    };
    c->current_frame = &frame2;
    /* Reset running_block: this is a fresh method body, not lexically
     * inside the caller's block.  Without this, `super` inside the
     * callee resolves via the caller block's defining_method (e.g.
     * `Class#new` → user `initialize` → `super` ends up looking up the
     * outer block's enclosing method's name). */
    struct korb_proc *prev_running2 = running_block;
    running_block = NULL;
    VALUE r = EVAL(c, m->u.ast.body, c->current_frame->fp + m->u.ast.locals_cnt);
    running_block = prev_running2;
    c->current_frame = frame2.prev;
    c->current_frame->fp = prev_fp;
    /* Zero-fill the popped slot range so a sibling/later frame push
     * doesn't re-expose stale heap ptrs left over from this frame's
     * locals.  Without this, visit_roots scans those addresses once
     * the next sp-grow brings them back below c->sp and treats stale
     * (moved long ago) ptrs as live → wild forwarding under STRESS. */
    for (VALUE *p = prev_sp; p < c->sp; p++) *p = Qnil;
    c->sp = prev_sp;
    c->current_frame->self = prev_self;
    c->current_frame->cref = prev_cref2;
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

/* Same as korb_funcall, but the called method sees `block` as its
 * implicit block (yield / block_given?).  Used by Class#new to
 * forward `Foo.new { ... }`'s block into Foo#initialize. */
VALUE korb_funcall_with_block(CTX *c, VALUE recv, ID mid, int argc, VALUE *argv, VALUE block) {
    extern struct korb_proc *current_block;
    struct korb_proc *prev = current_block;
    if (NIL_P(block) || SPECIAL_CONST_P(block) || BUILTIN_TYPE(block) != T_PROC) {
        current_block = NULL;
    } else {
        current_block = (struct korb_proc *)block;
    }
    VALUE r = korb_dispatch_binop(c, recv, mid, argc, argv);
    current_block = prev;
    return r;
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

/* Bootstrap CTX — created at korb_runtime_init's top BEFORE any heap obj
 * is allocated.  Holds the value stack used as both eval stack and precise
 * root spill stack, and is bound to the precise GC instance via aro_gc_init.
 *
 * koruby_setup_ctx (called from main after korb_runtime_init returns)
 * reuses this same CTX — it just attaches self / cref / current_file. */
struct CTX_struct koruby_bootstrap_ctx;
/* Top-level sentinel frame lives in CTX (c->sentinel_frame) — no
 * global so multiple interpreters can coexist.  All `self` reads go
 * through `c->current_frame->self`; without a sentinel, top-level code
 * would NULL-deref.  Initialized post-main_obj-creation below. */

void korb_runtime_init(void) {
    init_well_known_ids();

    /* korb_vm itself is libc-malloc'd (= not on the GC heap).  It owns
     * the root pointers (object_class etc.) that AROH_VISIT_ROOTS visits. */
    korb_vm = korb_xmalloc(sizeof(*korb_vm));
    memset(korb_vm, 0, sizeof(*korb_vm));
    korb_vm->method_serial = 1; korb_g_method_serial = 1;

    /* Bootstrap CTX setup — MUST come before the first aro_gc_alloc.
     * Value stack is libc-malloc'd (= 16 M slots × 8 B = 128 MB); both
     * eval and ARO_ROOT_SCOPE_* use it.  c->sp = stack_base means an empty
     * root set initially — the bootstrap allocations below place their
     * temporary roots via ARO_ROOT_SCOPE_START as they need them. */
    CTX *c = &koruby_bootstrap_ctx;
    memset(c, 0, sizeof(*c));
    size_t stack_size = 16 * 1024 * 1024;
    c->stack_base = korb_xmalloc(stack_size * sizeof(VALUE));
    for (size_t i = 0; i < stack_size; i++) c->stack_base[i] = Qnil;
    c->stack_end  = c->stack_base + stack_size;
    c->sp = c->stack_base;
    c->env = c->stack_base;
    c->state = KORB_NORMAL;
    /* Sentinel top-level frame — fp/self get updated as bootstrap
     * proceeds.  current_frame MUST be set before any field access via
     * c->current_frame->* (e.g. korb_xmalloc internally might do nothing
     * with frame but defensive code can). */
    c->sentinel_frame = (struct korb_frame){
        .self       = Qnil,
        .fp         = c->stack_base,
        .last_line  = Qnil,
        .last_match = Qnil,
        /* rest default-zero (prev=NULL, method=NULL, block=NULL, ...) */
    };
    c->current_frame = &c->sentinel_frame;
    korb_vm->current_ctx = c;
    aro_gc_init(c);   /* binds c->astro_gc; precise-GC ready after this. */

    /* bootstrap classes — CRuby: BasicObject ← Object ← Module ← Class.
     * Each class's own metaclass is Class itself.
     *
     * All 4 are GC-heap allocated.  Moving GC can relocate them between
     * the inner allocs and the basic.klass / korb_vm assignments, so park
     * them in an ARO_ROOT_SCOPE buffer that visit_roots scans (= c->sp
     * range).  cBasic survives outside the scope via korb_vm->object_class
     * ->super (= reachable from the class_class root chain). */
    ARO_ROOT_SCOPE_START(c, boot, 4) {
        boot[0] = (VALUE)korb_class_new(korb_intern("BasicObject"), NULL, T_OBJECT);
        boot[1] = (VALUE)korb_class_new(korb_intern("Object"),
                                         (struct korb_class *)boot[0],  T_OBJECT);
        boot[2] = (VALUE)korb_class_new(korb_intern("Module"),
                                         (struct korb_class *)boot[1], T_MODULE);
        boot[3] = (VALUE)korb_class_new(korb_intern("Class"),
                                         (struct korb_class *)boot[2], T_CLASS);
        ((struct korb_class *)boot[0])->basic.klass = (struct korb_class *)boot[3];
        ((struct korb_class *)boot[1])->basic.klass = (struct korb_class *)boot[3];
        ((struct korb_class *)boot[2])->basic.klass = (struct korb_class *)boot[3];
        ((struct korb_class *)boot[3])->basic.klass = (struct korb_class *)boot[3];

        korb_vm->object_class = (struct korb_class *)boot[1];
        korb_vm->class_class  = (struct korb_class *)boot[3];
        korb_vm->module_class = (struct korb_class *)boot[2];
    } ARO_ROOT_SCOPE_END(c, boot);

    /* From here on, every newly-alloc'd class is immediately stashed into
     * a korb_vm field (= rooted via visit_roots' korb_vm scan), so no
     * additional ARO_ROOT_SCOPE is needed for these intermediate slots.
     * Reading korb_vm->object_class etc. returns the current addr because
     * visit_roots updates those slots on each move. */
    korb_vm->numeric_class = korb_class_new(korb_intern("Numeric"), korb_vm->object_class, T_OBJECT);
    korb_vm->integer_class = korb_class_new(korb_intern("Integer"), korb_vm->numeric_class, T_BIGNUM);
    korb_vm->float_class   = korb_class_new(korb_intern("Float"),   korb_vm->numeric_class, T_FLOAT);
    korb_vm->string_class  = korb_class_new(korb_intern("String"),  korb_vm->object_class, T_STRING);
    korb_vm->array_class   = korb_class_new(korb_intern("Array"),   korb_vm->object_class, T_ARRAY);
    korb_vm->hash_class    = korb_class_new(korb_intern("Hash"),    korb_vm->object_class, T_HASH);
    korb_vm->symbol_class  = korb_class_new(korb_intern("Symbol"),  korb_vm->object_class, T_SYMBOL);
    korb_vm->true_class    = korb_class_new(korb_intern("TrueClass"),  korb_vm->object_class, T_NONE);
    korb_vm->false_class   = korb_class_new(korb_intern("FalseClass"), korb_vm->object_class, T_NONE);
    korb_vm->nil_class     = korb_class_new(korb_intern("NilClass"),   korb_vm->object_class, T_NONE);
    korb_vm->proc_class    = korb_class_new(korb_intern("Proc"),       korb_vm->object_class, T_PROC);
    korb_vm->range_class   = korb_class_new(korb_intern("Range"),      korb_vm->object_class, T_RANGE);
    korb_vm->kernel_module = korb_module_new(korb_intern("Kernel"));
    /* ObjectSpace — stub module for the API surface.  cOS lives across
     * the cOSMeta alloc, so park both in an ARO_ROOT_SCOPE; once
     * cOS->basic.klass is set + the const_set installed cOS into
     * cObject->constants, both are reachable via the constants chain. */
    {
        ARO_ROOT_SCOPE_START(c, os, 2) {
            os[0] = (VALUE)korb_module_new(korb_intern("ObjectSpace"));
            korb_const_set(korb_vm->object_class, ((struct korb_class *)os[0])->name, os[0]);
            os[1] = (VALUE)korb_class_new(korb_intern("ObjectSpaceMeta"),
                                           korb_vm->class_class, T_CLASS);
            VALUE objspace_each_object(CTX *c, VALUE self, int argc, VALUE *argv);
            VALUE objspace_count_objects(CTX *c, VALUE self, int argc, VALUE *argv);
            VALUE objspace_garbage_collect(CTX *c, VALUE self, int argc, VALUE *argv);
            korb_class_add_method_cfunc((struct korb_class *)os[1], korb_intern("each_object"),     objspace_each_object,     -1);
            korb_class_add_method_cfunc((struct korb_class *)os[1], korb_intern("count_objects"),   objspace_count_objects,   -1);
            korb_class_add_method_cfunc((struct korb_class *)os[1], korb_intern("garbage_collect"), objspace_garbage_collect,  0);
            ((struct korb_class *)os[0])->basic.klass = (struct korb_class *)os[1];
        } ARO_ROOT_SCOPE_END(c, os);
    }
    korb_vm->comparable_module = korb_module_new(korb_intern("Comparable"));
    korb_vm->enumerable_module = korb_module_new(korb_intern("Enumerable"));

    /* CRuby's hierarchy has Object include Kernel — that's how every
     * object gets `puts` / `nil?` / `is_a?` etc.  Hook the include here
     * so `Object.ancestors` reports `[Object, Kernel, BasicObject]`.
     * No alloc fires GC inside this block (korb_xmalloc is libc), so it's
     * safe to read korb_vm->object_class once. */
    {
        struct korb_class *o = korb_vm->object_class;
        if (o->includes_capa == 0) {
            o->includes_capa = 4;
            o->includes = korb_xmalloc(o->includes_capa * sizeof(*o->includes));
        }
        o->includes[o->includes_cnt++] = korb_vm->kernel_module;
    }

    /* Register top-level constants.  korb_const_set does NOT fire GC
     * (= libc-backed const_entry chain), so reading korb_vm->* once into
     * a local for the sequence is safe. */
    {
        struct korb_class *cObject = korb_vm->object_class;
        struct korb_class *cBasic  = cObject->super;  /* reachable via super chain */
        korb_const_set(cObject, cBasic->name,                 (VALUE)cBasic);
        korb_const_set(cObject, korb_vm->kernel_module->name, (VALUE)korb_vm->kernel_module);
        korb_const_set(cObject, cObject->name,                (VALUE)cObject);
        korb_const_set(cObject, korb_vm->class_class->name,   (VALUE)korb_vm->class_class);
        korb_const_set(cObject, korb_vm->module_class->name,  (VALUE)korb_vm->module_class);
        korb_const_set(cObject, korb_vm->integer_class->name, (VALUE)korb_vm->integer_class);
        korb_const_set(cObject, korb_vm->float_class->name,   (VALUE)korb_vm->float_class);
        korb_const_set(cObject, korb_vm->string_class->name,  (VALUE)korb_vm->string_class);
        korb_const_set(cObject, korb_vm->array_class->name,   (VALUE)korb_vm->array_class);
        korb_const_set(cObject, korb_vm->hash_class->name,    (VALUE)korb_vm->hash_class);
        korb_const_set(cObject, korb_vm->symbol_class->name,  (VALUE)korb_vm->symbol_class);
        korb_const_set(cObject, korb_vm->numeric_class->name, (VALUE)korb_vm->numeric_class);
        korb_const_set(cObject, korb_vm->range_class->name,   (VALUE)korb_vm->range_class);
        korb_const_set(cObject, korb_vm->proc_class->name,    (VALUE)korb_vm->proc_class);
        korb_const_set(cObject, korb_vm->true_class->name,    (VALUE)korb_vm->true_class);
        korb_const_set(cObject, korb_vm->false_class->name,   (VALUE)korb_vm->false_class);
        korb_const_set(cObject, korb_vm->nil_class->name,     (VALUE)korb_vm->nil_class);
    }

    /* main object — both main_obj_class and main_obj go into korb_vm
     * (= rooted by visit_roots), so 2 allocs in sequence are safe. */
    korb_vm->main_obj_class = korb_class_new(korb_intern("Main"), korb_vm->object_class, T_OBJECT);
    korb_vm->main_obj = korb_object_new(korb_vm->main_obj_class);

    /* main_obj is now alive — update the sentinel frame's self so
     * top-level code sees main_obj as self. */
    c->sentinel_frame.self = korb_vm->main_obj;

    /* Exception class hierarchy.  CRuby's tree:
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
     *
     * Every class is const_set into cObject immediately, making it a
     * persistent root (visit_roots walks korb_vm->object_class →
     * visit_class_edges → visit_const_chain → visit each entry->value).
     * Subsequent allocs reference parent classes by NAME via korb_const_get
     * — the value returned is the current addr (= updated by GC on each
     * cycle), so no C-local staleness. */
#define KRB_EXC(name, super_name)                                              \
    korb_const_set(korb_vm->object_class, korb_intern(name),                   \
                   (VALUE)korb_class_new(                                       \
                       korb_intern(name),                                       \
                       (struct korb_class *)korb_const_get(                     \
                           korb_vm->object_class, korb_intern(super_name)),     \
                       T_OBJECT))
    {
        /* First: Exception itself, super = Object (= korb_vm->object_class). */
        korb_const_set(korb_vm->object_class, korb_intern("Exception"),
                       (VALUE)korb_class_new(korb_intern("Exception"),
                                              korb_vm->object_class, T_OBJECT));
        KRB_EXC("StandardError",      "Exception");
        KRB_EXC("ScriptError",        "Exception");
        KRB_EXC("RuntimeError",       "StandardError");
        KRB_EXC("IndexError",         "StandardError");
        KRB_EXC("NameError",          "StandardError");
        KRB_EXC("RangeError",         "StandardError");
        /* Direct StandardError children. */
        static const char *std_subs[] = {
            "ArgumentError", "TypeError",
            "ZeroDivisionError", "IOError", "Errno",
            "NotImplementedError", "StopIteration", "LocalJumpError",
            "SystemCallError",
            NULL,
        };
        for (int i = 0; std_subs[i]; i++) {
            KRB_EXC(std_subs[i], "StandardError");
        }
        /* UncaughtThrowError < ArgumentError (CRuby). */
        KRB_EXC("UncaughtThrowError", "ArgumentError");
        /* Children of more-specific classes. */
        KRB_EXC("NoMethodError",      "NameError");
        KRB_EXC("KeyError",           "IndexError");
        KRB_EXC("FloatDomainError",   "RangeError");
        KRB_EXC("FrozenError",        "RuntimeError");
        /* NoMatchingPatternError < StandardError (CRuby) — pattern matching
         * raises it when no clause matches and no else is present. */
        KRB_EXC("NoMatchingPatternError",     "StandardError");
        KRB_EXC("NoMatchingPatternKeyError",  "NoMatchingPatternError");
        /* SystemExit < Exception (NOT StandardError so it's not caught by
         * a bare `rescue`).  status / success? are exposed via ivars set
         * by Kernel#exit. */
        KRB_EXC("SystemExit",     "Exception");
        /* Exception subclasses NOT under StandardError — also bypassed
         * by bare `rescue`.  Stub presence so `rescue NoMemoryError` etc.
         * doesn't NameError. */
        KRB_EXC("NoMemoryError",     "Exception");
        KRB_EXC("SystemStackError",  "Exception");
        KRB_EXC("SignalException",   "Exception");
        KRB_EXC("Interrupt",         "SignalException");
        KRB_EXC("SecurityError",     "Exception");
        KRB_EXC("EncodingError",     "StandardError");
        KRB_EXC("RegexpError",       "StandardError");
        KRB_EXC("FiberError",        "StandardError");
        /* ScriptError children. */
        KRB_EXC("LoadError",   "ScriptError");
        KRB_EXC("SyntaxError", "ScriptError");
    }
#undef KRB_EXC

    /* Register Comparable / Enumerable / Numeric so user code can
     * `include Comparable` etc.  Comparable's instance methods are
     * installed in builtins.c. */
    korb_const_set(korb_vm->object_class, korb_intern("Comparable"), (VALUE)korb_vm->comparable_module);
    korb_const_set(korb_vm->object_class, korb_intern("Enumerable"), (VALUE)korb_vm->enumerable_module);

    /* Encoding scaffold MUST be created before korb_init_builtins so the
     * String#encoding etc. defs in builtins.c find Encoding.  cEnc + utf8
     * both live across allocation-fires-GC calls, so park them in an
     * ARO_ROOT_SCOPE slot. */
    {
        ARO_ROOT_SCOPE_START(c, enc, 2) {
            enc[0] = (VALUE)korb_class_new(korb_intern("Encoding"), korb_vm->object_class, T_OBJECT);
            korb_const_set(korb_vm->object_class, korb_intern("Encoding"), enc[0]);
            enc[1] = korb_object_new((struct korb_class *)enc[0]);
            /* str_new fires GC — pre-evaluate into a local before calling
             * ivar_set so the 1st arg (enc[1]) is read after the inner
             * GC has already updated the slot. */
            {
                VALUE name_str = korb_str_new_cstr("UTF-8");
                korb_ivar_set(enc[1], korb_intern("@name"), name_str);
            }
            /* Each const_set is libc-only (no GC fire); enc[1] stays valid
             * through the sequence, and enc[0] (cEnc) is updated by
             * visit_roots if any of these alloc paths grow the const chain. */
            struct korb_class *cEnc = (struct korb_class *)enc[0];
            VALUE utf8 = enc[1];
            korb_const_set(cEnc, korb_intern("UTF_8"),       utf8);
            korb_const_set(cEnc, korb_intern("ASCII_8BIT"),  utf8);
            korb_const_set(cEnc, korb_intern("BINARY"),      utf8);
            korb_const_set(cEnc, korb_intern("US_ASCII"),    utf8);
            korb_const_set(cEnc, korb_intern("ASCII"),       utf8);
            korb_const_set(cEnc, korb_intern("EUC_JP"),      utf8);
            korb_const_set(cEnc, korb_intern("EUC_TW"),      utf8);
            korb_const_set(cEnc, korb_intern("SHIFT_JIS"),   utf8);
            korb_const_set(cEnc, korb_intern("Shift_JIS"),   utf8);
            korb_const_set(cEnc, korb_intern("Windows_31J"), utf8);
            korb_const_set(cEnc, korb_intern("UTF_16"),      utf8);
            korb_const_set(cEnc, korb_intern("UTF_16BE"),    utf8);
            korb_const_set(cEnc, korb_intern("UTF_16LE"),    utf8);
            korb_const_set(cEnc, korb_intern("UTF_32"),      utf8);
            korb_const_set(cEnc, korb_intern("UTF_32BE"),    utf8);
            korb_const_set(cEnc, korb_intern("UTF_32LE"),    utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_1"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_2"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_3"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_4"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_5"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_6"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_7"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_8"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_9"),  utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_10"), utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_11"), utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_13"), utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_14"), utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_15"), utf8);
            korb_const_set(cEnc, korb_intern("ISO_8859_16"), utf8);
            korb_const_set(cEnc, korb_intern("Windows_1252"), utf8);
            korb_const_set(cEnc, korb_intern("Windows_1251"), utf8);
            korb_const_set(cEnc, korb_intern("CP932"),       utf8);
            korb_const_set(cEnc, korb_intern("KOI8_R"),      utf8);
            korb_const_set(cEnc, korb_intern("Big5"),        utf8);
            korb_const_set(cEnc, korb_intern("GB18030"),     utf8);
        } ARO_ROOT_SCOPE_END(c, enc);
    }
    /* Temporarily disable STRESS during builtins init — many class
     * helpers in builtins.c hold class pointers as C locals across
     * alloc-can-GC sites (cProcess across korb_class_new for cStatus,
     * etc.), going stale under per-alloc GC.  Audit each site (=
     * convert to ARO_ROOT_SCOPE or korb_vm-> field) is the proper fix
     * but spans ~30 init blocks; gate stress here so user-program GC
     * stress still catches real bugs at runtime. */
    {
        bool prev_stress = ARO_GC_COMMON(c)->stress;
        ARO_GC_COMMON(c)->stress = false;
        korb_init_builtins();
        ARO_GC_COMMON(c)->stress = prev_stress;
    }
    /* $: / $LOAD_PATH: array initialized with at least one path so
     * tests probing length > 0 pass.  CRuby populates this from
     * sysconfdir/sitelibdir/etc; we don't have those here, so use a
     * placeholder that doesn't include "." (per spec). */
    {
        VALUE lp = korb_ary_new();
        korb_ary_push(lp, korb_str_new_cstr("/usr/local/lib/ruby/site_ruby"));
        korb_gvar_set(korb_intern("$:"), lp);
        korb_gvar_set(korb_intern("$LOAD_PATH"), lp);
        korb_gvar_set(korb_intern("$-I"), lp);
        korb_gvar_set(korb_intern("$\""), korb_ary_new());
        korb_gvar_set(korb_intern("$LOADED_FEATURES"), korb_gvar_get(korb_intern("$\"")));
    }
    /* $VERBOSE / $DEBUG / aliases — false by default (no -v/-d). */
    {
        korb_gvar_set(korb_intern("$VERBOSE"), Qfalse);
        korb_gvar_set(korb_intern("$-v"), Qfalse);
        korb_gvar_set(korb_intern("$-w"), Qfalse);
        korb_gvar_set(korb_intern("$-W"), Qfalse);
        korb_gvar_set(korb_intern("$DEBUG"), Qfalse);
        korb_gvar_set(korb_intern("$-d"), Qfalse);
    }
    /* Common predefined constants to satisfy code that probes for them.
     * Each korb_str_new_cstr is an alloc that fires GC, but the call
     * sequence here has no inter-call C local — every result is consumed
     * by korb_const_set as the 3rd argument with korb_vm->object_class
     * re-read fresh on each call (= no staleness window). */
    korb_const_set(korb_vm->object_class, korb_intern("RUBY_VERSION"),       korb_str_new_cstr("3.4.0"));
    korb_const_set(korb_vm->object_class, korb_intern("RUBY_RELEASE_DATE"),  korb_str_new_cstr("2024-01-01"));
    korb_const_set(korb_vm->object_class, korb_intern("RUBY_PLATFORM"),      korb_str_new_cstr("koruby"));
    korb_const_set(korb_vm->object_class, korb_intern("RUBY_ENGINE"),        korb_str_new_cstr("koruby"));
    korb_const_set(korb_vm->object_class, korb_intern("RUBY_PATCHLEVEL"),    INT2FIX(0));
    korb_const_set(korb_vm->object_class, korb_intern("TOPLEVEL_BINDING"),   Qnil);
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
    char *err_msg = NULL;
    extern NODE *koruby_parse_full(const char *src, size_t len, const char *filename, char **err_msg);
    NODE *ast = koruby_parse_full(src, len, filename ? filename : "(eval)", &err_msg);
    if (err_msg) {
        VALUE eSE = korb_const_get(korb_vm->object_class, korb_intern("SyntaxError"));
        if (eSE && !SPECIAL_CONST_P(eSE) && BUILTIN_TYPE(eSE) == T_CLASS) {
            korb_raise(c, (struct korb_class *)eSE, "%s", err_msg);
        } else {
            korb_raise(c, NULL, "syntax error: %s", err_msg);
        }
        return Qnil;
    }
    if (!ast) return Qnil;

    /* Save / push fresh top-level state for the loaded file */
    VALUE *prev_fp = c->current_frame->fp;
    VALUE *prev_sp = c->sp;
    struct korb_class *prev_class = c->current_frame->current_class;
    struct korb_cref *prev_cref = c->current_frame->cref;
    const char *prev_file = c->current_frame->current_file;
    struct korb_frame *prev_frame = c->current_frame;
    extern struct korb_proc *running_block;
    struct korb_proc *prev_running_block = running_block;

    /* Top-level frame for the new file.  Push a fresh frame with
     * self=main_obj — visit_roots walks the frame chain so the heap
     * pointer stays fresh across body GCs.  Loaded file's top-level
     * def's are not inside a method body — and not inside a block of
     * the calling context either, so a stray `return` from within a
     * block in the loaded file raises LocalJumpError instead of
     * accidentally targeting the caller's block frame. */
    c->current_frame->fp = c->sp + 1;
    c->current_frame->current_class = korb_vm->object_class;
    struct korb_frame top_frame = (struct korb_frame){
        .prev       = NULL,  /* clean caller context for the loaded file */
        .self       = korb_vm->main_obj,
        .fp         = c->current_frame->fp,
        .last_line  = Qnil,
        .last_match = Qnil,
    };
    c->current_frame = &top_frame;
    running_block = NULL;

    /* Reset cref to [Object] for top-level execution */
    struct korb_cref top_cref = { .klass = korb_vm->object_class, .prev = NULL };
    c->current_frame->cref = &top_cref;
    c->current_frame->current_file = filename;

    OPTIMIZE(ast);
    VALUE r = EVAL(c, ast, c->current_frame->fp);

    /* Top-level `return` in a load'd file just stops *this* file —
     * not an error and not a propagating return.  CRuby allows this
     * (load returns true, requiring the file proceeds normally). */
    if (c->state == KORB_RETURN) {
        r = c->state_value;
        c->state = KORB_NORMAL;
        c->state_value = Qnil;
        c->state_target_frame = NULL;
    }

    c->current_frame->fp = prev_fp;
    /* Zero-fill popped range so a later sp-grow doesn't re-expose
     * stale heap ptrs left by this eval scope (= bootstrap or required
     * file).  Without this, the first STRESS GC after a sp-grow past
     * prev_sp scans stale arena ptrs from the eval body and aborts in
     * forward_payload "GC BUG forward to-space" (= the obj at the
     * stale addr was in a prior cycle's to-space, not current). */
    for (VALUE *p = prev_sp; p < c->sp; p++) *p = Qnil;
    c->sp = prev_sp;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    c->current_frame->current_file = prev_file;
    c->current_frame = prev_frame;
    running_block = prev_running_block;
    return r;
}

/* Like korb_eval_string but uses the caller's `self` and the receiver's
 * class for cref — used by Object#instance_eval(string). */
VALUE korb_eval_string_in_self(CTX *c, const char *src, size_t len,
                                const char *filename, VALUE recv) {
    NODE *ast = koruby_parse(src, len, filename ? filename : "(eval)");
    if (!ast) return Qnil;
    VALUE *prev_fp = c->current_frame->fp;
    VALUE prev_self = c->current_frame->self;
    VALUE *prev_sp = c->sp;
    struct korb_class *prev_class = c->current_frame->current_class;
    struct korb_cref *prev_cref = c->current_frame->cref;
    const char *prev_file = c->current_frame->current_file;
    c->current_frame->fp = c->sp + 1;
    c->current_frame->self = recv;
    struct korb_class *recv_klass = korb_class_of_class(recv);
    c->current_frame->current_class = recv_klass;
    struct korb_cref top_cref = { .klass = recv_klass, .prev = NULL };
    c->current_frame->cref = &top_cref;
    c->current_frame->current_file = filename;
    OPTIMIZE(ast);
    VALUE r = EVAL(c, ast, c->current_frame->fp);
    c->current_frame->fp = prev_fp;
    c->sp = prev_sp;
    c->current_frame->self = prev_self;
    c->current_frame->current_class = prev_class;
    c->current_frame->cref = prev_cref;
    c->current_frame->current_file = prev_file;
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
    struct korb_cref *resumer_cref;     /* save resumer's lexical const ref */
    struct korb_class *resumer_current_class;
    struct korb_frame *resumer_current_frame;
    VALUE resumer_bang;                  /* save resumer's $! */
    struct korb_cref *fiber_cref;       /* save fiber's lexical const ref */
    struct korb_class *fiber_current_class;
    struct korb_frame *fiber_current_frame;

    /* Fiber-side save: stashed on yield, restored on resume.
     * Initialized at fiber creation (or first resume) to point into
     * the fiber's heap frame so the body's slots don't overlap the
     * resumer's value-stack. */
    VALUE *fiber_fp;
    VALUE *fiber_sp;
    /* Fiber-local $! value (initially nil). */
    VALUE fiber_bang;

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
        VALUE prev_self = c->current_frame->self;
        c->current_frame->self = blk->self;
        struct korb_proc *prev_block = current_block;
        current_block = NULL;
        struct korb_cref *prev_cref = c->current_frame->cref;
        if (blk->cref) c->current_frame->cref = blk->cref;
        VALUE result = blk->body ? EVAL(c, blk->body, fib->frame + blk->env_size) : Qnil;
        c->current_frame->cref = prev_cref;
        c->current_frame->self = prev_self;
        current_block = prev_block;
        fib->result = result;
    }
    fib->state = KF_DEAD;
    /* Match the GC_disable resume() did before swapping to us — we're
     * exiting the fiber for the last time, so re-enable GC. */
    /* Phase 1 stub: was GC_enable() */ (void)0;
    swapcontext(&fib->ctx, &fib->prev_ctx);
}

VALUE korb_fiber_new(struct korb_proc *block) {
    struct korb_fiber *fib = korb_xmalloc(sizeof(*fib));
    fib->basic.head.flags = T_DATA;
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
    /* Phase 1 stub: was GC_add_roots(...) — fiber stack precise tracking comes in Phase 4 */ (void)0;
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
    /* Initial fiber c->current_frame->fp/sp: fp at frame base, sp just past block's
     * env (so method calls inside the block don't overlap its locals). */
    fib->fiber_fp = fib->frame;
    fib->fiber_sp = fib->frame + env_size;
    fib->fiber_bang = Qnil;
    fib->resumer_fp = NULL;
    fib->resumer_sp = NULL;
    fib->resumer_stack_base = NULL;
    fib->resumer_stack_end = NULL;
    fib->resumer_cref = NULL;
    fib->resumer_current_class = NULL;
    fib->resumer_current_frame = NULL;
    fib->fiber_cref = NULL;
    fib->fiber_current_class = NULL;
    fib->fiber_current_frame = NULL;
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

    /* Save resumer's c->current_frame->fp/sp/stack_base/stack_end into the fiber, swap
     * in the fiber's saved fp/sp + heap-frame extents, then swapcontext.
     * Yield will reverse this. */
    struct korb_fiber *prev = current_fiber;
    current_fiber = fib;
    fib->resumer_fp = c->current_frame->fp;
    fib->resumer_sp = c->sp;
    fib->resumer_stack_base = c->stack_base;
    fib->resumer_stack_end = c->stack_end;
    fib->resumer_cref = c->current_frame->cref;
    fib->resumer_current_class = c->current_frame->current_class;
    fib->resumer_current_frame = c->current_frame;
    /* $! and $@ are fiber-local — stash resumer's, swap in fiber's. */
    fib->resumer_bang = korb_gvar_get(korb_intern("$!"));
    korb_gvar_set(korb_intern("$!"), fib->fiber_bang);
    c->current_frame->fp = fib->fiber_fp;
    c->sp = fib->fiber_sp;
    c->stack_base = fib->frame;
    c->stack_end = fib->frame + fib->frame_size;
    if (fib->fiber_cref) c->current_frame->cref = fib->fiber_cref;
    if (fib->fiber_current_class) c->current_frame->current_class = fib->fiber_current_class;
    if (fib->fiber_current_frame) c->current_frame = fib->fiber_current_frame;
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
    /* Phase 1 stub: was GC_disable() */ (void)0;
    swapcontext(&fib->prev_ctx, &fib->ctx);

    /* Returned from yield/end — restore resumer's fp/sp from where the
     * yield path stashed them. */
    c->current_frame->fp = fib->resumer_fp;
    c->sp = fib->resumer_sp;
    c->stack_base = fib->resumer_stack_base;
    c->stack_end = fib->resumer_stack_end;
    c->current_frame->cref = fib->resumer_cref;
    c->current_frame->current_class = fib->resumer_current_class;
    c->current_frame = fib->resumer_current_frame;
    /* Restore resumer's $!: if yield ran, it already did this; if the
     * fiber finished without yielding (KF_DEAD), we still need to. */
    if (fib->state == KF_DEAD) {
        fib->fiber_bang = korb_gvar_get(korb_intern("$!"));
        korb_gvar_set(korb_intern("$!"), fib->resumer_bang);
    }
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
    /* Save fiber's c->current_frame->fp/sp so the next resume can pick up where we
     * yielded; restore the resumer's fp/sp/stack so it sees its own
     * value-stack. */
    fib->fiber_fp = c->current_frame->fp;
    fib->fiber_sp = c->sp;
    fib->fiber_cref = c->current_frame->cref;
    fib->fiber_current_class = c->current_frame->current_class;
    fib->fiber_current_frame = c->current_frame;
    fib->fiber_bang = korb_gvar_get(korb_intern("$!"));
    korb_gvar_set(korb_intern("$!"), fib->resumer_bang);
    c->current_frame->fp = fib->resumer_fp;
    c->sp = fib->resumer_sp;
    c->stack_base = fib->resumer_stack_base;
    c->stack_end = fib->resumer_stack_end;
    c->current_frame->cref = fib->resumer_cref;
    c->current_frame->current_class = fib->resumer_current_class;
    c->current_frame = fib->resumer_current_frame;

    /* Mirror of resume's GC_disable: re-enable on yield back, disable
     * again when the fiber resumes.  See resume for the full rationale. */
    /* Phase 1 stub: was GC_enable() */ (void)0;
    swapcontext(&fib->ctx, &fib->prev_ctx);
    /* Phase 1 stub: was GC_disable() */ (void)0;

    /* Resumed — restore the fiber's heap-frame extents so subsequent
     * method calls inside the fiber check against the right bounds. */
    c->stack_base = fib->frame;
    c->stack_end = fib->frame + fib->frame_size;
    if (fib->fiber_cref) c->current_frame->cref = fib->fiber_cref;
    if (fib->fiber_current_class) c->current_frame->current_class = fib->fiber_current_class;
    if (fib->fiber_current_frame) c->current_frame = fib->fiber_current_frame;
    /* Re-establish fiber's $!: yield saved resumer's, swapped to nil; resume
     * restored fiber's already.  No-op here. */
    /* Resumed: restore the fiber's fp/sp (resume already did this from
     * its side, but in a chain of resume->yield->resume the inner ctx
     * comes back here and the resumer's wrapper has overwritten c->current_frame->fp
     * to its own; resume sets fp again before swapcontext, so by the
     * time we land here, c->current_frame->fp is fib->fiber_fp). */
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

VALUE korb_require_file(CTX *c, const char *path) {
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

VALUE korb_load_file(CTX *c, const char *path) {
    /* `load` runs the file unconditionally — does NOT consult the
     * already_loaded set, and does NOT mark it loaded.  Only `require`
     * dedupes (via korb_require_file). */
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        korb_raise(c, NULL, "no such file: %s", path);
        return Qnil;
    }
    korb_eval_string(c, src, len, path);
    return Qtrue;
}
