// gc_mark_compact.c — backend #8: mark + compact in a single region.
//
// Layout: one mmap'd region (1 GiB virtual, lazy-paged).  Bump alloc.
// On collection: classic "Lisp 2" sliding compactor —
//
//   1. Mark live (BFS from roots through the gray queue).
//   2. Forward-address pass: walk region linearly; each marked header
//      records its packed-destination address into ->gc_fwd.
//   3. Update-pointers pass: walk region; for each live, rewrite outgoing
//      pointers (a->items, s->bytes, items[i] VALUEs) using the target's
//      ->gc_fwd field.  Roots are updated the same way.
//   4. Slide pass: walk region; for each live, memmove from src to ->gc_fwd.
//      Destination is always ≤ source (compaction), and src_{i+1} > dst_i
//      so no overlap between iterations.  region_top reset to the end of
//      the last slid object.
//
// vs gc_mark.c (linked-list mark&sweep): no per-object malloc/free, no
// linked-list traversal during sweep.  Cost: each major collection moves
// all live objects, so pointer-rewrite + memmove dominates when |live|
// is large.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止、 AroObjectHeader at offset 0、
 * fwd は head.gc_fwd (HAS_FWD) を再利用。 */
_Static_assert(sizeof(AroObjectHeader) == 16, "moving GC: head must be 16 B");

#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)

#define REGION_BYTES ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)    (((n) + 7u) & ~(size_t)7u)

/* Adaptive GC trigger: max(16 MiB, 2 × live_post_compact).  Previously
 * region-fill basis only (= effectively never with 64 GiB virtual). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

/* Large-object threshold.  Payloads >= this size go to a separately-
 * malloc'd non-moving region (gc_copy.c iter 66 — same rationale,
 * adapted for the Lisp-2 slide compactor: large objs don't slide, so
 * their pointers stay stable across collect.  Win: sieve / hash_chain
 * doubling pattern's dead large items get free'd promptly. */
#define LARGE_THRESHOLD      4096u

typedef struct LargeObj {
    struct LargeObj *next;        /* live list */
    /* payload follows: large_payload(lo) = lo + 1 */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

static inline LargeObj *
large_from_payload(void *p)
{
    return (LargeObj *)((char *)p - sizeof(LargeObj));
}

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    char *region_base, *region_top, *region_end;
    CTX  *ctx;
    size_t bytes_since_gc;
    size_t gc_threshold;
    struct AroObjectHeader **gray_buf;
    size_t     gray_cnt;
    size_t     gray_capa;
    LargeObj *large_head;
} ASTroGC;

#define region_base     (gc->region_base)
#define region_top      (gc->region_top)
#define region_end      (gc->region_end)
#define gc_ctx          (gc->ctx)
#define bytes_since_gc  (gc->bytes_since_gc)
#define gc_threshold    (gc->gc_threshold)
#define gray_buf        (gc->gray_buf)
#define gray_cnt        (gc->gray_cnt)
#define gray_capa       (gc->gray_capa)
#define large_head      (gc->large_head)

const char *aro_gc_backend_name = "mark_compact";

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    gc_threshold = GC_THRESHOLD_MIN;

    region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (region_base == MAP_FAILED) { perror("mmap"); abort(); }
    region_top = region_base;
    region_end = region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=mark_compact] STRESS mode: collect on every alloc\n");
    }
    if (getenv("BARUBY_GC_PURGE")) ARO_GC_COMMON(c)->purge = true;
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void gc_collect_internal(CTX *c);

static void __attribute__((noinline, cold))
bump_slow(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    gc_collect_internal(c);
    if (region_top + total > region_end) {
        fprintf(stderr, "baruby_gc=mark_compact: OOM (need %zu)\n", total);
        abort();
    }
}

static inline AroObjectHeader *
bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;
    if (__builtin_expect(gc->common.stress
                         || bytes_since_gc + gc->common.external_bytes + payload_size > gc_threshold
                         || region_top + total > region_end, 0)) {
        bump_slow(c, total);
    }
    AroObjectHeader *h = (AroObjectHeader *)region_top;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    region_top += total;
    bytes_since_gc += payload_size;
    return h;
}

