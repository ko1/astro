#ifndef BARUBY_PRECISE_GC_H
#define BARUBY_PRECISE_GC_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward decls (defined in context.h)
struct CTX_struct;
typedef struct CTX_struct CTX;
typedef intptr_t VALUE;

// ----------------------------------------------------------------------------
// Precise mark&sweep GC.
//
// Design:
//   - Single contiguous VALUE stack (baruby_stack_base..baruby_stack_end).
//     c->fp / c->sp index into this stack.
//   - Every heap object is malloc'd individually and linked into a global
//     `all_objects` doubly-linked list via an embedded GC node (one per
//     object, allocated alongside the payload).
//   - Mark: walk c->fp_base..c->sp scanning VALUE roots, plus the function
//     table (function bodies reference AST = immortal, no VALUE roots there),
//     trace through BaArray.items / BaString fields.
//   - Sweep: walk all_objects, free unmarked.
//
// This is intentionally simple: no freelist, no moving, no generational.
// Goal is comparable correctness against the conservative-libgc baruby, so
// the perf delta exposed is the precise-rooting overhead alone (extra
// memory writes for sp[] spills) on top of plain malloc/free.
// ----------------------------------------------------------------------------

// Per-object GC header. Sits in front of each heap object payload so
// `(BarubyGCNode *)payload - 1` recovers it.
typedef struct BarubyGCNode {
    struct BarubyGCNode *prev;
    struct BarubyGCNode *next;
    uint32_t marked;       // 0 = unmarked, 1 = marked (reset on mark phase start)
    uint32_t payload_kind; // OBJ_ARRAY / OBJ_STRING / ... mirrors hdr.type
    // Followed by the actual object payload (BaArray / BaString).
} BarubyGCNode;

// Auxiliary record for separately-allocated `items` / `bytes` payloads
// (BaArray.items, BaString.bytes).  Tracked so sweep can free them too —
// they aren't reachable as a VALUE, only via their owning object.  We
// piggyback on the same list with a "payload_kind=0" tag.
//
// In this minimal version we just use plain `malloc` for these auxiliary
// arrays and rely on the owning object's sweep to free them (see
// gc_finalize_object).  No separate GC node for the items array.

// Stats (matches baruby's libgc counters).
typedef struct {
    size_t total_bytes;    // cumulative bytes allocated (never decreases)
    size_t heap_bytes;     // currently allocated bytes
    size_t gc_count;       // collection count
} BarubyGCStats;

extern BarubyGCStats baruby_gc_stats;

// Initialize the GC: allocate the VALUE stack, register the CTX.
// Must be called once before any allocation.
void baruby_gc_init(CTX *c);

// Allocate `payload_size` bytes for a payload of kind `kind`.  Returns a
// pointer to the payload (= immediately after the BarubyGCNode header).
// May trigger a collection if a threshold is exceeded.
//
// `sp_top` is the caller's current scratch top.  Before potentially
// triggering GC, the allocator updates c->sp = sp_top so the mark phase
// scans the correct root range.
void *baruby_gc_alloc(uint32_t kind, size_t payload_size, VALUE *sp_top);

// Allocate a "payload buffer" (BaArray.items / BaString.bytes).  These
// are tracked separately because they're not VALUE roots in their own
// right — the GC only visits them via their owning object.  In this
// minimal MVP they're plain malloc; their owning object's finalize frees
// them.
void *baruby_gc_alloc_payload(size_t size, VALUE *sp_top);
void *baruby_gc_realloc_payload(void *p, size_t new_size, VALUE *sp_top);

// Force a collection now.  Normal allocation triggers this internally
// when the heap grows past a threshold; this is the explicit entry point.
void baruby_gc_collect(VALUE *sp_top);

// Stats helpers (for `__GC_STATS__` print in main.c).
size_t baruby_gc_total_bytes(void);
size_t baruby_gc_heap_bytes(void);
size_t baruby_gc_count(void);

#endif
