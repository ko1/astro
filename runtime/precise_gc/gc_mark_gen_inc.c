// gc_mark_gen_inc.c — backend #4: gc_mark_gen + incremental major marking.
//
// Same slab/page heap as gc_mark_gen.c (9 size classes, 16 KiB pages,
// large-obj path).  Minor GC unchanged (STW).  Major splits into:
//
//   - inc_start_major: mark roots gray, set inc_marking=true.
//   - inc_step (called by mutator-side allocator): drain a fixed budget
//     of gray items.  Spreads mark over many tiny pauses.
//   - inc_finish_sweep: STW sweep when gray drains.
//
// SATB barrier: while inc_marking is active, WB records the OLD value
// being overwritten so a pointer reachable at cycle start stays
// reachable for the cycle.
//
// NB: INC_WORK_PER_ALLOC=SIZE_MAX makes the mark phase effectively STW
// (drains in one alloc tick).  True incremental needs VALUE stack WB
// too; without that the infrastructure stays but pauses are still long.
// The split into start/finish still gives 2-segment pause measurement
// (mark phase vs sweep phase), reducing max_pause_ms vs mark_gen even
// without real interleaving.

#define _GNU_SOURCE      /* mremap (no MAYMOVE); must precede stdio.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止、 ASTroObjectHeader が payload
 * offset 0。 backend は gc_flags の bit を使う。 */

#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_OLD_BIT      (uint16_t)0x0002u
#define HDR_DIRTY_BIT    (uint16_t)0x0004u
#define HDR_FREE_BIT     (uint16_t)0x0008u
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

typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

static inline FreeSlot *
free_slot_link(void *payload)
{
    return (FreeSlot *)((char *)payload + sizeof(ASTroObjectHeader));
}

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

#define NUM_SIZE_CLASSES 9
static const size_t size_class_bytes[NUM_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 3072, 4096
};

#define PAGE_SIZE       (16u * 1024u)
#define PAGE_HDR_BYTES  16

typedef struct Page {
    struct Page *next;
    uint16_t class_idx;
    uint16_t _pad0;
    uint32_t _pad1;
} Page;
_Static_assert(sizeof(Page) == PAGE_HDR_BYTES, "Page header size mismatch");

typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    /* payload follows */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

#define MAJOR_THRESHOLD_MIN  (16u * 1024u * 1024u)

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    Page     *page_head[NUM_SIZE_CLASSES];
    FreeSlot *freelist[NUM_SIZE_CLASSES];
    LargeObj *large_head;
    struct ASTroObjectHeader **young_objs;
    size_t            young_objs_cnt;
    size_t            young_objs_capa;
    size_t young_bytes;
    size_t old_bytes;
    size_t young_threshold;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    CTX  *ctx;
    bool  in_minor;
    bool  inc_marking;
    struct ASTroObjectHeader **gray_buf;
    size_t            gray_cnt;
    size_t            gray_capa;
    struct ASTroObjectHeader **remset_buf;
    size_t            remset_cnt;
    size_t            remset_capa;
    bool              remset_overflow;
} ASTroGC;

/* Field aliases — expand to `gc->field`; every helper has `ASTroGC *gc`
 * in scope (no module-static).  See gc_mark_gen.c for the pattern. */
#define page_head             (gc->page_head)
#define freelist              (gc->freelist)
#define large_head            (gc->large_head)
#define young_objs            (gc->young_objs)
#define young_objs_cnt        (gc->young_objs_cnt)
#define young_objs_capa       (gc->young_objs_capa)
#define young_bytes           (gc->young_bytes)
#define old_bytes             (gc->old_bytes)
#define young_threshold       (gc->young_threshold)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gc_ctx                (gc->ctx)
#define in_minor              (gc->in_minor)
#define inc_marking           (gc->inc_marking)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)

static inline void
young_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (young_objs_cnt >= young_objs_capa) {
        young_objs_capa = young_objs_capa ? young_objs_capa * 2 : 1024;
        young_objs = (ASTroObjectHeader **)realloc(young_objs, young_objs_capa * sizeof(ASTroObjectHeader *));
        if (!young_objs) abort();
    }
    young_objs[young_objs_cnt++] = h;
}

