#ifndef ASTRO_PRECISE_GC_TYPES_H
#define ASTRO_PRECISE_GC_TYPES_H 1

/* gc_types.h — types-only header for precise_gc.  Split out from gc.h
 * so sample headers (e.g. baruby_precise/context.h) can include it to
 * get `ASTroObjectHeader` BEFORE defining `CTX_struct`, without dragging
 * in gc.h's CTX-dependent static inlines.
 *
 * Layering:
 *   gc_types.h     — types (this file).  No CTX-dependent code.
 *   context.h      — defines CTX_struct, includes gc_types.h at top.
 *   gc.h           — wraps gc_types.h + adds CTX-dependent extern / static
 *                    inline declarations (= stats readers, aro_gc_wb fall-
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
#define BARUBY_GC_COPY_SCRAMBLE    17

#ifndef BARUBY_GC
#  define BARUBY_GC BARUBY_GC_COPY
#endif

/* Backends that scramble heap-pointer storage (= per-cycle XOR mask R).
 * Sample-visible storage of any heap pointer holds `raw_ptr ^ R`; sample
 * decodes via ARO_OBJ macro at deref.  R rotates each GC, so stale slots
 * (= GC mark/move 漏れ) decode to garbage and SEGV at next deref.  This is
 * a debug/audit backend to replace BARUBY_GC_STRESS for catching root /
 * SCAN_EDGES misses with less overhead than full GC-per-alloc. */
#if BARUBY_GC == BARUBY_GC_COPY_SCRAMBLE
#  define BARUBY_GC_HAS_SCRAMBLE 1
#endif

/* Backends that need a write barrier (gen / inc variants).  Callers
 * always go through aro_gc_wb / _bulk for heap-pointer writes — for
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
#  define BARUBY_GC_HAS_WB 1
#endif

/* Backends with a dedicated forwarding-pointer field in ASTroObjectHeader.
 * Only mark_compact-style (slide compactor) backends require it — phase 3
 * (pointer update) reads sample data AND fwd concurrently, so fwd cannot
 * overlap with sample payload.  Cheney-style backends (copy*, mark_bump_gen,
 * immix_gen) use payload-overlay forwarding instead (= 8 B header savings
 * per object). */
#if BARUBY_GC == BARUBY_GC_MARK_COMPACT     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN
#  define ASTRO_GC_HAS_FWD 1
#endif

/* WB fast-path bit mask (= bit-in-head backends).  gc.h's inline
 * `aro_gc_wb` checks `(gc_flags & MASK) == OLD_ONLY` to decide whether
 * the slow path (= aro_gc_wb_slow) is needed.  Each gen backend that
 * keeps OLD/DIRTY bits directly in head.gc_flags exposes its layout
 * here so the WB hot path inlines without a function call.
 *
 * Backends without this define (mark_bitmap_gen, mark_card_gen,
 * mark_gen_inc) keep the entire aro_gc_wb as an extern function (=
 * page-bitmap lookup or SATB barrier doesn't fold cleanly into a single
 * bit test). */
#if BARUBY_GC == BARUBY_GC_MARK_GEN         || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN
   /* head.gc_flags: MARKED=0x0001, OLD=0x0002, DIRTY=0x0004 */
#  define ASTRO_GC_WB_OLD_MASK   ((uint16_t)0x0002u)
#  define ASTRO_GC_WB_DIRTY_MASK ((uint16_t)0x0004u)
#elif BARUBY_GC == BARUBY_GC_COPY_GEN || \
      BARUBY_GC == BARUBY_GC_COPY_GEN_INC
   /* head.gc_flags: OLD=0x0001, DIRTY=0x0002 */
#  define ASTRO_GC_WB_OLD_MASK   ((uint16_t)0x0001u)
#  define ASTRO_GC_WB_DIRTY_MASK ((uint16_t)0x0002u)
#elif BARUBY_GC == BARUBY_GC_IMMIX_GEN
   /* head.gc_flags: epoch low 8 bits, OLD=0x0100, DIRTY=0x0200 */
#  define ASTRO_GC_WB_OLD_MASK   ((uint16_t)0x0100u)
#  define ASTRO_GC_WB_DIRTY_MASK ((uint16_t)0x0200u)
#endif

/* ASTroObjectHeader — every GC-managed object's first member.  Lives at
 * payload offset 0 (= NOT a separate prefix before payload).  Sample's
 * structs must place `ASTroObjectHeader head` as the first field; the
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
typedef struct ASTroObjectHeader {
    uint16_t flags;
    uint16_t gc_flags;
    uint32_t gc_size;
#ifdef ASTRO_GC_HAS_FWD
    void    *gc_fwd;
#endif
} ASTroObjectHeader;

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
    bool            stress;
    int             time_depth;
    struct timespec time_t0;
#ifdef BARUBY_GC_HAS_SCRAMBLE
    /* Per-cycle XOR mask used by ARO_OBJ / ARO_VAL.  Low 3 bits must be 0
     * to preserve 8-byte heap pointer alignment AND the LSB tag of fixnums
     * (= bit 0).  scramble_R is the CURRENT (sample-visible) R.
     * scramble_R_old is the PREVIOUS R, set only during a GC cycle so the
     * VALUE-slot forwarding wrapper can decode incoming edges with the
     * pre-GC encoding before re-encoding outgoing edges with the new R.
     * Outside GC, scramble_R_old is unused (= sample never reads it). */
    uintptr_t       scramble_R;
    uintptr_t       scramble_R_old;
#endif
} AroGcCommonState;

#endif  /* ASTRO_PRECISE_GC_TYPES_H */
