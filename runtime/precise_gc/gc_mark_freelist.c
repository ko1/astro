// gc_mark_freelist.c — backend #16: mark + sweep with per-class freelist
//                                   inside a single bump region.
//
// Region: ARO_GC_REGION_VIRT_BYTES (64 GiB virtual, lazy-paged).
// Allocator: try per-class freelist first (LIFO), fall back to bump.
// Collect: mark from roots, then sequential sweep walks region linearly:
//   - marked   → clear mark
//   - unmarked → set HDR_FREE_BIT, push to class freelist (size preserved
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

#define _GNU_SOURCE      /* mremap, MREMAP_MAYMOVE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止、 ASTroObjectHeader が payload
 * offset 0 にあり gc_size を保持。 region walk は gc_size から次 slot を
 * 算出するので、 free slot の gc_size には class slot size を入れる
 * (= 旧 design の `slot_size - sizeof(ASTroObjectHeader)` 相当)。 */

#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_FREE_BIT     (uint16_t)0x0002u
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_IS_FREE(h)     (((h)->gc_flags & HDR_FREE_BIT) != 0)
#define HDR_SET_FREE(h)    ((h)->gc_flags |= HDR_FREE_BIT)

/* Free slot overlay: payload[sizeof(ASTroObjectHeader)..+7] holds the link. */
typedef struct FreeSlot {
    struct FreeSlot *next;
} FreeSlot;

static inline FreeSlot *
free_slot_link(void *payload)
{
    return (FreeSlot *)((char *)payload + sizeof(ASTroObjectHeader));
}

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

/* Same 9 size classes as gc_mark.c — total slot bytes (header included).
 * For sweep correctness we need to look up the class index from the slot
 * total bytes (computed from header->size + align). */
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
    /* payload follows */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

/* iter 62: process-scope state を struct ASTroGC に集約 */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    char       *region_base;
    char       *region_top;
    char       *region_end;
    FreeSlot   *freelist[NUM_SIZE_CLASSES];
    LargeObj   *large_head;
    CTX        *ctx;
    VALUE      *sp_high_water;
    ASTroObjectHeader **gray_buf;
    size_t      gray_cnt;
    size_t      gray_capa;
    size_t      bytes_since_gc;
    size_t      gc_threshold;
} ASTroGC;

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
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    gc->ctx = c;
    gc->gc_threshold = GC_THRESHOLD_MIN;
    c->astro_gc = gc;
    gc->region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (gc->region_base == MAP_FAILED) {
        perror("mark_freelist mmap region");
        abort();
    }
    gc->region_top = gc->region_base;
    gc->region_end = gc->region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=mark_freelist] STRESS mode: collect on every alloc\n");
    }
}

/* --------------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------------- */

static void gc_collect_internal(CTX *c, VALUE *sp_top);

static ASTroObjectHeader *
alloc_large(ASTroGC *gc, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("mark_freelist mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = gc->large_head;
    lo->map_bytes = map_bytes;
    gc->large_head = lo;
    ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return h;
}

// iter 43: cold-path split (see gc_copy.c for rationale).
static void __attribute__((noinline, cold))
oom_abort(void)
{
    fprintf(stderr, "baruby_gc=mark_freelist: region OOM\n");
    abort();
}

/* Freelist holds slot pointers (= ASTroObjectHeader *).  FreeSlot link
 * lives at slot + sizeof(ASTroObjectHeader). */
static inline ASTroObjectHeader *
alloc_slot(CTX *c, size_t payload_size, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t slot_total = ALIGN8(payload_size);
    if (__builtin_expect(slot_total > MAX_SLOT_BYTES, 0)) {
        return alloc_large(gc, payload_size);
    }
    int ci = size_class_for(slot_total);
    size_t sb = size_class_bytes[ci];

    if (__builtin_expect(gc->common.stress || gc->bytes_since_gc + payload_size > gc->gc_threshold, 0)) {
        gc_collect_internal(c, sp_top);
    }

    ASTroObjectHeader *h;
    FreeSlot *fs = gc->freelist[ci];
    if (fs) {
        h = (ASTroObjectHeader *)fs;
        gc->freelist[ci] = free_slot_link(h)->next;
    } else {
        if (__builtin_expect(gc->region_top + sb > gc->region_end, 0)) {
            oom_abort();
        }
        h = (ASTroObjectHeader *)gc->region_top;
        gc->region_top += sb;
    }
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return h;
}

static inline void *
do_alloc(CTX *c, bool is_byte, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTroObjectHeader *h = alloc_slot(c, payload_size, sp_top);
    void *payload = (void *)h;
    /* iter 48 bug fix: freelist-popped slots contain stale data; zero
     * pointer-typed payloads so scan sees VAL_FALSE until caller fills. */
    if (!is_byte) {
        ASTRO_GC_INIT_PAYLOAD(payload, ALIGN8(payload_size));
    } else {
        ASTRO_GC_INIT_BYTE_PAYLOAD(payload, ALIGN8(payload_size));
    }
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    gc->bytes_since_gc += payload_size;
    return payload;
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    return do_alloc(c, false, payload_size);
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    return do_alloc(c, true, payload_size);
}

/* Write barrier: non-generational, so gc.h's static-inline no-op is used. */

/* --------------------------------------------------------------------------
 * Mark phase
 * -------------------------------------------------------------------------- */

static void
gray_push(ASTroGC *gc, ASTroObjectHeader *const h)
{
    if (gc->gray_cnt >= gc->gray_capa) {
        gc->gray_capa = gc->gray_capa ? gc->gray_capa * 2 : 256;
        gc->gray_buf = (ASTroObjectHeader **)realloc(gc->gray_buf, gc->gray_capa * sizeof(ASTroObjectHeader *));
        if (!gc->gray_buf) abort();
    }
    gc->gray_buf[gc->gray_cnt++] = h;
}

static void
mark_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)v;
    if (HDR_IS_FREE(h)) return;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

