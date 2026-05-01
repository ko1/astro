// jstro runtime: string intern, object/array/function management,
// abstract operations from ECMA-262 §7.

#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "context.h"

// =====================================================================
// GC root state
// =====================================================================
uint32_t        JSTRO_BR = JS_BR_NORMAL;
JsValue         JSTRO_BR_VAL = JV_UNDEFINED;
const char     *JSTRO_BR_LABEL = NULL;

NODE          **JSTRO_NODE_ARR = NULL;
uint32_t       *JSTRO_U32_ARR  = NULL;
JsValue        *JSTRO_VAL_ARR  = NULL;
struct JsString **JSTRO_STR_ARR = NULL;

static uint32_t JSTRO_NODE_ARR_cap = 0, JSTRO_NODE_ARR_cnt = 0;
static uint32_t JSTRO_U32_ARR_cap  = 0, JSTRO_U32_ARR_cnt  = 0;
static uint32_t JSTRO_STR_ARR_cap  = 0, JSTRO_STR_ARR_cnt  = 0;

uint32_t jstro_node_arr_alloc(uint32_t cnt) {
    if (JSTRO_NODE_ARR_cnt + cnt > JSTRO_NODE_ARR_cap) {
        uint32_t nc = JSTRO_NODE_ARR_cap ? JSTRO_NODE_ARR_cap * 2 : 64;
        while (nc < JSTRO_NODE_ARR_cnt + cnt) nc *= 2;
        JSTRO_NODE_ARR = (NODE **)realloc(JSTRO_NODE_ARR, nc * sizeof(NODE *));
        JSTRO_NODE_ARR_cap = nc;
    }
    uint32_t r = JSTRO_NODE_ARR_cnt;
    JSTRO_NODE_ARR_cnt += cnt;
    return r;
}
uint32_t jstro_u32_arr_alloc(uint32_t cnt) {
    if (JSTRO_U32_ARR_cnt + cnt > JSTRO_U32_ARR_cap) {
        uint32_t nc = JSTRO_U32_ARR_cap ? JSTRO_U32_ARR_cap * 2 : 64;
        while (nc < JSTRO_U32_ARR_cnt + cnt) nc *= 2;
        JSTRO_U32_ARR = (uint32_t *)realloc(JSTRO_U32_ARR, nc * sizeof(uint32_t));
        JSTRO_U32_ARR_cap = nc;
    }
    uint32_t r = JSTRO_U32_ARR_cnt;
    JSTRO_U32_ARR_cnt += cnt;
    return r;
}
uint32_t jstro_str_arr_alloc(uint32_t cnt) {
    if (JSTRO_STR_ARR_cnt + cnt > JSTRO_STR_ARR_cap) {
        uint32_t nc = JSTRO_STR_ARR_cap ? JSTRO_STR_ARR_cap * 2 : 64;
        while (nc < JSTRO_STR_ARR_cnt + cnt) nc *= 2;
        JSTRO_STR_ARR = (struct JsString **)realloc(JSTRO_STR_ARR, nc * sizeof(struct JsString *));
        JSTRO_STR_ARR_cap = nc;
    }
    uint32_t r = JSTRO_STR_ARR_cnt;
    JSTRO_STR_ARR_cnt += cnt;
    return r;
}

// =====================================================================
// GC — simple mark-sweep with explicit root set.  The hard part is
// finding roots in alloca'd call frames; we maintain a linked list
// (`c->frame_stack`) that every call entry/exit pushes and pops.
// `try`-frames snapshot the stack head so `longjmp` doesn't leave
// stale pointers.
// =====================================================================

// Per-allocation size record so sweep can update bytes_allocated.
// We piggyback on GCHead by reserving 4 high bits of the type-byte
// `_pad` for a size hint — but it's easier to keep a parallel size in
// a fixed offset.  Cheat: store sizes in a separate alloc list.  Even
// easier: don't track allocated bytes precisely; just count entries.
// For threshold heuristics we use object count instead of bytes.

static void mark_value(JsValue v);
static void mark_object_struct(struct JsObject *o);

// Mark color: we use a single bit, flipping the polarity each cycle so
// we don't need a separate clear pass.
static uint8_t  g_mark_live = 1;
static uint8_t  g_mark_dead = 0;

static inline bool
gc_is_marked(struct GCHead *h)
{
    return h->mark == g_mark_live;
}

static inline void
gc_set_marked(struct GCHead *h)
{
    h->mark = g_mark_live;
}

// Forward.
static void mark_string(struct JsString *s);
static void mark_shape(struct JsShape *s);
static void mark_function(struct JsFunction *fn);
static void mark_cfunction(struct JsCFunction *cf);

static void
mark_string(struct JsString *s)
{
    if (!s) return;
    if (gc_is_marked(&s->gc)) return;
    gc_set_marked(&s->gc);
}

static void
mark_shape(struct JsShape *s)
{
    if (!s) return;
    if (gc_is_marked(&s->gc)) return;
    gc_set_marked(&s->gc);
    for (uint32_t i = 0; i < s->nslots; i++) mark_string(s->names[i]);
    for (uint32_t i = 0; i < s->ntrans; i++) {
        mark_string(s->trans[i].name);
        mark_shape(s->trans[i].to);
    }
    if (s->parent) mark_shape(s->parent);
}

static void
mark_object_struct(struct JsObject *o)
{
    if (!o) return;
    if (gc_is_marked(&o->gc)) return;
    gc_set_marked(&o->gc);
    if (o->shape) mark_shape(o->shape);
    for (uint32_t i = 0; i < o->shape->nslots; i++) mark_value(o->slots[i]);
    if (o->proto) mark_object_struct(o->proto);
}

static void
mark_array(struct JsArray *a)
{
    if (!a) return;
    if (gc_is_marked(&a->gc)) return;
    gc_set_marked(&a->gc);
    // Trace dense up to dense_capa (slots past length may still be live
    // if the array has been shrunk; safer to trace allocated capa).
    for (uint32_t i = 0; i < a->length && i < a->dense_capa; i++) {
        mark_value(a->dense[i]);
    }
    if (a->fallback) mark_object_struct(a->fallback);
    if (a->proto)    mark_object_struct(a->proto);
}

static void
mark_function(struct JsFunction *fn)
{
    if (!fn) return;
    if (gc_is_marked(&fn->gc)) return;
    gc_set_marked(&fn->gc);
    if (fn->upvals) {
        for (uint32_t i = 0; i < fn->nupvals; i++) {
            // upvals[i] is a JsValue * pointing into a JsBox.value (or
            // a stack slot of a still-live caller).  We can't easily
            // recover the JsBox from the value pointer; just trace the
            // pointed-to value as a root.
            mark_value(*fn->upvals[i]);
        }
    }
    if (fn->home_proto) mark_object_struct(fn->home_proto);
    if (fn->bound_this) mark_object_struct(fn->bound_this);
    if (fn->own_props)  mark_object_struct(fn->own_props);
}

static void
mark_cfunction(struct JsCFunction *cf)
{
    if (!cf) return;
    if (gc_is_marked(&cf->gc)) return;
    gc_set_marked(&cf->gc);
    if (cf->own_props) mark_object_struct(cf->own_props);
}

static void mark_value(JsValue v);  // forward
void
jstro_gc_mark_value(JsValue v) { mark_value(v); }

static void
mark_value(JsValue v)
{
    if (!JV_IS_PTR(v)) return;
    struct GCHead *h = (struct GCHead *)(uintptr_t)v;
    if (gc_is_marked(h)) return;
    switch (h->type) {
    case JS_TSTRING:    mark_string((struct JsString *)h); break;
    case JS_TFLOAT:     gc_set_marked(h); break;
    case JS_TBOX: {
        gc_set_marked(h);
        struct JsBox *b = (struct JsBox *)h;
        mark_value(b->value);
        break;
    }
    case JS_TOBJECT:    mark_object_struct((struct JsObject *)h); break;
    case JS_TARRAY:     mark_array((struct JsArray *)h); break;
    case JS_TFUNCTION:  mark_function((struct JsFunction *)h); break;
    case JS_TCFUNCTION: mark_cfunction((struct JsCFunction *)h); break;
    case JS_TERROR:     mark_object_struct((struct JsObject *)h); break;
    case JS_TACCESSOR:  mark_object_struct((struct JsObject *)h); break;
    case JS_TSYMBOL: {
        gc_set_marked(h);
        struct JsSymObj { struct GCHead gc; struct JsString *desc; } *sy =
            (struct JsSymObj *)h;
        if (sy->desc) mark_string(sy->desc);
        break;
    }
    case JS_TMAP:
    case JS_TSET: {
        gc_set_marked(h);
        struct JsMapData { struct GCHead gc; void *entries; uint32_t size, capa; uint8_t is_set; } *m
            = (struct JsMapData *)h;
        // entries is JsMapEntry { JsValue k, v; bool used } * — but the
        // layout is private to js_stdlib.c.  We tracein via a public
        // helper.  Simpler: chase via a forward decl.
        extern void jstro_gc_mark_map(void *m_ptr);
        jstro_gc_mark_map(m);
        break;
    }
    case JS_TMAPITER: {
        gc_set_marked(h);
        // points to a JsMap; trace via the same helper.
        extern void jstro_gc_mark_mapiter(void *iter);
        jstro_gc_mark_mapiter(h);
        break;
    }
    case JS_TREGEX: {
        gc_set_marked(h);
        // {gc, source, flags, ...}
        struct { struct GCHead gc; struct JsString *src; struct JsString *fl; } *re = (void *)h;
        mark_string(re->src);
        mark_string(re->fl);
        break;
    }
    case JS_TPROXY: {
        gc_set_marked(h);
        struct { struct GCHead gc; JsValue target; JsValue handler; } *px = (void *)h;
        mark_value(px->target);
        mark_value(px->handler);
        break;
    }
    case JS_TPROMISE:   mark_object_struct((struct JsObject *)h); break;
    default:
        gc_set_marked(h);
        break;
    }
}

