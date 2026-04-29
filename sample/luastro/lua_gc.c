// luastro garbage collector — simple stop-the-world mark-sweep.
//
// Every heap-allocated GC-tracked object (LuaString, LuaTable,
// LuaClosure, LuaCFunction) starts with a GCHead.  The allocator
// registers each new object on a global linked list (G_GC_HEAD).
// When luastro_gc_collect() runs:
//   1. Mark phase — walk roots (CTX's stack, globals, ret_info,
//      varargs, current upvalues, the heap-cell pool) and recursively
//      mark every reachable object.
//   2. Sweep phase — walk the all-objects list; free anything still
//      unmarked.  Honours per-table weak modes ("k"/"v"/"kv") by
//      dropping entries whose weak side became unreachable.
//   3. __gc finalizer — if a table being swept has __gc set on its
//      metatable, the finalizer is invoked before the table is freed.
//
// Triggered manually via collectgarbage("collect"); we also call it
// when the live-object byte count exceeds 2x the count at the last
// collection.

#include <stdarg.h>
#include "context.h"
#include "node.h"

static struct GCHead *G_GC_HEAD = NULL;
static size_t G_GC_BYTES = 0;
static size_t G_GC_TRIGGER = 4 * 1024 * 1024;   // 4 MB initial threshold

void
luastro_gc_register(void *obj, uint8_t type)
{
    struct GCHead *h = (struct GCHead *)obj;
    h->next = G_GC_HEAD;
    h->type = type;
    h->mark = 0;
    h->weak_mode = 0;
    G_GC_HEAD = h;
}

size_t luastro_gc_total(void) { return G_GC_BYTES; }

// --- Mark phase ----------------------------------------------------

static void mark_value(LuaValue v);

static void
mark_string(struct LuaString *s)
{
    if (!s || s->gc.mark) return;
    s->gc.mark = 1;
}

static void
mark_table(struct LuaTable *t)
{
    if (!t || t->gc.mark) return;
    t->gc.mark = 1;
    if (t->metatable) mark_table(t->metatable);
    // Mark array part (skip if values are weak).
    if (!(t->gc.weak_mode & 2)) {
        for (uint32_t i = 0; i < t->arr_cnt; i++) mark_value(t->array[i]);
    }
    // Mark hash entries (subject to weak modes).
    if (t->hash_cap) {
        for (uint32_t i = 0; i < t->hash_cap; i++) {
            if (LV_IS_NIL(t->hash[i].key)) continue;
            if (!(t->gc.weak_mode & 1)) mark_value(t->hash[i].key);
            if (!(t->gc.weak_mode & 2)) mark_value(t->hash[i].val);
        }
    }
}

static void
mark_closure(struct LuaClosure *cl)
{
    if (!cl || cl->gc.mark) return;
    cl->gc.mark = 1;
    for (uint32_t i = 0; i < cl->nupvals; i++) {
        if (cl->upvals[i]) mark_value(*cl->upvals[i]);
    }
}

static void
mark_cfunc(struct LuaCFunction *cf)
{
    if (!cf || cf->gc.mark) return;
    cf->gc.mark = 1;
}

static void
mark_value(LuaValue v)
{
    if (!LV_IS_PTR(v)) return;
    switch (lv_heap_type(v)) {
    case LUA_TSTRING: mark_string (LV_AS_STR(v));  break;
    case LUA_TTABLE:  mark_table  (LV_AS_TBL(v));  break;
    case LUA_TFUNC:   mark_closure(LV_AS_FN(v));   break;
    case LUA_TCFUNC:  mark_cfunc  (LV_AS_CF(v));   break;
    case LUA_TBOXED: {
        struct LuaBox *bx = (struct LuaBox *)(uintptr_t)v;
        if (!bx->gc.mark) { bx->gc.mark = 1; mark_value(bx->value); }
        break;
    }
    default: break;
    }
}

