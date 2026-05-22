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
//   When free: GCHeader.kind = KIND_FREE and payload[0..7] = next free-link.
//
// Allocation: round payload up to size class, pop slot from freelist.
// If freelist empty, alloc new page (mmap) and seed freelist with its slots.
// Allocations larger than the largest size class go to a "large objects"
// linked list (each large obj is its own mmap region; still no malloc).
//
// Mark phase: scan VALUE stack roots c->env..sp_top, gray queue traces
// outgoing refs.  Same as the old linked-list version.
//
// Sweep phase: walk all pages, for each slot check `marked` (skip
// KIND_FREE slots).  Unmarked → kind = KIND_FREE + push to freelist.
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

// 16-byte header, payload follows.  No prev/next linked list — sweep
// walks pages instead.
/* 8-byte header.  kind has only ~5 values (fits in 3 bits) and `marked`
 * is 1 bit — all packed into a single `flags` byte.  Brings GCHeader
 * from 16 → 8 B, so BaArray (24 B payload) fits class 32 exactly (no
 * waste) — same density win mark_bitmap_gen got via per-page bitmaps,
 * but without the bitmap machinery. */
typedef struct GCHeader {
    uint8_t  flags;    /* bit 0-2: kind (KIND_*); bit 3: marked */
    uint8_t  _pad[3];  /* size's 4-byte alignment */
    uint32_t size;     /* requested payload bytes */
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_MARKED_BIT   0x08u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))
#define HDR_MARKED(h)      (((h)->flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->flags &= (uint8_t)~HDR_MARKED_BIT)

// Free slot overlay: when a slot is unallocated, kind = KIND_FREE and
// the payload area holds a FreeSlot link.  freelist[class] points to
// the payload of a free slot (= same address callers would receive
// from `aro_gc_alloc`).
typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

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
// for sweep + munmap-on-free.
typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;  // bytes passed to mmap (for munmap on free)
    // GCHeader follows
} LargeObj;

#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  Heap-allocated in aro_gc_init.
// `common` MUST be first field — contract for ASTRO_GC_COMMON(c) cast.
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
    GCHeader **gray_buf;
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
// class's freelist.
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
        GCHeader *h = (GCHeader *)slot;
        HDR_SET_KIND(h, KIND_FREE);
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = gc->freelist[class_idx];
        gc->freelist[class_idx] = fs;
        slot -= sb;
    }
}

static void *
slab_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size, int class_idx)
{
    if (!gc->freelist[class_idx]) new_page(gc, class_idx);
    FreeSlot *fs = gc->freelist[class_idx];
    gc->freelist[class_idx] = fs->next;
    void *payload = (void *)fs;
    GCHeader *h = (GCHeader *)payload - 1;
    HDR_SET_KIND(h, kind);
    ASTRO_GC_HEADER_SET_SIZE(h, payload_size);
    return payload;
}

static void *
large_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    size_t map_bytes = (need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = gc->large_head;
    lo->map_bytes = map_bytes;
    gc->large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    HDR_SET_KIND(h, kind);
    ASTRO_GC_HEADER_SET_SIZE(h, payload_size);
    return (void *)(h + 1);
}

static void gc_collect_internal(CTX *c, VALUE *sp_top);

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    if (gc->common.stress || gc->bytes_since_gc + payload_size > gc->gc_threshold) {
        gc_collect_internal(c, sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    void *payload = (cls >= 0) ? slab_alloc(gc, kind, payload_size, cls)
                               : large_alloc(gc, kind, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_PAYLOAD(payload, ALIGN8(payload_size));
    gc->bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (gc->common.stress || gc->bytes_since_gc + payload_size > gc->gc_threshold) {
        gc_collect_internal(c, sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    void *payload = (cls >= 0) ? slab_alloc(gc, KIND_PAYLOAD_BYTE, payload_size, cls)
                               : large_alloc(gc, KIND_PAYLOAD_BYTE, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_BYTE_PAYLOAD(payload, ALIGN8(payload_size));
    gc->bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define large_head                       gc->large_head
#define ARO_GC_INPLACE_THRESHOLD(n)      (size_class_for(sizeof(GCHeader) + ALIGN8(n)) >= 0)
#define ARO_GC_INPLACE_PAGE_SIZE         PAGE_SIZE
#define ARO_GC_INPLACE_MREMAP_FLAGS      MREMAP_MAYMOVE
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (gc->bytes_since_gc += (d))
#include "gc_inplace_mremap.h"
#undef large_head

// ---------------------------------------------------------------------------
// Mark phase
// ---------------------------------------------------------------------------

static void
gray_push(ASTroGC *gc, GCHeader *h)
{
    if (gc->gray_cnt >= gc->gray_capa) {
        gc->gray_capa = gc->gray_capa ? gc->gray_capa * 2 : 256;
        gc->gray_buf = (GCHeader **)realloc(gc->gray_buf, gc->gray_capa * sizeof(GCHeader *));
        if (!gc->gray_buf) abort();
    }
    gc->gray_buf[gc->gray_cnt++] = h;
}

static void
mark_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

/* edge_visit callback for ASTRO_GC_SCAN_EDGES.  `ctx` is `ASTroGC *gc`. */
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
        GCHeader *h = gc->gray_buf[--gc->gray_cnt];
        ASTRO_GC_SCAN_EDGES(h, gc, mark_edge);
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
                GCHeader *h = (GCHeader *)slot;
                if (HDR_KIND(h) == KIND_FREE) continue;
                if (HDR_MARKED(h)) {
                    HDR_CLR_MARKED(h);
                } else {
                    gc->common.stats.heap_bytes -= ASTRO_GC_HEADER_SIZE(h);
                    HDR_SET_KIND(h, KIND_FREE);
                    FreeSlot *fs = (FreeSlot *)(h + 1);
                    fs->next = gc->freelist[cls];
                    gc->freelist[cls] = fs;
                }
            }
        }
    }
    LargeObj **link = &gc->large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            link = &lo->next;
        } else {
            *link = lo->next;
            gc->common.stats.heap_bytes -= ASTRO_GC_HEADER_SIZE(h);
            munmap(lo, lo->map_bytes);
        }
    }
}

static void
gc_collect_internal(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(gc, *p);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

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
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    gc_collect_internal(c, sp_top);
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        Page *p = gc->page_head[cls];
        while (p) {
            Page *next = p->next;
            munmap(p, PAGE_SIZE);
            p = next;
        }
    }
    LargeObj *lo = gc->large_head;
    while (lo) {
        LargeObj *next = lo->next;
        munmap(lo, lo->map_bytes);
        lo = next;
    }
    free(gc->gray_buf);
    free(gc);
    c->astro_gc = NULL;
}


AroGcKind
aro_gc_kind_of(void *p)
{
    GCHeader *h = (GCHeader *)p - 1;
    return HDR_KIND(h);
}

size_t
aro_gc_size_of(void *p)
{
    GCHeader *h = (GCHeader *)p - 1;
    return h->size;
}