// Module cache (defined in js_stdlib.c).  Externally walked here.
extern void jstro_gc_mark_modules(void);

static void
gc_mark_roots(CTX *c)
{
    // CTX prototypes and globals.
    mark_object_struct(c->object_proto);
    mark_object_struct(c->function_proto);
    mark_object_struct(c->array_proto);
    mark_object_struct(c->string_proto);
    mark_object_struct(c->number_proto);
    mark_object_struct(c->boolean_proto);
    mark_object_struct(c->error_proto);
    mark_object_struct(c->map_proto);
    mark_object_struct(c->set_proto);
    mark_object_struct(c->mapiter_proto);
    mark_object_struct(c->regex_proto);
    mark_object_struct(c->globals);

    // Active call state.
    mark_value(c->this_val);
    mark_value(c->new_target);
    mark_value(c->last_thrown);
    mark_value(JSTRO_BR_VAL);
    if (c->cur_upvals) {
        // We don't know how many upvals the active closure has — but each
        // is referenced via cur_upvals[i] in body code through node_upval_*.
        // Without size info, we trust that the closures themselves are
        // already kept alive (via frame's locals or via own_props), and
        // their upvals[] arrays are reached through them.  So we don't
        // walk cur_upvals here.
    }
    if (c->cur_args) {
        for (uint32_t i = 0; i < c->cur_argc; i++) mark_value(c->cur_args[i]);
    }
    // The root shape singleton — needed because intermediate transitions
    // chain back to it.
    mark_shape(c->root_shape);

    // Active frame stack — every alloca'd frame in flight.
    for (struct js_frame_link *fl = c->frame_stack; fl; fl = fl->prev) {
        for (uint32_t i = 0; i < fl->nlocals; i++) mark_value(fl->frame[i]);
        if (fl->args) {
            for (uint32_t i = 0; i < fl->argc; i++) mark_value(fl->args[i]);
        }
    }

    // Throw-frame chain.  The frames themselves don't store JsValues, but
    // c->last_thrown is already covered above.

    // String intern table — strong root (interned strings live forever
    // until the intern table itself is rebuilt; this avoids the need for
    // weak refs).
    for (uint32_t i = 0; i < c->intern_cap; i++) {
        if (c->intern_buckets[i]) mark_string(c->intern_buckets[i]);
    }

    // Module cache (private to js_stdlib.c).
    jstro_gc_mark_modules();
}

// Free an unmarked object's auxiliary buffers (mallocs the GCHead doesn't own).
static void
gc_finalize(struct GCHead *h)
{
    switch (h->type) {
    case JS_TOBJECT: case JS_TERROR: case JS_TACCESSOR: case JS_TPROMISE: {
        struct JsObject *o = (struct JsObject *)h;
        free(o->slots);
        break;
    }
    case JS_TARRAY: {
        struct JsArray *a = (struct JsArray *)h;
        free(a->dense);
        break;
    }
    case JS_TFUNCTION: {
        struct JsFunction *fn = (struct JsFunction *)h;
        free(fn->upvals);
        break;
    }
    case JS_TMAP: case JS_TSET: {
        struct JsMapData2 { struct GCHead gc; void *entries; } *m = (void *)h;
        free(m->entries);
        break;
    }
    default:
        break;
    }
}

static size_t g_gc_alive_count = 0;

static void
gc_sweep(CTX *c)
{
    struct GCHead **link = &c->all_objects;
    size_t freed = 0;
    while (*link) {
        struct GCHead *h = *link;
        if (gc_is_marked(h)) {
            link = &h->next;
        } else {
            *link = h->next;
            gc_finalize(h);
            free(h);
            freed++;
        }
    }
    g_gc_alive_count = 0;
    for (struct GCHead *h = c->all_objects; h; h = h->next) g_gc_alive_count++;
    c->bytes_allocated = g_gc_alive_count;
    size_t want = g_gc_alive_count * 2;
    if (want < 4096)        want = 4096;
    if (want > 1024 * 1024) want = 1024 * 1024;
    c->gc_threshold = want;
    if (getenv("JSTRO_GC_TRACE")) {
        size_t cnt[64] = {0};
        for (struct GCHead *h = c->all_objects; h; h = h->next) {
            if (h->type < 64) cnt[h->type]++;
        }
        fprintf(stderr, "[GC] freed=%zu alive=%zu threshold=%zu",
                freed, g_gc_alive_count, c->gc_threshold);
        for (int i = 0; i < 64; i++) {
            if (cnt[i] > 5) fprintf(stderr, " t%d:%zu", i, cnt[i]);
        }
        fputc('\n', stderr);
    }
}

unsigned long jstro_gc_run_count = 0;

// Safepoint check: called from node_seq / node_while / node_for between
// statements.  Triggers GC if the allocation count crossed the threshold.
// Inlined into the dispatchers via the macro `JSTRO_SAFEPOINT(c)`.
void
jstro_gc_safepoint(CTX *c)
{
    if (!c->gc_disabled && c->bytes_allocated >= c->gc_threshold) {
        js_gc_collect(c);
    }
}

void
js_gc_collect(CTX *c)
{
    if (c->gc_disabled) return;
    jstro_gc_run_count++;
    // Flip live/dead polarity (so all currently-marked objects effectively
    // become unmarked without a clear pass).
    g_mark_dead  = g_mark_live;
    g_mark_live ^= 1;
    gc_mark_roots(c);
    gc_sweep(c);
}

void *
js_gc_alloc(CTX *c, size_t size, uint8_t type)
{
    // We do NOT trigger GC inside the allocator: newly-allocated objects
    // are typically referenced only from C-stack locals during multi-step
    // construction (e.g. js_object_new + js_object_set + ...), and tracing
    // those would require either a pin list or a write-barrier protocol.
    // Instead, GC fires at "safepoints" — `node_seq` between statements,
    // `node_while`/`for` bodies, and other places where we know the only
    // live JsValues are reachable through CTX (frame_stack + cur_args + ...).
    // See `jstro_gc_safepoint` below.
    void *p = calloc(1, size);
    if (!p) { fprintf(stderr, "OOM\n"); exit(1); }
    struct GCHead *gc = (struct GCHead *)p;
    gc->type = type;
    // New objects start out with the current live color so they aren't
    // swept until the next GC sweeps them after deciding they're dead.
    // (At the next GC entry we flip live↔dead; surviving = reachable +
    // re-marked, unreachable = mark stays at the now-dead color.)
    gc->mark = g_mark_live;
    gc->next = c->all_objects;
    c->all_objects = gc;
    c->bytes_allocated += 1;
    return p;
}

void
js_gc_register(CTX *c, void *obj, uint8_t type)
{
    struct GCHead *gc = (struct GCHead *)obj;
    gc->type = type;
    gc->mark = g_mark_live;
    gc->next = c->all_objects;
    c->all_objects = gc;
}

// =====================================================================
// Heap-boxed double (for out-of-range flonum encoding).
// =====================================================================
JsValue
jv_box_double(double d)
{
    extern CTX *jstro_main_ctx;
    struct JsHeapDouble *hd = (struct JsHeapDouble *)js_gc_alloc(
        jstro_main_ctx, sizeof(*hd), JS_TFLOAT);
    hd->value = d;
    return (JsValue)(uintptr_t)hd;
}

// =====================================================================
// String interning
// =====================================================================

static uint32_t
str_hash_bytes(const char *s, size_t len)
{
    // FNV-1a 32-bit
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static void
intern_grow(CTX *c)
{
    uint32_t old_cap = c->intern_cap;
    struct JsString **old = c->intern_buckets;
    uint32_t new_cap = old_cap ? old_cap * 2 : 256;
    c->intern_buckets = (struct JsString **)calloc(new_cap, sizeof(struct JsString *));
    c->intern_cap = new_cap;
    for (uint32_t i = 0; i < old_cap; i++) {
        struct JsString *s = old[i];
        if (s) {
            uint32_t h = s->hash & (new_cap - 1);
            while (c->intern_buckets[h]) h = (h + 1) & (new_cap - 1);
            c->intern_buckets[h] = s;
        }
    }
    free(old);
}

struct JsString *
js_str_intern_n(CTX *c, const char *bytes, size_t len)
{
    if (c->intern_cnt * 2 >= c->intern_cap) intern_grow(c);
    uint32_t hash = str_hash_bytes(bytes, len);
    uint32_t pos = hash & (c->intern_cap - 1);
    for (;;) {
        struct JsString *s = c->intern_buckets[pos];
        if (!s) break;
        if (s->hash == hash && s->len == len && memcmp(s->data, bytes, len) == 0) {
            return s;
        }
        pos = (pos + 1) & (c->intern_cap - 1);
    }
    struct JsString *s = (struct JsString *)js_gc_alloc(
        c, sizeof(struct JsString) + len + 1, JS_TSTRING);
    s->hash = hash;
    s->len = (uint32_t)len;
    memcpy(s->data, bytes, len);
    s->data[len] = '\0';
    c->intern_buckets[pos] = s;
    c->intern_cnt++;
    return s;
}

struct JsString *
js_str_intern(CTX *c, const char *cstr)
{
    return js_str_intern_n(c, cstr, strlen(cstr));
}

const char *js_str_data(struct JsString *s) { return s->data; }
size_t      js_str_len(struct JsString *s)  { return s->len; }
bool        js_str_eq(struct JsString *a, struct JsString *b) { return a == b; }

struct JsString *
js_str_concat(CTX *c, struct JsString *a, struct JsString *b)
{
    size_t len = (size_t)a->len + b->len;
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, a->data, a->len);
    memcpy(buf + a->len, b->data, b->len);
    buf[len] = 0;
    struct JsString *s = js_str_intern_n(c, buf, len);
    free(buf);
    return s;
}