/* edge_visit callback: mark phase. `ctx` is `ASTroGC *gc`. */
static void
mark_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    mark_value(gc, (VALUE)*slot);
}

static void
process_gray(ASTroGC *gc)
{
    while (gc->gray_cnt > 0) {
        ASTroObjectHeader *h = gc->gray_buf[--gc->gray_cnt];
        ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge);
    }
}

/* --------------------------------------------------------------------------
 * Sweep phase — sequential region walk, dead → push to class freelist.
 * -------------------------------------------------------------------------- */

static void
sweep_region(ASTroGC *gc)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        gc->freelist[i] = NULL;
    }
    size_t live_bytes = 0;
    char *p = gc->region_base;
    while (p < gc->region_top) {
        ASTroObjectHeader *const h = (ASTroObjectHeader *)p;
        size_t slot_total = ALIGN8((h)->gc_size);
        int ci = size_class_for(slot_total);
        ASTRO_ASSERT(ci >= 0 && "sweep_region: oversized slot in region");
        size_t sb = size_class_bytes[ci];
        if (HDR_IS_FREE(h)) {
            /* Already free from a prior cycle — re-thread. */
            free_slot_link(h)->next = gc->freelist[ci];
            gc->freelist[ci] = (FreeSlot *)h;
        } else if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            live_bytes += (h)->gc_size;
        } else {
            /* Unmarked → free.  gc_size = sb so the walker advances by sb. */
            h->flags    = 0;
            h->gc_flags = HDR_FREE_BIT;
            h->gc_size  = (uint32_t)sb;
            free_slot_link(h)->next = gc->freelist[ci];
            gc->freelist[ci] = (FreeSlot *)h;
        }
        p += sb;
    }

    LargeObj **link = &gc->large_head;
    while (*link) {
        LargeObj *lo = *link;
        ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            live_bytes += (h)->gc_size;
            link = &lo->next;
        } else {
            *link = lo->next;
            munmap(lo, lo->map_bytes);
        }
    }
    gc->common.stats.heap_bytes = live_bytes;
}

/* --------------------------------------------------------------------------
 * Collect
 * -------------------------------------------------------------------------- */

static void
gc_collect_internal(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    if (gc->sp_high_water == NULL || sp_top > gc->sp_high_water) {
        gc->sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < gc->sp_high_water; p++) *p = 0;
    }

    struct timespec tmark = aro_gc_phase_begin();
    aro_gc_visit_roots(c, gc, mark_edge);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    sweep_region(gc);
    aro_gc_phase_end(treclaim, &gc->common.stats.reclaim_seconds);

    gc->bytes_since_gc = 0;
    if (!gc->common.stress) {
        size_t next = gc->common.stats.heap_bytes * GC_THRESHOLD_FACTOR;
        gc->gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
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
    gc_collect_internal(c, sp_top);
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    if (gc->region_base) munmap(gc->region_base, REGION_BYTES);
    aro_gc_free_large_chain_mmap(gc->large_head);
    free(gc->gray_buf);
    free(gc);
    c->astro_gc = NULL;
}


size_t
aro_gc_size_of(void *p)
{
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    return h->gc_size;
}

/* In-place realloc for large objs via mremap.  See gc_mark.c for the
 * full rationale.  gc_mark_freelist uses sysconf(_SC_PAGESIZE) for
 * alignment instead of a fixed PAGE_SIZE macro. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (ALIGN8(n) <= MAX_SLOT_BYTES)
#define ARO_GC_INPLACE_PAGE_SIZE         sysconf(_SC_PAGESIZE)
#define ARO_GC_INPLACE_MREMAP_FLAGS      MREMAP_MAYMOVE
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (gc->bytes_since_gc += (d))
#include "gc_inplace_mremap.h"
