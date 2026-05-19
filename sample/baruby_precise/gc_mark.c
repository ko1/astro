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

static Page     *page_head[NUM_SIZE_CLASSES];   // pages of each class chain
static FreeSlot *freelist[NUM_SIZE_CLASSES];    // free slots, head per class
static Page     *all_pages = NULL;              // for sweep iteration (all pages, any class)
static LargeObj *large_head = NULL;

static size_t bytes_since_gc = 0;
// Adaptive threshold (same logic as before adaptive-fix).  After each
// sweep, threshold becomes max(MIN, 2 * live_bytes_post_sweep).  Saves
// ~50× GCs vs fixed 4 MiB on long-lived workloads.
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2
static size_t gc_threshold = GC_THRESHOLD_MIN;
static CTX   *gc_ctx       = NULL;

// Gray queue for mark traversal.
static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "mark";

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        gc_threshold = 0;
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
new_page(int class_idx)
{
    void *raw = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    Page *p = (Page *)raw;
    p->class_idx = (uint16_t)class_idx;
    p->next = page_head[class_idx];
    page_head[class_idx] = p;
    // Also link into all-pages chain (we use a separate field?  no — use
    // the same next: page chain per class IS the sweep iteration since
    // we walk all classes).  See sweep() below.

    // Populate freelist HIGH → LOW so pop returns LOW → HIGH (= memory
    // order).  This gives the prefetcher a forward sequential pattern
    // on consecutive allocs from this page, matching mark.c's previous
    // malloc-based allocator's roughly-sequential addresses.
    size_t sb = size_class_bytes[class_idx];
    size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
    char *slot = (char *)p + PAGE_HDR_BYTES + (n_slots - 1) * sb;
    for (size_t i = 0; i < n_slots; i++) {
        GCHeader *h = (GCHeader *)slot;
        HDR_SET_KIND(h, KIND_FREE);
        /* h->size, h->marked already 0 from mmap zero. */
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[class_idx];
        freelist[class_idx] = fs;
        slot -= sb;
    }
    (void)all_pages;  // see note: per-class chains serve as the sweep iter
}

// Pop a slot from the freelist of the given class, populate header,
// return payload pointer.
static void *
slab_alloc(AroGcKind kind, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(class_idx);
    FreeSlot *fs = freelist[class_idx];
    freelist[class_idx] = fs->next;
    void *payload = (void *)fs;
    GCHeader *h = (GCHeader *)payload - 1;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    /* marked is already false invariant (sweep frees only unmarked slots
     * + new_page populates with marked=false from mmap zero).  Skip the
     * extra store. */
    return payload;
}

// Large-object path: payload too big for in-page.  mmap a fresh region
// big enough for the object alone, link it into large_head.
static void *
large_alloc(AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    // Round up to page multiple for mmap hygiene.
    size_t map_bytes = (need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    /* h->marked already 0 from mmap zero. */
    return (void *)(h + 1);
}

static void gc_collect_internal(VALUE *sp_top);

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    if (aro_gc_stress || bytes_since_gc + payload_size > gc_threshold) {
        gc_collect_internal(sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    void *payload = (c >= 0) ? slab_alloc(kind, payload_size, c)
                             : large_alloc(kind, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    bytes_since_gc += payload_size;
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    if (aro_gc_stress || bytes_since_gc + payload_size > gc_threshold) {
        gc_collect_internal(sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    void *payload = (c >= 0) ? slab_alloc(KIND_PAYLOAD_BYTE, payload_size, c)
                             : large_alloc(KIND_PAYLOAD_BYTE, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    // No memset for byte payloads (no pointers inside).
    bytes_since_gc += payload_size;
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = HDR_KIND(oldh);
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    // Root old via sp_top[0] — uniform with other backends.  Non-moving
    // GC: sp_top[0] unchanged after GC.
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ---------------------------------------------------------------------------
// Mark phase
// ---------------------------------------------------------------------------

static void
gray_push(GCHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (GCHeader **)realloc(gray_buf, gray_capa * sizeof(GCHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void
mark_value(VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(h);
}

static void
process_gray(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        void *payload = (void *)(h + 1);
        switch (HDR_KIND(h)) {
          case KIND_OBJ_ARRAY: {
            BaArray *a = (BaArray *)payload;
            if (a->items) mark_value((VALUE)a->items);
            break;
          }
          case KIND_OBJ_STRING: {
            BaString *s = (BaString *)payload;
            if (s->bytes) mark_value((VALUE)s->bytes);
            break;
          }
          case KIND_PAYLOAD_VAL: {
            VALUE *items = (VALUE *)payload;
            size_t n = h->size / sizeof(VALUE);
            for (size_t i = 0; i < n; i++) mark_value(items[i]);
            break;
          }
          case KIND_PAYLOAD_BYTE:
          case KIND_FREE:
            break;
          default:
            ASTRO_ASSERT(0 && "process_gray: unknown kind");
        }
    }
}

// ---------------------------------------------------------------------------
// Sweep phase: walk pages + large objects, freelist unmarked, clear bits.
// ---------------------------------------------------------------------------

static void
sweep(void)
{
    // Walk each size class's page chain.
    for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
        size_t sb = size_class_bytes[c];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[c]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                GCHeader *h = (GCHeader *)slot;
                if (HDR_KIND(h) == KIND_FREE) continue;
                if (HDR_MARKED(h)) {
                    HDR_CLR_MARKED(h);
                } else {
                    aro_gc_stats.heap_bytes -= h->size;
                    HDR_SET_KIND(h, KIND_FREE);
                    FreeSlot *fs = (FreeSlot *)(h + 1);
                    fs->next = freelist[c];
                    freelist[c] = fs;
                }
            }
        }
    }
    // Walk large objects.  Freed ones return their mmap region.
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            link = &lo->next;
        } else {
            *link = lo->next;
            aro_gc_stats.heap_bytes -= h->size;
            munmap(lo, lo->map_bytes);
        }
    }
}

static void
gc_collect_internal(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    CTX *c = gc_ctx;

    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();
    aro_gc_phase_end(tmark, &aro_gc_stats.mark_seconds);

    struct timespec tsweep = aro_gc_phase_begin();
    sweep();
    aro_gc_phase_end(tsweep, &aro_gc_stats.reclaim_seconds);

    aro_gc_stats.gc_count++;
    bytes_since_gc = 0;
    if (!aro_gc_stress) {
        size_t live = aro_gc_stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(VALUE *sp_top)
{
    gc_collect_internal(sp_top);
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return aro_gc_stats.heap_bytes;  }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
