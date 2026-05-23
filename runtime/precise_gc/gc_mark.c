// gc_mark.c — backend #2: non-moving mark&sweep with slab-style page heap.
//
// Heap is provided by the GC itself, not libc malloc.  Pages are 16 KiB
// chunks obtained via mmap and divided into fixed-size slots (the size
// class).  Each size class has its own page chain + freelist.  This
// removes the per-object linked list and the per-alloc malloc roundtrip
// of the older "malloc + prev/next" design.  Architecturally similar to
// CRuby's heap pages.
//
// Layout:
//   Page = { Page *next; uint16_t class_idx; pad; slot[0]; slot[1]; ... }
//   Slot = { GCHeader; payload[slot_payload_bytes] }
//   When free: head.gc_flags has HDR_FREE_BIT, and bytes[8..15] (after head)
//   hold the FreeSlot.next link.
//
// Allocation: round payload up to size class, pop slot from freelist.
// If freelist empty, alloc new page (mmap) and seed freelist with its slots.
// Allocations larger than the largest size class go to a "large objects"
// linked list (each large obj is its own mmap region; still no malloc).
//
// Mark phase: scan sample roots via AROH_VISIT_ROOTS, gray queue traces
// outgoing refs.  Same as the old linked-list version.
//
// Sweep phase: walk all pages, for each slot check `marked` (skip
// HDR_FREE_BIT slots).  Unmarked → set HDR_FREE_BIT + push to freelist.
// Marked → clear marked bit.  Also walk the large-objects list with
// the same logic; freed large objects munmap their region back.
//
// Stress mode: collect on every alloc.  Non-moving = no mprotect tricks.

#define _GNU_SOURCE      /* mremap, MREMAP_MAYMOVE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止。 AroObjectHeader (= sample
 * struct head field) が payload offset 0 にあり、 backend は
 * head.gc_flags で MARKED / FREE bit を管理。 sample が type tag を
 * head.flags で持つので framework の kind 識別は不要。 */

#define HDR_MARKED_BIT   (uint16_t)0x0001u   /* bit 0 of gc_flags: marked */
#define HDR_FREE_BIT     (uint16_t)0x0002u   /* bit 1 of gc_flags: free slot marker */
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_IS_FREE(h)     (((h)->gc_flags & HDR_FREE_BIT) != 0)
#define HDR_SET_FREE(h)    ((h)->gc_flags |= HDR_FREE_BIT)
#define HDR_CLR_FREE(h)    ((h)->gc_flags &= (uint16_t)~HDR_FREE_BIT)

// Free slot overlay: when a slot is unallocated, head.gc_flags has the
// FREE bit set and the bytes at payload+sizeof(AroObjectHeader) hold
// the FreeSlot link.  freelist[class] points to the payload of a free
// slot (= same address callers would receive from `aro_gc_alloc`).
typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

static inline FreeSlot *
free_slot_link(void *payload)
{
    return (FreeSlot *)((char *)payload + sizeof(AroObjectHeader));
}

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

// Size classes (slot total, header included).  Covers payload sizes up
// to slot - sizeof(GCHeader).  Smallest 32-byte slot fits a 16-byte
// payload (e.g., empty PAYLOAD_VAL or 2-entry items).  Largest 4 KiB
// in-page slot covers most "medium" allocations; bigger ones go to the
// large-object path (one mmap per large object).
//
// Slot sizes chosen so common allocations land on a tight class:
//   BaArray (24 B payload + 16 B header = 40)  → class 64 (40% waste)
//   items[4] (32 B + 16 = 48)                  → class 64 (25% waste)
//   items[8] (64 B + 16 = 80)                  → class 128 (38% waste)
//   bytes "abc" (4 B + 16 = 20)                → class 32 (38% waste)
//
// Trade-off: more classes = less waste but more bookkeeping.  9 classes
// keep it simple while covering the bulk of allocations.
#define NUM_SIZE_CLASSES 9
static const size_t size_class_bytes[NUM_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 3072, 4096
};

// Page geometry.
#define PAGE_SIZE       (16u * 1024u)
#define PAGE_HDR_BYTES  16

typedef struct Page {
    struct Page *next;       // next page in same-class chain (and the "all pages" walk)
    uint16_t class_idx;
    uint16_t _pad0;
    uint32_t _pad1;
} Page;
_Static_assert(sizeof(Page) == PAGE_HDR_BYTES, "Page header size mismatch");

