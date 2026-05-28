#ifndef ASTRO_PRECISE_GC_TYPES_H
#define ASTRO_PRECISE_GC_TYPES_H 1

/* gc_types.h — types-only header for precise_gc.  Split out from gc.h
 * so sample headers (e.g. baruby_precise/context.h) can include it to
 * get `AroObjectHeader` BEFORE defining `CTX_struct`, without dragging
 * in gc.h's CTX-dependent static inlines.
 *
 * Layering:
 *   gc_types.h     — types (this file).  No CTX-dependent code.
 *   context.h      — defines CTX_struct, includes gc_types.h at top.
 *   gc.h           — wraps gc_types.h + adds CTX-dependent extern / static
 *                    inline declarations (= stats readers, aro_gc_store fall-
 *                    backs, etc.).  Sample .c files include gc.h.
 *   gc_*.c (backends) — include context.h (= CTX complete), then gc.h. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Backend selection.  Must be defined by build system (-DBARUBY_GC=<n>). */
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
#define BARUBY_GC_MARK_BITMAP_GEN  14
#define BARUBY_GC_MARK_CARD_GEN    15
#define BARUBY_GC_MARK_FREELIST    16
/* BARUBY_GC_COPY_SCRAMBLE (= ID 17) was removed when scramble was
 * superseded by the 64 GiB round-robin PURGE in gc_copy (= mprotect
 * PROT_NONE on retired planes catches stale ptrs deterministically with
 * cheaper per-GC cost than per-cycle XOR + R rotation).  Skip the ID
 * rather than reuse it so existing baked code stores stay valid. */

#ifndef BARUBY_GC
#  define BARUBY_GC BARUBY_GC_COPY
#endif

/* Backends that need a write barrier (gen / inc variants).  Callers
 * always go through aro_gc_store / _bulk for heap-pointer writes — for
 * non-WB backends it compiles to a plain `*slot = v`, free of cost. */
#if BARUBY_GC == BARUBY_GC_MARK_GEN         || \
    BARUBY_GC == BARUBY_GC_MARK_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_COPY_GEN         || \
    BARUBY_GC == BARUBY_GC_COPY_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN    || \
    BARUBY_GC == BARUBY_GC_IMMIX_GEN        || \
    BARUBY_GC == BARUBY_GC_MARK_BITMAP_GEN  || \
    BARUBY_GC == BARUBY_GC_MARK_CARD_GEN
#  define ARO_GC_HAS_WB 1
#endif

/* Backends with a dedicated forwarding-pointer field in AroObjectHeader.
 * Only mark_compact-style (slide compactor) backends require it — phase 3
 * (pointer update) reads sample data AND fwd concurrently, so fwd cannot
 * overlap with sample payload.  Cheney-style backends (copy*, mark_bump_gen,
 * immix_gen) use payload-overlay forwarding instead (= 8 B header savings
 * per object). */
#if BARUBY_GC == BARUBY_GC_MARK_COMPACT     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN
#  define ARO_GC_HAS_FWD 1
#endif

/* WB fast-path bit mask (= bit-in-head backends).  gc.h's inline
 * `aro_gc_store` checks `(gc_flags & MASK) == OLD_ONLY` to decide whether
 * the slow path (= aro_gc_remember) is needed.  Each gen backend that
 * keeps OLD/DIRTY bits directly in head.gc_flags exposes its layout
 * here so the WB hot path inlines without a function call.
 *
 * Backends without this define (mark_bitmap_gen, mark_card_gen,
 * mark_gen_inc) keep the entire aro_gc_store as an extern function (=
 * page-bitmap lookup or SATB barrier doesn't fold cleanly into a single
 * bit test). */
#if BARUBY_GC == BARUBY_GC_MARK_GEN         || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN
   /* head.gc_flags: MARKED=0x0001, OLD=0x0002, DIRTY=0x0004 */
#  define ARO_GC_WB_OLD_MASK   ((uint16_t)0x0002u)
#  define ARO_GC_WB_DIRTY_MASK ((uint16_t)0x0004u)
#elif BARUBY_GC == BARUBY_GC_COPY_GEN || \
      BARUBY_GC == BARUBY_GC_COPY_GEN_INC
   /* head.gc_flags: OLD=0x0001, DIRTY=0x0002 */
#  define ARO_GC_WB_OLD_MASK   ((uint16_t)0x0001u)
#  define ARO_GC_WB_DIRTY_MASK ((uint16_t)0x0002u)
#elif BARUBY_GC == BARUBY_GC_IMMIX_GEN
   /* head.gc_flags: epoch low 8 bits, OLD=0x0100, DIRTY=0x0200 */
#  define ARO_GC_WB_OLD_MASK   ((uint16_t)0x0100u)
#  define ARO_GC_WB_DIRTY_MASK ((uint16_t)0x0200u)
#endif

