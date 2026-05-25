// gc_mark_compact_gen.c — backend #9: generational hybrid with N-survive.
//
// Layout:
//   - Young: two semi-spaces (YOUNG_BYTES each).  Active half receives new
//     allocations; on minor, survivors copy to inactive half (age++ <
//     PROMOTE_AGE) or to tenured (age >= PROMOTE_AGE).
//   - Tenured: single mmap'd region (64 GiB virt).  Survivors are appended
//     on minor; major mark + Lisp-2 sliding compactor reclaims in place.
//
// Promotion: on PROMOTE_AGE-th (= 3) survival.  Age in 2 bits of gc_flags.
//
// Minor GC: N-survive Cheney copy young → young-to OR tenured.  Promoted
//   objs with young refs go to remset (= GC-internal WB).
// Major GC: force-promote all young to tenured (= ignore age), then mark
//   + Lisp-2 slide compact tenured.  Same algorithm as gc_mark_compact.c.
//
// Write barrier:
//   - User write of young VALUE into tenured: aro_gc_wb pushes holder to remset.
//   - GC promote: Cheney scan in minor detects tenured→young edges, pushes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define YOUNG_BYTES    ((size_t)16u << 20)        /* 16 MiB per young half */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virt */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C: framework GCHeader 廃止、 AroObjectHeader at offset 0. */
_Static_assert(sizeof(AroObjectHeader) == 16, "moving GC: head must be 16 B");

/* gc_flags layout:
 *   bit 0  : MARKED
 *   bit 1  : OLD
 *   bit 2  : DIRTY
 *   bits 3-4: AGE (= 0..3; promote at PROMOTE_AGE)
 *
 * WB mask exposed to gc.h is OLD+DIRTY (= bits 1-2), unaffected by AGE. */
#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_OLD_BIT      (uint16_t)0x0002u
#define HDR_DIRTY_BIT    (uint16_t)0x0004u
#define HDR_AGE_SHIFT    3
#define HDR_AGE_MASK     ((uint16_t)0x0018u)
#define PROMOTE_AGE      3u
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->gc_flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->gc_flags &= (uint16_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)
#define HDR_GET_AGE(h)     (uint16_t)(((h)->gc_flags & HDR_AGE_MASK) >> HDR_AGE_SHIFT)
#define HDR_SET_AGE(h, a)  ((h)->gc_flags = (uint16_t)                         \
                            (((h)->gc_flags & (uint16_t)~HDR_AGE_MASK)         \
                             | (((uint16_t)(a) << HDR_AGE_SHIFT) & HDR_AGE_MASK)))

/* Adaptive major threshold (iter 29). */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

typedef struct ASTroGC {
    AroGcCommonState common;

    /* Young: two halves alternate per minor. */
    char *young_active_base;
    char *young_top;
    char *young_alt_base;

    /* Tenured (single region, slide compactor). */
    char *tenured_base;
    char *tenured_top;
    char *tenured_end;

    CTX  *ctx;

    struct AroObjectHeader **remset_buf;
    size_t remset_cnt;
    size_t remset_capa;
    bool   remset_overflow;
    size_t old_alloc_since_major;
    size_t old_major_threshold;

    struct AroObjectHeader **gray_buf;
    size_t gray_cnt;
    size_t gray_capa;

    /* Minor scratch. */
    char *young_from_base;
    char *young_from_end;
    char *young_to_base;
    char *young_to_top;
    char *young_to_end;
    char *old_tenured_top;

    /* Major scratch (Cheney fold-young / from-tenured for slide). */
    char *to_top;
    char *to_base;
    char *from_base_cur;
    char *from_end_cur;

    bool  in_minor;
    bool  force_promote;     /* during major-fold-young: ignore age */
    bool  scan_saw_young;
} ASTroGC;