const char *aro_gc_backend_name = "mark_gen_inc";

// Incremental mark state.  See header comment.
static const size_t INC_WORK_PER_ALLOC = (size_t)-1;

static void inc_start_major(CTX *c, VALUE *sp_top);
static void inc_step(ASTroGC *gc, size_t budget);
static void inc_finish_sweep(CTX *c, VALUE *sp_top);
static void gray_push(ASTroGC *gc, ASTroObjectHeader *h);
static void mark_value(ASTroGC *gc, VALUE v);
static void scan_outgoing(ASTroGC *gc, ASTroObjectHeader *h);
static void mark_value_satb(ASTroGC *gc, VALUE v);

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    young_threshold     = 16u * 1024u * 1024u;
    old_major_threshold = MAJOR_THRESHOLD_MIN;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        young_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark_gen_inc] STRESS\n");
    }
}

// ---------------------------------------------------------------------------
// Slab management — identical to gc_mark_gen.c
// ---------------------------------------------------------------------------

static int
size_class_for(size_t slot_total)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (slot_total <= size_class_bytes[i]) return i;
    }
    return -1;
}

static void
new_page(ASTroGC *gc, int class_idx)
{
    void *raw = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    Page *p = (Page *)raw;
    p->class_idx = (uint16_t)class_idx;
    p->next = page_head[class_idx];
    page_head[class_idx] = p;

    // Populate freelist HIGH → LOW so pop returns LOW → HIGH (memory
    // order, friendly to the prefetcher).
    size_t sb = size_class_bytes[class_idx];
    size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
    char *slot = (char *)p + PAGE_HDR_BYTES + (n_slots - 1) * sb;
    for (size_t i = 0; i < n_slots; i++) {
        ASTroObjectHeader *h = (ASTroObjectHeader *)slot;
        HDR_SET_FREE(h); h->flags = 0;
        /* size / marked / old / dirty already 0 from mmap zero. */
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[class_idx];
        freelist[class_idx] = fs;
        slot -= sb;
    }
}

static ASTroObjectHeader *slab_alloc(ASTroGC *gc, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(gc, class_idx);
    FreeSlot *fs = freelist[class_idx];
    freelist[class_idx] = fs->next;
    ASTroObjectHeader *h = (ASTroObjectHeader *)fs;
    h->flags    = 0;     /* sample sets later */
    h->gc_flags = 0;     /* clear FREE bit + all gen bits */
    h->gc_size  = (uint32_t)payload_size;
    young_push(gc, h);
    young_bytes += ALIGN8(payload_size);
    return h;
}

static ASTroObjectHeader *large_alloc(ASTroGC *gc, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + ALIGN8(payload_size);
    size_t map_bytes = (need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    young_push(gc, h);
    young_bytes += ALIGN8(payload_size);
    return h;
}

static void
free_slot(ASTroGC *gc, ASTroObjectHeader *h)
{
    size_t total = ALIGN8(h->gc_size);
    int cls = size_class_for(total);
    if (cls >= 0) {
        HDR_SET_FREE(h); h->flags = 0;
        h->gc_size = 0;
        /* Clear all gen bits so slab_alloc invariant holds. */
        HDR_CLR_MARKED(h);
        HDR_CLR_OLD(h);
        HDR_CLR_DIRTY(h);
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[cls];
        freelist[cls] = fs;
    } else {
        LargeObj **link = &large_head;
        while (*link) {
            LargeObj *lo = *link;
            if ((ASTroObjectHeader *)large_payload(lo) == h) {
                *link = lo->next;
                munmap(lo, lo->map_bytes);
                return;
            }
            link = &lo->next;
        }
        ASTRO_ASSERT(0 && "large_obj not found");
    }
}

// ---------------------------------------------------------------------------
// Alloc API + incremental tick
// ---------------------------------------------------------------------------

static void minor_gc(CTX *c, VALUE *sp_top);

// iter 45: cold-split — pull threshold-triggered collect out of inline
// path.  inc_marking step stays inline since it runs every alloc when
// active.
static void __attribute__((noinline, cold))
maybe_collect_slow(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!inc_marking && old_alloc_since_major > old_major_threshold) {
        inc_start_major(c, sp_top);
        old_alloc_since_major = 0;
    } else {
        minor_gc(c, sp_top);
    }
}

