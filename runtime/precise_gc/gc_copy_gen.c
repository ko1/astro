// gc_copy_gen.c — backend #6: copying GC with nursery + tenured semi-space.
//
// Layout:
//   - Nursery: one bump region (16 MiB).  All new allocs go here.
//   - Tenured: two mmap'd semi-space regions (256 MiB each).  Promoted
//     objects live here.  Major GC alternates between them.
//
// Minor GC:
//   1. Scan sample roots via AROH_VISIT_ROOTS — forward young → tenured.
//   2. Scan dirty tenured (remset proxy) — for each young VALUE, forward.
//   3. Cheney scan-loop over freshly-tenured objects, forwarding their refs.
//   4. Reset nursery_top.
//
// Major GC:
//   1. Cheney over from-tenured → to-tenured.
//   2. Also forwards anything in nursery (= promote first).
//   3. Swap active tenured.
//   4. Reset nursery_top.
//
// Promotion: on first survival.  Simplest and matches mark_gen.
//
// Write barrier: caller invokes aro_gc_wb() on every heap-pointer write.
// If holder is old, set holder.dirty.  Minor GC scans dirty tenured.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define NURSERY_BYTES  ((size_t)16u  << 20)    /* 16 MiB (tuning knob, not program limit) */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES /* 64 GiB virtual per semispace, lazy-paged */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C+: fwd overlay (8 B head、 no dedicated gc_fwd field). */
_Static_assert(sizeof(AroObjectHeader) == 8, "Cheney: head must be 8 bytes");

#define HDR_OLD_BIT      (uint16_t)0x0001u
#define HDR_DIRTY_BIT    (uint16_t)0x0002u
#define HDR_FORWARDED    (uint16_t)0x0004u
#define HDR_OLD(h)         (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->gc_flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->gc_flags &= (uint16_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)
#define HDR_IS_FORWARDED(h)  (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)

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

/* Adaptive major threshold: matches other gen backends.  Previously
 * major triggered only when tenured couldn't hold worst-case promotion,
 * which with 64 GiB virtual = effectively never. */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// Single-instance binding via c->astro_gc; multi-instance future would
// allocate multiple ASTroGC and wire each to a different CTX.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    /* Nursery: small bump region for fresh allocations. */
    char *nursery_base;
    char *nursery_top;
    char *nursery_end;

    /* Tenured: two-space (Cheney) for promoted survivors. */
    char *tenured_base;
    char *tenured_top;
    char *tenured_end;
    char *tenured_alt_base;   /* "other" tenured region for major Cheney */

    /* CTX bind. */
    CTX   *ctx;

    /* Remembered set: tenured objects dirtied since the last minor GC. */
    struct AroObjectHeader **remset_buf;
    size_t     remset_cnt;
    size_t     remset_capa;
    bool       remset_overflow;

    /* Adaptive major trigger. */
    size_t old_alloc_since_major;
    size_t old_major_threshold;

    /* Cheney scratch (used during minor_gc / major_gc only). */
    char *to_top;
    char *to_base;
    char *from_base_cur;
    char *from_end_cur;
    bool  in_minor;
} ASTroGC;

/* Field aliases — expand to `gc->field`; every helper must have an
 * `ASTroGC *gc` in scope.  No module-static instance pointer. */
#define nursery_base          (gc->nursery_base)
#define nursery_top           (gc->nursery_top)
#define nursery_end           (gc->nursery_end)
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

    nursery_base = mmap_region(NURSERY_BYTES);
    nursery_top  = nursery_base;
    nursery_end  = nursery_base + NURSERY_BYTES;

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

// Allocate `bytes` (header + aligned payload).  See gc_copy.c rationale.
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
nursery_collect_slow(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    // If tenured can't safely hold the entire nursery (worst-case
    // promotion), do a major first to recover dead tenured space.
    size_t max_promotion = (size_t)(nursery_top - nursery_base);
    if (tenured_top + max_promotion > tenured_end) {
        major_gc(c);
    } else {
        minor_gc(c);
        if (old_alloc_since_major > old_major_threshold) {
            major_gc(c);
        }
    }
    if (nursery_top + total > nursery_end) {
        major_gc(c);
        if (nursery_top + total > nursery_end) {
            fprintf(stderr, "baruby_gc=copy_gen: OOM (need %zu)\n", total);
            abort();
        }
    }
}

