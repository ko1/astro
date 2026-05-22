// gc_mark_compact_gen.c — backend #9: generational hybrid.
//
// Same nursery layout as gc_copy_gen.c, but tenured uses a single-region
// mark + Lisp-2 sliding compactor (gc_mark_compact's algorithm) instead of
// a two-region semispace.  Saves half the tenured virtual address space
// at the cost of a more complex major.
//
// Layout:
//   - Nursery: one bump region (16 MiB).
//   - Tenured: single mmap'd region (512 MiB).  Survivors are appended on
//     minor; major mark+compact reclaims dead in place.
//
// Minor GC: Cheney-style copy nursery→tenured (same as copy_gen).
// Major GC: Lisp 2 sliding compactor over tenured (same as mark_compact).
// Write barrier: explicit remset, same as copy_gen / mark_gen.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define NURSERY_BYTES  ((size_t)16u  << 20)    /* 16 MiB (tuning knob, not program limit) */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C: framework GCHeader 廃止、 ASTroObjectHeader at offset 0. */
_Static_assert(sizeof(ASTroObjectHeader) == 16, "moving GC: head must be 16 B");

#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_OLD_BIT      (uint16_t)0x0002u
#define HDR_DIRTY_BIT    (uint16_t)0x0004u
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->gc_flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->gc_flags &= (uint16_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)

/* Adaptive major threshold (iter 29). */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    char *nursery_base, *nursery_top, *nursery_end;
    char *tenured_base, *tenured_top, *tenured_end;
    CTX  *ctx;
    VALUE *sp_high_water;
    struct ASTroObjectHeader **remset_buf;
    size_t            remset_cnt;
    size_t            remset_capa;
    bool              remset_overflow;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    struct ASTroObjectHeader **gray_buf;
    size_t            gray_cnt;
    size_t            gray_capa;
    char *to_top, *to_base, *from_base_cur, *from_end_cur;
    bool  in_minor;
} ASTroGC;

#define nursery_base          (gc->nursery_base)
#define nursery_top           (gc->nursery_top)
#define nursery_end           (gc->nursery_end)
#define tenured_base          (gc->tenured_base)
#define tenured_top           (gc->tenured_top)
#define tenured_end           (gc->tenured_end)
#define gc_ctx                (gc->ctx)
#define sp_high_water         (gc->sp_high_water)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define to_top                (gc->to_top)
#define to_base               (gc->to_base)
#define from_base_cur         (gc->from_base_cur)
#define from_end_cur          (gc->from_end_cur)
#define in_minor              (gc->in_minor)

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
    nursery_base = mmap_region(NURSERY_BYTES);
    nursery_top  = nursery_base;
    nursery_end  = nursery_base + NURSERY_BYTES;

    tenured_base = mmap_region(TENURED_BYTES);
    tenured_top  = tenured_base;
    tenured_end  = tenured_base + TENURED_BYTES;

    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=mark_compact_gen] STRESS mode: collect on every alloc\n");
    }
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(CTX *c, VALUE *sp_top);
static void major_gc(CTX *c, VALUE *sp_top);

static ASTroObjectHeader * __attribute__((noinline, cold))
pretenure_alloc(CTX *c, size_t payload_size, size_t total, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (tenured_top + total > tenured_end) {
        major_gc(c, sp_top);
        if (tenured_top + total > tenured_end) {
            fprintf(stderr, "baruby_gc=mark_compact_gen: OOM tenured (need %zu)\n", total);
            abort();
        }
    }
    ASTroObjectHeader *h = (ASTroObjectHeader *)tenured_top;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    tenured_top += total;
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_slow(CTX *c, size_t total, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t max_promotion = (size_t)(nursery_top - nursery_base);
    if (tenured_top + max_promotion > tenured_end) {
        major_gc(c, sp_top);
    } else {
        minor_gc(c, sp_top);
        if (old_alloc_since_major > old_major_threshold) {
            major_gc(c, sp_top);
        }
    }
    if (nursery_top + total > nursery_end) {
        major_gc(c, sp_top);
        if (nursery_top + total > nursery_end) {
            fprintf(stderr, "baruby_gc=mark_compact_gen: OOM (need %zu)\n", total);
            abort();
        }
    }
}

static inline ASTroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(total > NURSERY_BYTES / 2, 0)) {
        return pretenure_alloc(c, payload_size, total, sp_top);
    }

    if (__builtin_expect(gc->common.stress || nursery_top + total > nursery_end, 0)) {
        nursery_collect_slow(c, total, sp_top);
    }
    ASTroObjectHeader *h = (ASTroObjectHeader *)nursery_top;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    nursery_top += total;
    return h;
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    ASTroObjectHeader *h = nursery_bump(c, payload_size, aligned, sp_top);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(ASTroObjectHeader), 0,
           aligned - sizeof(ASTroObjectHeader));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    ASTroObjectHeader *h = nursery_bump(c, payload_size, aligned, sp_top);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

