// asom: SOM runtime support.
//
// Phase 1 scope: object model + globals/interning + minimal `asom_send`
// implementation that walks the method table, dispatches primitives, and runs
// AST method bodies. Many Smalltalk-level features are still stubs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <time.h>

#include "context.h"
#include "node.h"
#include "asom_runtime.h"

// ---------------------------------------------------------------------------
// String interning (used for both symbols and identifier-like literals).
//
// Strategy: linear-probe hash table keyed by the string bytes. Pointer
// equality of the returned `const char *` therefore holds for identical
// strings. Selector names share the same pool, which lets us compare
// selectors with `==` rather than `strcmp` in the inline-cache hot path.
// ---------------------------------------------------------------------------

struct asom_intern_pool {
    size_t cap;
    size_t cnt;
    const char **slots;
};

static struct asom_intern_pool g_intern;

static uint64_t
asom_strhash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void
asom_intern_grow(void)
{
    size_t old_cap = g_intern.cap;
    const char **old_slots = g_intern.slots;
    size_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
    const char **new_slots = calloc(new_cap, sizeof(*new_slots));
    if (!new_slots) { fprintf(stderr, "asom: intern grow OOM\n"); exit(1); }
    for (size_t i = 0; i < old_cap; i++) {
        const char *s = old_slots[i];
        if (!s) continue;
        size_t mask = new_cap - 1;
        size_t idx = (size_t)asom_strhash(s) & mask;
        while (new_slots[idx]) idx = (idx + 1) & mask;
        new_slots[idx] = s;
    }
    free(old_slots);
    g_intern.slots = new_slots;
    g_intern.cap = new_cap;
}

const char *
asom_intern_cstr(const char *s)
{
    if (g_intern.cnt * 2 >= g_intern.cap) asom_intern_grow();
    size_t mask = g_intern.cap - 1;
    size_t idx = (size_t)asom_strhash(s) & mask;
    for (;;) {
        const char *cur = g_intern.slots[idx];
        if (!cur) {
            char *dup = strdup(s);
            if (!dup) { fprintf(stderr, "asom: strdup OOM\n"); exit(1); }
            g_intern.slots[idx] = dup;
            g_intern.cnt++;
            return dup;
        }
        if (strcmp(cur, s) == 0) return cur;
        idx = (idx + 1) & mask;
    }
}

// ---------------------------------------------------------------------------
// Globals: name -> VALUE (used for class lookup at parse time + reflection).
// ---------------------------------------------------------------------------

struct asom_global_entry {
    const char *name; // interned
    VALUE value;
};

static struct asom_global_entry *g_globals;
static size_t g_globals_cnt;
static size_t g_globals_cap;

// Non-loading global lookup. Returns 0 if not found (so callers can
// distinguish "absent" from "explicitly bound to nil").
static VALUE
asom_global_lookup(const char *interned_name)
{
    for (size_t i = 0; i < g_globals_cnt; i++) {
        if (g_globals[i].name == interned_name) return g_globals[i].value;
    }
    return 0;
}

// Exposed to asom_loader.c so that asom_load_class_impl can break the
// recursion that asom_global_get would otherwise create.
VALUE
asom_global_lookup_for_loader(const char *interned_name)
{
    return asom_global_lookup(interned_name);
}

VALUE
asom_global_get(CTX *c, const char *name)
{
    name = asom_intern_cstr(name);
    VALUE v = asom_global_lookup(name);
    if (v) return v;
    // Lazy class loading: if a .som file matches, load it and try again.
    // This mirrors how PySOM / SOM++ resolve unknown globals.
    if (c) {
        struct asom_class *cls = asom_load_class(c, name);
        if (cls) return ASOM_OBJ2VAL(cls);
    }
    return c ? c->val_nil : 0;
}

void
asom_global_set(CTX *c, const char *name, VALUE v)
{
    (void)c;
    name = asom_intern_cstr(name);
    for (size_t i = 0; i < g_globals_cnt; i++) {
        if (g_globals[i].name == name) {
            g_globals[i].value = v;
            return;
        }
    }
    if (g_globals_cnt == g_globals_cap) {
        g_globals_cap = g_globals_cap ? g_globals_cap * 2 : 64;
        g_globals = realloc(g_globals, g_globals_cap * sizeof(*g_globals));
        if (!g_globals) { fprintf(stderr, "asom: globals OOM\n"); exit(1); }
    }
    g_globals[g_globals_cnt++] = (struct asom_global_entry){ .name = name, .value = v };
}

// ---------------------------------------------------------------------------
// String / symbol interning at runtime — produces actual asom_string objects.
// ---------------------------------------------------------------------------

VALUE
asom_intern_string(CTX *c, const char *bytes)
{
    const char *interned = asom_intern_cstr(bytes);
    struct asom_string *s = calloc(1, sizeof(*s));
    s->hdr.klass = c ? c->cls_string : NULL;
    s->bytes = interned;
    s->len = strlen(interned);
    return ASOM_OBJ2VAL(s);
}

