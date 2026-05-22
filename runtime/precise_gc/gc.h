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

/* Framework が GCHeader に詰める分類 = "category"。 sample-defined kind
 * (= OBJ_ARRAY / OBJ_STRING / OBJ_VALUE_ARRAY 等) は sample 側
 * (context.h) で別 enum、 framework は category だけ持つ。
 *
 * - SCAN  : sample の SCAN_EDGES が dispatch して edge を visit
 *           (sample は ObjectHeader.type で自身の object 種別を識別)
 * - BYTE  : scan skip (sample が即書き、 GC は touch しない)
 * - FREE  : backend 内部 sweep marker (= 公開 alloc API 無し)
 *
 * 旧 VALS category (= framework が直接 VALUE[] iterate) は廃止。
 * VALUE 配列も sample 側で ObjectHeader 付き payload にして SCAN
 * 経由で dispatch する (= e.g. baruby_precise の OBJ_VALUE_ARRAY)。
 *
 * 旧 AroGcKind enum (= KIND_OBJ_ARRAY 等を含む) も廃止。 sample kind を
 * framework が知る必要は無く、 framework は category のみで scan 戦略を決定。 */
typedef enum {
    ASTRO_GC_CAT_SCAN = 0,
    ASTRO_GC_CAT_BYTE = 1,
    ASTRO_GC_CAT_FREE = 2,
} AstroGcCategory;

/* Transitional alias: 旧 backend コードで `AroGcKind` typename と
 * `KIND_FREE` 値を使ってる箇所が残っている。 backend が category を
 * 2 bit 領域に詰めるのを想定して値も一致させる。 全 backend が
 * AstroGcCategory に移行したら削除する。 */
typedef AstroGcCategory AroGcKind;
#define KIND_FREE         ASTRO_GC_CAT_FREE
#define KIND_SCAN         ASTRO_GC_CAT_SCAN
#define KIND_BYTE         ASTRO_GC_CAT_BYTE
/* 旧 framework-internal kind の一時 alias。 iter 75 Step B 移行が
 * 完了したら backend を直接 CAT_* に書き換え、 削除。 sample-kind
 * (KIND_OBJ_ARRAY 等) は framework から見えないので alias しない。 */
#define KIND_PAYLOAD_BYTE ASTRO_GC_CAT_BYTE
#define KIND_PAYLOAD_VAL  ASTRO_GC_CAT_SCAN

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

/* AroGcCommonState: 各 backend の `struct ASTroGC` の **先頭 field** に
 * 置く約束の「共通ヘッダ」。 gc.h の helper / stat reader は
 * `(AroGcCommonState *)c->astro_gc` で取り出してアクセスするので、
 * backend の追加 field 内容は知らなくて済む (= ASTroGC を opaque に
 * 保てる)。 stats / stress / re-entrancy timer は backend 横断で必要
 * だが per-instance な値なので、 ここにまとめている。 */
typedef struct AroGcCommonState {
    AroGcStats      stats;
    bool            stress;       /* BARUBY_GC_STRESS=1 → collect on every alloc */
    int             time_depth;   /* re-entrancy guard (major calling minor 等) */
    struct timespec time_t0;      /* outermost begin の wall-clock anchor */
} AroGcCommonState;

/* Accessors.  `c->astro_gc` is `struct ASTroGC *` (forward decl in
 * context.h).  Cast to `AroGcCommonState *` is safe iff each backend's
 * ASTroGC has `AroGcCommonState common` as its first field. */
#define ASTRO_GC_COMMON(c) ((AroGcCommonState *)((c)->astro_gc))

/* `aro_gc_backend_name` identifies which backend was compiled in (=
 * compile-time constant per binary).  It is not per-instance state, so
 * keeping it as `const char *` global is fine. */
extern const char *aro_gc_backend_name;

void  aro_gc_init(CTX *c);

/* aro_gc_fini — tear down the per-instance ASTroGC: release backend
 * resources (mmap'd regions, free-lists, mark bitmaps, etc.) and free
 * the heap-allocated ASTroGC struct itself.  Sets `c->astro_gc` to
 * NULL.  Safe to call on an already-finalized instance (no-op).
 *
 * On process exit the OS reclaims everything anyway, but a symmetric
 * fini matters for: (a) multi-instance use (creating + destroying
 * multiple GCs in one process), (b) valgrind / leak-sanitizer clean
 * runs, (c) future tests that re-init mid-process. */
void  aro_gc_fini(CTX *c);