/* iter 36 remset overflow guard — see gc_mark_gen.c for rationale.
 * Storage: ASTroGC.remset_overflow (aliased above). */
#define MAX_REMSET_ENTRIES (1u << 17)

static void
remset_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (remset_overflow) return;
    if (remset_cnt >= MAX_REMSET_ENTRIES) { remset_overflow = true; return; }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (ASTroObjectHeader **)realloc(remset_buf, remset_capa * sizeof(ASTroObjectHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

static void process_object(ASTroGC *gc, ASTroObjectHeader *h);
static void
remset_visit_minor(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (HDR_DIRTY(h)) {
        process_object(gc, h);
        HDR_CLR_DIRTY(h);
    }
}

static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, ASTroObjectHeader *))
{
    char *scan = tenured_base;
    while (scan < tenured_top) {
        ASTroObjectHeader *h = (ASTroObjectHeader *)scan;
        visit(gc, h);
        scan += ALIGN8(h->gc_size);
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

// ---------------------------------------------------------------------------
// Cheney copy collector
// ---------------------------------------------------------------------------

// Cheney scratch (to_top / to_base / from_base_cur / from_end_cur)
// storage moved into ASTroGC — aliased above.

// Copy `oldh` (already in nursery or from-tenured) into to-tenured, returning
// the new payload pointer.  Sets oldh->gc_fwd so future references find the
// new home.
static void *
forward_obj(ASTroGC *gc, ASTroObjectHeader *oldh)
{
    if (oldh->gc_fwd) return oldh->gc_fwd;
    size_t aligned = ALIGN8(oldh->gc_size);
    size_t total = aligned;
    if (to_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_compact_gen: tenured OOM in forward_obj "
                        "(need %zu, tenured %zu / %zu)\n",
                total, (size_t)(to_top - tenured_base), TENURED_BYTES);
        abort();
    }
    ASTroObjectHeader *newh = (ASTroObjectHeader *)to_top;
    memcpy(newh, oldh, total);
    newh->gc_fwd   = NULL;
    HDR_SET_OLD(newh);
    HDR_CLR_DIRTY(newh);
    to_top += total;
    void *new_payload = (void *)newh;
    oldh->gc_fwd = new_payload;
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
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    if (in_minor) {
        if (!in_nursery(gc, p)) return p;
    } else {
        if (!in_nursery(gc, p) && !in_from_tenured(gc, p)) return p;
    }
    return forward_obj(gc, h);
}

static VALUE
forward_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_value(gc, (void *)v);
}

static void
forward_edge_minor(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (IS_PTR(v)) *slot = (void *)(VALUE)forward_payload_value(gc, (void *)v);
}

static void
process_object(ASTroGC *gc, ASTroObjectHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge_minor);
}


static void
minor_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;
    to_base = tenured_base;
    to_top  = tenured_top;
    from_base_cur = nursery_base;
    from_end_cur  = nursery_top;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    struct timespec tminor = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(gc, *p);

    if (remset_overflow) {
        remset_heap_walk(gc, remset_visit_minor);
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            ASTroObjectHeader *h = remset_buf[i];
            if (HDR_DIRTY(h)) {
                process_object(gc, h);
                HDR_CLR_DIRTY(h);
            }
        }
    }
    remset_cnt = 0;

    {
        char *scan = tenured_top;
        while (scan < to_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)scan;
            process_object(gc, h);
            scan += ALIGN8(h->gc_size);
        }
    }
    aro_gc_phase_end(tminor, &gc->common.stats.reclaim_seconds);

    old_alloc_since_major += (size_t)(to_top - tenured_top);
    tenured_top = to_top;
    nursery_top = nursery_base;
    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

