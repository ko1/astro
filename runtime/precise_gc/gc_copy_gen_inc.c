// gc_copy_gen_inc.c — backend #7: PLACEHOLDER (no actual incremental work).
//
// ⚠ iter 35 honesty fix: the file as written is **identical to
// gc_copy_gen.c** apart from comments and the backend_name string.  Unlike
// gc_mark_gen_inc.c which at least has inc_start_major / inc_step /
// inc_finish_sweep + an SATB barrier (even if INC_WORK_PER_ALLOC=SIZE_MAX
// makes it STW in practice), copy_gen_inc never had any of that
// infrastructure.  Including it as a separate "algorithm" in the
// comparison table is misleading.
//
// Until a real incremental Cheney is implemented here, this backend is
// **excluded from the matrix runner / perf table**.  Selecting GC=copy_gen_inc
// in the Makefile is still permitted (so the symbol exists for code-store
// IDs), but the bench harness treats copy_gen and copy_gen_inc as the same
// data point — picking either produces redundant rows.
//
// Future work (real incremental):
//   - Add inc_marking flag + start/step/finish entrypoints (analogous to
//     gc_mark_gen_inc.c).
//   - Add SATB barrier on heap writes during inc_marking.
//   - Process N bytes of to-space scan-loop per alloc, not the whole thing.
//   - Requires VALUE-stack write barrier for correctness (see todo.md).
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

/* iter 75 Step C+: fwd overlay (8 B head). */
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

/* Adaptive major threshold (iter 29).  See gc_copy_gen.c for rationale. */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    char *nursery_base, *nursery_top, *nursery_end;
    char *tenured_base, *tenured_top, *tenured_end;
    char *tenured_alt_base;   /* "other" tenured region for major Cheney */
    CTX   *ctx;
    struct AroObjectHeader **remset_buf;
    size_t     remset_cnt;
    size_t     remset_capa;
    bool       remset_overflow;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    char *to_top, *to_base, *from_base_cur, *from_end_cur;
    bool  in_minor;
} ASTroGC;

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

const char *aro_gc_backend_name = "copy_gen_inc";

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
        fprintf(stderr, "[baruby_gc=copy_gen_inc] STRESS mode: collect on every alloc\n");
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
            fprintf(stderr, "baruby_gc=copy_gen_inc: OOM tenured (need %zu)\n", total);
            abort();
        }
    }
    AroObjectHeader *h = (AroObjectHeader *)tenured_top;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;    /* tenured from the start */
    h->gc_size  = (uint32_t)payload_size;
    tenured_top += total;
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_slow(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
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
            fprintf(stderr, "baruby_gc=copy_gen_inc: OOM (need %zu)\n", total);
            abort();
        }
    }
}

static inline AroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(total > NURSERY_BYTES / 2, 0)) {
        return pretenure_alloc(c, payload_size, total);
    }

    /* External-memory pressure: fold libc-malloc'd external bytes into
     * nursery pressure so bignum-heavy workloads trigger minor GC and run
     * finalizers (= mpz_clear) promptly.  See gc_copy_gen.c for rationale. */
    if (__builtin_expect(gc->common.stress
                         || (size_t)(nursery_top - nursery_base) + gc->common.external_bytes + total > NURSERY_BYTES, 0)) {
        nursery_collect_slow(c, total);
    }
    AroObjectHeader *h = (AroObjectHeader *)nursery_top;
    h->flags    = 0;
    h->gc_flags = 0;     /* fresh nursery slot */
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

static void
remset_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        remset_buf = (AroObjectHeader **)realloc(remset_buf, remset_capa * sizeof(AroObjectHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

/* WB body — caller verified holder is old + not-yet-dirty. */
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

// Cheney scratch (to_top / to_base / from_base_cur / from_end_cur)
// storage moved into ASTroGC — aliased above.

static void *
forward_obj(ASTroGC *gc, AroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t aligned = ALIGN8(oldh->gc_size);
    size_t total = aligned;
    ASTRO_ASSERT(to_top + total <= tenured_end);
    AroObjectHeader *newh = (AroObjectHeader *)to_top;
    memcpy(newh, oldh, total);
    newh->gc_flags = HDR_OLD_BIT;
    to_top += total;
    void *new_payload = (void *)newh;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, new_payload);
    return new_payload;
}

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
        if (!in_nursery(gc, p)) return p;
    } else {
        if (!in_nursery(gc, p) && !in_from_tenured(gc, p)) return p;
    }
    return forward_obj(gc, h);
}

static void
forward_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = (void *)(VALUE)forward_payload_value(gc, (void *)v);
}

static void
process_object(ASTroGC *gc, AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge);
}


/* Keep cold (see gc_copy_gen.c iter (29)): inlining minor_gc into
 * nursery_bump bloats the alloc hot path past the inliner budget,
 * leaving an extra `call` on every fast-path alloc. */
static void __attribute__((noinline))
minor_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;
    to_base = tenured_base;
    to_top  = tenured_top;
    from_base_cur = nursery_base;
    from_end_cur  = nursery_top;

    AROH_VISIT_ROOTS(c, gc, forward_edge);

    for (size_t i = 0; i < remset_cnt; i++) {
        AroObjectHeader *h = remset_buf[i];
        if (HDR_DIRTY(h)) {
            process_object(gc, h);
            HDR_CLR_DIRTY(h);
        }
    }
    remset_cnt = 0;

    {
        char *scan = tenured_top;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            process_object(gc, h);
            scan += ALIGN8(h->gc_size);
        }
    }

    /* Finalize pass — see gc_copy_gen.c for the rationale. */
    aro_gc_finalize_walk(c);

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

    AROH_VISIT_ROOTS(c, gc, forward_edge);

    {
        char *scan = to_base;
        while (scan < to_top) {
            AroObjectHeader *h = (AroObjectHeader *)scan;
            process_object(gc, h);
            scan += ALIGN8(h->gc_size);
        }
    }

    /* Finalize pass — see gc_copy_gen.c. */
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

/* Same shape as gc_copy_gen.c finalize_check. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (HDR_IS_FORWARDED(h)) return fwd_overlay_get(h);
    if (in_minor) {
        return in_nursery(gc, payload) ? NULL : payload;
    }
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