/* Helpers for aro_gc_fini: release a backend's `large_head` chain.
 *
 * mmap-backed (LargeObj layout: { LargeObj *next; size_t map_bytes; ... }
 *              used by gc_mark / gc_mark_freelist / gc_immix / gc_mark_gen
 *              / gc_mark_gen_inc / gc_mark_bitmap_gen / gc_mark_card_gen /
 *              gc_immix_gen): munmap each entry.
 *
 * malloc-backed (LargeObj layout: { LargeObj *next; ... } used by
 *                gc_copy / gc_mark_compact): free each entry.
 *
 * Backends cast their `LargeObj *` to (void *)gc->large_head.  Both
 * helpers assume `next` is the first field — true for all our backends. */
struct AroGcLargeChainMmap   { struct AroGcLargeChainMmap   *next; size_t map_bytes; };
struct AroGcLargeChainMalloc { struct AroGcLargeChainMalloc *next; };

#include <sys/mman.h>
#include <stdlib.h>

static inline void
aro_gc_free_large_chain_mmap(void *head)
{
    struct AroGcLargeChainMmap *lo = (struct AroGcLargeChainMmap *)head;
    while (lo) {
        struct AroGcLargeChainMmap *next = lo->next;
        munmap(lo, lo->map_bytes);
        lo = next;
    }
}

static inline void
aro_gc_free_large_chain_malloc(void *head)
{
    struct AroGcLargeChainMalloc *lo = (struct AroGcLargeChainMalloc *)head;
    while (lo) {
        struct AroGcLargeChainMalloc *next = lo->next;
        free(lo);
        lo = next;
    }
}

/* aro_gc_alloc — allocate `payload_size` bytes for a sample-defined
 * scan-safe object.  Category = SCAN (= sample's SCAN_EDGES dispatches
 * at scan time, typically via sample's own ObjectHeader.type).
 *
 * **CONTRACT 1 (zero-init)**: backend zero-inits the payload so a GC
 * scan immediately after alloc sees no stale heap-pointer bits.
 *
 * **CONTRACT 2 (GC-scan bound)**: caller MUST have set `c->sp` to its
 * current spill top before calling.  `c->env..c->sp` defines the root
 * scan range during any inner GC trigger. */
void *aro_gc_alloc(CTX *c, size_t payload_size);

/* aro_gc_alloc_byte — allocate `payload_size` raw bytes (no VALUE
 * scanning, no zero-init).  Used for BaString.bytes / other char[]
 * payloads.  Caller fills the bytes after return.  GC's heap walk
 * skips this category, so leftover freelist-link bytes are harmless.
 * Same c->sp contract as aro_gc_alloc. */
void *aro_gc_alloc_byte(CTX *c, size_t payload_size);

/* aro_gc_realloc_payload — grow a payload, copying contents.  Preserves
 * the kind/scanability of the original payload.  Same c->sp contract;
 * internally bumps c->sp by 1 to park the old payload during the
 * inner alloc (so it stays scanned through any GC).
 *
 * Default impl lives in gc_common.c and calls back into the backend
 * via aro_gc_size_of (declared below) for the memcpy length.  Backends
 * with no per-object header (gc_none) override aro_gc_realloc_payload
 * themselves and don't implement the accessor. */
void *aro_gc_realloc_payload(CTX *c, void *p, size_t new_size);
/* aro_gc_realloc_byte_payload — same shape but for byte payloads (=
 * caller fills the new bytes, framework skips zero-init growth).
 * Pair-wise with aro_gc_alloc_byte.  Caller chooses based on what
 * they alloc'd; framework does not inspect kind. */
void *aro_gc_realloc_byte_payload(CTX *c, void *p, size_t new_size);

/* Backend-provided header accessor — given a payload pointer (the
 * value returned by aro_gc_alloc / aro_gc_alloc_byte), return the
 * stored size.  Used by the shared aro_gc_realloc_payload default in
 * gc_common.c to compute copy_bytes. */
size_t    aro_gc_size_of(void *payload);

/* Optional backend hook: try to grow `old` in place (no alloc + memcpy).
 * Returns the new payload pointer on success; the underlying buffer may
 * have moved (e.g., via realloc(3) → mremap), in which case the caller
 * must use the returned pointer.  Returns NULL to fall through to the
 * default alloc + memcpy path.
 *
 * Default impl in gc_common.c is `__attribute__((weak))` and returns
 * NULL.  Backends that own large objects on a malloc-backed list (e.g.,
 * gc_copy / gc_mark_compact via LargeObj) override this to call
 * realloc(3) on the matching LargeObj for cheap doublings of
 * BaArray.items / BaString.bytes during sieve / hash_chain workloads.
 *
 * NOT called from stress mode (we want every alloc to GC). */
void *aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size);

void  aro_gc_collect(CTX *c);

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
