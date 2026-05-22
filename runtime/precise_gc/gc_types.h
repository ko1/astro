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

#ifndef BARUBY_GC
#  define BARUBY_GC BARUBY_GC_COPY
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

/* Backends with a forwarding-pointer field in ASTroObjectHeader.  Cheney-
 * style backends could instead overlay fwd in payload — left as future
 * work (iter 75 Step C+).  For now every moving GC reserves the field. */
#if BARUBY_GC == BARUBY_GC_COPY             || \
    BARUBY_GC == BARUBY_GC_COPY_GEN         || \
    BARUBY_GC == BARUBY_GC_COPY_GEN_INC     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT     || \
    BARUBY_GC == BARUBY_GC_MARK_COMPACT_GEN || \
    BARUBY_GC == BARUBY_GC_MARK_BUMP_GEN    || \
    BARUBY_GC == BARUBY_GC_IMMIX_GEN
#  define ASTRO_GC_HAS_FWD 1
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
} AroGcCommonState;

#endif  /* ASTRO_PRECISE_GC_TYPES_H */