// ---------------------------------------------------------------------------
// Major GC: Lisp-2 sliding compactor over tenured.
// (Same algorithm as gc_mark_compact.c, but only over the tenured region —
// the nursery is first folded into tenured via minor_gc.)
// ---------------------------------------------------------------------------

static void
gray_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (ASTroObjectHeader **)realloc(gray_buf, gray_capa * sizeof(ASTroObjectHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void
mark_value_major(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)v;
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
scan_outgoing_major(ASTroGC *gc, ASTroObjectHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge_major);
}

static void
process_gray_major(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        ASTroObjectHeader *h = gray_buf[--gray_cnt];
        scan_outgoing_major(gc, h);
    }
}

static void *
fwd_payload_compact(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    if (in_nursery(gc, p)) return p;
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    ASTRO_ASSERT(HDR_MARKED(h));
    ASTRO_ASSERT(h->gc_fwd != NULL);
    return h->gc_fwd;
}

static VALUE
fwd_value_compact(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)fwd_payload_compact(gc, (void *)v);
}

static void
fwd_edge_compact(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (IS_PTR(v)) *slot = (void *)(VALUE)fwd_payload_compact(gc, (void *)v);
}

static void
update_pointers_major(ASTroGC *gc, ASTroObjectHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, fwd_edge_compact);
}

static void
major_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;

    bool defer_fold = false;
    if (nursery_top != nursery_base) {
        size_t max_promotion = (size_t)(nursery_top - nursery_base);
        if (tenured_top + max_promotion > tenured_end) {
            defer_fold = true;
        } else {
            minor_gc(c, sp_top);
        }
    }
    remset_cnt = 0;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value_major(gc, *p);
    process_gray_major(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    char *fwd = tenured_base;
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)p;
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

    {
        char *p = tenured_base;
        while (p < tenured_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) update_pointers_major(gc, h);
            p += total;
        }
    }

    for (VALUE *p = c->env; p < sp_top; p++) *p = fwd_value_compact(gc, *p);

    {
        char *p = tenured_base;
        while (p < tenured_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (!HDR_MARKED(h)) {
                p += total;
                continue;
            }
            char *run_src = p;
            char *run_dst = h->gc_fwd;
            char *run_p   = p;
            while (run_p < tenured_top) {
                ASTroObjectHeader *rh = (ASTroObjectHeader *)run_p;
                if (!HDR_MARKED(rh)) break;
                run_p += ALIGN8(rh->gc_size);
            }
            size_t run_size = (size_t)(run_p - run_src);
            if (run_dst != run_src) memmove(run_dst, run_src, run_size);
            char *q = run_dst, *q_end = run_dst + run_size;
            while (q < q_end) {
                ASTroObjectHeader *qh = (ASTroObjectHeader *)q;
                HDR_CLR_MARKED(qh);
                qh->gc_fwd    = NULL;
                HDR_CLR_DIRTY(qh);
                q += ALIGN8(qh->gc_size);
            }
            p = run_p;
        }
    }
    tenured_top = fwd;

    if (defer_fold) {
        char *q = nursery_base;
        while (q < nursery_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)q;
            HDR_CLR_MARKED(h);
            q += ALIGN8(h->gc_size);
        }

        in_minor = true;
        to_base = tenured_base;
        char *old_tenured_top = tenured_top;
        to_top = tenured_top;
        from_base_cur = nursery_base;
        from_end_cur  = nursery_top;

        if (sp_high_water == NULL || sp_top > sp_high_water) {
            sp_high_water = sp_top;
        } else {
            for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
        }

        for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(gc, *p);

        {
            char *p = tenured_base;
            while (p < old_tenured_top) {
                ASTroObjectHeader *h = (ASTroObjectHeader *)p;
                process_object(gc, h);
                p += ALIGN8(h->gc_size);
            }
        }
        {
            char *scan = old_tenured_top;
            while (scan < to_top) {
                ASTroObjectHeader *h = (ASTroObjectHeader *)scan;
                process_object(gc, h);
                scan += ALIGN8(h->gc_size);
            }
        }
        tenured_top = to_top;
        nursery_top = nursery_base;
        in_minor = false;

        gc->common.stats.minor_count++;
    }
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
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    major_gc(c, sp_top);
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    if (nursery_base) munmap(nursery_base, NURSERY_BYTES);
    if (tenured_base) munmap(tenured_base, TENURED_BYTES);
    free(remset_buf);
    free(gray_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    return h->gc_size;
}
