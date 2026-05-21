#ifndef BARUBY_PRECISE_GC_H
#define BARUBY_PRECISE_GC_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Virtual-address reservation for region-based backends.
 *
 * Region-based backends (copy*, mark_compact*, mark_bump_gen, immix*) used
 * to declare a fixed REGION_BYTES / ARENA_BYTES / TENURED_BYTES upper
 * bound (typically 512 MiB - 1 GiB).  That bound was a program-limiting
 * fixed-length value — heap growth past it aborts as OOM even when
 * physical memory is plenty.
 *
 * We now reserve ARO_GC_REGION_VIRT_BYTES (= 64 GiB) of virtual address
 * space per region.  Physical pages commit only on first touch (Linux
 * overcommit + MAP_NORESERVE); collectors may MADV_DONTNEED freed
 * regions to release physical memory back to the OS.  64 GiB is far
 * larger than any practical baruby_precise program; the heap is
 * effectively unbounded.  Per-page / per-block / per-line sizes are
 * left fixed (they're tuning knobs, not program-limiting).
 * --------------------------------------------------------------------------- */
#define ARO_GC_REGION_VIRT_BYTES  ((size_t)64u << 30)   /* 64 GiB virtual */

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
//  12: immix             — block (32 KiB) / line (128 B) mark-region.
//                          Non-moving v1 (no evacuation).  Bump-allocate
//                          within holes (unmarked-line runs).
//  13: immix_gen         — generational variant: bump nursery (16 MiB) +
//                          Immix tenured (512 MiB).  Minor copy-promotes
//                          nursery survivors into tenured holes.  Major
//                          is regular Immix mark + line-mark sweep.
//  14: mark_bitmap_gen       — sticky mark&sweep with per-page bitmaps.  Same
//                          semantics as mark_gen but GCHeader = 8 B (no
//                          marked/old/dirty bytes — bits live in page
//                          bitmaps).  Young set found by walking pages
//                          (no young_next list).  BaArray (24 B payload)
//                          fits class-32 perfectly = 2× density vs mark_gen.
//
// Gen / inc variants define BARUBY_GC_HAS_WB so callers know they must use
// aro_gc_wb() instead of plain `*slot = v` for heap-pointer writes.
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
#define BARUBY_GC_IMMIX            12
#define BARUBY_GC_IMMIX_GEN        13
#define BARUBY_GC_MARK_BITMAP_GEN      14
#define BARUBY_GC_MARK_CARD_GEN        15
#define BARUBY_GC_MARK_FREELIST        16

#ifndef BARUBY_GC
#  define BARUBY_GC BARUBY_GC_COPY
#endif

// Backends that need a write barrier (gen / inc variants).  Callers must
// always go through aro_gc_wb / _bulk for heap-pointer writes — for
// non-WB backends it compiles to a plain `*slot = v`, free of cost.
#if BARUBY_GC == BARUBY_GC_MARK_GEN         || \
    BARUBY_GC == BARUBY_GC_MARK_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_COPY_GEN         || \
    BARUBY_GC == BARUBY_GC_COPY_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN    || \
    BARUBY_GC == BARUBY_GC_IMMIX_GEN        || \
    BARUBY_GC == BARUBY_GC_MARK_BITMAP_GEN  || \
    BARUBY_GC == BARUBY_GC_MARK_CARD_GEN
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
} AroGcKind;

#include <time.h>

typedef struct {
    size_t total_bytes;      // cumulative alloc bytes
    size_t heap_bytes;       // current live bytes (best-effort)
    size_t gc_count;         // total collections
    size_t minor_count;      // minor (= nursery) collections, gen backends
    size_t major_count;      // major (= whole heap) collections, gen backends
    double total_seconds;    // cumulative wall-clock seconds spent in collection
    double max_pause_seconds;// longest single GC pause (latency upper-bound)
    double mark_seconds;
    double reclaim_seconds;
} AroGcStats;

/* AroGcCommonState: 各 backend の `struct AstroGc` の **先頭 field** に
 * 置く約束の「共通ヘッダ」。 gc.h の helper / stat reader は
 * `(AroGcCommonState *)c->astro_gc` で取り出してアクセスするので、
 * backend の追加 field 内容は知らなくて済む (= AstroGc を opaque に
 * 保てる)。 stats / stress / re-entrancy timer は backend 横断で必要
 * だが per-instance な値なので、 ここにまとめている。 */
typedef struct AroGcCommonState {
    AroGcStats      stats;
    bool            stress;       /* BARUBY_GC_STRESS=1 → collect on every alloc */
    int             time_depth;   /* re-entrancy guard (major calling minor 等) */
    struct timespec time_t0;      /* outermost begin の wall-clock anchor */
} AroGcCommonState;

/* Accessors.  `c->astro_gc` is `struct AstroGc *` (forward decl in
 * context.h).  Cast to `AroGcCommonState *` is safe iff each backend's
 * AstroGc has `AroGcCommonState common` as its first field. */
#define ASTRO_GC_COMMON(c) ((AroGcCommonState *)((c)->astro_gc))

/* `aro_gc_backend_name` identifies which backend was compiled in (=
 * compile-time constant per binary).  It is not per-instance state, so
 * keeping it as `const char *` global is fine. */
extern const char *aro_gc_backend_name;

void  aro_gc_init(CTX *c);

