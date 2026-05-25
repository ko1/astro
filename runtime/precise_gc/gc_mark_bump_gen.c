// gc_mark_bump_gen.c — backend #11: bump-allocated young (2 halves) +
// bump-allocated mark&sweep tenured, with N-survive promotion.
//
// Layout:
//   - Young: two semi-spaces (YOUNG_BYTES each).  Active half receives new
//     allocs.  On minor, survivors copy to alt half (age++ < PROMOTE_AGE)
//     or to tenured (age >= PROMOTE_AGE).
//   - Tenured: bump-allocated within a single mmap'd region (64 GiB virt).
//     Survivors are appended; major mark + linear sweep reclaims.
//
// Promotion: N-survive (= PROMOTE_AGE-th survival, age in 2 bits of gc_flags).
//
// Minor GC: N-survive Cheney young → young-to OR tenured.  Promoted with
//   young refs gets remset_push (= GC-internal WB).
// Major GC: force-promote all live young to tenured, then mark + linear sweep.
//
// Write barrier:
//   - User write of young VALUE into tenured: aro_gc_wb pushes holder to remset.
//   - GC promote: Cheney scan in minor detects tenured→young, pushes.

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

_Static_assert(sizeof(AroObjectHeader) == 8, "head must be 8 bytes");

/* gc_flags layout:
 *   bit 0  : MARKED
 *   bit 1  : OLD
 *   bit 2  : DIRTY
 *   bit 3  : FREE     (unused here, kept for symmetry)
 *   bit 4  : FORWARDED (= nursery→to copied; for moving GC fwd_overlay)
 *   bits 5-6: AGE
 *
 * WB mask (gc_types.h): MARKED=0x1, OLD=0x2, DIRTY=0x4 → AGE/FORWARDED don't
 * interfere with OLD+DIRTY check. */
#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_OLD_BIT      (uint16_t)0x0002u
#define HDR_DIRTY_BIT    (uint16_t)0x0004u
#define HDR_FREE_BIT     (uint16_t)0x0008u
#define HDR_FORWARDED    (uint16_t)0x0010u
#define HDR_AGE_SHIFT    5
#define HDR_AGE_MASK     ((uint16_t)0x0060u)
#define PROMOTE_AGE      3u
#define HDR_IS_FORWARDED(h)  (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->gc_flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->gc_flags &= (uint16_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)
#define HDR_IS_FREE(h)     (((h)->gc_flags & HDR_FREE_BIT) != 0)
#define HDR_SET_FREE(h)    ((h)->gc_flags |= HDR_FREE_BIT)
#define HDR_GET_AGE(h)     (uint16_t)(((h)->gc_flags & HDR_AGE_MASK) >> HDR_AGE_SHIFT)
#define HDR_SET_AGE(h, a)  ((h)->gc_flags = (uint16_t)                         \
                            (((h)->gc_flags & (uint16_t)~HDR_AGE_MASK)         \
                             | (((uint16_t)(a) << HDR_AGE_SHIFT) & HDR_AGE_MASK)))

static inline void *
fwd_overlay_get(AroObjectHeader *h)
{
    return *(void **)((char *)h + sizeof(AroObjectHeader));
}

static inline void
fwd_overlay_set(AroObjectHeader *h, void *new_payload)
{
    *(void **)((char *)h + sizeof(AroObjectHeader)) = new_payload;
}

#define MAJOR_THRESHOLD_MIN  (16u * 1024u * 1024u)

typedef struct ASTroGC {
    AroGcCommonState common;

    char *young_active_base;
    char *young_top;
    char *young_alt_base;

    char *tenured_base;
    char *tenured_top;
    char *tenured_end;

    size_t old_bytes;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    CTX  *ctx;
    bool  in_minor;
    bool  force_promote;
    bool  scan_saw_young;

    /* Minor scratch. */
    char *young_from_base;
    char *young_from_end;
    char *young_to_base;
    char *young_to_top;
    char *young_to_end;
    char *old_tenured_top;

    struct AroObjectHeader **scan_buf;
    size_t scan_head, scan_tail, scan_capa;
    struct AroObjectHeader **gray_buf;
    size_t gray_cnt, gray_capa;
    struct AroObjectHeader **remset_buf;
    size_t remset_cnt, remset_capa;
    bool   remset_overflow;
} ASTroGC;

#define young_active_base     (gc->young_active_base)
#define young_top             (gc->young_top)
#define young_alt_base        (gc->young_alt_base)
#define tenured_base          (gc->tenured_base)
#define tenured_top           (gc->tenured_top)
#define tenured_end           (gc->tenured_end)
#define old_bytes             (gc->old_bytes)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gc_ctx                (gc->ctx)
#define in_minor              (gc->in_minor)
#define force_promote         (gc->force_promote)
#define young_from_base       (gc->young_from_base)
#define young_from_end        (gc->young_from_end)
#define young_to_base         (gc->young_to_base)
#define young_to_top          (gc->young_to_top)
#define young_to_end          (gc->young_to_end)
#define old_tenured_top       (gc->old_tenured_top)
#define scan_buf              (gc->scan_buf)
#define scan_head             (gc->scan_head)
#define scan_tail             (gc->scan_tail)
#define scan_capa             (gc->scan_capa)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)

const char *aro_gc_backend_name = "mark_bump_gen";

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
        fprintf(stderr, "[baruby_gc=mark_bump_gen] STRESS mode: collect on every alloc\n");
    }
    if (getenv("BARUBY_GC_PURGE")) ARO_GC_COMMON(c)->purge = true;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(CTX *c);
static void major_gc(CTX *c);