// =====================================================================
// Shape (V8-style hidden class)
// =====================================================================

struct JsShape *
js_shape_root(CTX *c)
{
    if (c->root_shape) return c->root_shape;
    struct JsShape *s = (struct JsShape *)js_gc_alloc(c, sizeof(struct JsShape), JS_TOBJECT);
    s->gc.type = 0;  // not really an object
    s->nslots = 0;
    s->capa = 0;
    s->names = NULL;
    s->parent = NULL;
    s->ntrans = s->tcap = 0;
    s->trans = NULL;
    c->root_shape = s;
    return s;
}

unsigned long jstro_shape_find_count = 0;
unsigned long jstro_object_set_count = 0;
unsigned long jstro_call_ic_miss     = 0;

int
js_shape_find_slot(struct JsShape *s, struct JsString *name)
{
    jstro_shape_find_count++;
    if (!s) return -1;
    for (uint32_t i = 0; i < s->nslots; i++) {
        if (s->names[i] == name) return (int)i;
    }
    return -1;
}

struct JsShape *
js_shape_transition(CTX *c, struct JsShape *from, struct JsString *name)
{
    // Search transition table for existing transition.
    for (uint32_t i = 0; i < from->ntrans; i++) {
        if (from->trans[i].name == name) return from->trans[i].to;
    }
    // Create new shape extending `from` by `name`.
    struct JsShape *to = (struct JsShape *)js_gc_alloc(c, sizeof(struct JsShape), JS_TOBJECT);
    to->gc.type = 0;
    to->nslots = from->nslots + 1;
    to->capa = to->nslots;
    to->names = (struct JsString **)malloc(sizeof(struct JsString *) * to->nslots);
    for (uint32_t i = 0; i < from->nslots; i++) to->names[i] = from->names[i];
    to->names[from->nslots] = name;
    to->parent = from;
    to->ntrans = to->tcap = 0;
    to->trans = NULL;
    if (from->ntrans + 1 > from->tcap) {
        uint32_t nc = from->tcap ? from->tcap * 2 : 4;
        from->trans = (struct JsShapeTrans *)realloc(from->trans, sizeof(struct JsShapeTrans) * nc);
        from->tcap = nc;
    }
    from->trans[from->ntrans].name = name;
    from->trans[from->ntrans].to = to;
    from->ntrans++;
    return to;
}

// =====================================================================
// Object
// =====================================================================

struct JsObject *
js_object_new(CTX *c, struct JsObject *proto)
{
    struct JsObject *o = (struct JsObject *)js_gc_alloc(c, sizeof(struct JsObject), JS_TOBJECT);
    o->shape = js_shape_root(c);
    o->slots = NULL;
    o->slot_capa = 0;
    o->proto = proto;
    return o;
}

static void
object_grow_slots(struct JsObject *o, uint32_t need)
{
    uint32_t nc = o->slot_capa ? o->slot_capa * 2 : 4;
    while (nc < need) nc *= 2;
    o->slots = (JsValue *)realloc(o->slots, sizeof(JsValue) * nc);
    for (uint32_t i = o->slot_capa; i < nc; i++) o->slots[i] = JV_UNDEFINED;
    o->slot_capa = nc;
}

void
js_object_set(CTX *c, struct JsObject *o, struct JsString *key, JsValue v)
{
    if (o->gc.flags & JS_OBJ_FROZEN) {
        // Silently ignore in non-strict; throw in strict.  jstro is strict.
        js_throw_type_error(c, "Cannot assign to read-only property '%s' of object",
                            js_str_data(key));
    }
    int slot = js_shape_find_slot(o->shape, key);
    if (slot >= 0) {
        // Accessor property: invoke setter.
        JsValue cur = o->slots[slot];
        if (JV_IS_PTR(cur) && jv_heap_type(cur) == JS_TACCESSOR) {
            struct JsObject *acc = JV_AS_OBJ(cur);
            JsValue setter = js_object_get(c, acc, js_str_intern(c, "set"));
            if (JV_IS_PTR(setter) && (jv_heap_type(setter) == JS_TFUNCTION || jv_heap_type(setter) == JS_TCFUNCTION)) {
                JsValue args[1] = { v };
                js_call(c, setter, JV_OBJ(o), args, 1);
                return;
            }
            return;  // accessor without setter — ignore
        }
        o->slots[slot] = v;
        return;
    }
    if (o->gc.flags & JS_OBJ_NOT_EXTENS) {
        js_throw_type_error(c, "Cannot add property '%s', object is not extensible",
                            js_str_data(key));
    }
    o->shape = js_shape_transition(c, o->shape, key);
    if (o->shape->nslots > o->slot_capa) object_grow_slots(o, o->shape->nslots);
    o->slots[o->shape->nslots - 1] = v;
}

void
js_object_set_with_ic(CTX *c, struct JsObject *o, struct JsString *key, JsValue v, struct JsPropIC *ic)
{
    if (o->gc.flags & JS_OBJ_FROZEN) {
        js_throw_type_error(c, "Cannot assign to read-only property '%s' of object",
                            js_str_data(key));
    }
    struct JsShape *cur = o->shape;
    for (uint32_t i = 0; i < cur->ntrans; i++) {
        if (cur->trans[i].name == key) {
            if (o->gc.flags & JS_OBJ_NOT_EXTENS) {
                js_throw_type_error(c, "Cannot add property '%s', object is not extensible", js_str_data(key));
            }
            struct JsShape *next = cur->trans[i].to;
            o->shape = next;
            if (next->nslots > o->slot_capa) object_grow_slots(o, next->nslots);
            o->slots[next->nslots - 1] = v;
            ic->shape = (uintptr_t)next;
            ic->slot = next->nslots - 1;
            return;
        }
    }
    int slot = js_shape_find_slot(cur, key);
    if (slot >= 0) {
        JsValue old = o->slots[slot];
        if (JV_IS_PTR(old) && jv_heap_type(old) == JS_TACCESSOR) {
            struct JsObject *acc = JV_AS_OBJ(old);
            JsValue setter = js_object_get(c, acc, js_str_intern(c, "set"));
            if (JV_IS_PTR(setter) && (jv_heap_type(setter) == JS_TFUNCTION || jv_heap_type(setter) == JS_TCFUNCTION)) {
                JsValue args[1] = { v };
                js_call(c, setter, JV_OBJ(o), args, 1);
                return;
            }
            return;  // accessor with no setter: ignore in non-strict
        }
        o->slots[slot] = v;
        ic->shape = (uintptr_t)cur;
        ic->slot = (uint32_t)slot;
        return;
    }
    if (o->gc.flags & JS_OBJ_NOT_EXTENS) {
        js_throw_type_error(c, "Cannot add property '%s', object is not extensible", js_str_data(key));
    }
    o->shape = js_shape_transition(c, cur, key);
    if (o->shape->nslots > o->slot_capa) object_grow_slots(o, o->shape->nslots);
    o->slots[o->shape->nslots - 1] = v;
    ic->shape = (uintptr_t)o->shape;
    ic->slot = o->shape->nslots - 1;
}

// Internal: walk proto chain looking for `key`.  Invokes accessor
// getters with `receiver` as `this` (so `obj.foo` where foo lives on a
// proto still binds `this` to obj, not the proto).
static JsValue
js_object_get_with_receiver(CTX *c, struct JsObject *o, struct JsString *key, JsValue receiver)
{
    while (o) {
        int slot = js_shape_find_slot(o->shape, key);
        if (slot >= 0) {
            JsValue v = o->slots[slot];
            if (v == JV_HOLE) {
                o = o->proto;
                continue;
            }
            if (JV_IS_PTR(v) && jv_heap_type(v) == JS_TACCESSOR) {
                struct JsObject *acc = JV_AS_OBJ(v);
                int gs = js_shape_find_slot(acc->shape, js_str_intern(c, "get"));
                if (gs < 0) return JV_UNDEFINED;
                JsValue getter = acc->slots[gs];
                if (JV_IS_PTR(getter) && (jv_heap_type(getter) == JS_TFUNCTION || jv_heap_type(getter) == JS_TCFUNCTION)) {
                    return js_call(c, getter, receiver, NULL, 0);
                }
                return JV_UNDEFINED;
            }
            return v;
        }
        o = o->proto;
    }
    return JV_UNDEFINED;
}

JsValue
js_object_get(CTX *c, struct JsObject *o, struct JsString *key)
{
    return js_object_get_with_receiver(c, o, key, JV_OBJ(o));
}

JsValue
js_object_get_with_ic(CTX *c, struct JsObject *o, struct JsString *key, struct JsPropIC *ic)
{
    int slot = js_shape_find_slot(o->shape, key);
    if (slot >= 0) {
        ic->shape = (uintptr_t)o->shape;
        ic->slot = (uint32_t)slot;
        return o->slots[slot];
    }
    return js_object_get(c, o->proto, key);
}

bool
js_object_has(struct JsObject *o, struct JsString *key)
{
    while (o) {
        int slot = js_shape_find_slot(o->shape, key);
        if (slot >= 0) return true;
        o = o->proto;
    }
    return false;
}

