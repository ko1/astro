// gc_copy_gen.c — backend #6: generational copying GC with N-survive
// promotion (= 4-面 + age bits).
//
// Layout:
//   - Young: two semi-spaces (YOUNG_BYTES each).  Active half receives new
//     allocations; on minor, survivors copy to the inactive half (= halves
//     alternate per minor).
//   - Tenured: two semi-spaces (TENURED_BYTES each, MAP_NORESERVE lazy
//     paged).  Active half receives promotions; on major, all live →
//     inactive half, halves swap.
//
// Promotion: on PROMOTE_AGE-th (= 3) survival.  Age stored in 2 bits of
// gc_flags (bits 3-4).  age 0..2 → stay young (= copy to young-to + age++);
// age == 3 → promote to tenured.
//
// Minor GC:
//   1. Roots → forward via forward_edge_minor.
//   2. Old remset (= tenured DIRTY from previous minor / user WB) — for each
//      entry: SCAN_EDGES + track if any forwarded ref still in young-to.
//      Compact remset in place: keep entry + DIRTY only if it still has
//      young refs; else CLR_DIRTY and drop.
//   3. Cheney scan over young-to AND newly-promoted tenured range.  For each
//      scanned tenured (= promoted) obj: if any outgoing ref ended in
//      young-to, SET_DIRTY + remset_push.  This is the GC-internal WB:
//      promote can create persistent tenured→young edges that the standard
//      user-write WB never sees.
//   4. Swap young halves.  Reset old active to empty.
//
// Major GC:
//   1. Forward all live (from young AND from-tenured) to to-tenured.
//   2. Cheney scan to-tenured.
//   3. Swap tenured halves.  Reset young (= active stays, top = base).
//
// Write barrier:
//   - User: aro_gc_wb pushes holder to remset on first young → tenured
//     write (= same as before).
//   - GC: Cheney scan in minor step 3 above pushes newly-promoted objs
//     with young refs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define YOUNG_BYTES    ((size_t)16u << 20)        /* 16 MiB per young half */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virt per tenured half */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C+: fwd overlay (8 B head、 no dedicated gc_fwd field). */
_Static_assert(sizeof(AroObjectHeader) == 8, "Cheney: head must be 8 bytes");

/* gc_flags layout:
 *   bit 0  : OLD       (= tenured)
 *   bit 1  : DIRTY     (= in remset, has young child ref)
 *   bit 2  : FORWARDED (= moved during current GC; new addr in fwd_overlay)
 *   bits 3-4: AGE      (= 0..3; promote at PROMOTE_AGE)
 *   bits 5-15: unused
 *
 * The mask exposed to gc.h's WB fast path covers only OLD+DIRTY (= bits
 * 0-1).  FORWARDED is only set during a GC; AGE is meaningless for tenured
 * objs (= reset to 0 on promote).  WB check `(gc_flags & 0x3) == 1` is
 * unaffected by bits 2-4. */
#define HDR_OLD_BIT      (uint16_t)0x0001u
#define HDR_DIRTY_BIT    (uint16_t)0x0002u
#define HDR_FORWARDED    (uint16_t)0x0004u
#define HDR_AGE_SHIFT    3
#define HDR_AGE_MASK     ((uint16_t)0x0018u)
#define PROMOTE_AGE      3u

#define HDR_OLD(h)           (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_DIRTY(h)         (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)     ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)     ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)
#define HDR_IS_FORWARDED(h)  (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)
#define HDR_GET_AGE(h)       (uint16_t)(((h)->gc_flags & HDR_AGE_MASK) >> HDR_AGE_SHIFT)
#define HDR_SET_AGE(h, a)    ((h)->gc_flags = (uint16_t)                       \
                              (((h)->gc_flags & (uint16_t)~HDR_AGE_MASK)       \
                               | (((uint16_t)(a) << HDR_AGE_SHIFT) & HDR_AGE_MASK)))

static inline void *
fwd_overlay_get(AroObjectHeader *const h)
{
    return *(void **)((char *)h + sizeof(AroObjectHeader));
}

static inline void
fwd_overlay_set(AroObjectHeader *const h, void *const new_payload)
{
    *(void **)((char *)h + sizeof(AroObjectHeader)) = new_payload;
}

/* Adaptive major threshold (same as other gen backends). */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC instance.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */

    /* Young: two halves alternate per minor.  young_active_base receives
     * new allocs; young_alt_base is the to-space at next minor. */
    char *young_active_base;
    char *young_top;
    char *young_alt_base;

    /* Tenured: two halves alternate per major. */
    char *tenured_base;
    char *tenured_top;
    char *tenured_end;
    char *tenured_alt_base;

    CTX  *ctx;

    /* Remembered set: tenured objects with refs into young (set by user WB
     * AND by promote-time scan in step 3 of minor). */
    AroObjectHeader **remset_buf;
    size_t            remset_cnt;
    size_t            remset_capa;
    bool              remset_overflow;

    /* Adaptive major trigger. */
    size_t old_alloc_since_major;
    size_t old_major_threshold;

    /* Minor scratch (set up at minor entry, used by forward_obj /
     * forward_edge_minor). */
    char *young_from_base;
    char *young_from_end;
    char *young_to_base;
    char *young_to_top;
    char *young_to_end;
    char *old_tenured_top;

    /* Major scratch (Cheney over from-tenured → to-tenured). */
    char *to_top;
    char *to_base;
    char *from_base_cur;
    char *from_end_cur;

    bool in_minor;
    bool scan_saw_young;     /* set by forward_edge_minor when target lands
                              * in young-to; consumed by process_object's
                              * "promote needs WB" check.  Per-scan-pass. */
} ASTroGC;

