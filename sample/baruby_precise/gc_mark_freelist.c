// gc_mark_freelist.c — backend #16: mark + sweep with per-class freelist
//                                   inside a single bump region.
//
// Region: ARO_GC_REGION_VIRT_BYTES (64 GiB virtual, lazy-paged).
// Allocator: try per-class freelist first (LIFO), fall back to bump.
// Collect: mark from roots, then sequential sweep walks region linearly:
//   - marked   → clear mark
//   - unmarked → mark KIND_FREE, push to class freelist (size preserved
//                so the region walk can skip past dead slots correctly)
//
// vs gc_mark (slab pages): no page chain, no page metadata; one
//                          contiguous region.
// vs gc_mark_compact (compact): no slide pass; dead bytes stay where they
//                               are, available via freelist only.
//
// Trade-offs: simpler than mark_compact (no forward/update/slide
// passes — just mark + sweep), but suffers fragmentation — bump head only
// advances, dead bytes return via freelist keyed by exact size class.
// No coalescing.  Demonstrates the cost of fragmentation vs compaction.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* 8-byte header.  size is requested payload bytes (not slot total). */
typedef struct GCHeader {
    uint8_t  flags;     /* bit 0-2: kind (KIND_*); bit 3: marked */
    uint8_t  _pad[3];
    uint32_t size;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_MARKED_BIT   0x08u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))
#define HDR_MARKED(h)      (((h)->flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->flags &= (uint8_t)~HDR_MARKED_BIT)

/* Free slot overlay: payload[0..7] holds the freelist link. */
typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

/* Same 9 size classes as gc_mark.c — total slot bytes (header included).
 * For sweep correctness we need to look up the class index from the slot
 * total bytes (computed from header->size + sizeof(GCHeader) + align). */
#define NUM_SIZE_CLASSES 9
static const size_t size_class_bytes[NUM_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 3072, 4096
};

#define MAX_SLOT_BYTES   (size_class_bytes[NUM_SIZE_CLASSES - 1])
#define REGION_BYTES     ARO_GC_REGION_VIRT_BYTES  /* 64 GiB virtual */

/* Large objects (> MAX_SLOT_BYTES) are each their own mmap region. */
typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    /* GCHeader follows */
} LargeObj;

static char       *region_base = NULL;
static char       *region_top  = NULL;
static char       *region_end  = NULL;
static FreeSlot   *freelist[NUM_SIZE_CLASSES];
static LargeObj   *large_head = NULL;

static CTX        *gc_ctx       = NULL;
static VALUE      *sp_high_water = NULL;
static GCHeader  **gray_buf     = NULL;
static size_t      gray_cnt     = 0;
static size_t      gray_capa    = 0;

/* Adaptive GC trigger: max(16 MiB, 2 × live_post_sweep). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2
static size_t bytes_since_gc = 0;
static size_t gc_threshold   = GC_THRESHOLD_MIN;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "mark_freelist";

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

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (region_base == MAP_FAILED) {
        perror("mark_freelist mmap region");
        abort();
    }
    region_top = region_base;
    region_end = region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        fprintf(stderr, "[baruby_gc=mark_freelist] STRESS mode: collect on every alloc\n");
    }
}

/* --------------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------------- */

static void gc_collect_internal(VALUE *sp_top);

static GCHeader *
alloc_large(AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mark_freelist mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->flags &= (uint8_t)~HDR_MARKED_BIT;
    return h;
}

// iter 43: cold-path split (see gc_copy.c for rationale).
static void __attribute__((noinline, cold))
oom_abort(void)
{
    fprintf(stderr, "baruby_gc=mark_freelist: region OOM\n");
    abort();
}

