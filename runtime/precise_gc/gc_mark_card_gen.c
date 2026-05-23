// gc_mark_card_gen.c — backend #15: card (page)-level remset variant.
//
// Semantically equivalent to gc_mark_gen.c (sticky mark-bits / non-moving
// promotion, remset, in-place "promote"), but with two structural changes:
//
//   1. GCHeader shrinks from 24 B → 8 B.  mark / old / dirty bits live in
//      per-page bitmaps instead of header bytes; young_next linked list
//      removed (minor sweep walks pages directly).
//
//   2. Pages are 16 KiB *aligned* (over-mmap then trim), so the page base
//      of any object is `((uintptr_t)obj & ~(PAGE_SIZE-1))` — single AND.
//
// The 8 B header is a big win for small classes.  In the 32 B size class
// (the most common one for BaArray = 24 B payload), with 24 B headers
// you waste 50% of every slot to header; with 8 B headers payload fills
// the slot exactly.  Density doubles for small Arrays.
//
// Trade-offs vs gc_mark_gen.c:
//   + smaller header (8 B vs 24 B)
//   + denser slot packing (BaArray 24 B fits class 32 now, not class 64)
//   + page-aligned, bitmap operations are cache-friendly
//   - minor sweep is O(heap) (walks all pages) instead of O(young)
//   - per-page bitmap area (~192 B = ~1.2% of a 16 KiB page) overhead

#define _GNU_SOURCE      /* mremap; must precede stdio.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止、 AroObjectHeader at offset 0。
 * mark / old / dirty / young_next は per-page bitmap、 head には FREE bit のみ。 */
_Static_assert(sizeof(AroObjectHeader) == 8, "non-moving GC: head must be 8 B");

#define HDR_FREE_BIT     (uint16_t)0x0001u
#define HDR_IS_FREE(h)     (((h)->gc_flags & HDR_FREE_BIT) != 0)
#define HDR_SET_FREE(h)    ((h)->gc_flags |= HDR_FREE_BIT)

typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

static inline FreeSlot *
free_slot_link(void *payload)
{
    return (FreeSlot *)((char *)payload + sizeof(AroObjectHeader));
}

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

/* Size classes (slot total, header included).  Same 9 classes as gc_mark.c
 * but the smaller header means much better fit for small payloads. */
#define NUM_SIZE_CLASSES 9
/* Per-class shift for fast (off / sb) -> (off >> shift) in `locate`.  All
 * classes are power-of-2 except 3072 (idx 7), which gets shift=0 → fall
 * back to runtime division.  Cuts mark_value's per-call cost by ~5×
 * since BaArray (class 32, shift 5) and most other hot allocations skip
 * the integer-div. */
static const uint8_t size_class_shift[9] = {
    5,  /* 32   */
    6,  /* 64   */
    7,  /* 128  */
    8,  /* 256  */
    9,  /* 512  */
    10, /* 1024 */
    11, /* 2048 */
    0,  /* 3072 — not pow2, fall back to div */
    12, /* 4096 */
};

static const size_t size_class_bytes[NUM_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 3072, 4096
};

#define PAGE_SIZE       (16u * 1024u)
#define PAGE_HDR_BYTES  16
/* Max 512 slots/page (for class 32 with 8 B header).  Each bitmap holds
 * 1 bit per slot, so up to 64 bytes per bitmap.  We reserve 3 bitmaps ×
 * 64 B = 192 B per page header region — fits all classes (larger classes
 * have fewer slots → smaller actual bitmap, but the unused tail is just
 * dead bytes inside the bitmap region). */
#define MAX_SLOTS_PER_PAGE 512
#define BITMAP_BYTES       (MAX_SLOTS_PER_PAGE / 8)   /* 64 */
#define BITMAP_REGION_BYTES (3 * BITMAP_BYTES)        /* 192 */
#define SLOTS_REGION_OFFSET (PAGE_HDR_BYTES + BITMAP_REGION_BYTES)  /* 208 */