#define young_active_base     (gc->young_active_base)
#define young_top             (gc->young_top)
#define young_alt_base        (gc->young_alt_base)
#define tenured_base          (gc->tenured_base)
#define tenured_top           (gc->tenured_top)
#define tenured_end           (gc->tenured_end)
#define gc_ctx                (gc->ctx)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define young_from_base       (gc->young_from_base)
#define young_from_end        (gc->young_from_end)
#define young_to_base         (gc->young_to_base)
#define young_to_top          (gc->young_to_top)
#define young_to_end          (gc->young_to_end)
#define old_tenured_top       (gc->old_tenured_top)
#define to_top                (gc->to_top)
#define to_base               (gc->to_base)
#define from_base_cur         (gc->from_base_cur)
#define from_end_cur          (gc->from_end_cur)
#define in_minor              (gc->in_minor)
#define force_promote         (gc->force_promote)

const char *aro_gc_backend_name = "mark_compact_gen";

static char *
mmap_region(size_t bytes)
{
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    old_major_threshold = MAJOR_THRESHOLD_MIN;
    young_active_base = mmap_region(YOUNG_BYTES);
    young_alt_base    = mmap_region(YOUNG_BYTES);
    young_top         = young_active_base;

    tenured_base = mmap_region(TENURED_BYTES);
    tenured_top  = tenured_base;
    tenured_end  = tenured_base + TENURED_BYTES;

    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=mark_compact_gen] STRESS mode: collect on every alloc\n");
    }
    if (getenv("BARUBY_GC_PURGE")) ARO_GC_COMMON(c)->purge = true;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(CTX *c);
static void major_gc(CTX *c);

static AroObjectHeader * __attribute__((noinline, cold))
pretenure_alloc(CTX *c, size_t payload_size, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (tenured_top + total > tenured_end) {
        major_gc(c);
        if (tenured_top + total > tenured_end) {
            fprintf(stderr, "baruby_gc=mark_compact_gen: OOM tenured (need %zu)\n", total);
            abort();
        }
    }
    AroObjectHeader *h = (AroObjectHeader *)tenured_top;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    tenured_top += total;
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_cold(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t young_used = (size_t)(young_top - young_active_base);
    /* major XOR minor (see gc_copy_gen.c rationale) */
    if (tenured_top + young_used > tenured_end
        || old_alloc_since_major > old_major_threshold
        || gc->common.external_bytes > old_major_threshold) {
        major_gc(c);
    } else {
        minor_gc(c);
    }
    if (young_top + total > young_active_base + YOUNG_BYTES) {
        major_gc(c);
        if (young_top + total > young_active_base + YOUNG_BYTES) {
            fprintf(stderr, "baruby_gc=mark_compact_gen: OOM young (need %zu)\n", total);
            abort();
        }
    }
}

static inline AroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(total > YOUNG_BYTES / 2, 0)) {
        return pretenure_alloc(c, payload_size, total);
    }

    if (__builtin_expect(gc->common.stress
                         || (size_t)(young_top - young_active_base) + total > YOUNG_BYTES
                         || gc->common.external_bytes > old_major_threshold, 0)) {
        nursery_collect_cold(c, total);
    }
    AroObjectHeader *h = (AroObjectHeader *)young_top;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    young_top += total;
    return h;
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    AroObjectHeader *h = nursery_bump(c, payload_size, aligned);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(AroObjectHeader), 0,
           aligned - sizeof(AroObjectHeader));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    AroObjectHeader *h = nursery_bump(c, payload_size, aligned);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

#define MAX_REMSET_ENTRIES (1u << 17)

