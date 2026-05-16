#ifndef BARUBY_PRECISE_GC_H
#define BARUBY_PRECISE_GC_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Forward decls (defined in context.h)
struct CTX_struct;
typedef struct CTX_struct CTX;
typedef intptr_t VALUE;

// ---------------------------------------------------------------------------
// Pluggable GC backend interface.
//
// One of eleven backends is selected at build time via -DBARUBY_GC=<n>:
//   1: none              — no GC, malloc + leak (baseline)
//   2: mark              — non-moving mark&sweep (per-object malloc list)
//   3: mark_gen          — mark&sweep + 2-gen
//   4: mark_gen_inc      — mark&sweep + 2-gen + incremental marking
//   5: copy              — Cheney semi-space (default)
//   6: copy_gen          — copying nursery + tenured (semispace tenured)
//   7: copy_gen_inc      — generational + incremental copy
//   8: mark_compact      — single-region mark + Lisp-2 sliding compactor
//   9: mark_compact_gen  — nursery (copy) + tenured (mark + Lisp-2 compact)
//  10: bump              — bump-only, no GC (strictly faster `none`)
//  11: mark_bump_gen     — bump nursery + linked-list mark&sweep tenured
//                          (isolates nursery alloc strategy vs mark_gen)
//
// Gen / inc variants define BARUBY_GC_HAS_WB so callers know they must use
// baruby_gc_wb() instead of plain `*slot = v` for heap-pointer writes.
// ---------------------------------------------------------------------------

#define BARUBY_GC_NONE             1
#define BARUBY_GC_MARK             2
#define BARUBY_GC_MARK_GEN         3
#define BARUBY_GC_MARK_GEN_INC     4
#define BARUBY_GC_COPY             5
#define BARUBY_GC_COPY_GEN         6
#define BARUBY_GC_COPY_GEN_INC     7
#define BARUBY_GC_MARK_COMPACT     8
#define BARUBY_GC_MARK_COMPACT_GEN 9
#define BARUBY_GC_BUMP             10
#define BARUBY_GC_MARK_BUMP_GEN    11

#ifndef BARUBY_GC
#  define BARUBY_GC BARUBY_GC_COPY
#endif

// Backends that need a write barrier (gen / inc variants).  Callers must
// always go through baruby_gc_wb / _bulk for heap-pointer writes — for
// non-WB backends it compiles to a plain `*slot = v`, free of cost.
#if BARUBY_GC == BARUBY_GC_MARK_GEN         || \
    BARUBY_GC == BARUBY_GC_MARK_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_COPY_GEN         || \
    BARUBY_GC == BARUBY_GC_COPY_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN
#  define BARUBY_GC_HAS_WB 1
#endif

// Kind tag in GCHeader (each backend has its own header layout but the kind
// values are shared so node.c can pick the right alloc variant).
typedef enum {
    KIND_FREE         = 0,
    KIND_OBJ_ARRAY    = 1,   // BaArray header (items field is a pointer)
    KIND_OBJ_STRING   = 2,   // BaString header (bytes field is a pointer)
    KIND_PAYLOAD_VAL  = 3,   // VALUE[] (BaArray.items target)
    KIND_PAYLOAD_BYTE = 4,   // char[]  (BaString.bytes target)
} BarubyGCKind;

typedef struct {
    size_t total_bytes;      // cumulative alloc bytes
    size_t heap_bytes;       // current live bytes (best-effort)
    size_t gc_count;         // total collections
    size_t minor_count;      // minor (= nursery) collections, gen backends
    size_t major_count;      // major (= whole heap) collections, gen backends
    double total_seconds;    // cumulative wall-clock seconds spent in collection
} BarubyGCStats;

extern BarubyGCStats baruby_gc_stats;
extern int  baruby_gc_stress;
extern const char *baruby_gc_backend_name;

void  baruby_gc_init(CTX *c);
void *baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top);
void *baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top);
void *baruby_gc_realloc_payload(void *p, size_t new_size, VALUE *sp_top);
void  baruby_gc_collect(VALUE *sp_top);

size_t baruby_gc_total_bytes(void);
size_t baruby_gc_heap_bytes(void);
size_t baruby_gc_count(void);
size_t baruby_gc_minor_count(void);
size_t baruby_gc_major_count(void);
double baruby_gc_total_seconds(void);

// Helper used inside each backend's collect entry point — accumulates wall
// time into baruby_gc_stats.total_seconds.  Re-entrant: if a major calls
// minor (gc_mark_compact_gen), only the outermost begin/end pair times
// the work; inner pairs are no-ops via gc_time_depth.
#include <time.h>

extern int baruby_gc_time_depth;
extern struct timespec baruby_gc_time_t0;

static inline struct timespec
baruby_gc_time_begin(void)
{
    struct timespec t = {0, 0};
    if (baruby_gc_time_depth++ == 0) {
        clock_gettime(CLOCK_MONOTONIC, &baruby_gc_time_t0);
    }
    return t;   // unused — kept for API compat
}

static inline void
baruby_gc_time_end(struct timespec t0)
{
    (void)t0;
    if (--baruby_gc_time_depth == 0) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (double)(t1.tv_sec  - baruby_gc_time_t0.tv_sec) +
                    (double)(t1.tv_nsec - baruby_gc_time_t0.tv_nsec) / 1e9;
        baruby_gc_stats.total_seconds += dt;
    }
}

// Write barrier.
//
// `holder` is the heap object (payload pointer) that contains `slot`.  For
// item-array writes (`a->items[i] = v`) holder is the items payload, not
// the BaArray.  For pointer-field writes (`a->items = new_payload`) holder
// is the BaArray.  Use NULL for stack-root writes (= no barrier needed).
//
// For non-gen backends WB collapses to `*slot = v`; gen / inc backends
// override with a real implementation that maintains a remembered set or
// dirty-card bitmap.
#ifdef BARUBY_GC_HAS_WB
void baruby_gc_wb(void *holder, VALUE *slot, VALUE v);
void baruby_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n);
#else
static inline void
baruby_gc_wb(void *holder, VALUE *slot, VALUE v)
{
    (void)holder;
    *slot = v;
}

static inline void
baruby_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    (void)holder;
    if (n) memcpy(dst, src, n * sizeof(VALUE));
}
#endif

#endif
