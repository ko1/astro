// gc_mark_gen.c — backend #3: non-moving mark&sweep with 2 generations,
// slab/page allocated.
//
// Heap layout: same slab pages as gc_mark.c (9 size classes, 16 KiB
// pages, large-obj path for >4 KiB).  All allocations come from those
// pages; no libc malloc.
//
// Generation tracking: each GCHeader carries an `old` bit.  Young
// objects form a single-linked list through GCHeader.young_next; the
// list head is `young_head`.  Old objects don't need a list since the
// major sweep walks pages directly.  Saves 8 B/header vs the old
// young+old doubly-linked design.
//
// Minor GC:
//   1. Mark from roots + dirty remset (old objects with heap writes).
//   2. Walk young_head list.  Marked → promote in place (set old=true,
//      clear marked); unmarked → return slot to size-class freelist.
//      The young list is empty post-minor (promoted survivors are no
//      longer "young").
//
// Major GC:
//   1. Mark from roots through both generations.
//   2. Walk young_head: promote marked young in place; free unmarked.
//   3. Walk all pages by region (slot-prefix walk).  Free unmarked old
//      slots; clear marked bit on survivors.  Free slots are skipped.
//
// Write barrier: WB on heap-pointer write into an old object marks it
// dirty + pushes to the remset.  Minor scan walks just the remset, not
// the whole old set.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

typedef struct GCHeader {
    struct GCHeader *young_next;   // single-linked young list; garbage when old
    uint32_t kind;
    uint32_t size;
    bool     marked;
    bool     old;
    bool     dirty;
    uint8_t  _pad[5];              // pad to 24 B so payload stays 8-aligned
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 24, "GCHeader must be 24 bytes");

typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

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
} LargeObj;

static Page     *page_head[NUM_SIZE_CLASSES];
static FreeSlot *freelist[NUM_SIZE_CLASSES];
static LargeObj *large_head = NULL;

static GCHeader *young_head = NULL;
static size_t   young_bytes = 0;
static size_t   old_bytes   = 0;
static size_t   young_threshold     = 4u * 1024u * 1024u;
static size_t   old_alloc_since_major = 0;
#define MAJOR_THRESHOLD_MIN  (64u * 1024u * 1024u)
static size_t   old_major_threshold = MAJOR_THRESHOLD_MIN;

static CTX     *gc_ctx = NULL;
static bool     in_minor = false;

static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

BarubyGCStats baruby_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0};
int baruby_gc_stress = 0;
const char *baruby_gc_backend_name = "mark_gen";

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
        young_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark_gen] STRESS\n");
    }
}

// ---------------------------------------------------------------------------
// Slab management
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
new_page(int class_idx)
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
        GCHeader *h = (GCHeader *)slot;
        h->kind   = KIND_FREE;
        h->size   = 0;
        h->marked = false;
        h->old    = false;
        h->dirty  = false;
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[class_idx];
        freelist[class_idx] = fs;
        slot -= sb;
    }
}

static GCHeader *
slab_alloc(BarubyGCKind kind, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(class_idx);
    FreeSlot *fs = freelist[class_idx];
    freelist[class_idx] = fs->next;
    GCHeader *h = (GCHeader *)fs - 1;
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->marked = false;
    h->old    = false;
    h->dirty  = false;
    h->young_next = young_head;
    young_head = h;
    young_bytes += payload_size;
    return h;
}