bool
js_object_delete(struct JsObject *o, struct JsString *key)
{
    // Mark the slot as JV_HOLE so subsequent reads return undefined and
    // enumeration (Object.keys / for-in) skips it.  We don't actually
    // shrink the shape — that would invalidate every IC pointing at it.
    int slot = js_shape_find_slot(o->shape, key);
    if (slot >= 0) {
        o->slots[slot] = JV_HOLE;
        return true;
    }
    return false;
}

// =====================================================================
// Array
// =====================================================================

struct JsArray *
js_array_new(CTX *c, uint32_t length)
{
    struct JsArray *a = (struct JsArray *)js_gc_alloc(c, sizeof(struct JsArray), JS_TARRAY);
    uint32_t capa = length < 4 ? 4 : length;
    // Round up to power-of-two for cheaper grow checks (not currently used).
    a->dense = (JsValue *)malloc(sizeof(JsValue) * capa);
    for (uint32_t i = 0; i < capa; i++) a->dense[i] = JV_HOLE;
    a->length = length;
    a->dense_capa = capa;
    a->fallback = NULL;
    a->proto = c->array_proto;
    return a;
}

static void
array_grow_dense(struct JsArray *a, uint32_t need)
{
    uint32_t nc = a->dense_capa ? a->dense_capa * 2 : 4;
    while (nc < need) nc *= 2;
    a->dense = (JsValue *)realloc(a->dense, sizeof(JsValue) * nc);
    for (uint32_t i = a->dense_capa; i < nc; i++) a->dense[i] = JV_HOLE;
    a->dense_capa = nc;
}

JsValue
js_array_get(CTX *c, struct JsArray *a, int64_t i)
{
    if (i >= 0 && (uint64_t)i < a->dense_capa && i < a->length) {
        JsValue v = a->dense[i];
        if (v != JV_HOLE) return v;
    }
    // Past length or past dense range: search prototype chain.
    if (i >= 0 && i < a->length) return JV_UNDEFINED;
    if (a->fallback) {
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)i);
        return js_object_get(c, a->fallback, js_str_intern_n(c, buf, n));
    }
    return JV_UNDEFINED;
}

void
js_array_set(CTX *c, struct JsArray *a, int64_t i, JsValue v)
{
    (void)c;
    if (i < 0) {
        if (!a->fallback) a->fallback = js_object_new(c, NULL);
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)i);
        js_object_set(c, a->fallback, js_str_intern_n(c, buf, n), v);
        return;
    }
    if ((uint64_t)i >= a->dense_capa) {
        if ((uint64_t)i > 1024 * 1024 * 64) {
            // pathological: switch to fallback to avoid OOM.
            if (!a->fallback) a->fallback = js_object_new(c, NULL);
            char buf[24];
            int n = snprintf(buf, sizeof buf, "%lld", (long long)i);
            js_object_set(c, a->fallback, js_str_intern_n(c, buf, n), v);
            if ((uint32_t)i + 1 > a->length) a->length = (uint32_t)i + 1;
            return;
        }
        array_grow_dense(a, (uint32_t)i + 1);
    }
    a->dense[i] = v;
    if ((uint32_t)i + 1 > a->length) a->length = (uint32_t)i + 1;
}

JsValue
js_array_get_v(CTX *c, struct JsArray *a, JsValue key)
{
    if (JV_IS_SMI(key)) return js_array_get(c, a, JV_AS_SMI(key));
    if (JV_IS_FLONUM(key)) {
        double d = JV_AS_DBL(key);
        int64_t i = (int64_t)d;
        if ((double)i == d && i >= 0) return js_array_get(c, a, i);
    }
    if (JV_IS_STR(key)) {
        struct JsString *s = JV_AS_STR(key);
        // canonical numeric string → array index?
        if (s->len > 0 && s->data[0] >= '0' && s->data[0] <= '9') {
            char *end;
            long long i = strtoll(s->data, &end, 10);
            if ((size_t)(end - s->data) == s->len && i >= 0) {
                return js_array_get(c, a, i);
            }
        }
        if (s == js_str_intern(c, "length")) return JV_INT((int64_t)a->length);
        if (a->fallback) return js_object_get(c, a->fallback, s);
        if (a->proto) return js_object_get(c, a->proto, s);
        return JV_UNDEFINED;
    }
    return JV_UNDEFINED;
}

void
js_array_set_v(CTX *c, struct JsArray *a, JsValue key, JsValue v)
{
    if (JV_IS_SMI(key)) { js_array_set(c, a, JV_AS_SMI(key), v); return; }
    if (JV_IS_STR(key)) {
        struct JsString *s = JV_AS_STR(key);
        if (s == js_str_intern(c, "length")) {
            int32_t newlen = js_to_int32(c, v);
            if (newlen < 0) js_throw_range_error(c, "Invalid array length");
            js_array_set_length(c, a, (uint32_t)newlen);
            return;
        }
        if (s->len > 0 && s->data[0] >= '0' && s->data[0] <= '9') {
            char *end;
            long long i = strtoll(s->data, &end, 10);
            if ((size_t)(end - s->data) == s->len && i >= 0) {
                js_array_set(c, a, i, v);
                return;
            }
        }
        if (!a->fallback) a->fallback = js_object_new(c, NULL);
        js_object_set(c, a->fallback, s, v);
    }
}

void
js_array_set_length(CTX *c, struct JsArray *a, uint32_t len)
{
    (void)c;
    if (len < a->length) {
        for (uint32_t i = len; i < a->length && i < a->dense_capa; i++) a->dense[i] = JV_HOLE;
    }
    a->length = len;
}

void
js_array_push(CTX *c, struct JsArray *a, JsValue v)
{
    js_array_set(c, a, a->length, v);
}

JsValue
js_array_pop(CTX *c, struct JsArray *a)
{
    (void)c;
    if (a->length == 0) return JV_UNDEFINED;
    a->length--;
    if (a->length < a->dense_capa) {
        JsValue v = a->dense[a->length];
        a->dense[a->length] = JV_HOLE;
        if (v == JV_HOLE) return JV_UNDEFINED;
        return v;
    }
    return JV_UNDEFINED;
}

// =====================================================================
// Function
// =====================================================================

struct JsFunction *
js_func_new(CTX *c, struct Node *body, uint32_t np, uint32_t nl, uint32_t nu, bool is_arrow, const char *name)
{
    struct JsFunction *fn = (struct JsFunction *)js_gc_alloc(c, sizeof(struct JsFunction), JS_TFUNCTION);
    fn->body = body;
    fn->nparams = np;
    fn->nlocals = nl;
    fn->nupvals = nu;
    fn->is_arrow = is_arrow;
    fn->is_strict = 1;  // jstro is strict-mode by default
    fn->name = name;
    fn->upvals = NULL;
    fn->home_proto = NULL;   // lazy: created on first `new`
    fn->bound_this = NULL;
    return fn;
}

struct JsCFunction *
js_cfunc_new(CTX *c, const char *name, js_cfunc_ptr_t fn, uint32_t nparams)
{
    struct JsCFunction *f = (struct JsCFunction *)js_gc_alloc(c, sizeof(struct JsCFunction), JS_TCFUNCTION);
    f->name = name;
    f->fn = fn;
    f->nparams = nparams;
    return f;
}

JsValue *
js_runtime_capture_upval(CTX *c, JsValue *frame, uint32_t is_local, uint32_t slot)
{
    (void)c;
    if (is_local) {
        // Promote frame slot to a heap box if not already.
        JsValue v = frame[slot];
        if (JV_IS_PTR(v) && jv_heap_type(v) == JS_TBOX) {
            return &((struct JsBox *)(uintptr_t)v)->value;
        }
        struct JsBox *box = (struct JsBox *)js_gc_alloc(c, sizeof(struct JsBox), JS_TBOX);
        box->value = v;
        frame[slot] = (JsValue)(uintptr_t)box;
        return &box->value;
    } else {
        return c->cur_upvals[slot];
    }
}

// =====================================================================
// Property access on arbitrary value (handles primitives via wrappers)
// =====================================================================