/* aro_gc_alloc — allocate `payload_size` bytes of a pointer-scanned
 * object (KIND_OBJ_ARRAY / KIND_OBJ_STRING / KIND_PAYLOAD_VAL).
 *
 * **CONTRACT**: every backend MUST zero-initialize the returned payload
 * (`memset(payload, 0, ALIGN8(payload_size))` or equivalent) before
 * returning.  The GC mark phase walks these fields as VALUEs / pointers
 * via `scan_outgoing`; if any byte is stale heap-pointer-shaped data
 * from a recycled slot, mark will dereference it and SEGV.  Region-bump
 * backends get this for free (region is touched once, lazy zero from
 * the OS); freelist-recycling backends MUST emit an explicit memset.
 * iter 48 fix in gc_mark_freelist.c was due to this contract being
 * silently violated.
 *
 * `sp_top` is the GC-scan upper bound: roots are `c->env..sp_top`.
 * Callers must spill any heap pointers they hold into slots below
 * `sp_top` before calling alloc, or pass a higher sp_top so those slots
 * fall in the scan range.
 */
void *aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top);

/* aro_gc_alloc_byte — allocate `payload_size` raw bytes (no VALUE
 * scanning, so no zero-init required).  Used for BaString.bytes / other
 * char[] payloads.  Caller fills the bytes after return.  GC's
 * `scan_outgoing` skips KIND_PAYLOAD_BYTE so leftover freelist-link
 * bytes are harmless. */
void *aro_gc_alloc_byte(CTX *c, size_t payload_size, VALUE *sp_top);

/* aro_gc_realloc_payload — grow a payload, copying contents.  Preserves
 * the kind/scanability of the original payload. */
void *aro_gc_realloc_payload(CTX *c, void *p, size_t new_size, VALUE *sp_top);

void  aro_gc_collect(CTX *c, VALUE *sp_top);

/* Stat readers — all take CTX so the data is sourced from the per-instance
 * common state (no global variable). */
static inline size_t aro_gc_total_bytes      (CTX *c) { return ASTRO_GC_COMMON(c)->stats.total_bytes;       }
static inline size_t aro_gc_heap_bytes       (CTX *c) { return ASTRO_GC_COMMON(c)->stats.heap_bytes;        }
static inline size_t aro_gc_count            (CTX *c) { return ASTRO_GC_COMMON(c)->stats.gc_count;          }
static inline size_t aro_gc_minor_count      (CTX *c) { return ASTRO_GC_COMMON(c)->stats.minor_count;       }
static inline size_t aro_gc_major_count      (CTX *c) { return ASTRO_GC_COMMON(c)->stats.major_count;       }
static inline double aro_gc_total_seconds    (CTX *c) { return ASTRO_GC_COMMON(c)->stats.total_seconds;     }
static inline double aro_gc_max_pause_seconds(CTX *c) { return ASTRO_GC_COMMON(c)->stats.max_pause_seconds; }
static inline double aro_gc_mark_seconds     (CTX *c) { return ASTRO_GC_COMMON(c)->stats.mark_seconds;      }
static inline double aro_gc_reclaim_seconds  (CTX *c) { return ASTRO_GC_COMMON(c)->stats.reclaim_seconds;   }

// Helper used inside each backend's collect entry point — accumulates wall
// time into c->astro_gc_stats.total_seconds.  Re-entrant: if a major calls
// minor (gc_mark_compact_gen), only the outermost begin/end pair times
// the work; inner pairs are no-ops via the depth counter (also in CTX).
//
// All timer state (depth + start timestamp + stats fields) lives inside
// CTX, so backends can't accidentally rely on hidden global mutable
// state.  The `struct timespec` return is kept only for API compat —
// the real depth/start tracking happens through `c`.

static inline struct timespec
aro_gc_time_begin(CTX *c)
{
    AroGcCommonState *cs = ASTRO_GC_COMMON(c);
    struct timespec t = {0, 0};
    if (cs->time_depth++ == 0) {
        clock_gettime(CLOCK_MONOTONIC, &cs->time_t0);
    }
    return t;
}

static inline void
aro_gc_time_end(CTX *c, struct timespec t0)
{
    (void)t0;
    AroGcCommonState *cs = ASTRO_GC_COMMON(c);
    if (--cs->time_depth == 0) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (double)(t1.tv_sec  - cs->time_t0.tv_sec) +
                    (double)(t1.tv_nsec - cs->time_t0.tv_nsec) / 1e9;
        cs->stats.total_seconds += dt;
        if (dt > cs->stats.max_pause_seconds) {
            cs->stats.max_pause_seconds = dt;
        }
    }
}

// Phase-level timer: brackets a sub-phase (mark, sweep, slide, etc.) within
// a single collection and adds the elapsed time to *phase_field.  Unlike
// aro_gc_time_begin / _end this does NOT use a depth counter — phases are
// expected to be flat (caller wraps each non-overlapping section).
static inline struct timespec
aro_gc_phase_begin(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static inline void
aro_gc_phase_end(struct timespec t0, double *phase_field)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec  - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    *phase_field += dt;
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
void aro_gc_wb     (CTX *c, void *holder, VALUE *slot, VALUE v);
void aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n);
#else
static inline void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    (void)c; (void)holder;
    *slot = v;
}

static inline void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    (void)c; (void)holder;
    if (n) memcpy(dst, src, n * sizeof(VALUE));
}
#endif

#endif