static inline void
maybe_collect(CTX *c, size_t add, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (inc_marking) {
        inc_step(gc, INC_WORK_PER_ALLOC);
        if (!inc_marking) {
            inc_finish_sweep(c, sp_top);
        }
    }
    if (__builtin_expect(gc->common.stress || young_bytes + add > young_threshold, 0)) {
        maybe_collect_slow(c, sp_top);
    }
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    maybe_collect(c, ALIGN8(payload_size), sp_top);
    size_t slot_total = ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    ASTroObjectHeader *h = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                           : large_alloc(gc, payload_size);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Zero post-head region only (head was init'd by slab/large_alloc). */
    memset((char *)payload + sizeof(ASTroObjectHeader), 0,
           ALIGN8(payload_size) - sizeof(ASTroObjectHeader));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    maybe_collect(c, ALIGN8(payload_size), sp_top);
    size_t slot_total = ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    ASTroObjectHeader *h = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                           : large_alloc(gc, payload_size);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Byte payloads skip post-head zero-fill. */
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier with SATB during inc_marking
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

static void scan_outgoing(ASTroGC *gc, ASTroObjectHeader *h);
static void
remset_visit_minor(ASTroGC *gc, ASTroObjectHeader *h)
{
    HDR_CLR_DIRTY(h);
    scan_outgoing(gc, h);
}

static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, ASTroObjectHeader *))
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        size_t sb = size_class_bytes[cls];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[cls]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                ASTroObjectHeader *h = (ASTroObjectHeader *)slot;
                if (HDR_IS_FREE(h)) continue;
                if (HDR_OLD(h) && HDR_DIRTY(h)) visit(gc, h);
            }
        }
    }
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
        if (HDR_OLD(h) && HDR_DIRTY(h)) visit(gc, h);
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (inc_marking) {
        VALUE old = *slot;
        if (IS_PTR(old)) mark_value_satb(gc, old);
    }
    *slot = v;
    if (holder == NULL) return;
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (inc_marking) {
        for (size_t i = 0; i < n; i++) {
            VALUE old = dst[i];
            if (IS_PTR(old)) mark_value_satb(gc, old);
        }
    }
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

// ---------------------------------------------------------------------------
// Mark phase
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
mark_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)v;
    if (HDR_IS_FREE(h)) return;   /* defensive: stale sp slot to freed obj */
    if (HDR_MARKED(h)) return;
    if (in_minor && HDR_OLD(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

// SATB barrier: mark v regardless of minor/major filter.  inc_marking
// only fires during major-style passes, so no in_minor guard needed.
static void
mark_value_satb(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)v;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

/* edge_visit callback for ASTRO_GC_SCAN_EDGES.  `ctx` is `ASTroGC *gc`. */
static void
mark_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    mark_value(gc, (VALUE)*slot);
}

static void
scan_outgoing(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (1) {
        ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge);
    }
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        ASTroObjectHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(gc, h);
    }
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

static void
sweep_young(ASTroGC *gc, bool clear_marked)
{
    young_bytes = 0;
    for (size_t i = 0; i < young_objs_cnt; i++) {
        ASTroObjectHeader *h = young_objs[i];
        if (HDR_MARKED(h)) {
            if (clear_marked) HDR_CLR_MARKED(h);
            HDR_SET_OLD(h);
            old_bytes += h->gc_size;
            old_alloc_since_major += ALIGN8(h->gc_size); /* iter 36 fairness: occupancy not payload */
        } else {
            gc->common.stats.heap_bytes -= h->gc_size;
            free_slot(gc, h);
        }
    }
    young_objs_cnt = 0;
}