// Large objects (> max slot) are each their own mmap region.  Linked
// for sweep + munmap-on-free.  Payload (= sample struct with head at
// offset 0) immediately follows the metadata.
typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;  // bytes passed to mmap (for munmap on free)
    /* payload follows: (void *)(lo + 1) */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  Heap-allocated in aro_gc_init.
// `common` MUST be first field — contract for ARO_GC_COMMON(c) cast.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;
    Page     *page_head[NUM_SIZE_CLASSES];   // pages of each class chain
    FreeSlot *freelist[NUM_SIZE_CLASSES];    // free slots, head per class
    LargeObj *large_head;
    size_t bytes_since_gc;
    size_t gc_threshold;
    CTX   *ctx;
    /* Gray queue for mark traversal */
    AroObjectHeader **gray_buf;
    size_t     gray_cnt;
    size_t     gray_capa;
} ASTroGC;

const char *aro_gc_backend_name = "mark";

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    gc->ctx = c;
    gc->gc_threshold = GC_THRESHOLD_MIN;
    c->astro_gc = gc;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        gc->gc_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark] STRESS mode: collect on every alloc\n");
    }
    if (getenv("BARUBY_GC_PURGE")) ARO_GC_COMMON(c)->purge = true;
}

// ---------------------------------------------------------------------------
// Page / freelist management
// ---------------------------------------------------------------------------

static inline int
size_class_for(size_t slot_total)
{
    // iter 44: O(1) clz-based dispatch.  Classes are 32, 64, 128, 256, 512,
    // 1024, 2048, 3072, 4096 — pure powers of 2 except 3072.  ceil-log2
    // maps directly to class index minus 5, with a one-line adjust for
    // the 3072 irregularity.  Shrinks aro_gc_alloc body ~30 bytes vs the
    // 9-cmp unrolled linear scan, helping gcc consider inlining
    // aro_gc_alloc into baruby_ary_new.
    if (slot_total <= 32) return 0;
    if (slot_total > 4096) return -1;
    int bits = 64 - __builtin_clzll(slot_total - 1);
    int c = bits - 5;
    if (c == 7 && slot_total > 3072) c = 8;
    return c;
}

// Allocate a fresh page (mmap), divide into slots, push them to the
// class's freelist.  Each slot is sb bytes total = payload area.  Head
// at offset 0 carries the FREE bit; FreeSlot link is at payload+8.
static void
new_page(ASTroGC *gc, int class_idx)
{
    void *raw = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    Page *p = (Page *)raw;
    p->class_idx = (uint16_t)class_idx;
    p->next = gc->page_head[class_idx];
    gc->page_head[class_idx] = p;

    size_t sb = size_class_bytes[class_idx];
    size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
    char *slot = (char *)p + PAGE_HDR_BYTES + (n_slots - 1) * sb;
    for (size_t i = 0; i < n_slots; i++) {
        AroObjectHeader *h = (AroObjectHeader *)slot;
        h->flags    = 0;
        h->gc_flags = HDR_FREE_BIT;
        h->gc_size  = 0;
        free_slot_link(slot)->next = gc->freelist[class_idx];
        gc->freelist[class_idx] = (FreeSlot *)slot;
        slot -= sb;
    }
}

static void *
slab_alloc(ASTroGC *gc, size_t payload_size, int class_idx)
{
    if (!gc->freelist[class_idx]) new_page(gc, class_idx);
    void *payload = (void *)gc->freelist[class_idx];
    gc->freelist[class_idx] = free_slot_link(payload)->next;
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return payload;
}