static AroObjectHeader *
old_alloc(ASTroGC *gc, size_t payload_size, size_t aligned)
{
    size_t total = aligned;
    if (tenured_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_bump_gen: tenured OOM (%zu / %zu)\n",
                (size_t)(tenured_top - tenured_base), (size_t)TENURED_BYTES);
        abort();
    }
    AroObjectHeader *h = (AroObjectHeader *)tenured_top;
    tenured_top += total;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;
    h->gc_size  = (uint32_t)payload_size;
    old_bytes += payload_size;
    old_alloc_since_major += ALIGN8(payload_size);
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_cold(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (old_alloc_since_major > old_major_threshold
        || gc->common.external_bytes > old_major_threshold) {
        major_gc(c);
    } else {
        minor_gc(c);
    }
    if (young_top + total > young_active_base + YOUNG_BYTES) {
        major_gc(c);
        if (young_top + total > young_active_base + YOUNG_BYTES) {
            fprintf(stderr, "baruby_gc=mark_bump_gen: OOM young (need %zu)\n", total);
            abort();
        }
    }
}

static inline AroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(payload_size >= YOUNG_BYTES / 2, 0)) {
        return old_alloc(gc, payload_size, aligned);
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
// Cheney copy (minor + major-fold)
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
in_young_to(const ASTroGC *const gc, const void *const p)
{
    return (const char *)p >= young_to_base && (const char *)p < young_to_top;
}

/* Forward young-from obj to young-to (age++) or tenured (= promote). */
static void *
forward_obj(ASTroGC *gc, AroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t aligned = ALIGN8(oldh->gc_size);
    AroObjectHeader *newh;

    if (in_minor && !force_promote) {
        uint16_t age = HDR_GET_AGE(oldh);
        if (age >= PROMOTE_AGE) {
            newh = old_alloc(gc, oldh->gc_size, aligned);
            memcpy(newh, oldh, aligned);
            newh->gc_flags = HDR_OLD_BIT;
        } else {
            ASTRO_ASSERT(young_to_top + aligned <= young_to_end);
            newh = (AroObjectHeader *)young_to_top;
            young_to_top += aligned;
            memcpy(newh, oldh, aligned);
            newh->gc_flags = 0;
            HDR_SET_AGE(newh, age + 1);
        }
    } else {
        /* Major-fold-young: force promote. */
        newh = old_alloc(gc, oldh->gc_size, aligned);
        memcpy(newh, oldh, aligned);
        newh->gc_flags = HDR_OLD_BIT | HDR_MARKED_BIT;  /* major needs MARKED */
    }

    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, newh);
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

static void __attribute__((noinline))
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

    aro_gc_finalize_walk(c);

    /* Swap young halves. */
    char *old_active = young_active_base;
    young_active_base = young_to_base;
    young_top         = young_to_top;
    young_alt_base    = old_active;

    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    gc->common.stats.heap_bytes = old_bytes;
    aro_gc_time_end(c, t0);
}

// ---------------------------------------------------------------------------
// Major GC: fold young (force promote, set MARKED on new), mark, sweep.
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
scan_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (scan_tail >= scan_capa) {
        scan_capa = scan_capa ? scan_capa * 2 : 256;
        scan_buf = (AroObjectHeader **)realloc(scan_buf, scan_capa * sizeof(AroObjectHeader *));
        if (!scan_buf) abort();
    }
    scan_buf[scan_tail++] = h;
}

/* Major edge: forward (force-promote) young VALUE to tenured + MARK, or
 * mark in-place if already tenured. */
static void
major_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (!AROH_IS_GC_OBJECT(v)) return;
    AroObjectHeader *vh = (AroObjectHeader *)v;
    if (in_young_active(gc, (void *)v)) {
        /* force_promote=true ensures forward_obj writes HDR_OLD_BIT|MARKED. */
        AroObjectHeader *newh = (AroObjectHeader *)forward_obj(gc, vh);
        *slot = newh;
        scan_push(gc, newh);
    } else if (!HDR_MARKED(vh)) {
        HDR_SET_MARKED(vh);
        gray_push(gc, vh);
    }
}

static void
major_process(ASTroGC *gc, AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, major_edge);
}

static void
major_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    force_promote = true;

    remset_cnt = 0;
    remset_overflow = false;
    scan_head = scan_tail = 0;
    gray_cnt = 0;

    /* Set up major-fold context (= treat young-active as from-space). */
    young_from_base = young_active_base;
    young_from_end  = young_top;

    AROH_VISIT_ROOTS(c, gc, major_edge);

    while (gray_cnt > 0 || scan_head < scan_tail) {
        while (gray_cnt > 0) {
            AroObjectHeader *h = gray_buf[--gray_cnt];
            major_process(gc, h);
        }
        while (scan_head < scan_tail) {
            AroObjectHeader *h = scan_buf[scan_head++];
            major_process(gc, h);
        }
    }
    scan_head = scan_tail = 0;

    /* Finalize pass before sweep clears MARKED bits. */
    aro_gc_finalize_walk(c);

    /* Linear sweep tenured: free unmarked, clear bits on survivors. */
    {
        char *p = tenured_base;
        size_t live = 0;
        while (p < tenured_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) {
                HDR_CLR_MARKED(h);
                HDR_CLR_DIRTY(h);
                live += h->gc_size;
            }
            p += total;
        }
        old_bytes = live;
    }

    young_top = young_active_base;
    force_promote = false;

    if (!gc->common.stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;

    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    gc->common.stats.heap_bytes = old_bytes;
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
    if (HDR_IS_FORWARDED(h)) return fwd_overlay_get(h);
    if (in_minor) {
        return in_young_from(gc, payload) ? NULL : payload;
    }
    return HDR_MARKED(h) ? payload : NULL;
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
    free(scan_buf);
    free(gray_buf);
    free(remset_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    AroObjectHeader *h = (AroObjectHeader *)p;
    return h->gc_size;
}