// minor / major GC + retry on space pressure.
static inline AroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    // Pretenure huge allocations directly into tenured.
    if (__builtin_expect(total > NURSERY_BYTES / 2, 0)) {
        return pretenure_alloc(c, payload_size, total);
    }

    /* External-memory pressure: treat libc-malloc'd external bytes
     * (e.g., GMP buffers) as nursery pressure so bignum-heavy workloads
     * trigger minor GC and run finalizers (= mpz_clear) promptly.  Without
     * this, framework heap stays tiny while libc heap balloons.  The
     * (nursery_used + external + total > NURSERY_BYTES) form subsumes the
     * plain (nursery_top + total > nursery_end) check. */
    if (__builtin_expect(gc->common.stress
                         || (size_t)(nursery_top - nursery_base) + gc->common.external_bytes + total > NURSERY_BYTES, 0)) {
        nursery_collect_slow(c, total);
    }
    AroObjectHeader *h = (AroObjectHeader *)nursery_top;
    h->flags    = 0;
    h->gc_flags = 0;          /* clear OLD/DIRTY for fresh nursery slot */
    h->gc_size  = (uint32_t)payload_size;
    nursery_top += total;
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
    /* Zero post-head region only (head was init'd by nursery_bump). */
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
    /* Byte payloads skip post-head zero-fill. */
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

/* iter 36 remset overflow guard — see gc_mark_gen.c for rationale.
 * Storage moved to ASTroGC.remset_overflow (aliased above). */
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

static void process_object(ASTroGC *gc, AroObjectHeader *h);
static void
remset_visit_minor(ASTroGC *gc, AroObjectHeader *h)
{
    if (HDR_DIRTY(h)) {
        process_object(gc, h);
        HDR_CLR_DIRTY(h);
    }
}

/* Heap-walk fallback over the bump-allocated tenured region. */
static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, AroObjectHeader *))
{
    char *scan = tenured_base;
    while (scan < tenured_top) {
        AroObjectHeader *h = (AroObjectHeader *)scan;
        visit(gc, h);
        scan += ALIGN8(h->gc_size);
    }
}

/* WB body — caller verified holder is old + not-yet-dirty.  See gc.h
 * aro_gc_wb for the inline fast path that gates this call. */
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
// Cheney scratch (to_top / to_base / from_base_cur / from_end_cur) and
// `in_minor` storage live in ASTroGC — aliased above.

// Copy `oldh` (already in nursery or from-tenured) into to-tenured, returning
// the new payload pointer.  Marks oldh as FORWARDED + stores fwd ptr at
// payload-overlay slot (offset 8) so future references find the new home.
static void *
forward_obj(ASTroGC *gc, AroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t aligned = ALIGN8(oldh->gc_size);
    size_t total = aligned;
    ASTRO_ASSERT(to_top + total <= tenured_end);
    AroObjectHeader *newh = (AroObjectHeader *)to_top;
    memcpy(newh, oldh, total);
    /* memcpy copied oldh's gc_flags (without FORWARDED yet); reset for new. */
    newh->gc_flags = HDR_OLD_BIT;  /* fresh tenured slot, no marked/dirty/fwd */
    to_top += total;
    void *new_payload = (void *)newh;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, new_payload);
    return new_payload;
}

// During MINOR GC: nursery objects only (in_minor=true).
// During MAJOR GC: all in from-tenured (and any nursery survivors).

static inline bool
in_nursery(ASTroGC *gc, void *p)
{
    return (char *)p >= nursery_base && (char *)p < nursery_end;
}

static inline bool
in_from_tenured(ASTroGC *gc, void *p)
{
    return (char *)p >= from_base_cur && (char *)p < from_end_cur;
}

static void *
forward_payload_value(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    AroObjectHeader *h = (AroObjectHeader *)p;
    if (in_minor) {
        if (!in_nursery(gc, p)) return p;     // already tenured; nothing to do
    } else {
        // Major: forward anything in nursery OR from-tenured.
        if (!in_nursery(gc, p) && !in_from_tenured(gc, p)) return p;
    }
    return forward_obj(gc, h);
}

// edge_visit callback for writeback: slot is either a raw payload pointer
// (BaArray.items / BaString.bytes) or a tagged VALUE (PAYLOAD_VAL slot).
// AROH_IS_GC_OBJECT(v) filters non-PTR tagged values; raw payload ptrs always pass.
static void
forward_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = (void *)(VALUE)forward_payload_value(gc, (void *)v);
}