static inline GCHeader *
alloc_slot(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    if (__builtin_expect(slot_total > MAX_SLOT_BYTES, 0)) {
        return alloc_large(kind, payload_size);
    }
    int ci = size_class_for(slot_total);
    size_t sb = size_class_bytes[ci];

    if (__builtin_expect(aro_gc_stress || bytes_since_gc + payload_size > gc_threshold, 0)) {
        gc_collect_internal(sp_top);
    }

    GCHeader *h;
    FreeSlot *fs = freelist[ci];
    if (fs) {
        freelist[ci] = fs->next;
        /* fs is at the payload offset; back up to header. */
        h = (GCHeader *)((char *)fs - sizeof(GCHeader));
    } else {
        if (__builtin_expect(region_top + sb > region_end, 0)) {
            oom_abort();
        }
        h = (GCHeader *)region_top;
        region_top += sb;
    }
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->flags &= (uint8_t)~HDR_MARKED_BIT;
    return h;
}

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    GCHeader *h = alloc_slot(kind, payload_size, sp_top);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    bytes_since_gc += payload_size;
    return (void *)(h + 1);
}

void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    return aro_gc_alloc(KIND_PAYLOAD_BYTE, payload_size, sp_top);
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = HDR_KIND(oldh);
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    /* Root old via sp_top[0] so collection during alloc keeps it live.
     * Non-moving: pointer unchanged after GC. */
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

/* Write barrier: non-generational, so gc.h's static-inline no-op is used. */

/* --------------------------------------------------------------------------
 * Mark phase
 * -------------------------------------------------------------------------- */

static void
gray_push(GCHeader *const h)
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
scan_outgoing(GCHeader *h)
{
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
        ASTRO_ASSERT(0 && "mark_freelist scan_outgoing: unknown kind");
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

/* --------------------------------------------------------------------------
 * Sweep phase — sequential region walk, dead → push to class freelist.
 * -------------------------------------------------------------------------- */

static void
sweep_region(void)
{
    /* Rebuild freelists from scratch. */
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        freelist[i] = NULL;
    }
    size_t live_bytes = 0;
    char *p = region_base;
    while (p < region_top) {
        GCHeader *const h = (GCHeader *)p;
        size_t slot_total = sizeof(GCHeader) + ALIGN8(h->size);
        /* Locate the class so we can compute the actual slot stride
         * (allocation rounded up to size_class_bytes[ci]). */
        int ci = size_class_for(slot_total);
        ASTRO_ASSERT(ci >= 0 && "sweep_region: oversized slot in region");
        size_t sb = size_class_bytes[ci];
        if (HDR_KIND(h) == KIND_FREE) {
            /* Already-free slot from a prior sweep — push back to freelist. */
            FreeSlot *fs = (FreeSlot *)(h + 1);
            fs->next = freelist[ci];
            freelist[ci] = fs;
        } else if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            live_bytes += h->size;
        } else {
            /* Unmarked → free.  Set size to the maximum payload that fits
             * the slot, so a subsequent sweep walks past the same span.
             * (We can't restore the original `size` after rebirth — alloc
             * resets it.) */
            HDR_SET_KIND(h, KIND_FREE);
            h->size = (uint32_t)(sb - sizeof(GCHeader));
            FreeSlot *fs = (FreeSlot *)(h + 1);
            fs->next = freelist[ci];
            freelist[ci] = fs;
        }
        p += sb;
    }

    /* Large objects: separate sweep. */
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            live_bytes += h->size;
            link = &lo->next;
        } else {
            *link = lo->next;
            munmap(lo, lo->map_bytes);
        }
    }
    aro_gc_stats.heap_bytes = live_bytes;
}

/* --------------------------------------------------------------------------
 * Collect
 * -------------------------------------------------------------------------- */

static void
gc_collect_internal(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    CTX *c = gc_ctx;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();
    aro_gc_phase_end(tmark, &aro_gc_stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    sweep_region();
    aro_gc_phase_end(treclaim, &aro_gc_stats.reclaim_seconds);

    bytes_since_gc = 0;
    if (!aro_gc_stress) {
        size_t next = aro_gc_stats.heap_bytes * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    aro_gc_stats.gc_count++;
    aro_gc_stats.major_count++;
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
double aro_gc_total_seconds(void)   { return aro_gc_stats.total_seconds; }
double aro_gc_mark_seconds(void)    { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