typedef struct Page {
    struct Page *next;
    uint16_t class_idx;
    uint16_t n_slots;
    uint8_t  card_dirty;   /* iter 36 card_gen: 1 if this page is in remset */
    uint8_t  _pad[3];
    uint8_t  mark_bm[BITMAP_BYTES];
    uint8_t  old_bm[BITMAP_BYTES];
    uint8_t  dirty_bm[BITMAP_BYTES];
    /* slots follow at offset SLOTS_REGION_OFFSET */
} Page;
_Static_assert(sizeof(Page) == SLOTS_REGION_OFFSET, "Page header size mismatch");

/* Large objects (> max slot) get their own mmap region.  No bitmap — flags
 * live in a side struct here. */
typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    bool             mark;
    bool             old;
    bool             dirty;
    uint8_t          _pad[5];
    /* payload follows */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2
#define MINOR_THRESHOLD         (16u * 1024u * 1024u)

typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    Page     *page_head[NUM_SIZE_CLASSES];
    FreeSlot *freelist[NUM_SIZE_CLASSES];
    LargeObj *large_head;
    size_t bytes_since_gc;
    size_t old_bytes;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    CTX   *ctx;
    bool   in_minor;
    struct AroObjectHeader **gray_buf;
    size_t            gray_cnt;
    size_t            gray_capa;
    Page            **remset_buf;
    size_t            remset_cnt;
    size_t            remset_capa;
} ASTroGC;

#define page_head             (gc->page_head)
#define freelist              (gc->freelist)
#define large_head            (gc->large_head)
#define bytes_since_gc        (gc->bytes_since_gc)
#define old_bytes             (gc->old_bytes)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gc_ctx                (gc->ctx)
#define in_minor              (gc->in_minor)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)

const char *aro_gc_backend_name = "mark_card_gen";

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    old_major_threshold = MAJOR_THRESHOLD_MIN;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        old_major_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark_card_gen] STRESS mode: collect on every alloc\n");
    }
}

/* ---------------------------------------------------------------------------
 * Page + bitmap helpers
 * --------------------------------------------------------------------------- */

static inline Page *
page_of(const void *p)
{
    return (Page *)((uintptr_t)p & ~(uintptr_t)(PAGE_SIZE - 1));
}

static inline size_t
slot_idx_of(const Page *pg, const AroObjectHeader *h, size_t slot_bytes)
{
    return ((uintptr_t)h - (uintptr_t)pg - SLOTS_REGION_OFFSET) / slot_bytes;
}