// Walk a freshly-copied object's outgoing references and forward them.
static void
process_object(ASTroGC *gc, AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge);
}


/* Keep minor_gc out-of-line so the inliner doesn't grow nursery_bump
 * (which is called only from the alloc fast paths).  Inlining minor_gc
 * into nursery_bump bloats it past the inliner's budget for aro_gc_alloc,
 * leaving a `call nursery_bump` on every allocation.  See iter (29). */
static void __attribute__((noinline))
minor_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;
    to_base = tenured_base;
    to_top  = tenured_top;            // append onto current tenured
    from_base_cur = nursery_base;
    from_end_cur  = nursery_top;      // tight bound on valid nursery objects

    /* Cheney has no separate mark phase: trace and relocate are interleaved. */
    struct timespec tcheney = aro_gc_phase_begin();
    // (1) Roots (sample-provided macro handles any stale-slot cleanup).
    AROH_VISIT_ROOTS(c, gc, forward_edge);

    // (2) Dirty tenured (explicit remset).
    if (remset_overflow) {
        remset_heap_walk(gc, remset_visit_minor);
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            AroObjectHeader *h = remset_buf[i];
            if (HDR_DIRTY(h)) {
                process_object(gc, h);
                HDR_CLR_DIRTY(h);
            }
        }
    }
    remset_cnt = 0;

    // (3) Cheney scan-loop over freshly-tenured objects.
    {
        char *scan = tenured_top;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            process_object(gc, h);
            scan += ALIGN8(h->gc_size);
        }
    }
    aro_gc_phase_end(tcheney, &gc->common.stats.reclaim_seconds);

    /* Finalize pass: live = HDR_FORWARDED (nursery, promoted) or HDR_OLD
     * (tenured, not visited by minor); dead = nursery-but-not-forwarded.
     * Run before the nursery commit so the dead-nursery payload memory is
     * still readable by AROH_FINALIZE (the macro reads head.flags).
     * For moving (forwarded) entries this updates the list to the new
     * tenured addr — necessary for the next collection to find the obj. */
    aro_gc_finalize_walk(c);

    /* (4) Commit. */
    old_alloc_since_major += (size_t)(to_top - tenured_top);
    tenured_top = to_top;
    nursery_top = nursery_base;
    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    aro_gc_time_end(c, t0);
}

static void
major_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    remset_cnt = 0;
    char *new_active_base = tenured_alt_base;
    tenured_alt_base = tenured_base;
    char *old_active_base = tenured_base;
    char *old_active_top  = tenured_top;

    from_base_cur = old_active_base;
    from_end_cur  = old_active_top;

    tenured_base = new_active_base;
    tenured_end  = new_active_base + TENURED_BYTES;
    to_base = new_active_base;
    to_top  = new_active_base;

    struct timespec tcheney = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, forward_edge);

    {
        char *scan = to_base;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            process_object(gc, h);
            scan += ALIGN8(h->gc_size);
        }
    }
    aro_gc_phase_end(tcheney, &gc->common.stats.reclaim_seconds);

    /* Finalize pass: after Cheney into new tenured.  Old-tenured payloads
     * that were FORWARDED → return fwd_overlay (new addr in alt-tenured);
     * old-tenured without FORWARDED → dead.  Must run before tenured_top
     * commit so old-tenured memory is still readable. */
    aro_gc_finalize_walk(c);

    tenured_top = to_top;
    nursery_top = nursery_base;

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
 *   Else, in minor: tenured entries (= not in nursery) are conservatively
 *     live (we don't trace tenured in minor); nursery entries without
 *     HDR_FORWARDED are dead.
 *   Else, in major: anything in from-tenured or nursery without
 *     HDR_FORWARDED is dead. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (HDR_IS_FORWARDED(h)) return fwd_overlay_get(h);
    if (in_minor) {
        return in_nursery(gc, payload) ? NULL : payload;
    }
    /* major: nursery or from-tenured without fwd is dead. */
    return NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (nursery_base)     munmap(nursery_base,     NURSERY_BYTES);
    if (tenured_base)     munmap(tenured_base,     TENURED_BYTES);
    if (tenured_alt_base) munmap(tenured_alt_base, TENURED_BYTES);
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