/* Field aliases — every helper must have `ASTroGC *gc` in scope. */
#define young_active_base     (gc->young_active_base)
#define young_top             (gc->young_top)
#define young_alt_base        (gc->young_alt_base)
#define tenured_base          (gc->tenured_base)
#define tenured_top           (gc->tenured_top)
#define tenured_end           (gc->tenured_end)
#define tenured_alt_base      (gc->tenured_alt_base)
#define gc_ctx                (gc->ctx)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
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

const char *aro_gc_backend_name = "copy_gen";

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

    tenured_base     = mmap_region(TENURED_BYTES);
    tenured_top      = tenured_base;
    tenured_end      = tenured_base + TENURED_BYTES;
    tenured_alt_base = mmap_region(TENURED_BYTES);

    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=copy_gen] STRESS mode: collect on every alloc\n");
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
            fprintf(stderr, "baruby_gc=copy_gen: OOM tenured (need %zu)\n", total);
            abort();
        }
    }
    AroObjectHeader *h = (AroObjectHeader *)tenured_top;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;  /* direct to tenured = old from the start */
    h->gc_size  = (uint32_t)payload_size;
    tenured_top += total;
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_cold(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t young_used = (size_t)(young_top - young_active_base);
    /* major XOR minor: never minor-then-major chain.
     * Pick major when:
     *   (a) tenured can't safely hold worst-case promotion of the full young
     *       (= every young obj happens to be at PROMOTE_AGE),
     *   (b) tenured has already grown past major threshold,
     *   (c) external_bytes (= GMP buffer etc.) exceeds major threshold
     *       — only major full-finalize releases libc-backed memory.  Routing
     *       to minor caused matmul livelock (= minor promotes still-live
     *       bignum, external never drops, retrigger). */
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
            fprintf(stderr, "baruby_gc=copy_gen: OOM young (need %zu)\n", total);
            abort();
        }
    }
}

static inline AroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    /* Pretenure huge allocations directly into tenured. */
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
    h->gc_flags = 0;          /* young, age=0, no OLD/DIRTY/FORWARDED */
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
// Remset + Write barrier
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

/* WB body — caller verified holder is old + not-yet-dirty (see gc.h
 * aro_gc_wb).  User-driven path; GC-internal promote uses its own
 * remset_push directly in process_object_minor below. */
void __attribute__((noinline, cold))
aro_gc_remember(CTX *c, AroObjectHeader *h)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    HDR_SET_DIRTY(h);
    remset_push(gc, h);
}

// ---------------------------------------------------------------------------
// Cheney copy collector
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

/* Copy oldh into either young-to (= still young after age++) or tenured-to
 * (= promote).  Marks oldh as FORWARDED with fwd_overlay → newh.
 *
 * Note: this routine alone does NOT push promoted objs to remset — the
 * decision needs to know whether the promoted obj has outgoing young refs,
 * which is only known after its edges are scanned.  Cheney scan in step 3
 * of minor_gc handles the remset_push.  See process_object_minor. */
static void *
forward_obj(ASTroGC *gc, AroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);

    size_t aligned = ALIGN8(oldh->gc_size);
    AroObjectHeader *newh;

    if (in_minor) {
        uint16_t age = HDR_GET_AGE(oldh);
        if (age >= PROMOTE_AGE) {
            ASTRO_ASSERT(tenured_top + aligned <= tenured_end);
            newh = (AroObjectHeader *)tenured_top;
            tenured_top += aligned;
            memcpy(newh, oldh, aligned);
            newh->gc_flags = HDR_OLD_BIT;  /* fresh tenured, age=0, clean */
        } else {
            ASTRO_ASSERT(young_to_top + aligned <= young_to_end);
            newh = (AroObjectHeader *)young_to_top;
            young_to_top += aligned;
            memcpy(newh, oldh, aligned);
            newh->gc_flags = 0;            /* fresh young slot, clean */
            HDR_SET_AGE(newh, age + 1);
        }
    } else {
        /* Major: forward to to-tenured. */
        ASTRO_ASSERT(to_top + aligned <= tenured_alt_base + TENURED_BYTES);
        newh = (AroObjectHeader *)to_top;
        to_top += aligned;
        memcpy(newh, oldh, aligned);
        newh->gc_flags = HDR_OLD_BIT;
    }

    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, (void *)newh);
    return (void *)newh;
}