static AroObjectHeader *large_alloc(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress
                         || bytes_since_gc + gc->common.external_bytes + payload_size > gc_threshold, 0)) {
        gc_collect_internal(c);
    }
    LargeObj *lo = (LargeObj *)malloc(sizeof(LargeObj) + aligned);
    if (!lo) { fprintf(stderr, "baruby_gc=mark_compact: large OOM (%zu)\n", payload_size); abort(); }
    lo->next = large_head;
    large_head = lo;
    AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    h->gc_fwd   = NULL;
    bytes_since_gc += payload_size;
    return h;
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    AroObjectHeader *h = __builtin_expect(payload_size >= LARGE_THRESHOLD, 0)
        ? large_alloc(c, payload_size, aligned)
        : bump       (c, payload_size, aligned);
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
    AroObjectHeader *h = __builtin_expect(payload_size >= LARGE_THRESHOLD, 0)
        ? large_alloc(c, payload_size, aligned)
        : bump       (c, payload_size, aligned);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* In-place realloc for large objs.  See gc_copy.c::aro_gc_realloc_in_place
 * for the full rationale.  This backend's GCHeader uses `uint8_t flags`
 * (bits 0-2 kind, bit 3 marked) and `uint32_t size` directly. */
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (gc->common.stress) return NULL;
    if (new_size < LARGE_THRESHOLD) return NULL;
    char *p = (char *)old;
    if (p >= region_base && p < region_top) return NULL;   /* small (in region) */

    LargeObj **link = &large_head;
    while (*link && large_payload(*link) != old) link = &(*link)->next;
    if (!*link) return NULL;
    LargeObj *lo = *link;

    size_t old_size = ((AroObjectHeader *)large_payload(lo))->gc_size;
    size_t old_aligned = ALIGN8(old_size);
    size_t new_aligned = ALIGN8(new_size);
    LargeObj *new_lo = (LargeObj *)realloc(lo, sizeof(LargeObj) + new_aligned);
    if (!new_lo) { perror("baruby_gc=mark_compact: realloc large"); abort(); }
    *link = new_lo;
    ((AroObjectHeader *)large_payload(new_lo))->gc_size = (uint32_t)new_size;

    /* Zero grown tail unconditionally — sample's OBJ_BYTE_DATA case in
     * SCAN_EDGES short-circuits anyway. */
    if (new_aligned > old_aligned) {
        memset((char *)large_payload(new_lo) + old_aligned, 0,
               new_aligned - old_aligned);
    }
    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        gc->common.stats.total_bytes += delta;
        gc->common.stats.heap_bytes  += delta;
        bytes_since_gc += delta;
    }
    return large_payload(new_lo);
}

// ---------------------------------------------------------------------------
// Mark phase
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
mark_value(ASTroGC *gc, VALUE v)
{
    if (!AROH_IS_GC_OBJECT(v)) return;
    AroObjectHeader *h = (AroObjectHeader *)v;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

/* edge_visit callback: ctx は ASTroGC *。 mark_value は VALUE 全般を
 * 受けて AROH_IS_GC_OBJECT check 後に payload を gray push する。 */
static void
mark_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    mark_value(gc, (VALUE)*slot);
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        AroObjectHeader *h = gray_buf[--gray_cnt];
        AROH_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge);
    }
}

// ---------------------------------------------------------------------------
// Pointer update (forward-address pass + update pass)
// ---------------------------------------------------------------------------

// Translate an old payload pointer to its compacted (new) payload pointer.
// iter 75 Step C: payload starts with head at offset 0, so h->gc_fwd
// (= the new head location) IS the new payload pointer.
static void *
fwd_payload(void *p)
{
    if (!p) return NULL;
    AroObjectHeader *h = (AroObjectHeader *)p;
    ASTRO_ASSERT(HDR_MARKED(h));
    ASTRO_ASSERT(h->gc_fwd != NULL);
    return h->gc_fwd;
}

/* edge_visit callback for writeback (compaction post-move pointer update).
 * SCAN_EDGES が KIND_OBJ_ARRAY / OBJ_STRING / PAYLOAD_VAL を統一して呼ぶので、
 * slot は raw pointer (= BaArray.items / BaString.bytes) と tagged VALUE
 * (= PAYLOAD_VAL の items[i]) のどちらでもあり得る。 AROH_IS_GC_OBJECT check で
 * 両方を fold。 */
static void
fwd_edge(void *ctx, void **slot)
{
    (void)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = (void *)(VALUE)fwd_payload((void *)v);
}

static void
update_pointers(AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, NULL, fwd_edge);
}

// ---------------------------------------------------------------------------
// Collect (Lisp 2 style)
// ---------------------------------------------------------------------------

