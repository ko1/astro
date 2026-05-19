// gc_mark_bitmap.c — backend #14: sticky mark&sweep with per-page bitmaps.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* 8-byte header.  No mark / old / dirty / young_next fields — those live
 * in per-page bitmaps. */
typedef struct GCHeader {
    uint32_t kind;
    uint32_t size;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

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
    uint32_t _pad;
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
    /* GCHeader follows */
} LargeObj;

static Page     *page_head[NUM_SIZE_CLASSES];
static FreeSlot *freelist[NUM_SIZE_CLASSES];
static LargeObj *large_head = NULL;

/* GC trigger state */
static size_t bytes_since_gc = 0;
static size_t old_bytes = 0;
static size_t old_alloc_since_major = 0;
/* MAJOR_THRESHOLD_MIN matches mark_gen / mark_gen_inc / mark_bump_gen so
 * non-moving sticky backends fire major at the same cadence (64 MiB MIN
 * + adaptive 2×live).  Earlier 4 MiB MIN caused mark_bitmap_gen to fire
 * major 16× more often than its peers on the same workload. */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2
static size_t old_major_threshold = MAJOR_THRESHOLD_MIN;
/* Fixed 16 MiB minor threshold — matches all other gen backends.
 * Iter 34 added an adaptive 16-256 MiB knob to speed up binary_trees,
 * but that broke fairness (only this backend had it).  Iter 36 reviewer
 * caught it; reverted to fixed 16 MiB to keep the comparison contract
 * one policy across all gen backends. */
#define MINOR_THRESHOLD         (16u * 1024u * 1024u)

static CTX   *gc_ctx       = NULL;
static bool   in_minor     = false;
static VALUE *sp_high_water = NULL;

/* Gray queue for mark traversal */
static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

/* Remset: list of dirty old GCHeader*s (we re-derive page from ptr) */
static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "mark_bitmap_gen";

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        old_major_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark_bitmap_gen] STRESS mode: collect on every alloc\n");
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
slot_idx_of(const Page *pg, const GCHeader *h, size_t slot_bytes)
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
new_page(int class_idx)
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
     * (= memory order — prefetcher friendly).  FreeSlot.next is stored
     * in PAYLOAD area (slot+8), NOT overlapping the GCHeader at slot+0,
     * so we can freely write h->kind in slab_alloc without strict-
     * aliasing risk of clobbering the still-needed freelist linkage. */
    char *slot = (char *)p + SLOTS_REGION_OFFSET + (n_slots - 1) * sb;
    for (size_t i = 0; i < n_slots; i++) {
        GCHeader *h = (GCHeader *)slot;
        h->kind = KIND_FREE;
        h->size = 0;
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[class_idx];
        freelist[class_idx] = fs;
        slot -= sb;
    }
}

static void *
slab_alloc(AroGcKind kind, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(class_idx);
    FreeSlot *fs = freelist[class_idx];
    freelist[class_idx] = fs->next;
    /* fs points into PAYLOAD area; GCHeader is at fs-1 (one header back). */
    void *payload = (void *)fs;
    GCHeader *h = (GCHeader *)payload - 1;
    h->kind = (uint32_t)kind;
    h->size = (uint32_t)payload_size;
    /* mark / old / dirty bits are already 0 for this slot:
     * - fresh page from mmap is zero-initialized
     * - sweep_page clears all 3 bits when freeing a slot (both minor's
     *   young-dead and major's any-dead path)
     * So we don't need to touch the bitmaps here — saves a `locate` +
     * 3 bitmap ops per alloc. */
    return payload;
}

static void *
large_alloc(AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
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
    GCHeader *h = (GCHeader *)(lo + 1);
    h->kind = (uint32_t)kind;
    h->size = (uint32_t)payload_size;
    return (void *)(h + 1);
}

/* Resolve an object pointer to (page, slot_idx).  Large objects return
 * page = NULL, callers should fall back to walking large_head. */
static inline bool
locate(const GCHeader *h, Page **out_page, size_t *out_idx)
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
find_large(const GCHeader *h)
{
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        if ((GCHeader *)(lo + 1) == h) return lo;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Mark / old / dirty bit accessors that handle slab + large transparently
 * --------------------------------------------------------------------------- */

static inline bool
get_mark(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->mark_bm, idx);
    LargeObj *lo = find_large(h);
    return lo ? lo->mark : false;
}

static inline void
set_mark(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->mark_bm, idx); return; }
    LargeObj *lo = find_large(h);
    if (lo) lo->mark = true;
}

static inline bool
get_old(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->old_bm, idx);
    LargeObj *lo = find_large(h);
    return lo ? lo->old : false;
}

static inline void
set_old(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->old_bm, idx); return; }
    LargeObj *lo = find_large(h);
    if (lo) lo->old = true;
}

static inline bool
get_dirty(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) return bm_get(pg->dirty_bm, idx);
    LargeObj *lo = find_large(h);
    return lo ? lo->dirty : false;
}