/* ARO_GC_EDGE — qualifier for fields that hold a heap pointer scanned by
 * SCAN_EDGES (= outgoing graph edges).  Sample marks such fields and
 * sample-owned VALUE flex arrays with this qualifier:
 *
 *     typedef struct BaArray {
 *         AroObjectHeader  head;
 *         uint32_t         len, capa;
 *         BaArrayItems    *ARO_GC_EDGE items;   // GC edge field
 *     } BaArray;
 *     typedef struct BaArrayItems {
 *         AroObjectHeader head;
 *         VALUE ARO_GC_EDGE data[];             // each slot is a GC edge
 *     } BaArrayItems;
 *
 * Placement is POST-type (= `T *ARO_GC_EDGE field`, not `ARO_GC_EDGE T *field`)
 * so it qualifies the SLOT, not the pointee.  Otherwise const would prevent
 * mutation of the referenced object rather than enforce write-barrier usage.
 *
 * Audit build (`-DARO_GC_WB_AUDIT`) expands to `const`, turning direct
 * assignment (`a->items = x;`) into a compile error.  ARO_STORE /
 * ARO_STORE_BULK cast the const away inside gc.h so the WB-managed path
 * remains writable.  Release build expands to nothing — no const, so the
 * optimizer is free to model writes through ARO_STORE without risking CSE
 * across opaque WB calls. */
#ifdef ARO_GC_WB_AUDIT
#  define ARO_GC_EDGE const
#else
#  define ARO_GC_EDGE
#endif

/* AroObjectHeader — every GC-managed object's first member.  Lives at
 * payload offset 0 (= NOT a separate prefix before payload).  Sample's
 * structs must place `AroObjectHeader head` as the first field; the
 * framework writes `gc_*` fields, sample owns `flags`.
 *
 * Layout:
 *   flags    (16b) — sample-controlled.  Holds sample's type tag and
 *                    sample-specific bits.  Framework never reads/writes.
 *   gc_flags (16b) — framework-controlled.  Per-backend layout (mark
 *                    bit, old-gen, dirty-card, mark_epoch, etc.).
 *   gc_size  (32b) — framework: total payload size in bytes (= sizeof of
 *                    the containing sample struct).  Stored at alloc time.
 *   gc_fwd   (64b) — moving GCs only.  NULL = live; non-NULL = forwarded.
 *
 * Size: 8 B (non-moving) / 16 B (moving). */
typedef struct AroObjectHeader {
    uint16_t flags;
    uint16_t gc_flags;
    uint32_t gc_size;
#ifdef ARO_GC_HAS_FWD
    void    *gc_fwd;
#endif
} AroObjectHeader;

typedef struct {
    size_t total_bytes;
    size_t heap_bytes;
    size_t gc_count;
    size_t minor_count;
    size_t major_count;
    double total_seconds;
    double max_pause_seconds;
    double mark_seconds;
    double reclaim_seconds;
} AroGcStats;

typedef struct AroGcCommonState {
    AroGcStats      stats;
    /* stress: GC trigger point ごとに必ず GC を発火 (= threshold を 0 化、
     *  reuse from-space, alloc-per-collect cost を testing で露呈)。
     * purge:  Cheney 系 moving backend で from-space を munmap、 to-space を
     *  fresh mmap (= stale heap pointer の deref を即 SEGV で検出)。 GC
     *  trigger 頻度には影響しない。
     *
     * 通常組合せ:
     *   stress         = 高頻度 GC、 ただし from-space 再利用 (= cheap audit)
     *   purge          = 普通の GC 頻度、 ただし stale ptr は SEGV 確実
     *   stress + purge = 高頻度 GC + munmap (= 最強 audit、 旧 STRESS 相当) */
    bool            stress;
    bool            purge;
    /* stress_interval / stress_count: when set via BARUBY_GC_STRESS=N
     * (N > 1), force GC every N allocs instead of every alloc.  Lets
     * larger workloads finish under STRESS+PURGE without timing out
     * while still surfacing stale-ptr bugs. */
    uint64_t        stress_interval;
    uint64_t        stress_count;
    int             time_depth;
    struct timespec time_t0;
    /* Finalizer list — libc-malloc'd dynamic array of payload pointers.
     * Weak references: framework does NOT visit these in SCAN_EDGES /
     * VISIT_ROOTS, so they don't keep objects alive.  After mark/forward
     * but before sweep/swap, each backend calls aro_gc_finalize_walk(c)
     * which iterates the list:
     *   - For each payload still alive (= live after mark/forward), update
     *     the entry to the new addr (= moving GCs forward the entry).
     *   - For each payload that became unreachable, invoke the sample-
     *     provided AROH_FINALIZE macro (typical use: mpz_clear,
     *     close FILE *, etc.) and drop the entry.
     *
     * Cost is O(finalizable_count) per GC, not O(heap) — preserves the
     * "GC time is O(live)" property of copying collectors. */
    void          **finalize_list;
    size_t          finalize_count;
    /* External memory pressure (e.g., GMP libc-malloc'd buffers owned via
     * a finalizer).  Updated by `aro_gc_account_external`; the framework
     * forces GC when this crosses a threshold so finalizers run promptly.
     * Counted in BYTES (not slots). */
    size_t          external_bytes;
    size_t          finalize_cap;
} AroGcCommonState;

#endif  /* ASTRO_PRECISE_GC_TYPES_H */