static GCHeader *
large_alloc(BarubyGCKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    size_t map_bytes = (need + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->marked = false;
    h->old    = false;
    h->dirty  = false;
    h->young_next = young_head;
    young_head = h;
    young_bytes += payload_size;
    return h;
}

static void
free_slot(GCHeader *h)
{
    size_t total = sizeof(GCHeader) + ALIGN8(h->size);
    int c = size_class_for(total);
    if (c >= 0) {
        h->kind = KIND_FREE;
        h->size = 0;
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[c];
        freelist[c] = fs;
    } else {
        // Large object: find + unlink + munmap.
        LargeObj **link = &large_head;
        while (*link) {
            LargeObj *lo = *link;
            if ((GCHeader *)(lo + 1) == h) {
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
// Alloc API
// ---------------------------------------------------------------------------

static void minor_gc(VALUE *sp_top);
static void major_gc(VALUE *sp_top);

static inline void
maybe_collect(size_t add, VALUE *sp_top)
{
    if (baruby_gc_stress || young_bytes + add > young_threshold) {
        if (old_alloc_since_major > old_major_threshold) {
            major_gc(sp_top);
            old_alloc_since_major = 0;
        } else {
            minor_gc(sp_top);
        }
    }
}

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    maybe_collect(payload_size, sp_top);
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    GCHeader *h = (c >= 0) ? slab_alloc(kind, payload_size, c)
                           : large_alloc(kind, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    maybe_collect(payload_size, sp_top);
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int c = size_class_for(slot_total);
    GCHeader *h = (c >= 0) ? slab_alloc(KIND_PAYLOAD_BYTE, payload_size, c)
                           : large_alloc(KIND_PAYLOAD_BYTE, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return baruby_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    BarubyGCKind kind = (BarubyGCKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? baruby_gc_alloc_byte(new_size, sp_top + 1)
        : baruby_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

static void
remset_push(GCHeader *h)
{
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        remset_buf = (GCHeader **)realloc(remset_buf, remset_capa * sizeof(GCHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

void
baruby_gc_wb(void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (hh->old && !hh->dirty) {
        hh->dirty = true;
        remset_push(hh);
    }
}

void
baruby_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (hh->old && !hh->dirty) {
        hh->dirty = true;
        remset_push(hh);
    }
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
    if (h->marked) return;
    // Minor: don't traverse already-old objects via root scan; the
    // remset is responsible for those.  Major: mark everything.
    if (in_minor && h->old) return;
    h->marked = true;
    gray_push(h);
}

static void
scan_outgoing(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch ((BarubyGCKind)h->kind) {
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

// ---------------------------------------------------------------------------
// Sweep / promote
// ---------------------------------------------------------------------------

// Walk young list: marked → promote in place; unmarked → free.
// young_head/young_bytes reset.
//
// NB: keep h->marked=true on promoted objects.  In major, sweep_old_pages
// runs AFTER sweep_young and would otherwise see the just-promoted slot
// as "old + unmarked" and free it.  The marked bit is cleared during
// sweep_old_pages instead (which is also when minor's promoted-young
// would later get cleared on the next minor cycle... actually no, minor
// doesn't call sweep_old_pages — see clear_marked param).
static void
sweep_young(bool clear_marked)
{
    GCHeader *h = young_head;
    young_head = NULL;
    young_bytes = 0;
    while (h) {
        GCHeader *next = h->young_next;
        if (h->marked) {
            // Promote: stays in place.  In minor, clear marked (no
            // follow-up scan).  In major, keep marked so the subsequent
            // sweep_old_pages doesn't free us; it'll clear it then.
            if (clear_marked) h->marked = false;
            h->old    = true;
            old_bytes += h->size;
            old_alloc_since_major += h->size;
        } else {
            baruby_gc_stats.heap_bytes -= h->size;
            free_slot(h);
        }
        h = next;
    }
}

// Major: walk slab pages + large objects, free unmarked OLD slots.
// Young slots have already been handled by sweep_young.
static void
sweep_old_pages(void)
{
    for (int c = 0; c < NUM_SIZE_CLASSES; c++) {
        size_t sb = size_class_bytes[c];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[c]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                GCHeader *h = (GCHeader *)slot;
                if (h->kind == KIND_FREE) continue;
                if (!h->old) continue;
                if (h->marked) {
                    h->marked = false;
                    h->dirty  = false;
                } else {
                    old_bytes -= h->size;
                    baruby_gc_stats.heap_bytes -= h->size;
                    free_slot(h);
                }
            }
        }
    }
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (!h->old) { link = &lo->next; continue; }
        if (h->marked) {
            h->marked = false;
            h->dirty  = false;
            link = &lo->next;
        } else {
            *link = lo->next;
            old_bytes -= h->size;
            baruby_gc_stats.heap_bytes -= h->size;
            munmap(lo, lo->map_bytes);
        }
    }
}

// ---------------------------------------------------------------------------
// Collection drivers
// ---------------------------------------------------------------------------

static void
minor_gc(VALUE *sp_top)
{
    struct timespec t0 = baruby_gc_time_begin();
    in_minor = true;

    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();

    // Process remset: old objects with heap writes since last minor.
    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *h = remset_buf[i];
        h->dirty = false;
        scan_outgoing(h);
    }
    remset_cnt = 0;
    process_gray();

    sweep_young(/*clear_marked=*/true);

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.minor_count++;
    in_minor = false;
    c->sp = sp_top;
    baruby_gc_time_end(t0);
}

static void
major_gc(VALUE *sp_top)
{
    struct timespec t0 = baruby_gc_time_begin();
    in_minor = false;
    remset_cnt = 0;

    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();

    // In major, keep marked=true on promoted young objects so the
    // subsequent sweep_old_pages doesn't free them as unmarked-old.
    sweep_young(/*clear_marked=*/false);
    sweep_old_pages();

    if (!baruby_gc_stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.major_count++;
    c->sp = sp_top;
    baruby_gc_time_end(t0);
}

void
baruby_gc_collect(VALUE *sp_top)
{
    major_gc(sp_top);
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return baruby_gc_stats.heap_bytes;  }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }
size_t baruby_gc_minor_count(void) { return baruby_gc_stats.minor_count; }
size_t baruby_gc_major_count(void) { return baruby_gc_stats.major_count; }
double baruby_gc_total_seconds(void) { return baruby_gc_stats.total_seconds; }
double baruby_gc_max_pause_seconds(void) { return baruby_gc_stats.max_pause_seconds; }