static inline bool bm_get(const uint8_t *bm, size_t i) { return (bm[i >> 3] >> (i & 7)) & 1u; }
static inline void bm_set(uint8_t *bm, size_t i)       { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bm_clr(uint8_t *bm, size_t i)       { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static inline int
size_class_for(size_t slot_total)
{
    // iter 44: O(1) clz-based dispatch — see gc_mark.c for rationale.
    if (slot_total <= 32) return 0;
    if (slot_total > 4096) return -1;
    int bits = 64 - __builtin_clzll(slot_total - 1);
    int c = bits - 5;
    if (c == 7 && slot_total > 3072) c = 8;
    return c;
}

/* mmap a 16 KiB-aligned page (over-allocate then trim). */
static Page *
mmap_aligned_page(void)
{
    char *raw = (char *)mmap(NULL, 2 * PAGE_SIZE, PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    uintptr_t aligned = ((uintptr_t)raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    size_t prefix = aligned - (uintptr_t)raw;
    if (prefix > 0) munmap(raw, prefix);
    size_t suffix = 2 * PAGE_SIZE - prefix - PAGE_SIZE;
    if (suffix > 0) munmap((void *)(aligned + PAGE_SIZE), suffix);
    return (Page *)aligned;
}

static void
new_page(ASTroGC *gc, int class_idx)
{
    Page *p = mmap_aligned_page();
    p->class_idx = (uint16_t)class_idx;
    size_t sb = size_class_bytes[class_idx];
    size_t n_slots = (PAGE_SIZE - SLOTS_REGION_OFFSET) / sb;
    if (n_slots > MAX_SLOTS_PER_PAGE) n_slots = MAX_SLOTS_PER_PAGE;
    p->n_slots = (uint16_t)n_slots;
    /* bitmaps are already zero from mmap */
    p->next = page_head[class_idx];
    page_head[class_idx] = p;

    /* Populate freelist HIGH → LOW so subsequent pops return LOW → HIGH
     * (= memory order — prefetcher friendly).  Freelist holds SLOT
     * pointers (= AroObjectHeader *).  FreeSlot.next link is at
     * `slot + sizeof(AroObjectHeader)`. */
    char *slot = (char *)p + SLOTS_REGION_OFFSET + (n_slots - 1) * sb;
    for (size_t i = 0; i < n_slots; i++) {
        AroObjectHeader *h = (AroObjectHeader *)slot;
        h->flags    = 0;
        h->gc_flags = HDR_FREE_BIT;
        h->gc_size  = 0;
        free_slot_link(slot)->next = freelist[class_idx];
        freelist[class_idx] = (FreeSlot *)slot;
        slot -= sb;
    }
}

static void *
slab_alloc(ASTroGC *gc, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(gc, class_idx);
    void *payload = (void *)freelist[class_idx];
    freelist[class_idx] = free_slot_link(payload)->next;
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
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    lo->mark = false;
    lo->old = false;
    lo->dirty = false;
    large_head = lo;
    AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return (void *)h;
}

/* Resolve an object pointer to (page, slot_idx).  Large objects return
 * page = NULL, callers should fall back to walking large_head. */
static inline bool
locate(const AroObjectHeader *h, Page **out_page, size_t *out_idx)
{
    Page *pg = page_of(h);
    /* Heuristic: if the location is inside its own page metadata area,
     * it's not a heap slot — must be a large object header. */
    uintptr_t off = (uintptr_t)h - (uintptr_t)pg;
    if (off < SLOTS_REGION_OFFSET) {
        *out_page = NULL;
        *out_idx = 0;
        return false;
    }
    uint8_t shift = size_class_shift[pg->class_idx];
    *out_page = pg;
    *out_idx = shift ? ((off - SLOTS_REGION_OFFSET) >> shift)
                     : ((off - SLOTS_REGION_OFFSET) / size_class_bytes[pg->class_idx]);
    return true;
}

static LargeObj *
find_large(ASTroGC *gc, const AroObjectHeader *h)
{
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        if ((AroObjectHeader *)large_payload(lo) == h) return lo;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Mark / old / dirty bit accessors that handle slab + large transparently
 * --------------------------------------------------------------------------- */

static inline bool
get_mark(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->mark_bm, idx);
    LargeObj *lo = find_large(gc, h);
    return lo ? lo->mark : false;
}

static inline void
set_mark(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->mark_bm, idx); return; }
    LargeObj *lo = find_large(gc, h);
    if (lo) lo->mark = true;
}

static inline bool
get_old(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->old_bm, idx);
    LargeObj *lo = find_large(gc, h);
    return lo ? lo->old : false;
}

static inline void
set_old(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->old_bm, idx); return; }
    LargeObj *lo = find_large(gc, h);
    if (lo) lo->old = true;
}

static inline bool
get_dirty(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->dirty_bm, idx);
    LargeObj *lo = find_large(gc, h);
    return lo ? lo->dirty : false;
}

static inline void
set_dirty(ASTroGC *gc, const AroObjectHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->dirty_bm, idx); return; }
    LargeObj *lo = find_large(gc, h);
    if (lo) lo->dirty = true;
}

/* ---------------------------------------------------------------------------
 * Allocation public API
 * --------------------------------------------------------------------------- */

static void gc_collect_minor(CTX *c);
static void gc_collect_major(CTX *c);