static inline void
set_dirty(const GCHeader *h)
{
    Page *pg; size_t idx;
    if (locate(h, &pg, &idx)) { bm_set(pg->dirty_bm, idx); return; }
    LargeObj *lo = find_large(h);
    if (lo) lo->dirty = true;
}

/* ---------------------------------------------------------------------------
 * Allocation public API
 * --------------------------------------------------------------------------- */

static void gc_collect_minor(VALUE *sp_top);
static void gc_collect_major(VALUE *sp_top);

// iter 45: cold-split.  Pull collect dispatch into a noinline cold helper
// so aro_gc_alloc / aro_gc_alloc_byte stay inliner-budget friendly.
static void __attribute__((noinline, cold))
maybe_collect_slow(VALUE *sp_top)
{
    gc_collect_minor(sp_top);
    if (old_alloc_since_major > old_major_threshold) {
        gc_collect_major(sp_top);
    }
}

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    if (__builtin_expect(aro_gc_stress || bytes_since_gc + (sizeof(GCHeader) + ALIGN8(payload_size)) > MINOR_THRESHOLD, 0)) {
        maybe_collect_slow(sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    void *payload = (c >= 0) ? slab_alloc(kind, payload_size, c)
                             : large_alloc(kind, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    bytes_since_gc += sizeof(GCHeader) + ALIGN8(payload_size); /* iter 35: alloc-bytes */
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    if (__builtin_expect(aro_gc_stress || bytes_since_gc + (sizeof(GCHeader) + ALIGN8(payload_size)) > MINOR_THRESHOLD, 0)) {
        maybe_collect_slow(sp_top);
    }
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    void *payload = (c >= 0) ? slab_alloc(KIND_PAYLOAD_BYTE, payload_size, c)
                             : large_alloc(KIND_PAYLOAD_BYTE, payload_size);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    bytes_since_gc += sizeof(GCHeader) + ALIGN8(payload_size); /* iter 35: alloc-bytes */
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = (AroGcKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

/* ---------------------------------------------------------------------------
 * Write barrier
 * --------------------------------------------------------------------------- */

/* iter 36 remset overflow guard.  Cap at 128 K entries.  iter 38 wired
 * up a heap-walk fallback: on overflow, set a flag and on the next minor
 * walk every page's dirty_bm + the large list for old+dirty objects.
 * Bitmaps are already the source of truth (header has no dirty bit at
 * all in this backend), so no extra bookkeeping is needed. */
#define MAX_REMSET_ENTRIES (1u << 17)
static bool remset_overflow = false;

static void
remset_push(GCHeader *h)
{
    if (remset_overflow) return;   /* dirty bit already set in per-page bitmap */
    if (remset_cnt >= MAX_REMSET_ENTRIES) {
        remset_overflow = true;
        return;
    }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (GCHeader **)realloc(remset_buf, remset_capa * sizeof(GCHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

/* Heap-walk fallback: enumerate dirty old objects across all pages + the
 * large-object list.  Inlined into gc_collect_minor below — the dirty
 * bits in dirty_bm / lo->dirty are the source of truth. */

void
aro_gc_wb(void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (get_old(hh) && !get_dirty(hh)) {
        set_dirty(hh);
        remset_push(hh);
    }
}

void
aro_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (get_old(hh) && !get_dirty(hh)) {
        set_dirty(hh);
        remset_push(hh);
    }
}

/* ---------------------------------------------------------------------------
 * Mark phase
 * --------------------------------------------------------------------------- */

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
    if (get_mark(h)) return;
    /* Minor: skip old objects (they're guaranteed live; their dirty
     * bits are processed via the remset). */
    if (in_minor && get_old(h)) return;
    set_mark(h);
    gray_push(h);
}

static void
scan_outgoing(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch ((AroGcKind)h->kind) {
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
        ASTRO_ASSERT(0 && "scan_outgoing: unknown kind");
    }
}

static void
process_gray(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(h);
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
sweep_page(Page *pg, bool minor)
{
    size_t sb = size_class_bytes[pg->class_idx];
    size_t freed_bytes = 0;
    size_t n = pg->n_slots;
    char *slot = (char *)pg + SLOTS_REGION_OFFSET;
    for (size_t i = 0; i < n; i++, slot += sb) {
        GCHeader *h = (GCHeader *)slot;
        bool old    = bm_get(pg->old_bm, i);
        bool marked = bm_get(pg->mark_bm, i);
        if (minor) {
            /* Minor: only consider young (old==0) slots.  Old slots
             * stay; their mark_bm bit was not touched this cycle. */
            if (old) continue;
            if (h->kind == KIND_FREE) continue;   /* already on freelist */
            if (marked) {
                /* Promote: set old, clear mark, clear dirty (fresh tenured). */
                bm_set(pg->old_bm, i);
                bm_clr(pg->mark_bm, i);
                bm_clr(pg->dirty_bm, i);
                old_bytes += h->size;
                old_alloc_since_major += sizeof(GCHeader) + ALIGN8(h->size); /* iter 36 fairness: occupancy not payload */
            } else {
                /* Young dead.  Return slot to freelist. */
                aro_gc_stats.heap_bytes -= h->size;
                freed_bytes += h->size;
                h->kind = KIND_FREE;
                FreeSlot *fs = (FreeSlot *)(h + 1);
                fs->next = freelist[pg->class_idx];
                freelist[pg->class_idx] = fs;
            }
        } else {
            /* Major: free all unmarked, clear mark on survivors. */
            if (h->kind == KIND_FREE) continue;
            if (marked) {
                bm_clr(pg->mark_bm, i);
                /* old_bm stays set (survivor remains old) — newly
                 * promoted via this cycle's minor (if any) also OK. */
            } else {
                aro_gc_stats.heap_bytes -= h->size;
                if (bm_get(pg->old_bm, i)) {
                    /* freeing an old slot — decrement old_bytes */
                    old_bytes = (old_bytes > h->size) ? old_bytes - h->size : 0;
                }
                bm_clr(pg->old_bm, i);
                bm_clr(pg->dirty_bm, i);
                h->kind = KIND_FREE;
                FreeSlot *fs = (FreeSlot *)(h + 1);
                fs->next = freelist[pg->class_idx];
                freelist[pg->class_idx] = fs;
            }
        }
    }
    return freed_bytes;
}

static void
sweep(bool minor)
{
    for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
        for (Page *pg = page_head[c]; pg; pg = pg->next) {
            sweep_page(pg, minor);
        }
    }
    /* Large objects */
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (minor) {
            if (lo->old) { link = &lo->next; continue; }
            if (lo->mark) {
                lo->old = true;
                lo->mark = false;
                lo->dirty = false;
                old_bytes += h->size;
                old_alloc_since_major += sizeof(GCHeader) + ALIGN8(h->size); /* iter 36 fairness: occupancy not payload */
                link = &lo->next;
            } else {
                *link = lo->next;
                aro_gc_stats.heap_bytes -= h->size;
                munmap(lo, lo->map_bytes);
            }
        } else {
            if (lo->mark) {
                lo->mark = false;
                link = &lo->next;
            } else {
                *link = lo->next;
                aro_gc_stats.heap_bytes -= h->size;
                if (lo->old) old_bytes = (old_bytes > h->size) ? old_bytes - h->size : 0;
                munmap(lo, lo->map_bytes);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Collect entrypoints
 * --------------------------------------------------------------------------- */

static void
gc_collect_minor(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    in_minor = true;

    CTX *c = gc_ctx;
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    /* Iter 36 fairness fix: adaptive minor threshold removed — all gen
     * backends now use a fixed 16 MiB MINOR_THRESHOLD. */

    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();

    /* Remset: old objects with heap writes since last minor.  On
     * overflow, fall back to a per-page heap walk over the dirty
     * bitmaps. */
    if (remset_overflow) {
        for (int sc = 0; sc < NUM_SIZE_CLASSES; sc++) {
            const size_t sb = size_class_bytes[sc];
            for (Page *pg = page_head[sc]; pg; pg = pg->next) {
                const size_t n = pg->n_slots;
                char *slot = (char *)pg + SLOTS_REGION_OFFSET;
                for (size_t i = 0; i < n; i++, slot += sb) {
                    if (bm_get(pg->old_bm, i) && bm_get(pg->dirty_bm, i)) {
                        bm_clr(pg->dirty_bm, i);
                        scan_outgoing((GCHeader *)slot);
                    }
                }
            }
        }
        for (LargeObj *lo = large_head; lo; lo = lo->next) {
            if (lo->old && lo->dirty) {
                lo->dirty = false;
                scan_outgoing((GCHeader *)(lo + 1));
            }
        }
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            GCHeader *h = remset_buf[i];
            Page *pg; size_t idx;
            if (locate(h, &pg, &idx)) bm_clr(pg->dirty_bm, idx);
            else { LargeObj *lo = find_large(h); if (lo) lo->dirty = false; }
            scan_outgoing(h);
        }
    }
    remset_cnt = 0;
    process_gray();

    sweep(/*minor=*/true);

    aro_gc_stats.gc_count++;
    aro_gc_stats.minor_count++;
    bytes_since_gc = 0;
    in_minor = false;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

static void
gc_collect_major(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    in_minor = false;
    remset_cnt = 0;

    CTX *c = gc_ctx;
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();

    sweep(/*minor=*/false);

    if (!aro_gc_stress) {
        size_t next = old_bytes * MAJOR_THRESHOLD_FACTOR;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;
    aro_gc_stats.gc_count++;
    aro_gc_stats.major_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(VALUE *sp_top)
{
    gc_collect_major(sp_top);
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return aro_gc_stats.heap_bytes;  }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