static void
remset_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (remset_overflow) return;
    if (remset_cnt >= MAX_REMSET_ENTRIES) { remset_overflow = true; return; }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (AroObjectHeader **)realloc(remset_buf, remset_capa * sizeof(AroObjectHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

void __attribute__((noinline, cold))
aro_gc_remember(CTX *c, AroObjectHeader *h)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    HDR_SET_DIRTY(h);
    remset_push(gc, h);
}

// ---------------------------------------------------------------------------
// Cheney copy collector (minor + major-fold)
// ---------------------------------------------------------------------------

static inline bool
in_young_from(const ASTroGC *const gc, const void *const p)
{
    return (const char *)p >= young_from_base && (const char *)p < young_from_end;
}

static inline bool
in_young_active(const ASTroGC *const gc, const void *const p)
{
    return (const char *)p >= young_active_base && (const char *)p < young_top;
}

static inline bool
in_from_tenured(const ASTroGC *const gc, const void *const p)
{
    return (const char *)p >= from_base_cur && (const char *)p < from_end_cur;
}

static inline bool
in_young_to(const ASTroGC *const gc, const void *const p)
{
    return (const char *)p >= young_to_base && (const char *)p < young_to_top;
}

/* Forward `oldh` (in young-from) — to young-to (= age++) or to tenured
 * (= age >= PROMOTE_AGE, OR force_promote during major-fold-young). */
static void *
forward_obj(ASTroGC *gc, AroObjectHeader *oldh)
{
    if (oldh->gc_fwd) return oldh->gc_fwd;
    size_t aligned = ALIGN8(oldh->gc_size);
    AroObjectHeader *newh;

    if (in_minor && !force_promote) {
        uint16_t age = HDR_GET_AGE(oldh);
        if (age >= PROMOTE_AGE) {
            ASTRO_ASSERT(tenured_top + aligned <= tenured_end);
            newh = (AroObjectHeader *)tenured_top;
            tenured_top += aligned;
            memcpy(newh, oldh, aligned);
            newh->gc_flags = HDR_OLD_BIT;
            newh->gc_fwd   = NULL;
        } else {
            ASTRO_ASSERT(young_to_top + aligned <= young_to_end);
            newh = (AroObjectHeader *)young_to_top;
            young_to_top += aligned;
            memcpy(newh, oldh, aligned);
            newh->gc_flags = 0;
            HDR_SET_AGE(newh, age + 1);
            newh->gc_fwd   = NULL;
        }
    } else {
        /* Major fold-young OR pretenure path: always promote. */
        ASTRO_ASSERT(to_top + aligned <= tenured_end);
        newh = (AroObjectHeader *)to_top;
        to_top += aligned;
        memcpy(newh, oldh, aligned);
        newh->gc_flags = HDR_OLD_BIT;
        newh->gc_fwd   = NULL;
    }

    oldh->gc_fwd = (void *)newh;
    return (void *)newh;
}

static void *
forward_payload_value(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    AroObjectHeader *h = (AroObjectHeader *)p;
    if (in_minor) {
        if (!in_young_from(gc, p)) return p;
    } else {
        /* Major-fold or pretenure: forward anything in young-active. */
        if (!in_young_active(gc, p)) return p;
    }
    return forward_obj(gc, h);
}

static void
forward_edge_minor(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) {
        void *new = forward_payload_value(gc, (void *)v);
        *slot = new;
        if (in_young_to(gc, new)) gc->scan_saw_young = true;
    }
}

/* Major-fold edge: no scan_saw_young tracking (= force_promote means
 * everything goes to tenured anyway). */
static void
forward_edge_promote(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = forward_payload_value(gc, (void *)v);
}

static void
process_object_young(ASTroGC *gc, AroObjectHeader *h)
{
    gc->scan_saw_young = false;
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
}

static void
process_object_promoted(ASTroGC *gc, AroObjectHeader *h)
{
    gc->scan_saw_young = false;
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
    if (gc->scan_saw_young) {
        HDR_SET_DIRTY(h);
        remset_push(gc, h);
    }
}

// ---------------------------------------------------------------------------
// minor_gc
// ---------------------------------------------------------------------------