// Symbol VALUE interning: identical names map to the same heap object so
// that `==` is enough for equality (matching SOM/Smalltalk semantics).
struct asom_sym_pool_entry {
    const char *bytes;   // already interned via asom_intern_cstr
    VALUE       value;
};
static struct asom_sym_pool_entry *g_sym_pool;
static size_t g_sym_pool_cnt;
static size_t g_sym_pool_cap;

VALUE
asom_intern_symbol(CTX *c, const char *bytes)
{
    const char *interned = asom_intern_cstr(bytes);
    for (size_t i = 0; i < g_sym_pool_cnt; i++) {
        if (g_sym_pool[i].bytes == interned) return g_sym_pool[i].value;
    }
    struct asom_string *s = calloc(1, sizeof(*s));
    s->hdr.klass = c ? c->cls_symbol : NULL;
    s->bytes = interned;
    s->len = strlen(interned);
    VALUE v = ASOM_OBJ2VAL(s);
    if (g_sym_pool_cnt == g_sym_pool_cap) {
        g_sym_pool_cap = g_sym_pool_cap ? g_sym_pool_cap * 2 : 64;
        g_sym_pool = realloc(g_sym_pool, g_sym_pool_cap * sizeof(*g_sym_pool));
    }
    g_sym_pool[g_sym_pool_cnt].bytes = interned;
    g_sym_pool[g_sym_pool_cnt].value = v;
    g_sym_pool_cnt++;
    return v;
}

// ---------------------------------------------------------------------------
// Object allocation.
// ---------------------------------------------------------------------------

VALUE
asom_object_new(CTX *c, struct asom_class *cls)
{
    (void)c;
    size_t fields_bytes = (size_t)cls->num_instance_fields * sizeof(VALUE);
    struct asom_object *o = calloc(1, sizeof(*o) + fields_bytes);
    o->klass = cls;
    VALUE *fields = (VALUE *)(o + 1);
    for (uint32_t i = 0; i < cls->num_instance_fields; i++) {
        fields[i] = c ? c->val_nil : 0;
    }
    return ASOM_OBJ2VAL(o);
}

VALUE
asom_array_new(CTX *c, uint32_t len)
{
    struct asom_array *a = calloc(1, sizeof(*a));
    a->hdr.klass = c->cls_array;
    a->len = len;
    a->data = len ? calloc(len, sizeof(VALUE)) : NULL;
    for (uint32_t i = 0; i < len; i++) a->data[i] = c->val_nil;
    return ASOM_OBJ2VAL(a);
}

// Double bump arena. Tight numeric loops (Mandelbrot, NBody) create one
// asom_double per op; calloc dominates the cost. Bump-allocation from a
// growing chain of slabs reduces per-op cost to ~1 ns. Slabs are never
// freed — until we have a GC, doubles leak just like frames did before
// the pool, but the rate is bounded per benchmark run.
#define ASOM_DOUBLE_SLAB_COUNT 4096

struct asom_double_arena {
    struct asom_double *next;
    struct asom_double *end;
};

static __thread struct asom_double_arena g_double_arena;

static __attribute__((noinline)) struct asom_double *
asom_double_arena_grow(void)
{
    struct asom_double *slab = malloc(ASOM_DOUBLE_SLAB_COUNT * sizeof(*slab));
    if (!slab) { fprintf(stderr, "asom: double slab OOM\n"); exit(1); }
    g_double_arena.next = slab;
    g_double_arena.end = slab + ASOM_DOUBLE_SLAB_COUNT;
    return slab;
}

VALUE
asom_double_new(CTX *c, double v)
{
    struct asom_double *d = g_double_arena.next;
    if (UNLIKELY(d >= g_double_arena.end)) {
        d = asom_double_arena_grow();
    }
    g_double_arena.next = d + 1;
    d->hdr.klass = c->cls_double;
    d->value = v;
    return ASOM_OBJ2VAL(d);
}

VALUE
asom_string_new(CTX *c, const char *bytes, size_t len)
{
    char *dup = malloc(len + 1);
    memcpy(dup, bytes, len);
    dup[len] = '\0';
    struct asom_string *s = calloc(1, sizeof(*s));
    s->hdr.klass = c->cls_string;
    s->bytes = dup;
    s->len = len;
    return ASOM_OBJ2VAL(s);
}

// ---------------------------------------------------------------------------
// Method tables.
// ---------------------------------------------------------------------------

static void
asom_method_table_grow(struct asom_method_table *t)
{
    uint32_t old_cap = t->cap;
    struct asom_method **old = t->slots;
    uint32_t new_cap = old_cap == 0 ? 8 : old_cap * 2;
    struct asom_method **fresh = calloc(new_cap, sizeof(*fresh));
    for (uint32_t i = 0; i < old_cap; i++) {
        struct asom_method *m = old[i];
        if (!m) continue;
        uint32_t idx = ((uintptr_t)m->selector >> 3) & (new_cap - 1);
        while (fresh[idx]) idx = (idx + 1) & (new_cap - 1);
        fresh[idx] = m;
    }
    free(old);
    t->slots = fresh;
    t->cap = new_cap;
}