JsValue
js_get_member(CTX *c, JsValue obj, struct JsString *name)
{
    if (JV_IS_NULLISH(obj)) {
        js_throw_type_error(c, "Cannot read properties of %s (reading '%s')",
                            JV_IS_NULL(obj) ? "null" : "undefined",
                            js_str_data(name));
    }
    if (JV_IS_STR(obj)) {
        struct JsString *s = JV_AS_STR(obj);
        if (name == js_str_intern(c, "length")) return JV_INT((int64_t)s->len);
        return js_object_get(c, c->string_proto, name);
    }
    if (JV_IS_ARRAY(obj)) {
        struct JsArray *a = JV_AS_ARRAY(obj);
        if (name == js_str_intern(c, "length")) return JV_INT((int64_t)a->length);
        if (a->fallback) {
            int slot = js_shape_find_slot(a->fallback->shape, name);
            if (slot >= 0) return a->fallback->slots[slot];
        }
        return js_object_get(c, a->proto, name);
    }
    if (JV_IS_PTR(obj)) {
        if (jv_heap_type(obj) == JS_TFUNCTION) {
            struct JsFunction *fn = JV_AS_FUNC(obj);
            if (name == js_str_intern(c, "prototype")) {
                if (!fn->home_proto) {
                    fn->home_proto = js_object_new(c, c->object_proto);
                    js_object_set(c, fn->home_proto, js_str_intern(c, "constructor"), obj);
                }
                return JV_OBJ(fn->home_proto);
            }
            if (name == js_str_intern(c, "name"))
                return JV_STR(js_str_intern(c, fn->name ? fn->name : ""));
            if (name == js_str_intern(c, "length"))
                return JV_INT((int64_t)fn->nparams);
            // Own properties (static methods, etc.).
            if (fn->own_props) {
                int slot = js_shape_find_slot(fn->own_props->shape, name);
                if (slot >= 0) return fn->own_props->slots[slot];
            }
            return js_object_get(c, c->function_proto, name);
        }
        if (jv_heap_type(obj) == JS_TCFUNCTION) {
            struct JsCFunction *cf = JV_AS_CFUNC(obj);
            if (name == js_str_intern(c, "name"))
                return JV_STR(js_str_intern(c, cf->name ? cf->name : ""));
            if (name == js_str_intern(c, "length"))
                return JV_INT((int64_t)cf->nparams);
            if (cf->own_props) {
                int slot = js_shape_find_slot(cf->own_props->shape, name);
                if (slot >= 0) return cf->own_props->slots[slot];
            }
            return js_object_get(c, c->function_proto, name);
        }
        if (jv_heap_type(obj) == JS_TMAP || jv_heap_type(obj) == JS_TSET) {
            // Map/Set are not JsObject; route through dedicated proto.
            if (name == js_str_intern(c, "size")) {
                struct { struct GCHead gc; void *e; uint32_t sz, capa; uint8_t is; } *m = (void *)(uintptr_t)obj;
                return JV_INT((int64_t)m->sz);
            }
            return js_object_get(c, jv_heap_type(obj) == JS_TMAP ? c->map_proto : c->set_proto, name);
        }
        if (jv_heap_type(obj) == JS_TMAPITER) {
            return js_object_get(c, c->mapiter_proto, name);
        }
        if (jv_heap_type(obj) == JS_TREGEX) {
            return js_object_get(c, c->regex_proto, name);
        }
        if (jv_heap_type(obj) == JS_TPROXY) {
            // Proxy: invoke handler.get(target, key) if present.
            struct { struct GCHead gc; JsValue target; JsValue handler; } *px = (void *)(uintptr_t)obj;
            JsValue trap = js_get_member(c, px->handler, js_str_intern(c, "get"));
            if (JV_IS_PTR(trap) && (jv_heap_type(trap) == JS_TFUNCTION || jv_heap_type(trap) == JS_TCFUNCTION)) {
                JsValue args[3] = { px->target, JV_STR(name), obj };
                return js_call(c, trap, px->handler, args, 3);
            }
            return js_get_member(c, px->target, name);
        }
        if (jv_heap_type(obj) >= JS_TOBJECT) {
            return js_object_get(c, JV_AS_OBJ(obj), name);
        }
    }
    if (JV_IS_BOOL(obj)) return js_object_get(c, c->boolean_proto, name);
    if (JV_IS_NUM(obj))  return js_object_get(c, c->number_proto, name);
    return JV_UNDEFINED;
}

JsValue
js_get_index(CTX *c, JsValue obj, JsValue key)
{
    if (JV_IS_NULLISH(obj)) {
        struct JsString *s = js_to_string(c, key);
        js_throw_type_error(c, "Cannot read properties of %s (reading '%s')",
                            JV_IS_NULL(obj) ? "null" : "undefined",
                            js_str_data(s));
    }
    if (JV_IS_ARRAY(obj)) return js_array_get_v(c, JV_AS_ARRAY(obj), key);
    if (JV_IS_STR(obj)) {
        struct JsString *s = JV_AS_STR(obj);
        if (JV_IS_SMI(key)) {
            int64_t i = JV_AS_SMI(key);
            if (i >= 0 && (uint32_t)i < s->len) {
                char ch[2] = { s->data[i], 0 };
                return JV_STR(js_str_intern_n(c, ch, 1));
            }
            return JV_UNDEFINED;
        }
        struct JsString *kn = js_to_string(c, key);
        if (kn == js_str_intern(c, "length")) return JV_INT((int64_t)s->len);
        return js_object_get(c, c->string_proto, kn);
    }
    if (JV_IS_PTR(obj) && jv_heap_type(obj) >= JS_TOBJECT) {
        struct JsString *kn = JV_IS_STR(key) ? JV_AS_STR(key) : js_to_string(c, key);
        return js_object_get(c, JV_AS_OBJ(obj), kn);
    }
    return js_get_member(c, obj, js_to_string(c, key));
}

void
js_set_member(CTX *c, JsValue obj, struct JsString *name, JsValue v)
{
    if (JV_IS_NULLISH(obj)) {
        js_throw_type_error(c, "Cannot set properties of %s (setting '%s')",
                            JV_IS_NULL(obj) ? "null" : "undefined",
                            js_str_data(name));
    }
    if (JV_IS_ARRAY(obj)) {
        struct JsArray *a = JV_AS_ARRAY(obj);
        if (name == js_str_intern(c, "length")) {
            int32_t newlen = js_to_int32(c, v);
            if (newlen < 0) js_throw_range_error(c, "Invalid array length");
            js_array_set_length(c, a, (uint32_t)newlen);
            return;
        }
        if (!a->fallback) a->fallback = js_object_new(c, NULL);
        js_object_set(c, a->fallback, name, v);
        return;
    }
    if (JV_IS_PTR(obj)) {
        uint8_t t = jv_heap_type(obj);
        if (t == JS_TOBJECT || t == JS_TERROR) {
            js_object_set(c, JV_AS_OBJ(obj), name, v);
            return;
        }
        if (t == JS_TFUNCTION) {
            struct JsFunction *fn = JV_AS_FUNC(obj);
            if (name == js_str_intern(c, "prototype")) {
                if (JV_IS_PTR(v) && jv_heap_type(v) == JS_TOBJECT) {
                    fn->home_proto = JV_AS_OBJ(v);
                }
                return;
            }
            if (!fn->own_props) fn->own_props = js_object_new(c, NULL);
            js_object_set(c, fn->own_props, name, v);
            return;
        }
        if (t == JS_TCFUNCTION) {
            struct JsCFunction *cf = JV_AS_CFUNC(obj);
            if (!cf->own_props) cf->own_props = js_object_new(c, NULL);
            js_object_set(c, cf->own_props, name, v);
            return;
        }
        if (t == JS_TPROXY) {
            struct { struct GCHead gc; JsValue target; JsValue handler; } *px = (void *)(uintptr_t)obj;
            JsValue trap = js_get_member(c, px->handler, js_str_intern(c, "set"));
            if (JV_IS_PTR(trap) && (jv_heap_type(trap) == JS_TFUNCTION || jv_heap_type(trap) == JS_TCFUNCTION)) {
                JsValue args[4] = { px->target, JV_STR(name), v, obj };
                js_call(c, trap, px->handler, args, 4);
                return;
            }
            js_set_member(c, px->target, name, v);
            return;
        }
    }
    js_throw_type_error(c, "Cannot set property '%s' on a primitive value",
                        js_str_data(name));
}

void
js_set_index(CTX *c, JsValue obj, JsValue key, JsValue v)
{
    if (JV_IS_ARRAY(obj)) { js_array_set_v(c, JV_AS_ARRAY(obj), key, v); return; }
    if (JV_IS_PTR(obj) && jv_heap_type(obj) == JS_TOBJECT) {
        struct JsString *kn = JV_IS_STR(key) ? JV_AS_STR(key) : js_to_string(c, key);
        js_object_set(c, JV_AS_OBJ(obj), kn, v);
        return;
    }
    js_set_member(c, obj, js_to_string(c, key), v);
}

// =====================================================================
// Calls — set up frame, run body, restore.
// =====================================================================

CTX *jstro_main_ctx = NULL;

JsValue
js_call_func_direct(CTX *c, struct JsFunction *fn, JsValue thisv, JsValue *args, uint32_t argc)
{
    uint32_t nlocals = fn->nlocals;
    uint32_t np = fn->nparams;
    JsValue *frame = (JsValue *)alloca(sizeof(JsValue) * (nlocals > 0 ? nlocals : 1));
    if (fn->is_vararg) {
        // The last declared param collects extra args as an array.
        uint32_t nfixed = np - 1;
        for (uint32_t i = 0; i < nfixed; i++) {
            frame[i] = (i < argc) ? args[i] : JV_UNDEFINED;
        }
        uint32_t rest_len = argc > nfixed ? argc - nfixed : 0;
        struct JsArray *rest = js_array_new(c, rest_len);
        for (uint32_t i = 0; i < rest_len; i++) rest->dense[i] = args[nfixed + i];
        rest->length = rest_len;
        frame[nfixed] = JV_OBJ(rest);
        if (nlocals > np) memset(frame + np, 0, (nlocals - np) * sizeof(JsValue));
    } else {
        for (uint32_t i = 0; i < np; i++) {
            frame[i] = (i < argc) ? args[i] : JV_UNDEFINED;
        }
        if (nlocals > np) memset(frame + np, 0, (nlocals - np) * sizeof(JsValue));
    }

    JsValue saved_this    = c->this_val;
    JsValue **saved_upvals = c->cur_upvals;
    JsValue *saved_args   = c->cur_args;
    uint32_t saved_argc   = c->cur_argc;
    c->this_val = (fn->is_arrow && fn->bound_this) ? (JsValue)(uintptr_t)fn->bound_this : thisv;
    c->cur_upvals = fn->upvals;
    c->cur_args   = args;
    c->cur_argc   = argc;

    struct js_frame_link link = { frame, nlocals, args, argc, c->frame_stack };
    c->frame_stack = &link;

    JsValue r = EVAL(c, fn->body, frame);

    // Throws longjmp to the nearest try-frame, never reaching here.
    // BR is either NORMAL (no return seen) or RETURN.
    if (JSTRO_BR == JS_BR_RETURN) {
        r = JSTRO_BR_VAL;
        JSTRO_BR = JS_BR_NORMAL;
        JSTRO_BR_VAL = JV_UNDEFINED;
    } else {
        r = JV_UNDEFINED;
    }
    c->frame_stack = link.prev;
    c->this_val = saved_this;
    c->cur_upvals = saved_upvals;
    c->cur_args = saved_args;
    c->cur_argc = saved_argc;
    return r;
}