static void
minor_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    in_minor = true;
    force_promote = false;
    young_from_base = young_active_base;
    young_from_end  = young_top;
    young_to_base   = young_alt_base;
    young_to_top    = young_to_base;
    young_to_end    = young_to_base + YOUNG_BYTES;
    old_tenured_top = tenured_top;

    struct timespec tminor = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, forward_edge_minor);

    /* Remset compaction (see gc_copy_gen.c). */
    if (remset_overflow) {
        remset_cnt = 0;
        char *scan = tenured_base;
        while (scan < old_tenured_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            if (HDR_DIRTY(h)) {
                gc->scan_saw_young = false;
                AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
                if (gc->scan_saw_young) {
                    remset_push(gc, h);
                } else {
                    HDR_CLR_DIRTY(h);
                }
            }
            scan += ALIGN8(h->gc_size);
        }
        remset_overflow = false;
    } else {
        size_t orig_cnt = remset_cnt;
        size_t write = 0;
        for (size_t i = 0; i < orig_cnt; i++) {
            AroObjectHeader *h = remset_buf[i];
            if (!HDR_DIRTY(h)) continue;
            gc->scan_saw_young = false;
            AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
            if (gc->scan_saw_young) {
                remset_buf[write++] = h;
            } else {
                HDR_CLR_DIRTY(h);
            }
        }
        remset_cnt = write;
    }

    /* Cheney loop over young-to + freshly-promoted tenured. */
    {
        char *young_scan = young_to_base;
        char *tenured_scan = old_tenured_top;
        for (;;) {
            bool advanced = false;
            while (young_scan < young_to_top) {
                AroObjectHeader *h = (AroObjectHeader *)young_scan;
                process_object_young(gc, h);
                young_scan += ALIGN8(h->gc_size);
                advanced = true;
            }
            while (tenured_scan < tenured_top) {
                AroObjectHeader *h = (AroObjectHeader *)tenured_scan;
                process_object_promoted(gc, h);
                tenured_scan += ALIGN8(h->gc_size);
                advanced = true;
            }
            if (!advanced) break;
        }
    }
    aro_gc_phase_end(tminor, &gc->common.stats.reclaim_seconds);

    /* Finalize pass — see gc_copy_gen.c. */
    aro_gc_finalize_walk(c);

    old_alloc_since_major += (size_t)(tenured_top - old_tenured_top);

    /* Clear gc_fwd on stale young-from objs (the alt becomes next minor's
     * fresh side, but objs there have gc_fwd set from this minor). */
    char *p = young_from_base;
    while (p < young_from_end) {
        AroObjectHeader *h = (AroObjectHeader *)p;
        h->gc_fwd = NULL;
        p += ALIGN8(h->gc_size);
    }

    /* Swap young halves. */
    char *old_active = young_active_base;
    young_active_base = young_to_base;
    young_top         = young_to_top;
    young_alt_base    = old_active;

    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    aro_gc_time_end(c, t0);
}

// ---------------------------------------------------------------------------
// Major GC: fold young (force-promote all live), then mark + Lisp-2 slide.
// ---------------------------------------------------------------------------

static void
gray_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (AroObjectHeader **)realloc(gray_buf, gray_capa * sizeof(AroObjectHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void
mark_value_major(ASTroGC *gc, VALUE v)
{
    if (!AROH_IS_GC_OBJECT(v)) return;
    AroObjectHeader *h = (AroObjectHeader *)v;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

static void
mark_edge_major(void *ctx, void **slot)
{
    mark_value_major((ASTroGC *)ctx, (VALUE)*slot);
}

static void
process_gray_major(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        AroObjectHeader *h = gray_buf[--gray_cnt];
        AROH_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge_major);
    }
}

static void *
fwd_payload_compact(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    AroObjectHeader *h = (AroObjectHeader *)p;
    ASTRO_ASSERT(HDR_MARKED(h));
    ASTRO_ASSERT(h->gc_fwd != NULL);
    return h->gc_fwd;
}

static void
fwd_edge_compact(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = fwd_payload_compact(gc, (void *)v);
}

/* Cheney fold-young: force-promote all live young to tenured before mark. */
static void
major_fold_young(ASTroGC *gc, CTX *c)
{
    if (young_top == young_active_base) return;  /* young is empty */

    in_minor = false;
    force_promote = true;
    from_base_cur = young_active_base;
    from_end_cur  = young_top;
    to_base = tenured_base;
    to_top  = tenured_top;
    char *fold_start = tenured_top;

    AROH_VISIT_ROOTS(c, gc, forward_edge_promote);

    /* Existing tenured may have refs into young (= remset).  Scan all
     * tenured (= heap walk) for those refs.  Simpler than maintaining a
     * separate remset path during major. */
    if (remset_overflow || true) {
        char *scan = tenured_base;
        while (scan < fold_start) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            if (HDR_DIRTY(h)) {
                AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_promote);
                HDR_CLR_DIRTY(h);
            }
            scan += ALIGN8(h->gc_size);
        }
    }
    remset_cnt = 0;
    remset_overflow = false;

    /* Cheney scan of freshly-promoted tenured (= newly added at fold_start). */
    {
        char *scan = fold_start;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_promote);
            scan += ALIGN8(h->gc_size);
        }
    }

    tenured_top = to_top;
    young_top = young_active_base;
    /* Clear gc_fwd on stale young objs. */
    char *p = young_active_base;
    while (p < from_end_cur) {
        AroObjectHeader *h = (AroObjectHeader *)p;
        h->gc_fwd = NULL;
        p += ALIGN8(h->gc_size);
    }
    force_promote = false;
}