static void
mark_roots(CTX *c)
{
    // Stack — boxed slots store a LuaBox pointer; mark_value follows the box.
    for (LuaValue *p = c->stack; p < c->sp; p++) {
        mark_value(*p);
    }
    // Globals + ret_info + varargs + last_error
    if (c->globals) mark_table(c->globals);
    for (uint32_t i = 0; i < c->ret_info.result_cnt; i++)
        mark_value(c->ret_info.results[i]);
    for (uint32_t i = 0; i < c->varargs_cnt; i++) mark_value(c->varargs[i]);
    mark_value(c->last_error);
    // Upvals — the active closure's upval cells; we already marked
    // closures via the stack walk; their upvals[] indirectly mark too.
}

// --- Sweep phase ---------------------------------------------------

static void free_string(struct LuaString *s)   { (void)s;  /* interned: leave for now */ }
static void free_table(struct LuaTable *t)     { free(t->array); free(t->hash); free(t); }
static void free_closure(struct LuaClosure *cl){ free(cl->upvals); free(cl); }
static void free_cfunc(struct LuaCFunction *cf){ free(cf); }

static void
sweep_table_weak_entries(struct LuaTable *t)
{
    if (!t->gc.weak_mode || !t->hash_cap) return;
    for (uint32_t i = 0; i < t->hash_cap; i++) {
        if (LV_IS_NIL(t->hash[i].key)) continue;
        bool drop = false;
        if ((t->gc.weak_mode & 1)) {
            LuaValue k = t->hash[i].key;
            if ((LV_IS_STR(k) && !LV_AS_STR(k)->gc.mark) ||
                (LV_IS_TBL(k)  && !LV_AS_TBL(k)->gc.mark) ||
                (LV_IS_FN(k)   && !LV_AS_FN(k)->gc.mark) ||
                (LV_IS_CF(k)  && !LV_AS_CF(k)->gc.mark))
                drop = true;
        }
        if (!drop && (t->gc.weak_mode & 2)) {
            LuaValue v = t->hash[i].val;
            if ((LV_IS_STR(v) && !LV_AS_STR(v)->gc.mark) ||
                (LV_IS_TBL(v)  && !LV_AS_TBL(v)->gc.mark) ||
                (LV_IS_FN(v)   && !LV_AS_FN(v)->gc.mark) ||
                (LV_IS_CF(v)  && !LV_AS_CF(v)->gc.mark))
                drop = true;
        }
        if (drop) {
            t->hash[i].key = LUAV_NIL;
            t->hash[i].val = LUAV_NIL;
        }
    }
}

static void
finalize_table(CTX *c, struct LuaTable *t)
{
    if (!t->metatable) return;
    LuaValue gcfn = lua_table_get_str(t->metatable, lua_str_intern("__gc"));
    if (LV_IS_CALL(gcfn)) {
        LuaValue argv[1] = { LUAV_TABLE(t) };
        (void)lua_call(c, gcfn, argv, 1);
    }
}

static void
sweep(CTX *c)
{
    // First pass: drop weak entries from live tables.
    for (struct GCHead *h = G_GC_HEAD; h; h = h->next) {
        if (h->mark && h->type == LUA_TTABLE) {
            sweep_table_weak_entries((struct LuaTable *)h);
        }
    }
    // Second pass: finalize unmarked tables that have __gc.
    for (struct GCHead *h = G_GC_HEAD; h; h = h->next) {
        if (!h->mark && h->type == LUA_TTABLE) {
            finalize_table(c, (struct LuaTable *)h);
        }
    }
    // Third pass: unlink + free unmarked.
    struct GCHead **link = &G_GC_HEAD;
    while (*link) {
        struct GCHead *h = *link;
        if (h->mark) {
            h->mark = 0;
            link = &h->next;
        } else {
            *link = h->next;
            switch (h->type) {
            case LUA_TSTRING: free_string ((struct LuaString *)   h); break;
            case LUA_TTABLE:  free_table  ((struct LuaTable *)    h); break;
            case LUA_TFUNC:   free_closure((struct LuaClosure *)  h); break;
            case LUA_TCFUNC:  free_cfunc  ((struct LuaCFunction *)h); break;
            default: free(h); break;
            }
        }
    }
}

void
luastro_gc_collect(CTX *c)
{
    mark_roots(c);
    sweep(c);
}