static void
gc_collect_internal(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    // (1) Mark from roots.
    struct timespec tmark = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, mark_edge);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    // (2) Forward-address pass: pack live region objects to the start.
    // Large objs don't move; for them set h->gc_fwd to their own header
    // so fwd_payload (which adds sizeof(AroObjectHeader)) returns the same payload.
    char *fwd = region_base;
    size_t live_bytes = 0;
    {
        char *p = region_base;
        while (p < region_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) {
                h->gc_fwd = fwd;
                fwd += total;
                live_bytes += h->gc_size;
            } else {
                h->gc_fwd = NULL;
            }
            p += total;
        }
    }
    /* Large objs: marked → fwd = self-header (non-moving), so
     * fwd_payload returns the same payload pointer.  Unmarked stays
     * fwd=NULL and will be free'd in the sweep at end. */
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
        if (HDR_MARKED(h)) {
            h->gc_fwd = (char *)h;
            live_bytes += h->gc_size;
        } else {
            h->gc_fwd = NULL;
        }
    }

    // (3) Update outgoing pointers inside each live object (region + large).
    {
        char *p = region_base;
        while (p < region_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) update_pointers(h);
            p += total;
        }
    }
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        if (HDR_MARKED((AroObjectHeader *)large_payload(lo))) update_pointers((AroObjectHeader *)large_payload(lo));
    }

    // (4) Update roots.
    AROH_VISIT_ROOTS(c, gc, fwd_edge);

    /* Finalize pass.  At this point each live object's OLD-location header
     * still has HDR_MARKED set AND its gc_fwd field holds the destination
     * address (= post-slide new addr, OR self-addr for large objs).
     * Dead objects have gc_fwd == NULL.  We must run before (5) slide, as
     * the slide both clears HDR_MARKED/gc_fwd and memmoves the data away
     * from the OLD location. */
    aro_gc_finalize_walk(c);

    // (5) Slide live region objects to their forwarding addresses.
    //     Large objs don't slide — they stay in place.
    {
        char *p = region_base;
        while (p < region_top) {
            AroObjectHeader *h = (AroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (!HDR_MARKED(h)) {
                p += total;
                continue;
            }
            char *run_src_start = p;
            char *run_dst_start = h->gc_fwd;
            char *run_p = p;
            while (run_p < region_top) {
                AroObjectHeader *rh = (AroObjectHeader *)run_p;
                if (!HDR_MARKED(rh)) break;
                run_p += ALIGN8(rh->gc_size);
            }
            size_t run_size = (size_t)(run_p - run_src_start);
            if (run_dst_start != run_src_start) {
                memmove(run_dst_start, run_src_start, run_size);
            }
            char *q = run_dst_start;
            char *q_end = run_dst_start + run_size;
            while (q < q_end) {
                AroObjectHeader *qh = (AroObjectHeader *)q;
                HDR_CLR_MARKED(qh);
                qh->gc_fwd    = NULL;
                q += ALIGN8(qh->gc_size);
            }
            p = run_p;
        }
    }
    region_top = fwd;

    // (6) Sweep large_head: clear marked bit + fwd on survivors, free unmarked.
    {
        LargeObj **link = &large_head;
        while (*link) {
            LargeObj *lo = *link;
            AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
            if (HDR_MARKED(h)) {
                HDR_CLR_MARKED(h);
                h->gc_fwd = NULL;
                link = &lo->next;
            } else {
                *link = lo->next;
                free(lo);
            }
        }
    }
    aro_gc_phase_end(treclaim, &gc->common.stats.reclaim_seconds);

    gc->common.stats.heap_bytes = live_bytes;
    bytes_since_gc = 0;
    if (!gc->common.stress) {
        size_t next = live_bytes * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    gc_collect_internal(c);
}

/* Called between mark/forward-address pass and the slide.  At this point:
 *   live region obj    : HDR_MARKED + gc_fwd = post-slide addr
 *   live large obj     : HDR_MARKED + gc_fwd = self header (non-moving)
 *   dead either-kind   : !HDR_MARKED, gc_fwd = NULL
 * Return the new payload pointer (= gc_fwd) for live, NULL for dead. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    (void)c;
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (!HDR_MARKED(h)) return NULL;
    /* gc_fwd holds either the new addr (region) or self (large).  Both
     * are valid post-finalize entries; the slide will then move the
     * region data into place at the new addr. */
    return h->gc_fwd;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (region_base) munmap(region_base, REGION_BYTES);
    aro_gc_free_large_chain_malloc(large_head);
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