void
asom_class_define_method(struct asom_class *cls, struct asom_method *m)
{
    struct asom_method_table *t = &cls->methods;
    if (t->cnt * 2 >= t->cap) asom_method_table_grow(t);
    uint32_t idx = ((uintptr_t)m->selector >> 3) & (t->cap - 1);
    bool fresh = true;
    for (;;) {
        struct asom_method *cur = t->slots[idx];
        if (!cur) { t->slots[idx] = m; t->cnt++; break; }
        if (cur->selector == m->selector) { t->slots[idx] = m; fresh = false; break; }
        idx = (idx + 1) & (t->cap - 1);
    }
    if (fresh) {
        if (t->order_cnt == t->order_cap) {
            t->order_cap = t->order_cap ? t->order_cap * 2 : 16;
            t->ordered = realloc(t->ordered, t->order_cap * sizeof(*t->ordered));
        }
        t->ordered[t->order_cnt++] = m;
    }
}

struct asom_method *
asom_class_lookup(struct asom_class *cls, const char *selector)
{
    // The hash bucket is keyed on the interned pointer (`m->selector`), so
    // we first try pointer-equality probing. On miss (e.g. when the call
    // came from SD-compiled code that uses a bare string literal not in
    // the intern pool), fall back to a linear strcmp scan over the
    // ordered method list. Both layers walk the superclass chain.
    for (struct asom_class *cur = cls; cur; cur = cur->superclass) {
        struct asom_method_table *t = &cur->methods;
        if (t->cap == 0) continue;
        uint32_t idx = ((uintptr_t)selector >> 3) & (t->cap - 1);
        for (uint32_t probed = 0; probed < t->cap; probed++) {
            struct asom_method *m = t->slots[idx];
            if (!m) break;
            if (m->selector == selector) return m;
            idx = (idx + 1) & (t->cap - 1);
        }
    }
    // Slow path: bare-literal selectors from SD code.
    for (struct asom_class *cur = cls; cur; cur = cur->superclass) {
        struct asom_method_table *t = &cur->methods;
        for (uint32_t i = 0; i < t->order_cnt; i++) {
            struct asom_method *m = t->ordered[i];
            if (m && strcmp(m->selector, selector) == 0) return m;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Class-of for VALUEs.
// ---------------------------------------------------------------------------

struct asom_class *
asom_class_of(CTX *c, VALUE v)
{
    if (ASOM_IS_INT(v)) return c->cls_integer;
    struct asom_object *o = ASOM_VAL2OBJ(v);
    return o->klass;
}

// ---------------------------------------------------------------------------
// Frame chain + non-local return.
//
// Each method invocation pushes a frame; nested blocks share the home frame.
// `^` inside a block translates to longjmp() to the home frame's setjmp,
// which is installed by asom_invoke_method around the body evaluation.
// ---------------------------------------------------------------------------

struct asom_unwind {
    jmp_buf jb;
    struct asom_frame *home;       // method-body unwind (asom_invoke installs)
    bool is_block_value;           // block_invoke installs as escape catcher
    struct asom_block *current_block; // for escape: which block is escaping
    VALUE value;
    bool escape;                   // set on escape longjmp
    struct asom_unwind *parent;
};

static __thread struct asom_unwind *g_unwind_top;

void
asom_nonlocal_return(CTX *c, VALUE v)
{
    struct asom_frame *home = c->frame->home;
    for (struct asom_unwind *u = g_unwind_top; u; u = u->parent) {
        if (u->home == home) {
            u->value = v;
            u->escape = false;
            longjmp(u->jb, 1);
        }
    }
    // Home is gone — find the innermost block_invoke and longjmp there with
    // the escape flag, so it can dispatch `escapedBlock:` to the sender.
    for (struct asom_unwind *u = g_unwind_top; u; u = u->parent) {
        if (u->is_block_value) {
            u->value = v;
            u->escape = true;
            longjmp(u->jb, 1);
        }
    }
    fprintf(stderr, "asom: non-local return: home frame is gone (no block escape catcher)\n");
    exit(1);
}

// ---------------------------------------------------------------------------
// Block construction + invocation.
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  Inline-frame push/pop for control-flow inlining (node_iftrue, etc.).
//  The block has been verified at parse time to have 0 params, 0 locals,
//  and no nested block literals — so a stack-allocated frame is safe:
//  no locals to allocate, and no closure created during the body could
//  outlive this call to capture us. The body's `lvar_get(scope=N)` refs
//  walk the lexical chain unchanged because lexical_parent points to
//  what asom_block_invoke would have set (the current frame).
// -----------------------------------------------------------------------------

// Forward decls for the per-bucket frame pool (defined later in this file).
static struct asom_frame *frame_alloc(uint32_t slots, VALUE **out_locals, uint16_t *out_pool_slots);
static void frame_free(struct asom_frame *frame);

void
asom_inline_frame_push(CTX *c, struct asom_frame *frame)
{
    frame->self = c->frame->self;
    frame->locals = NULL;
    frame->method = c->frame->method;   // home method, for traceback
    frame->parent = c->frame;
    frame->home = c->frame->home;       // `^` still targets the enclosing method
    frame->lexical_parent = c->frame;
    frame->returned = 0;
    frame->captured = false;
    frame->pool_slots = 0;              // never returned to the pool
    c->frame = frame;
}

void
asom_inline_frame_pop(CTX *c, struct asom_frame *frame)
{
    c->frame = frame->parent;
}

// Pool-allocated inline frame with N locals. Used by node_to_do (and
// future N-local inline patterns) where the body may create nested
// closures: the heap-resident frame can be pinned via `captured` if a
// closure escapes, exactly like asom_block_invoke. One pool pop per
// outer call, vs the un-inlined path's per-iteration block_invoke.
struct asom_frame *
asom_inline_pool_frame_push(CTX *c, uint32_t num_locals, VALUE **out_locals)
{
    VALUE *locals;
    uint16_t pool_slots;
    struct asom_frame *frame = frame_alloc(num_locals, &locals, &pool_slots);
    frame->self = c->frame->self;
    frame->locals = locals;
    frame->method = c->frame->method;
    frame->parent = c->frame;
    frame->home = c->frame->home;
    frame->lexical_parent = c->frame;
    frame->returned = 0;
    frame->captured = false;
    frame->pool_slots = pool_slots;
    for (uint32_t i = 0; i < num_locals; i++) locals[i] = c->val_nil;
    c->frame = frame;
    *out_locals = locals;
    return frame;
}

void
asom_inline_pool_frame_pop(CTX *c, struct asom_frame *frame)
{
    c->frame = frame->parent;
    frame_free(frame);
}

// Bump arena for the combined (asom_method + asom_block) records that
// asom_make_block produces every time a block literal evaluates. Each
// pair is fixed-size so we allocate them adjacent; only the slab head
// pointer leaks (until GC). For benchmarks like Sieve where each
// iteration of the outer to:do: body re-creates the same if-true
// block, this turns 2 callocs/iter into 2 pointer increments.
#define ASOM_BLOCK_SLAB_COUNT 1024

struct asom_block_record {
    struct asom_method method;
    struct asom_block  block;
};

struct asom_block_arena {
    struct asom_block_record *next;
    struct asom_block_record *end;
};

static __thread struct asom_block_arena g_block_arena;

static __attribute__((noinline)) struct asom_block_record *
asom_block_arena_grow(void)
{
    struct asom_block_record *slab = calloc(ASOM_BLOCK_SLAB_COUNT, sizeof(*slab));
    if (!slab) { fprintf(stderr, "asom: block slab OOM\n"); exit(1); }
    g_block_arena.next = slab;
    g_block_arena.end = slab + ASOM_BLOCK_SLAB_COUNT;
    return slab;
}

VALUE
asom_make_block(CTX *c, struct Node *body, uint32_t num_params, uint32_t num_locals)
{
    struct asom_block_record *r = g_block_arena.next;
    if (UNLIKELY(r >= g_block_arena.end)) {
        r = asom_block_arena_grow();
    }
    g_block_arena.next = r + 1;
    // Slab is calloc'd once per slab; a fresh record from the bump
    // pointer is therefore zero-initialised — overwrite only the fields
    // that vary per call, the rest stays zero.
    struct asom_method *m = &r->method;
    m->body = body;
    m->num_params = num_params;
    m->num_locals = num_locals;
    m->holder = c->frame->method ? c->frame->method->holder : NULL;
    m->selector = "<block>";

    struct asom_block *b = &r->block;
    // SOM has a separate Block1/Block2/Block3 class per arity (the block
    // includes the receiver in its argument count terminology, so 0/1/2
    // user-args map to Block1/Block2/Block3 respectively). Falls back to
    // generic Block for higher arities.
    switch (num_params) {
        case 0:  b->hdr.klass = c->cls_block1; break;
        case 1:  b->hdr.klass = c->cls_block2; break;
        case 2:  b->hdr.klass = c->cls_block3; break;
        default: b->hdr.klass = c->cls_block;  break;
    }
    b->method = m;
    b->home = c->frame->home;       // ^ inside the block targets the same method
    b->lexical_parent = c->frame;   // outer scope for var lookup
    b->captured_self = c->frame->self;
    // Pin our caller's frame to the heap: this closure may outlive the
    // call (returned, stored in an ivar, etc.), and reusing the frame in
    // a subsequent block_invoke would alias the closure's lexical_parent.
    // Conservative — even non-escaping closures pin their parent frame,
    // but that's a small price (frame stays heap-resident, no UAF).
    if (c->frame) c->frame->captured = true;
    // Also pin every enclosing lexical frame: a block stored in this
    // frame can be invoked later, which sets up *its* nested closures
    // pointing back through the chain.
    for (struct asom_frame *f = c->frame ? c->frame->lexical_parent : NULL; f; f = f->lexical_parent) {
        f->captured = true;
    }
    return ASOM_OBJ2VAL(b);
}

// -----------------------------------------------------------------------------
// Frame pool. Pools blocks of frame+locals storage by slot-count bucket so
// the common case (small block, no closure escape) avoids two callocs per
// invocation. `captured` frames are never returned to the pool; they leak
// onto the heap, which is the existing behaviour and keeps closures sound.
// -----------------------------------------------------------------------------

#define ASOM_FRAME_POOL_BUCKETS 16  // slots 0..ASOM_FRAME_POOL_BUCKETS-1

struct asom_frame_pool_node {
    struct asom_frame frame;
    struct asom_frame_pool_node *next;
    // locals[] follows inline (variable length per bucket)
};

static __thread struct asom_frame_pool_node *g_frame_pool[ASOM_FRAME_POOL_BUCKETS];

// Returns (frame, locals) where locals points just past `frame` in the
// same allocation. `out_pool_slots` is set to the bucket size used (0 if
// not pooled — slot count exceeded the pool, so a separate calloc was
// needed and the entry should not be returned to the pool on exit).
static inline struct asom_frame *
frame_alloc(uint32_t slots, VALUE **out_locals, uint16_t *out_pool_slots)
{
    if (slots < ASOM_FRAME_POOL_BUCKETS) {
        struct asom_frame_pool_node *e = g_frame_pool[slots];
        if (e) {
            g_frame_pool[slots] = e->next;
            // Zero out frame + locals area before re-use.
            VALUE *locals = (VALUE *)(e + 1);
            memset(&e->frame, 0, sizeof(struct asom_frame));
            for (uint32_t i = 0; i < slots; i++) locals[i] = 0;
            *out_locals = slots ? locals : NULL;
            *out_pool_slots = (uint16_t)(slots + 1); // +1 so 0 means "not pooled"
            return &e->frame;
        }
        // Cold: bucket empty — allocate a fresh combined node.
        size_t total = sizeof(struct asom_frame_pool_node) + slots * sizeof(VALUE);
        e = calloc(1, total);
        VALUE *locals = (VALUE *)(e + 1);
        *out_locals = slots ? locals : NULL;
        *out_pool_slots = (uint16_t)(slots + 1);
        return &e->frame;
    }
    // Slot count exceeds pool buckets — fall back to two callocs (rare,
    // method bodies with > ASOM_FRAME_POOL_BUCKETS-1 locals).
    struct asom_frame *frame = calloc(1, sizeof(*frame));
    *out_locals = slots ? calloc(slots, sizeof(VALUE)) : NULL;
    *out_pool_slots = 0;
    return frame;
}

static inline void
frame_free(struct asom_frame *frame)
{
    if (frame->captured || frame->pool_slots == 0) {
        // Captured — closure may still reference this frame's locals.
        // Or non-pooled (oversized). Leave it on the heap (existing leak).
        return;
    }
    uint32_t slots = (uint32_t)frame->pool_slots - 1;
    struct asom_frame_pool_node *e = (struct asom_frame_pool_node *)frame;
    e->next = g_frame_pool[slots];
    g_frame_pool[slots] = e;
}

// Invoke a block with `nargs` already-evaluated args. Used by Block primitives
// (value, value:, ...) and by Boolean/whileTrue:/etc. control-flow primitives.
VALUE
asom_block_invoke(CTX *c, struct asom_block *b, VALUE *args, uint32_t nargs)
{
    struct asom_method *m = b->method;
    if (UNLIKELY(nargs != m->num_params)) {
        fprintf(stderr, "asom: Block expected %u args, got %u\n",
                m->num_params, nargs);
        exit(1);
    }

    uint32_t slots = m->num_params + m->num_locals;
    VALUE *locals;
    uint16_t pool_slots;
    struct asom_frame *frame = frame_alloc(slots, &locals, &pool_slots);
    for (uint32_t i = 0; i < nargs; i++) locals[i] = args[i];
    for (uint32_t i = nargs; i < slots; i++) locals[i] = c->val_nil;

    frame->self = b->captured_self;
    frame->locals = locals;
    frame->method = m;
    frame->parent = c->frame;
    frame->home = b->home;                  // ^ targets the home method
    frame->lexical_parent = b->lexical_parent;
    frame->pool_slots = pool_slots;
    c->frame = frame;

    struct asom_unwind unwind = {
        .home = NULL,
        .is_block_value = true,
        .current_block = b,
        .parent = g_unwind_top,
    };
    g_unwind_top = &unwind;

    VALUE result;
    if (setjmp(unwind.jb) == 0) {
        result = EVAL(c, m->body);
        g_unwind_top = unwind.parent;
        c->frame = frame->parent;
        frame_free(frame);
        return result;
    }

    // Reached via escape NLR: home method already returned. Restore caller
    // frame and dispatch `escapedBlock:` to the original sender of `value`.
    // We don't return the frame to the pool: an escape happens because a
    // closure called `^` from inside a method whose home frame is gone,
    // so the frame state may still be inspected.
    g_unwind_top = unwind.parent;
    c->frame = frame->parent;
    VALUE sender_self = c->frame ? c->frame->self : c->val_nil;
    VALUE blk_val = ASOM_OBJ2VAL(b);
    return asom_send(c, sender_self, asom_intern_cstr("escapedBlock:"), 1, &blk_val, NULL);
}

// ---------------------------------------------------------------------------
// Method invocation (used by asom_send and asom_super_send).
// ---------------------------------------------------------------------------

static VALUE
asom_invoke(CTX *c, struct asom_method *m, VALUE receiver, VALUE *args, uint32_t nargs)
{
    if (m->primitive) {
        // Primitive fast path is duplicated in `asom_invoke_method` (header
        // inline) so SD-compiled code can elide the call into this TU; this
        // copy is the in-process fallback for cases that bypass the inline.
        switch (nargs) {
            case 0: return ((asom_prim0_t)m->primitive)(c, receiver);
            case 1: return ((asom_prim1_t)m->primitive)(c, receiver, args[0]);
            case 2: return ((asom_prim2_t)m->primitive)(c, receiver, args[0], args[1]);
            case 3: return ((asom_prim3_t)m->primitive)(c, receiver, args[0], args[1], args[2]);
            default:
                fprintf(stderr, "asom: primitive arity %u not supported\n", nargs);
                exit(1);
        }
    }

    if (!m->body) {
        fprintf(stderr, "asom: doesNotUnderstand: #%s (no body, no primitive)\n",
                m->selector ? m->selector : "?");
        exit(1);
    }

    // Bump dispatch count on the method's body so `--pg-compile` can later
    // pick out the hot entries by threshold.
    m->body->head.dispatch_cnt++;

    uint32_t slots = m->num_params + m->num_locals;
    VALUE *locals;
    uint16_t pool_slots;
    struct asom_frame *frame = frame_alloc(slots, &locals, &pool_slots);
    for (uint32_t i = 0; i < nargs && i < m->num_params; i++) locals[i] = args[i];
    for (uint32_t i = nargs; i < slots; i++) locals[i] = c->val_nil;

    frame->self = receiver;
    frame->locals = locals;
    frame->method = m;
    frame->parent = c->frame;
    frame->home = frame;
    frame->lexical_parent = NULL;
    frame->returned = 0;
    frame->pool_slots = pool_slots;
    c->frame = frame;

    VALUE result;
    if (m->no_nlr) {
        // Body has no nested block, so no NLR can target this frame.
        // Skip setjmp on the home frame entirely — saves ~50 ns per
        // method invocation in tight call chains.
        result = EVAL(c, m->body);
    } else {
        struct asom_unwind unwind = { .home = frame, .parent = g_unwind_top };
        g_unwind_top = &unwind;
        if (setjmp(unwind.jb) == 0) {
            result = EVAL(c, m->body);
        } else {
            result = unwind.value;
        }
        g_unwind_top = unwind.parent;
    }
    c->frame = frame->parent;
    // Pool the frame storage if no closure captured us. `captured` is set
    // by asom_make_block when a nested block grabs us as lexical_parent.
    frame_free(frame);
    return result;
}

// ---------------------------------------------------------------------------
// Sends.
// ---------------------------------------------------------------------------

VALUE
asom_send_slow(CTX *c, VALUE receiver, const char *selector,
               uint32_t nargs, VALUE *args, struct asom_callcache *cc)
{
    struct asom_class *cls = asom_class_of(c, receiver);
    struct asom_method *m = asom_class_lookup(cls, selector);
    if (UNLIKELY(m == NULL)) {
        // Smalltalk dispatch: try `doesNotUnderstand: aSymbol arguments: anArray`.
        struct asom_method *dnu = asom_class_lookup(cls, asom_intern_cstr("doesNotUnderstand:arguments:"));
        if (dnu) {
            VALUE arr = asom_array_new(c, nargs);
            for (uint32_t i = 0; i < nargs; i++) {
                ((struct asom_array *)ASOM_VAL2OBJ(arr))->data[i] = args[i];
            }
            VALUE dnuArgs[2] = { asom_intern_symbol(c, selector), arr };
            return asom_invoke(c, dnu, receiver, dnuArgs, 2);
        }
        fprintf(stderr, "asom: %s>>%s does not understand\n",
                cls && cls->name ? cls->name : "?", selector);
        exit(1);
    }
    if (cc) {
        cc->serial = c->serial;
        cc->cached_class = cls;
        cc->cached_method = m;
    }
    return asom_invoke(c, m, receiver, args, nargs);
}

// AST-only invocation path; called from the inline asom_invoke_method when
// m->primitive is NULL. Keeps the heavyweight setjmp/heap-frame work out of
// the inline header so call sites remain compact.
VALUE
asom_invoke_ast(CTX *c, struct asom_method *m, VALUE recv, VALUE *args, uint32_t nargs)
{
    return asom_invoke(c, m, recv, args, nargs);
}

VALUE
asom_super_send(CTX *c, VALUE receiver, const char *selector,
                uint32_t nargs, VALUE *args, struct asom_callcache *cc)
{
    (void)cc;
    struct asom_class *holder = c->frame->method ? c->frame->method->holder : NULL;
    struct asom_class *start = holder && holder->superclass ? holder->superclass : NULL;
    if (!start) {
        fprintf(stderr, "asom: super send with no superclass\n");
        exit(1);
    }
    struct asom_method *m = asom_class_lookup(start, selector);
    if (!m) {
        fprintf(stderr, "asom: super>>%s not found\n", selector);
        exit(1);
    }
    return asom_invoke(c, m, receiver, args, nargs);
}

// ---------------------------------------------------------------------------
// Classpath. Stub: stored as a single colon-separated string for now.
// ---------------------------------------------------------------------------

void
asom_classpath_add(const char *dir)
{
    (void)dir;
    // The class loader currently reads OPTION.classpath directly; this hook
    // is here for symmetry with how SOM++ exposes classpath manipulation.
}

// ---------------------------------------------------------------------------
// Compilation entry-point registry. AST method bodies installed from .som
// files end up here so the AOT / PG drivers can iterate over them.
// ---------------------------------------------------------------------------

static struct asom_entry *g_entries;
static uint32_t           g_entries_cnt;
static uint32_t           g_entries_cap;

void
asom_register_entry(struct Node *body, const char *label, const char *file)
{
    if (!body) return;
    if (g_entries_cnt == g_entries_cap) {
        g_entries_cap = g_entries_cap ? g_entries_cap * 2 : 64;
        g_entries = realloc(g_entries, g_entries_cap * sizeof(*g_entries));
    }
    g_entries[g_entries_cnt].body = body;
    g_entries[g_entries_cnt].label = label;
    g_entries[g_entries_cnt].file = file;
    g_entries_cnt++;
}

struct asom_entry *
asom_entries(uint32_t *count_out)
{
    *count_out = g_entries_cnt;
    return g_entries;
}

// ---------------------------------------------------------------------------
// Class loader: defined in asom_loader.c.
// ---------------------------------------------------------------------------

extern struct asom_class *asom_load_class_impl(CTX *c, const char *name);

struct asom_class *
asom_load_class(CTX *c, const char *name)
{
    return asom_load_class_impl(c, name);
}

// ---------------------------------------------------------------------------
// Bootstrap: construct minimal Object / Class / Integer / String / Block /
// Symbol / System / nil / true / false. The full SOM standard library is
// loaded on demand from .som files; this just installs enough that literal
// evaluation and class lookup don't dereference NULL.
// ---------------------------------------------------------------------------

static struct asom_class *
asom_bootstrap_class(const char *name, struct asom_class *super)
{
    struct asom_class *cls = calloc(1, sizeof(*cls));
    cls->name = asom_intern_cstr(name);
    cls->superclass = super;
    return cls;
}

long
asom_get_ticks_us(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000);
}

void
asom_runtime_init(CTX *c)
{
    c->cls_object   = asom_bootstrap_class("Object",      NULL);
    c->cls_class    = asom_bootstrap_class("Class",       c->cls_object);
    c->cls_metaclass= asom_bootstrap_class("Metaclass",   c->cls_class);
    c->cls_nil      = asom_bootstrap_class("Nil",         c->cls_object);
    c->cls_boolean  = asom_bootstrap_class("Boolean",     c->cls_object);
    c->cls_true     = asom_bootstrap_class("True",        c->cls_boolean);
    c->cls_false    = asom_bootstrap_class("False",       c->cls_boolean);
    c->cls_integer  = asom_bootstrap_class("Integer",     c->cls_object);
    c->cls_double   = asom_bootstrap_class("Double",      c->cls_object);
    c->cls_string   = asom_bootstrap_class("String",      c->cls_object);
    c->cls_symbol   = asom_bootstrap_class("Symbol",      c->cls_string);
    c->cls_array    = asom_bootstrap_class("Array",       c->cls_object);
    c->cls_block    = asom_bootstrap_class("Block",       c->cls_object);
    c->cls_block1   = asom_bootstrap_class("Block1",      c->cls_block);
    c->cls_block2   = asom_bootstrap_class("Block2",      c->cls_block);
    c->cls_block3   = asom_bootstrap_class("Block3",      c->cls_block);
    c->cls_method   = asom_bootstrap_class("Method",      c->cls_object);
    c->cls_system   = asom_bootstrap_class("System",      c->cls_object);
    c->cls_random   = asom_bootstrap_class("Random",      c->cls_object);

    struct asom_object *nil_obj = calloc(1, sizeof(*nil_obj));
    nil_obj->klass = c->cls_nil;
    c->val_nil = ASOM_OBJ2VAL(nil_obj);

    struct asom_object *t_obj = calloc(1, sizeof(*t_obj));
    t_obj->klass = c->cls_true;
    c->val_true = ASOM_OBJ2VAL(t_obj);

    struct asom_object *f_obj = calloc(1, sizeof(*f_obj));
    f_obj->klass = c->cls_false;
    c->val_false = ASOM_OBJ2VAL(f_obj);

    asom_global_set(c, "nil",   c->val_nil);
    asom_global_set(c, "true",  c->val_true);
    asom_global_set(c, "false", c->val_false);

    // Give each bootstrap class its own metaclass so that class-side
    // methods loaded from .som files (e.g. `System class>>new` errors with
    // 'system object is singular') don't pollute every other bootstrap
    // class. The metaclass hierarchy mirrors the class hierarchy:
    //   <C class>'s superclass = <C's superclass> class
    //   Object class's superclass = Class
    //   Class class's class = Metaclass
    // matching the canonical Smalltalk metaclass diagram.
    struct asom_class *bootstrap_classes[] = {
        c->cls_object, c->cls_class, c->cls_metaclass,
        c->cls_nil, c->cls_boolean, c->cls_true, c->cls_false,
        c->cls_integer, c->cls_double, c->cls_string, c->cls_symbol,
        c->cls_array, c->cls_block, c->cls_block1, c->cls_block2, c->cls_block3,
        c->cls_method, c->cls_system, c->cls_random,
    };
    size_t n_bootstrap = sizeof(bootstrap_classes)/sizeof(bootstrap_classes[0]);
    for (size_t i = 0; i < n_bootstrap; i++) {
        struct asom_class *cls = bootstrap_classes[i];
        struct asom_class *meta = calloc(1, sizeof(*meta));
        char name_buf[128];
        snprintf(name_buf, sizeof(name_buf), "%s class", cls->name);
        meta->name = asom_intern_cstr(name_buf);
        meta->hdr.klass = c->cls_metaclass;
        meta->metaclass = c->cls_metaclass;
        cls->hdr.klass = meta;
        cls->metaclass = meta;
    }
    // Second pass: wire up meta->superclass so the chain mirrors the
    // class hierarchy.
    for (size_t i = 0; i < n_bootstrap; i++) {
        struct asom_class *cls = bootstrap_classes[i];
        cls->metaclass->superclass = cls->superclass ? cls->superclass->metaclass : c->cls_class;
    }

    asom_global_set(c, "Object",   ASOM_OBJ2VAL(c->cls_object));
    asom_global_set(c, "Class",    ASOM_OBJ2VAL(c->cls_class));
    asom_global_set(c, "Metaclass",ASOM_OBJ2VAL(c->cls_metaclass));
    asom_global_set(c, "Integer",  ASOM_OBJ2VAL(c->cls_integer));
    asom_global_set(c, "Double",   ASOM_OBJ2VAL(c->cls_double));
    asom_global_set(c, "String",   ASOM_OBJ2VAL(c->cls_string));
    asom_global_set(c, "Symbol",   ASOM_OBJ2VAL(c->cls_symbol));
    asom_global_set(c, "Array",    ASOM_OBJ2VAL(c->cls_array));
    asom_global_set(c, "Block",    ASOM_OBJ2VAL(c->cls_block));
    asom_global_set(c, "Block1",   ASOM_OBJ2VAL(c->cls_block1));
    asom_global_set(c, "Block2",   ASOM_OBJ2VAL(c->cls_block2));
    asom_global_set(c, "Block3",   ASOM_OBJ2VAL(c->cls_block3));
    asom_global_set(c, "Method",   ASOM_OBJ2VAL(c->cls_method));
    asom_global_set(c, "System",   ASOM_OBJ2VAL(c->cls_system));
    asom_global_set(c, "Random",   ASOM_OBJ2VAL(c->cls_random));
    asom_global_set(c, "Boolean",  ASOM_OBJ2VAL(c->cls_boolean));
    asom_global_set(c, "True",     ASOM_OBJ2VAL(c->cls_true));
    asom_global_set(c, "False",    ASOM_OBJ2VAL(c->cls_false));
    asom_global_set(c, "Nil",      ASOM_OBJ2VAL(c->cls_nil));

    // SOM convention: lowercase `system` is a singleton instance of System,
    // available everywhere as a global. The standard library reaches for it
    // from Object>>println etc.
    struct asom_object *sys_inst = calloc(1, sizeof(*sys_inst));
    sys_inst->klass = c->cls_system;
    asom_global_set(c, "system", ASOM_OBJ2VAL(sys_inst));

    c->serial = 1;
}