static void *
forward_payload_value(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    AroObjectHeader *h = (AroObjectHeader *)p;
    if (in_minor) {
        if (!in_young_from(gc, p)) return p;        /* tenured / already moved */
    } else {
        /* Major: forward anything live (= young active OR from-tenured). */
        if (!in_young_active(gc, p) && !in_from_tenured(gc, p)) return p;
    }
    return forward_obj(gc, h);
}

/* Minor edge visit: forward + set scan_saw_young flag if the (possibly
 * just-forwarded) ref lives in young-to.  Caller (process_object_minor for
 * a tenured-promoted obj) consumes the flag to decide remset_push. */
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

/* Major edge visit: forward only.  No remset tracking. */
static void
forward_edge_major(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = forward_payload_value(gc, (void *)v);
}

/* Cheney scan of a young-to obj (= survived but stayed young).  Edges get
 * forwarded; scan_saw_young is set as a side effect but unused for young
 * scan (= the obj is already in young, no remset concerns). */
static void
process_object_young(ASTroGC *gc, AroObjectHeader *h)
{
    gc->scan_saw_young = false;
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
}

/* Cheney scan of a newly-promoted tenured obj.  After scanning, if any
 * outgoing ref ended up in young-to, push to remset + SET_DIRTY.  This is
 * the GC-internal WB for promote-time tenured→young edges. */
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
    young_from_base = young_active_base;
    young_from_end  = young_top;
    young_to_base   = young_alt_base;
    young_to_top    = young_to_base;
    young_to_end    = young_to_base + YOUNG_BYTES;
    old_tenured_top = tenured_top;

    struct timespec tcheney = aro_gc_phase_begin();

    /* Phase 1: roots. */
    AROH_VISIT_ROOTS(c, gc, forward_edge_minor);

    /* Phase 2: old remset (= tenured DIRTY from prior minor / user WB).
     * For each entry, SCAN_EDGES + check if any forwarded ref stayed in
     * young-to.  Compact remset in-place: keep entry only if still has
     * young refs after the scan. */
    if (remset_overflow) {
        /* Heap-walk fallback: scan tenured [tenured_base, old_tenured_top).
         * Clear remset and re-push survivors. */
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
                remset_buf[write++] = h;  /* keep, DIRTY stays */
            } else {
                HDR_CLR_DIRTY(h);
            }
        }
        remset_cnt = write;
    }

    /* Phase 3: Cheney scan over young-to AND freshly-promoted tenured.
     * Loop until both scan cursors stabilize.  Newly-promoted objs whose
     * scan reveals young refs are pushed to remset by process_object_promoted. */
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
    aro_gc_phase_end(tcheney, &gc->common.stats.reclaim_seconds);

    /* Finalize pass: live = HDR_FORWARDED (young → young-to OR tenured) or
     * HDR_OLD (existing tenured, not visited by minor); dead = young-from
     * without HDR_FORWARDED.  Run before the swap so dead-young payload
     * memory is still readable by AROH_FINALIZE. */
    aro_gc_finalize_walk(c);

    /* Commit. */
    old_alloc_since_major += (size_t)(tenured_top - old_tenured_top);

    /* Swap young halves: alt (= where survivors landed) becomes active. */
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
// major_gc
// ---------------------------------------------------------------------------

static void
major_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    in_minor = false;
    remset_cnt = 0;
    remset_overflow = false;

    char *new_active_base = tenured_alt_base;
    char *old_active_base = tenured_base;
    char *old_active_top  = tenured_top;

    from_base_cur = old_active_base;
    from_end_cur  = old_active_top;

    tenured_base = new_active_base;
    tenured_end  = new_active_base + TENURED_BYTES;
    to_base = new_active_base;
    to_top  = new_active_base;

    struct timespec tcheney = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, forward_edge_major);

    {
        char *scan = to_base;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_major);
            scan += ALIGN8(h->gc_size);
        }
    }
    aro_gc_phase_end(tcheney, &gc->common.stats.reclaim_seconds);

    aro_gc_finalize_walk(c);

    tenured_top = to_top;
    /* Reset young: active stays active, but empty (= survivors all promoted). */
    young_top = young_active_base;
    tenured_alt_base = old_active_base;
    (void)old_active_top;

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

/* Liveness for finalizable entry post Cheney pass:
 *   HDR_FORWARDED on payload header → live, return fwd_overlay (new addr).
 *   Else, in minor: young-from without HDR_FORWARDED is dead; tenured (=
 *     not in young-from range) is conservatively live (we don't trace
 *     existing tenured in minor).
 *   Else, in major: anything in young-active or from-tenured without
 *     HDR_FORWARDED is dead. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (HDR_IS_FORWARDED(h)) return fwd_overlay_get(h);
    if (in_minor) {
        return in_young_from(gc, payload) ? NULL : payload;
    }
    return NULL;
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
    if (tenured_alt_base)  munmap(tenured_alt_base,  TENURED_BYTES);
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