JsValue
js_call(CTX *c, JsValue fnv, JsValue thisv, JsValue *args, uint32_t argc)
{
    if (!JV_IS_PTR(fnv)) {
        js_throw_type_error(c, "value is not a function");
    }
    uint8_t t = jv_heap_type(fnv);
    if (t == JS_TFUNCTION) {
        return js_call_func_direct(c, JV_AS_FUNC(fnv), thisv, args, argc);
    } else if (t == JS_TCFUNCTION) {
        struct JsCFunction *cf = JV_AS_CFUNC(fnv);
        return cf->fn(c, thisv, args, argc);
    }
    js_throw_type_error(c, "value is not a function");
}

JsValue
js_construct(CTX *c, JsValue fnv, JsValue *args, uint32_t argc)
{
    if (!JV_IS_PTR(fnv)) js_throw_type_error(c, "value is not a constructor");
    uint8_t t = jv_heap_type(fnv);
    if (t == JS_TFUNCTION) {
        struct JsFunction *fn = JV_AS_FUNC(fnv);
        if (!fn->home_proto) {
            fn->home_proto = js_object_new(c, c->object_proto);
            js_object_set(c, fn->home_proto, js_str_intern(c, "constructor"), fnv);
        }
        struct JsObject *self = js_object_new(c, fn->home_proto);
        JsValue saved_nt = c->new_target;
        c->new_target = fnv;
        JsValue r = js_call_func_direct(c, fn, JV_OBJ(self), args, argc);
        c->new_target = saved_nt;
        if (JV_IS_PTR(r) && jv_heap_type(r) >= JS_TOBJECT) return r;
        return JV_OBJ(self);
    } else if (t == JS_TCFUNCTION) {
        return JV_AS_CFUNC(fnv)->fn(c, JV_UNDEFINED, args, argc);
    }
    js_throw_type_error(c, "value is not a constructor");
}

// =====================================================================
// try/catch/finally helper (out-of-line so node_try EVAL stays inlinable).
// =====================================================================
JsValue
jstro_try_run(CTX *c, NODE *body, JsValue *frame,
              uint32_t catch_var, uint32_t catch_is_box,
              uint32_t has_handler, NODE *handler,
              uint32_t has_final, NODE *final_)
{
    struct js_throw_frame jf;
    jf.prev = c->throw_top;
    jf.frame_save = frame;
    jf.sp_save = c->sp;
    jf.frame_stack_save = c->frame_stack;
    uint32_t saved_br = JSTRO_BR;
    JsValue  saved_br_val = JSTRO_BR_VAL;

    if (setjmp(jf.jb) == 0) {
        c->throw_top = &jf;
        EVAL(c, body, frame);
        c->throw_top = jf.prev;
    } else {
        c->throw_top = jf.prev;
        c->sp = jf.sp_save;
        // longjmp skipped frame_link pops; restore the stack head.
        c->frame_stack = jf.frame_stack_save;
        JSTRO_BR = saved_br;
        JSTRO_BR_VAL = saved_br_val;
        if (has_handler) {
            JsValue exn = c->last_thrown;
            if (catch_is_box) {
                ((struct JsBox *)(uintptr_t)frame[catch_var])->value = exn;
            } else {
                frame[catch_var] = exn;
            }
            EVAL(c, handler, frame);
        } else {
            if (has_final) {
                uint32_t pre = JSTRO_BR;
                JsValue  preval = JSTRO_BR_VAL;
                JSTRO_BR = JS_BR_NORMAL; JSTRO_BR_VAL = JV_UNDEFINED;
                EVAL(c, final_, frame);
                if (JSTRO_BR == JS_BR_NORMAL) { JSTRO_BR = pre; JSTRO_BR_VAL = preval; }
            }
            js_throw(c, c->last_thrown);
        }
    }
    if (has_final) {
        uint32_t pre = JSTRO_BR;
        JsValue  preval = JSTRO_BR_VAL;
        JSTRO_BR = JS_BR_NORMAL; JSTRO_BR_VAL = JV_UNDEFINED;
        EVAL(c, final_, frame);
        if (JSTRO_BR == JS_BR_NORMAL) { JSTRO_BR = pre; JSTRO_BR_VAL = preval; }
    }
    return JV_UNDEFINED;
}

// =====================================================================
// Errors
// =====================================================================

JsValue
js_make_error(CTX *c, const char *kind, const char *msg)
{
    struct JsObject *o = js_object_new(c, c->error_proto);
    o->gc.type = JS_TERROR;
    js_object_set(c, o, js_str_intern(c, "name"), JV_STR(js_str_intern(c, kind)));
    js_object_set(c, o, js_str_intern(c, "message"), JV_STR(js_str_intern(c, msg)));
    return JV_OBJ(o);
}

void
js_throw(CTX *c, JsValue err)
{
    c->last_thrown = err;
    if (c->throw_top) {
        longjmp(c->throw_top->jb, 1);
    }
    fprintf(stderr, "Uncaught (in jstro): ");
    js_print_value(c, stderr, err);
    fprintf(stderr, "\n");
    exit(1);
}

void js_throw_type_error(CTX *c, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    js_throw(c, js_make_error(c, "TypeError", buf));
}
void js_throw_range_error(CTX *c, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    js_throw(c, js_make_error(c, "RangeError", buf));
}
void js_throw_syntax_error(CTX *c, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    js_throw(c, js_make_error(c, "SyntaxError", buf));
}
void js_throw_reference_error(CTX *c, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    js_throw(c, js_make_error(c, "ReferenceError", buf));
}

// =====================================================================
// ECMAScript abstract operations.
// =====================================================================

const char *
js_typeof(JsValue v)
{
    if (JV_IS_UNDEFINED(v)) return "undefined";
    if (JV_IS_NULL(v))      return "object";   // historical bug in spec
    if (JV_IS_BOOL(v))      return "boolean";
    if (JV_IS_NUM(v))       return "number";
    if (JV_IS_STR(v))       return "string";
    if (JV_IS_PTR(v)) {
        uint8_t t = jv_heap_type(v);
        if (t == JS_TFUNCTION || t == JS_TCFUNCTION) return "function";
        if (t == JS_TSYMBOL) return "symbol";
        if (t == JS_TBIGINT) return "bigint";
    }
    return "object";
}

JsValue
js_to_primitive(CTX *c, JsValue v, const char *hint)
{
    if (!JV_IS_PTR(v) || jv_heap_type(v) < JS_TOBJECT) return v;
    // §7.1.1 OrdinaryToPrimitive: try valueOf and toString in order.
    const char *first = strcmp(hint, "string") == 0 ? "toString" : "valueOf";
    const char *second = strcmp(hint, "string") == 0 ? "valueOf" : "toString";
    for (int round = 0; round < 2; round++) {
        const char *name = round == 0 ? first : second;
        JsValue m = js_get_member(c, v, js_str_intern(c, name));
        if (JV_IS_PTR(m) && (jv_heap_type(m) == JS_TFUNCTION || jv_heap_type(m) == JS_TCFUNCTION)) {
            JsValue r = js_call(c, m, v, NULL, 0);
            if (!JV_IS_PTR(r) || jv_heap_type(r) < JS_TOBJECT) return r;
        }
    }
    js_throw_type_error(c, "Cannot convert object to primitive value");
}

double
js_to_double(CTX *c, JsValue v)
{
    if (JV_IS_SMI(v)) return (double)JV_AS_SMI(v);
    if (JV_IS_FLONUM(v) || JV_IS_FLOAT_BOX(v)) return JV_AS_DBL(v);
    if (JV_IS_UNDEFINED(v)) return NAN;
    if (JV_IS_NULL(v)) return 0;
    if (JV_IS_TRUE(v)) return 1;
    if (JV_IS_FALSE(v)) return 0;
    if (JV_IS_STR(v)) {
        struct JsString *s = JV_AS_STR(v);
        if (s->len == 0) return 0;
        // Trim
        const char *p = s->data, *e = p + s->len;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
        if (p == e) return 0;
        char buf[64];
        size_t len = e - p;
        if (len >= sizeof buf) return NAN;
        memcpy(buf, p, len);
        buf[len] = 0;
        if (strcmp(buf, "Infinity") == 0 || strcmp(buf, "+Infinity") == 0) return INFINITY;
        if (strcmp(buf, "-Infinity") == 0) return -INFINITY;
        char *end;
        double d;
        if (len > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
            d = (double)strtoll(buf, &end, 16);
        } else {
            d = strtod(buf, &end);
        }
        if (end != buf + len) return NAN;
        return d;
    }
    if (JV_IS_PTR(v) && jv_heap_type(v) >= JS_TOBJECT) {
        JsValue p = js_to_primitive(c, v, "number");
        return js_to_double(c, p);
    }
    return NAN;
}

JsValue
js_to_number(CTX *c, JsValue v)
{
    if (JV_IS_SMI(v) || JV_IS_FLONUM(v)) return v;
    if (JV_IS_FLOAT_BOX(v)) return v;
    return JV_DBL(js_to_double(c, v));
}