static void *
large_alloc(ASTroGC *gc, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + ALIGN8(payload_size);
    size_t map_bytes = (need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = gc->large_head;
    lo->map_bytes = map_bytes;
    gc->large_head = lo;
    void *payload = large_payload(lo);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return payload;
}

static void gc_collect_internal(CTX *c);

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (gc->common.stress || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold) {
        gc_collect_internal(c);
    }
    size_t aligned = ALIGN8(payload_size);
    int cls = size_class_for(aligned);
    void *payload = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                               : large_alloc(gc, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Zero post-head region for GC-scan safety. */
    memset((char *)payload + sizeof(AroObjectHeader), 0,
           aligned - sizeof(AroObjectHeader));
    gc->bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (gc->common.stress || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold) {
        gc_collect_internal(c);
    }
    size_t aligned = ALIGN8(payload_size);
    int cls = size_class_for(aligned);
    void *payload = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                               : large_alloc(gc, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Byte payloads skip post-head zero-fill (caller writes immediately). */
    gc->bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (size_class_for(ALIGN8(n)) >= 0)
#define ARO_GC_INPLACE_PAGE_SIZE         PAGE_SIZE
#define ARO_GC_INPLACE_MREMAP_FLAGS      MREMAP_MAYMOVE
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (gc->bytes_since_gc += (d))
#include "gc_inplace_mremap.h"

// ---------------------------------------------------------------------------
// Mark phase
// ---------------------------------------------------------------------------

static void
gray_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (gc->gray_cnt >= gc->gray_capa) {
        gc->gray_capa = gc->gray_capa ? gc->gray_capa * 2 : 256;
        gc->gray_buf = (AroObjectHeader **)realloc(gc->gray_buf,
                                                     gc->gray_capa * sizeof(AroObjectHeader *));
        if (!gc->gray_buf) abort();
    }
    gc->gray_buf[gc->gray_cnt++] = h;
}

static void
mark_value(ASTroGC *gc, VALUE v)
{
    if (!AROH_IS_GC_OBJECT(v)) return;
    AroObjectHeader *h = (AroObjectHeader *)v;
    /* Defensive: a stale sp slot pointing to a freed slot must not be
     * promoted to live.  Old design recognised this via kind==FREE; new
     * design uses HDR_FREE_BIT in gc_flags. */
    if (HDR_IS_FREE(h)) return;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

/* edge_visit callback for AROH_SCAN_EDGES.  `ctx` is `ASTroGC *gc`. */
static void
mark_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    mark_value(gc, v);
}

static void
process_gray(ASTroGC *gc)
{
    while (gc->gray_cnt > 0) {
        AroObjectHeader *h = gc->gray_buf[--gc->gray_cnt];
        AROH_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge);
    }
}

// ---------------------------------------------------------------------------
// Sweep phase: walk pages + large objects, freelist unmarked, clear bits.
// ---------------------------------------------------------------------------

static void
sweep(ASTroGC *gc)
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        size_t sb = size_class_bytes[cls];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = gc->page_head[cls]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                AroObjectHeader *h = (AroObjectHeader *)slot;
                if (HDR_IS_FREE(h)) continue;
                if (HDR_MARKED(h)) {
                    HDR_CLR_MARKED(h);
                } else {
                    gc->common.stats.heap_bytes -= h->gc_size;
                    h->flags    = 0;       /* clear stale sample type tag */
                    h->gc_flags = HDR_FREE_BIT;
                    h->gc_size  = 0;
                    free_slot_link(slot)->next = gc->freelist[cls];
                    gc->freelist[cls] = (FreeSlot *)slot;
                }
            }
        }
    }
    LargeObj **link = &gc->large_head;
    while (*link) {
        LargeObj *lo = *link;
        AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            link = &lo->next;
        } else {
            *link = lo->next;
            gc->common.stats.heap_bytes -= h->gc_size;
            munmap(lo, lo->map_bytes);
        }
    }
}

static void
gc_collect_internal(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    struct timespec tmark = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, mark_edge);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    /* Finalize pass: between mark and sweep, before unmarked slots get
     * recycled.  Live = MARKED; dead = !MARKED. */
    aro_gc_finalize_walk(c);

    struct timespec tsweep = aro_gc_phase_begin();
    sweep(gc);
    aro_gc_phase_end(tsweep, &gc->common.stats.reclaim_seconds);

    gc->common.stats.gc_count++;
    gc->bytes_since_gc = 0;
    if (!gc->common.stress) {
        size_t live = gc->common.stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc->gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    gc_collect_internal(c);
}

void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    (void)c;
    AroObjectHeader *h = (AroObjectHeader *)payload;
    return HDR_MARKED(h) ? payload : NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        Page *p = gc->page_head[cls];
        while (p) {
            Page *next = p->next;
            munmap(p, PAGE_SIZE);
            p = next;
        }
    }
    aro_gc_free_large_chain_mmap(gc->large_head);
    free(gc->gray_buf);
    free(gc);
    c->astro_gc = NULL;
}


size_t
aro_gc_size_of(void *p)
{
    return ((AroObjectHeader *)p)->gc_size;
}