static void __attribute__((noinline, cold))
maybe_collect_slow(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    gc_collect_minor(c);
    if (old_alloc_since_major > old_major_threshold) {
        gc_collect_major(c);
    }
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + gc->common.external_bytes + (ALIGN8(payload_size)) > MINOR_THRESHOLD, 0)) {
        maybe_collect_slow(c);
    }
    size_t slot_total = ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    void *payload = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                             : large_alloc(gc, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(AroObjectHeader), 0,
           ALIGN8(payload_size) - sizeof(AroObjectHeader));
    bytes_since_gc += ALIGN8(payload_size);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + gc->common.external_bytes + (ALIGN8(payload_size)) > MINOR_THRESHOLD, 0)) {
        maybe_collect_slow(c);
    }
    size_t slot_total = ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    void *payload = (cls >= 0) ? slab_alloc(gc, payload_size, cls)
                             : large_alloc(gc, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    bytes_since_gc += ALIGN8(payload_size);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* ---------------------------------------------------------------------------
 * Write barrier
 * --------------------------------------------------------------------------- */

/* card_gen: remset is page-level, bounded by total page count.  No
 * MAX_REMSET_ENTRIES cap needed — heap_size / 16 KiB = max # entries. */
static void
remset_push_page(ASTroGC *gc, Page *pg)
{
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        remset_buf = (Page **)realloc(remset_buf, remset_capa * sizeof(Page *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = pg;
}

static inline void
mark_dirty(ASTroGC *gc, AroObjectHeader *hh)
{
    Page *pg; size_t idx;
    if (locate(hh, &pg, &idx)) {
        if (!bm_get(pg->dirty_bm, idx)) {
            bm_set(pg->dirty_bm, idx);
        }
        if (!pg->card_dirty) {
            pg->card_dirty = 1;
            remset_push_page(gc, pg);
        }
    } else {
        LargeObj *lo = find_large(gc, hh);
        if (lo && !lo->dirty) lo->dirty = true;
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    AroObjectHeader *hh = (AroObjectHeader *)holder;
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (get_old(gc, hh)) mark_dirty(gc, hh);
}

void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    AroObjectHeader *hh = (AroObjectHeader *)holder;
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (get_old(gc, hh)) mark_dirty(gc, hh);
}

/* ---------------------------------------------------------------------------
 * Mark phase
 * --------------------------------------------------------------------------- */

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
    if (HDR_IS_FREE(h)) return;
    if (get_mark(gc, h)) return;
    if (in_minor && get_old(gc, h)) return;
    set_mark(gc, h);
    gray_push(gc, h);
}

static void
mark_edge(void *ctx, void **slot)
{
    mark_value((ASTroGC *)ctx, (VALUE)*slot);
}

static void
scan_outgoing(ASTroGC *gc, AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge);
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        AroObjectHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(gc, h);
    }
}

/* ---------------------------------------------------------------------------
 * Sweep
 * --------------------------------------------------------------------------- */

/* Walk a page's slots, applying `decide` per slot:
 *   1 = keep / promote, 0 = free.
 * `pre_promote_old` is true during minor; we promote freshly-marked
 * young to old (set old_bm, clear mark_bm so next minor mark works).
 * During major we clear mark_bm on all survivors. */
static size_t
sweep_page(ASTroGC *gc, Page *pg, bool minor)
{
    size_t sb = size_class_bytes[pg->class_idx];
    size_t freed_bytes = 0;
    size_t n = pg->n_slots;
    char *slot = (char *)pg + SLOTS_REGION_OFFSET;
    for (size_t i = 0; i < n; i++, slot += sb) {
        AroObjectHeader *h = (AroObjectHeader *)slot;
        bool old    = bm_get(pg->old_bm, i);
        bool marked = bm_get(pg->mark_bm, i);
        if (minor) {
            if (old) continue;
            if (HDR_IS_FREE(h)) continue;
            if (marked) {
                bm_set(pg->old_bm, i);
                bm_clr(pg->mark_bm, i);
                bm_clr(pg->dirty_bm, i);
                old_bytes += h->gc_size;
                old_alloc_since_major += ALIGN8(h->gc_size);
            } else {
                gc->common.stats.heap_bytes -= h->gc_size;
                freed_bytes += h->gc_size;
                HDR_SET_FREE(h);
                free_slot_link(h)->next = freelist[pg->class_idx];
                freelist[pg->class_idx] = (FreeSlot *)h;
            }
        } else {
            if (HDR_IS_FREE(h)) continue;
            if (marked) {
                bm_clr(pg->mark_bm, i);
            } else {
                gc->common.stats.heap_bytes -= h->gc_size;
                if (bm_get(pg->old_bm, i)) {
                    old_bytes = (old_bytes > h->gc_size) ? old_bytes - h->gc_size : 0;
                }
                bm_clr(pg->old_bm, i);
                bm_clr(pg->dirty_bm, i);
                HDR_SET_FREE(h);
                free_slot_link(h)->next = freelist[pg->class_idx];
                freelist[pg->class_idx] = (FreeSlot *)h;
            }
        }
    }
    return freed_bytes;
}