int32_t
js_to_int32(CTX *c, JsValue v)
{
    if (JV_IS_SMI(v)) return (int32_t)JV_AS_SMI(v);
    double d = js_to_double(c, v);
    if (!isfinite(d)) return 0;
    double f = trunc(d);
    double m = fmod(f, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    if (m >= 2147483648.0) m -= 4294967296.0;
    return (int32_t)m;
}

uint32_t
js_to_uint32(CTX *c, JsValue v)
{
    return (uint32_t)js_to_int32(c, v);
}

static struct JsString *
double_to_str(CTX *c, double d)
{
    if (isnan(d)) return js_str_intern(c, "NaN");
    if (isinf(d)) return js_str_intern(c, d > 0 ? "Infinity" : "-Infinity");
    if (d == 0) return js_str_intern(c, "0");
    char buf[64];
    if (trunc(d) == d && fabs(d) < 1e21) {
        snprintf(buf, sizeof buf, "%lld", (long long)d);
    } else {
        snprintf(buf, sizeof buf, "%.17g", d);
        // trim trailing zeros.
    }
    return js_str_intern(c, buf);
}

struct JsString *
js_to_string(CTX *c, JsValue v)
{
    if (JV_IS_STR(v)) return JV_AS_STR(v);
    if (JV_IS_UNDEFINED(v)) return js_str_intern(c, "undefined");
    if (JV_IS_NULL(v))      return js_str_intern(c, "null");
    if (JV_IS_TRUE(v))      return js_str_intern(c, "true");
    if (JV_IS_FALSE(v))     return js_str_intern(c, "false");
    if (JV_IS_SMI(v)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", (long long)JV_AS_SMI(v));
        return js_str_intern(c, buf);
    }
    if (JV_IS_FLONUM(v) || JV_IS_FLOAT_BOX(v)) return double_to_str(c, JV_AS_DBL(v));
    if (JV_IS_PTR(v)) {
        uint8_t t = jv_heap_type(v);
        if (t == JS_TARRAY) {
            // join with ","  (Array.prototype.toString = join(","))
            struct JsArray *a = JV_AS_ARRAY(v);
            if (a->length == 0) return js_str_intern(c, "");
            // crude: build with realloc
            size_t cap = 64; size_t len = 0; char *buf = malloc(cap);
            for (uint32_t i = 0; i < a->length; i++) {
                if (i > 0) {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = ',';
                }
                JsValue ev = a->dense[i];
                if (ev == JV_HOLE || JV_IS_UNDEFINED(ev) || JV_IS_NULL(ev)) continue;
                struct JsString *es = js_to_string(c, ev);
                if (len + es->len >= cap) { while (len + es->len + 2 >= cap) cap *= 2; buf = realloc(buf, cap); }
                memcpy(buf + len, es->data, es->len);
                len += es->len;
            }
            struct JsString *s = js_str_intern_n(c, buf, len);
            free(buf);
            return s;
        }
        if (t == JS_TFUNCTION) {
            struct JsFunction *fn = JV_AS_FUNC(v);
            char buf[256];
            snprintf(buf, sizeof buf, "function %s() { [native code] }", fn->name ? fn->name : "");
            return js_str_intern(c, buf);
        }
        if (t == JS_TCFUNCTION) {
            struct JsCFunction *fn = JV_AS_CFUNC(v);
            char buf[256];
            snprintf(buf, sizeof buf, "function %s() { [native code] }", fn->name ? fn->name : "");
            return js_str_intern(c, buf);
        }
        if (t == JS_TERROR) {
            struct JsObject *o = JV_AS_OBJ(v);
            JsValue nm = js_object_get(c, o, js_str_intern(c, "name"));
            JsValue ms = js_object_get(c, o, js_str_intern(c, "message"));
            const char *namc = JV_IS_STR(nm) ? js_str_data(JV_AS_STR(nm)) : "Error";
            const char *msgc = JV_IS_STR(ms) ? js_str_data(JV_AS_STR(ms)) : "";
            char buf[1024];
            if (msgc[0]) snprintf(buf, sizeof buf, "%s: %s", namc, msgc);
            else         snprintf(buf, sizeof buf, "%s", namc);
            return js_str_intern(c, buf);
        }
        if (t >= JS_TOBJECT) {
            // Try toString method.
            JsValue m = js_object_get(c, JV_AS_OBJ(v), js_str_intern(c, "toString"));
            if (JV_IS_PTR(m) && (jv_heap_type(m) == JS_TFUNCTION || jv_heap_type(m) == JS_TCFUNCTION)) {
                JsValue r = js_call(c, m, v, NULL, 0);
                if (JV_IS_STR(r)) return JV_AS_STR(r);
            }
            return js_str_intern(c, "[object Object]");
        }
    }
    return js_str_intern(c, "");
}

struct JsObject *
js_to_object(CTX *c, JsValue v)
{
    if (JV_IS_NULLISH(v)) js_throw_type_error(c, "Cannot convert undefined or null to object");
    if (JV_IS_PTR(v) && jv_heap_type(v) >= JS_TOBJECT) return JV_AS_OBJ(v);
    // Wrap primitives — we just return a minimal object; benchmarks rarely
    // depend on the specific wrapper class.
    struct JsObject *o = js_object_new(c, c->object_proto);
    js_object_set(c, o, js_str_intern(c, "_primitive"), v);
    return o;
}

bool
js_strict_eq(JsValue a, JsValue b)
{
    if (a == b) {
        if (JV_IS_FLONUM(a) || JV_IS_FLOAT_BOX(a)) {
            double d = JV_AS_DBL(a);
            return d == d;     // NaN !== NaN
        }
        return true;
    }
    // Different bit patterns but possibly equal values:
    //   - SMI vs flonum holding same numeric value
    //   - flonum vs heap-double
    if (JV_IS_NUM(a) && JV_IS_NUM(b)) {
        double da = JV_IS_SMI(a) ? (double)JV_AS_SMI(a) : JV_AS_DBL(a);
        double db = JV_IS_SMI(b) ? (double)JV_AS_SMI(b) : JV_AS_DBL(b);
        if (da != da || db != db) return false;
        return da == db;
    }
    return false;
}

bool
js_loose_eq(CTX *c, JsValue a, JsValue b)
{
    if (js_strict_eq(a, b)) return true;
    // §7.2.14 IsLooselyEqual.
    if ((JV_IS_NULL(a) && JV_IS_UNDEFINED(b)) || (JV_IS_UNDEFINED(a) && JV_IS_NULL(b))) return true;
    if (JV_IS_NUM(a) && JV_IS_STR(b)) return js_to_double(c, a) == js_to_double(c, b);
    if (JV_IS_STR(a) && JV_IS_NUM(b)) return js_to_double(c, a) == js_to_double(c, b);
    if (JV_IS_BOOL(a)) return js_loose_eq(c, JV_DBL(JV_AS_BOOL(a) ? 1.0 : 0.0), b);
    if (JV_IS_BOOL(b)) return js_loose_eq(c, a, JV_DBL(JV_AS_BOOL(b) ? 1.0 : 0.0));
    if ((JV_IS_NUM(a) || JV_IS_STR(a)) && JV_IS_PTR(b) && jv_heap_type(b) >= JS_TOBJECT)
        return js_loose_eq(c, a, js_to_primitive(c, b, "default"));
    if ((JV_IS_NUM(b) || JV_IS_STR(b)) && JV_IS_PTR(a) && jv_heap_type(a) >= JS_TOBJECT)
        return js_loose_eq(c, js_to_primitive(c, a, "default"), b);
    return false;
}

// Tri-state version: 1 = true, 0 = false, -1 = undefined (NaN result).
int
js_lt_ts(CTX *c, JsValue a, JsValue b, bool left_first)
{
    JsValue px, py;
    if (left_first) {
        px = js_to_primitive(c, a, "number");
        py = js_to_primitive(c, b, "number");
    } else {
        py = js_to_primitive(c, b, "number");
        px = js_to_primitive(c, a, "number");
    }
    if (JV_IS_STR(px) && JV_IS_STR(py)) {
        struct JsString *sa = JV_AS_STR(px), *sb = JV_AS_STR(py);
        size_t n = sa->len < sb->len ? sa->len : sb->len;
        int r = memcmp(sa->data, sb->data, n);
        if (r != 0) return r < 0 ? 1 : 0;
        return sa->len < sb->len ? 1 : 0;
    }
    double da = js_to_double(c, px);
    double db = js_to_double(c, py);
    if (isnan(da) || isnan(db)) return -1;
    return da < db ? 1 : 0;
}

bool
js_lt(CTX *c, JsValue a, JsValue b, bool left_first)
{
    int r = js_lt_ts(c, a, b, left_first);
    return r > 0;
}

JsValue
js_add(CTX *c, JsValue a, JsValue b)
{
    if (JV_IS_SMI(a) && JV_IS_SMI(b)) return JV_INT(JV_AS_SMI(a) + JV_AS_SMI(b));
    JsValue lp = js_to_primitive(c, a, "default");
    JsValue rp = js_to_primitive(c, b, "default");
    if (JV_IS_STR(lp) || JV_IS_STR(rp)) {
        struct JsString *ls = js_to_string(c, lp);
        struct JsString *rs = js_to_string(c, rp);
        return JV_STR(js_str_concat(c, ls, rs));
    }
    return JV_DBL(js_to_double(c, lp) + js_to_double(c, rp));
}
JsValue js_sub(CTX *c, JsValue a, JsValue b) { return JV_DBL(js_to_double(c,a) - js_to_double(c,b)); }
JsValue js_mul(CTX *c, JsValue a, JsValue b) { return JV_DBL(js_to_double(c,a) * js_to_double(c,b)); }
JsValue js_div(CTX *c, JsValue a, JsValue b) {
    if (JV_IS_SMI(a) && JV_IS_SMI(b)) {
        int64_t bx = JV_AS_SMI(b);
        if (bx == 0) {
            int64_t ax = JV_AS_SMI(a);
            if (ax == 0) return JV_DBL(NAN);
            return JV_DBL(ax > 0 ? INFINITY : -INFINITY);
        }
        int64_t ax = JV_AS_SMI(a);
        if (ax % bx == 0) return JV_INT(ax / bx);
        return JV_DBL((double)ax / (double)bx);
    }
    return JV_DBL(js_to_double(c,a) / js_to_double(c,b));
}
JsValue js_mod(CTX *c, JsValue a, JsValue b) {
    if (JV_IS_SMI(a) && JV_IS_SMI(b)) {
        int64_t bx = JV_AS_SMI(b);
        if (bx == 0) return JV_DBL(NAN);
        return JV_INT(JV_AS_SMI(a) % bx);
    }
    return JV_DBL(fmod(js_to_double(c,a), js_to_double(c,b)));
}
JsValue js_pow(CTX *c, JsValue a, JsValue b) {
    return JV_DBL(pow(js_to_double(c,a), js_to_double(c,b)));
}
JsValue js_neg(CTX *c, JsValue v) {
    if (JV_IS_SMI(v)) {
        int64_t x = JV_AS_SMI(v);
        if (x != INT64_MIN) return JV_INT(-x);
    }
    return JV_DBL(-js_to_double(c, v));
}
JsValue js_bnot(CTX *c, JsValue v) {
    int32_t x = js_to_int32(c, v);
    return JV_INT((int64_t)(int32_t)(~x));
}
JsValue js_band(CTX *c, JsValue a, JsValue b) {
    return JV_INT((int64_t)(int32_t)(js_to_int32(c,a) & js_to_int32(c,b)));
}
JsValue js_bor(CTX *c, JsValue a, JsValue b) {
    return JV_INT((int64_t)(int32_t)(js_to_int32(c,a) | js_to_int32(c,b)));
}
JsValue js_bxor(CTX *c, JsValue a, JsValue b) {
    return JV_INT((int64_t)(int32_t)(js_to_int32(c,a) ^ js_to_int32(c,b)));
}
JsValue js_shl(CTX *c, JsValue a, JsValue b) {
    int32_t ax = js_to_int32(c,a);
    uint32_t bx = js_to_uint32(c,b) & 31;
    return JV_INT((int64_t)(int32_t)(ax << bx));
}
JsValue js_sar(CTX *c, JsValue a, JsValue b) {
    int32_t ax = js_to_int32(c,a);
    uint32_t bx = js_to_uint32(c,b) & 31;
    return JV_INT((int64_t)(int32_t)(ax >> bx));
}
JsValue js_shr(CTX *c, JsValue a, JsValue b) {
    uint32_t ax = js_to_uint32(c,a);
    uint32_t bx = js_to_uint32(c,b) & 31;
    return JV_INT((int64_t)(uint32_t)(ax >> bx));
}
JsValue js_in(CTX *c, JsValue key, JsValue obj) {
    if (!JV_IS_PTR(obj) || jv_heap_type(obj) < JS_TOBJECT)
        js_throw_type_error(c, "Cannot use 'in' on a non-object");
    struct JsString *kn = js_to_string(c, key);
    if (JV_IS_ARRAY(obj)) {
        struct JsArray *a = JV_AS_ARRAY(obj);
        if (kn->len > 0 && kn->data[0] >= '0' && kn->data[0] <= '9') {
            char *end;
            long long i = strtoll(kn->data, &end, 10);
            if ((size_t)(end - kn->data) == kn->len && i >= 0 && i < a->length) return JV_TRUE;
        }
        if (a->fallback && js_object_has(a->fallback, kn)) return JV_TRUE;
        return JV_FALSE;
    }
    return JV_BOOL(js_object_has(JV_AS_OBJ(obj), kn));
}
JsValue js_instanceof(CTX *c, JsValue v, JsValue ctor) {
    if (!JV_IS_PTR(ctor) || (jv_heap_type(ctor) != JS_TFUNCTION && jv_heap_type(ctor) != JS_TCFUNCTION))
        js_throw_type_error(c, "Right-hand side of instanceof is not callable");
    if (!JV_IS_PTR(v) || jv_heap_type(v) < JS_TOBJECT) return JV_FALSE;
    struct JsObject *proto = NULL;
    if (jv_heap_type(ctor) == JS_TFUNCTION) {
        struct JsFunction *fn = JV_AS_FUNC(ctor);
        if (!fn->home_proto) return JV_FALSE;
        proto = fn->home_proto;
    } else {
        return JV_FALSE;
    }
    struct JsObject *p;
    if (JV_IS_ARRAY(v)) p = JV_AS_ARRAY(v)->proto;
    else                p = JV_AS_OBJ(v)->proto;
    while (p) {
        if (p == proto) return JV_TRUE;
        p = p->proto;
    }
    return JV_FALSE;
}

// =====================================================================
// Print
// =====================================================================
void
js_print_value(CTX *c, FILE *fp, JsValue v)
{
    if (JV_IS_UNDEFINED(v)) { fputs("undefined", fp); return; }
    if (JV_IS_NULL(v))      { fputs("null", fp); return; }
    if (JV_IS_TRUE(v))      { fputs("true", fp); return; }
    if (JV_IS_FALSE(v))     { fputs("false", fp); return; }
    if (JV_IS_SMI(v))       { fprintf(fp, "%lld", (long long)JV_AS_SMI(v)); return; }
    if (JV_IS_FLONUM(v) || JV_IS_FLOAT_BOX(v)) {
        double d = JV_AS_DBL(v);
        if (isnan(d))      fputs("NaN", fp);
        else if (isinf(d)) fputs(d > 0 ? "Infinity" : "-Infinity", fp);
        else if (trunc(d) == d && fabs(d) < 1e21) fprintf(fp, "%lld", (long long)d);
        else                fprintf(fp, "%.17g", d);
        return;
    }
    if (JV_IS_STR(v)) {
        struct JsString *s = JV_AS_STR(v);
        fwrite(s->data, 1, s->len, fp);
        return;
    }
    if (JV_IS_ARRAY(v)) {
        struct JsArray *a = JV_AS_ARRAY(v);
        fputc('[', fp);
        for (uint32_t i = 0; i < a->length; i++) {
            if (i > 0) fputs(", ", fp);
            JsValue ev = (i < a->dense_capa) ? a->dense[i] : JV_UNDEFINED;
            if (ev == JV_HOLE) fputs("<hole>", fp);
            else js_print_value(c, fp, ev);
        }
        fputc(']', fp);
        return;
    }
    if (JV_IS_PTR(v)) {
        uint8_t t = jv_heap_type(v);
        if (t == JS_TFUNCTION) {
            struct JsFunction *fn = JV_AS_FUNC(v);
            fprintf(fp, "[Function: %s]", fn->name ? fn->name : "(anon)");
            return;
        }
        if (t == JS_TCFUNCTION) {
            struct JsCFunction *fn = JV_AS_CFUNC(v);
            fprintf(fp, "[Function: %s]", fn->name ? fn->name : "(anon)");
            return;
        }
        if (t == JS_TERROR) {
            struct JsString *s = js_to_string(c, v);
            fwrite(s->data, 1, s->len, fp);
            return;
        }
        if (t >= JS_TOBJECT) {
            struct JsObject *o = JV_AS_OBJ(v);
            fputc('{', fp);
            for (uint32_t i = 0; i < o->shape->nslots; i++) {
                if (i > 0) fputs(", ", fp);
                struct JsString *k = o->shape->names[i];
                fwrite(k->data, 1, k->len, fp); fputs(": ", fp);
                js_print_value(c, fp, o->slots[i]);
            }
            fputc('}', fp);
            return;
        }
    }
    fprintf(fp, "<unknown>");
}

// =====================================================================
// Context creation
// =====================================================================

CTX *
js_create_context(void)
{
    CTX *c = (CTX *)calloc(1, sizeof(CTX));
    c->stack = (JsValue *)calloc(JSTRO_STACK_SIZE, sizeof(JsValue));
    c->stack_end = c->stack + JSTRO_STACK_SIZE;
    c->sp = c->stack;
    c->intern_cap = 256;
    c->intern_buckets = (struct JsString **)calloc(c->intern_cap, sizeof(struct JsString *));
    c->intern_cnt = 0;
    c->this_val = JV_UNDEFINED;
    c->cur_upvals = NULL;
    c->throw_top = NULL;
    c->all_objects = NULL;
    c->bytes_allocated = 0;
    c->gc_threshold = 4096;     // initial: collect after 4K objects allocated
    c->gc_disabled = false;
    c->root_shape = NULL;
    jstro_main_ctx = c;
    return c;
}

void
js_init_globals(CTX *c)
{
    // Set up prototype objects.
    c->object_proto   = js_object_new(c, NULL);
    c->function_proto = js_object_new(c, c->object_proto);
    c->array_proto    = js_object_new(c, c->object_proto);
    c->string_proto   = js_object_new(c, c->object_proto);
    c->number_proto   = js_object_new(c, c->object_proto);
    c->boolean_proto  = js_object_new(c, c->object_proto);
    c->error_proto    = js_object_new(c, c->object_proto);
    c->map_proto      = js_object_new(c, c->object_proto);
    c->set_proto      = js_object_new(c, c->object_proto);
    c->mapiter_proto  = js_object_new(c, c->object_proto);
    c->regex_proto    = js_object_new(c, c->object_proto);

    c->globals = js_object_new(c, c->object_proto);

    // Stdlib hookup happens in js_stdlib.c.
}