static void
major_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    /* Phase 1: fold all young to tenured. */
    major_fold_young(gc, c);

    /* Phase 2: mark from roots. */
    in_minor = false;
    struct timespec tmark = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, mark_edge_major);
    process_gray_major(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    /* Phase 3: Lisp-2 slide compact tenured. */
    struct timespec treclaim = aro_gc_phase_begin();

    /* Compute forwarding addresses. */
    char *fwd = tenured_base;
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) {
                h->gc_fwd = fwd;
                fwd += total;
            } else {
                h->gc_fwd = NULL;
            }
            p += total;
        }
    }

    /* Update interior pointers. */
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) {
                AROH_SCAN_EDGES((void *)h, h->gc_size, gc, fwd_edge_compact);
            }
            p += total;
        }
    }

    AROH_VISIT_ROOTS(c, gc, fwd_edge_compact);

    /* Finalize pass before slide (= gc_fwd still set on live, NULL on dead). */
    aro_gc_finalize_walk(c);

    /* Slide.  Memmove runs of live → their new addrs. */
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (!HDR_MARKED(h)) {
                p += total;
                continue;
            }
            char *run_src = p;
            char *run_dst = h->gc_fwd;
            char *run_p   = p;
            while (run_p < tenured_top) {
                AroObjectHeader *rh = (AroObjectHeader *)run_p;
                if (!HDR_MARKED(rh)) break;
                run_p += ALIGN8(rh->gc_size);
            }
            size_t run_size = (size_t)(run_p - run_src);
            if (run_dst != run_src) memmove(run_dst, run_src, run_size);
            char *q = run_dst, *q_end = run_dst + run_size;
            while (q < q_end) {
                AroObjectHeader *qh = (AroObjectHeader *)q;
                HDR_CLR_MARKED(qh);
                qh->gc_fwd = NULL;
                HDR_CLR_DIRTY(qh);
                q += ALIGN8(qh->gc_size);
            }
            p = run_p;
        }
    }
    tenured_top = fwd;

    aro_gc_phase_end(treclaim, &gc->common.stats.reclaim_seconds);

    size_t live = (size_t)(tenured_top - tenured_base);
    gc->common.stats.heap_bytes = live;
    old_alloc_since_major = 0;
    if (!gc->common.stress) {
        size_t next = live * MAJOR_THRESHOLD_FACTOR;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }

    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    major_gc(c);
}

void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (in_minor) {
        if (h->gc_fwd) return h->gc_fwd;
        return in_young_from(gc, payload) ? NULL : payload;
    }
    /* Major between forward-addr pass and slide. */
    return HDR_MARKED(h) ? h->gc_fwd : NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (young_active_base) munmap(young_active_base, YOUNG_BYTES);
    if (young_alt_base)    munmap(young_alt_base,    YOUNG_BYTES);
    if (tenured_base)      munmap(tenured_base,      TENURED_BYTES);
    free(remset_buf);
    free(gray_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    AroObjectHeader *h = (AroObjectHeader *)p;
    return h->gc_size;
}