static void
sweep(ASTroGC *gc, bool minor)
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        for (Page *pg = page_head[cls]; pg; pg = pg->next) {
            sweep_page(gc, pg, minor);
        }
    }
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
        if (minor) {
            if (lo->old) { link = &lo->next; continue; }
            if (lo->mark) {
                lo->old = true;
                lo->mark = false;
                lo->dirty = false;
                old_bytes += h->gc_size;
                old_alloc_since_major += ALIGN8(h->gc_size);
                link = &lo->next;
            } else {
                *link = lo->next;
                gc->common.stats.heap_bytes -= h->gc_size;
                munmap(lo, lo->map_bytes);
            }
        } else {
            if (lo->mark) {
                lo->mark = false;
                link = &lo->next;
            } else {
                *link = lo->next;
                gc->common.stats.heap_bytes -= h->gc_size;
                if (lo->old) old_bytes = (old_bytes > h->gc_size) ? old_bytes - h->gc_size : 0;
                munmap(lo, lo->map_bytes);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Collect entrypoints
 * --------------------------------------------------------------------------- */

static void
gc_collect_minor(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;

    AROH_VISIT_ROOTS(c, gc, mark_edge);
    process_gray(gc);

    for (size_t i = 0; i < remset_cnt; i++) {
        Page *pg = remset_buf[i];
        pg->card_dirty = 0;
        size_t sb = size_class_bytes[pg->class_idx];
        size_t n = pg->n_slots;
        char *slot = (char *)pg + SLOTS_REGION_OFFSET;
        for (size_t j = 0; j < n; j++, slot += sb) {
            if (!bm_get(pg->dirty_bm, j)) continue;
            bm_clr(pg->dirty_bm, j);
            scan_outgoing(gc, (AroObjectHeader *)slot);
        }
    }
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        if (lo->dirty) {
            lo->dirty = false;
            scan_outgoing(gc, (AroObjectHeader *)large_payload(lo));
        }
    }
    remset_cnt = 0;
    process_gray(gc);

    /* Finalize pass — see gc_mark_bitmap_gen.c. */
    aro_gc_finalize_walk(c);

    sweep(gc, /*minor=*/true);

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    bytes_since_gc = 0;
    in_minor = false;
    aro_gc_time_end(c, t0);
}

static void
gc_collect_major(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    remset_cnt = 0;

    AROH_VISIT_ROOTS(c, gc, mark_edge);
    process_gray(gc);

    /* Finalize pass — major. */
    aro_gc_finalize_walk(c);

    sweep(gc, /*minor=*/false);

    if (!gc->common.stress) {
        size_t next = old_bytes * MAJOR_THRESHOLD_FACTOR;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;
    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    gc_collect_major(c);
}


/* Same shape as gc_mark_bitmap_gen.c finalize_check. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    const AroObjectHeader *h = (const AroObjectHeader *)payload;
    if (in_minor) {
        if (get_old(gc, h))  return payload;
        if (get_mark(gc, h)) return payload;
        return NULL;
    }
    return get_mark(gc, h) ? payload : NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        Page *p = page_head[cls];
        while (p) {
            Page *next = p->next;
            munmap(p, PAGE_SIZE);
            p = next;
        }
    }
    aro_gc_free_large_chain_mmap(large_head);
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

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (size_class_for(ALIGN8(n)) >= 0)
#define ARO_GC_INPLACE_PAGE_SIZE         PAGE_SIZE
#define ARO_GC_INPLACE_MREMAP_FLAGS      0
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (bytes_since_gc += (d))
#undef large_head
#include "gc_inplace_mremap.h"
#define large_head (gc->large_head)