static void
sweep_old_pages(ASTroGC *gc)
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        size_t sb = size_class_bytes[cls];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[cls]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                ASTroObjectHeader *h = (ASTroObjectHeader *)slot;
                if (HDR_IS_FREE(h)) continue;
                if (!HDR_OLD(h)) continue;
                if (HDR_MARKED(h)) {
                    HDR_CLR_MARKED(h);
                    HDR_CLR_DIRTY(h);
                } else {
                    old_bytes -= h->gc_size;
                    gc->common.stats.heap_bytes -= h->gc_size;
                    free_slot(gc, h);
                }
            }
        }
    }
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
        if (!HDR_OLD(h)) { link = &lo->next; continue; }
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            HDR_CLR_DIRTY(h);
            link = &lo->next;
        } else {
            *link = lo->next;
            old_bytes -= h->gc_size;
            gc->common.stats.heap_bytes -= h->gc_size;
            munmap(lo, lo->map_bytes);
        }
    }
}

// ---------------------------------------------------------------------------
// Collection drivers
// ---------------------------------------------------------------------------

static void
minor_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;

    struct timespec tmark = aro_gc_phase_begin();
    aro_gc_visit_roots(c, gc, mark_edge);
    process_gray(gc);

    if (remset_overflow) {
        remset_heap_walk(gc, remset_visit_minor);
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            ASTroObjectHeader *h = remset_buf[i];
            HDR_CLR_DIRTY(h);
            scan_outgoing(gc, h);
        }
    }
    remset_cnt = 0;
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    struct timespec tsweep = aro_gc_phase_begin();
    sweep_young(gc, /*clear_marked=*/true);
    aro_gc_phase_end(tsweep, &gc->common.stats.reclaim_seconds);

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    in_minor = false;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

static void
inc_start_major(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    inc_marking = true;
    remset_cnt = 0;
    struct timespec tmark = aro_gc_phase_begin();
    aro_gc_visit_roots(c, gc, mark_edge);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

static void
inc_step(ASTroGC *gc, size_t budget)
{
    /* iter 35 fairness fix: inc_step is the *bulk of marking work* on the
     * allocator path.  Without timing it, gc_seconds and max_pause_ms
     * underreport the collector cost — comparing mark_gen_inc to mark_gen
     * on those numbers is misleading.  Counted under aro_gc_time_begin so
     * it accumulates into total_seconds, and under mark_seconds so the
     * phase split is honest. */
    struct timespec t0 = aro_gc_time_begin(gc->ctx);
    struct timespec tmark = aro_gc_phase_begin();
    while (gray_cnt > 0 && budget > 0) {
        ASTroObjectHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(gc, h);
        budget--;
    }
    if (gray_cnt == 0) inc_marking = false;
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);
    aro_gc_time_end(gc->ctx, t0);
}

static void
inc_finish_sweep(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    // Re-scan roots before sweeping.  Objects allocated during the
    // inc_marking window may have been stored into the VALUE stack
    // by the mutator without going through any write barrier (we
    // only have heap-to-heap WB, not stack WB).  Without this re-scan
    // they would be unmarked-young and freed by sweep_young.
    struct timespec tmark = aro_gc_phase_begin();
    aro_gc_visit_roots(c, gc, mark_edge);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    struct timespec tsweep = aro_gc_phase_begin();
    sweep_young(gc, /*clear_marked=*/false);
    sweep_old_pages(gc);
    aro_gc_phase_end(tsweep, &gc->common.stats.reclaim_seconds);
    if (!gc->common.stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;
    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    // External full GC: STW major (skip incremental dance).
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    inc_marking = false;
    remset_cnt = 0;
    aro_gc_visit_roots(c, gc, mark_edge);
    process_gray(gc);
    sweep_young(gc, /*clear_marked=*/false);
    sweep_old_pages(gc);
    if (!gc->common.stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;
    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}


void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        Page *p = page_head[cls];
        while (p) {
            Page *next = p->next;
            munmap(p, PAGE_SIZE);
            p = next;
        }
    }
    aro_gc_free_large_chain_mmap(large_head);
    free(young_objs);
    free(gray_buf);
    free(remset_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    return h->gc_size;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (size_class_for(ALIGN8(n)) >= 0)
#define ARO_GC_INPLACE_PAGE_SIZE         PAGE_SIZE
#define ARO_GC_INPLACE_MREMAP_FLAGS      0
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (young_bytes += (d))
#undef large_head
#include "gc_inplace_mremap.h"
#define large_head (gc->large_head)
