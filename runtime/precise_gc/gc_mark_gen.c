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

#define _GNU_SOURCE      /* mremap (no MAYMOVE) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* 8-byte header (iter 33).  young set moved to external `young_objs[]`
 * array (was per-header young_next pointer = 8 B).  That packs BaArray
 * (24 B payload) into class 32 (32 B slot) instead of class 64 — 50%
 * memory density on hash_chain. */
typedef struct GCHeader {
    uint8_t  flags;                /* bits 0-2: kind, bit 3: marked, bit 4: old, bit 5: dirty */
    uint8_t  _pad[3];              /* pad to size's 4-byte alignment */
    uint32_t size;                 /* payload bytes */
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_MARKED_BIT   0x08u
#define HDR_OLD_BIT      0x10u
#define HDR_DIRTY_BIT    0x20u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))
#define HDR_MARKED(h)      (((h)->flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->flags &= (uint8_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->flags &= (uint8_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->flags &= (uint8_t)~HDR_DIRTY_BIT)

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

#define MAJOR_THRESHOLD_MIN  (16u * 1024u * 1024u)

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    Page     *page_head[NUM_SIZE_CLASSES];
    FreeSlot *freelist[NUM_SIZE_CLASSES];
    LargeObj *large_head;
    /* young set: contiguous array of pointers (was per-header young_next list). */
    struct GCHeader **young_objs;
    size_t            young_objs_cnt;
    size_t            young_objs_capa;
    size_t young_bytes;
    size_t old_bytes;
    size_t young_threshold;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    CTX  *ctx;
    bool  in_minor;
    struct GCHeader **gray_buf;
    size_t            gray_cnt;
    size_t            gray_capa;
    struct GCHeader **remset_buf;
    size_t            remset_cnt;
    size_t            remset_capa;
    bool              remset_overflow;
} ASTroGC;

/* Field aliases — expand to `gc->field` so every helper must have an
 * `ASTroGC *gc` in scope.  No module-static `g_astro_gc` — instance is
 * heap-alloc'd in aro_gc_init and reached via `c->astro_gc`. */
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
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)

static inline void
young_push(ASTroGC *gc, GCHeader *h)
{
    if (young_objs_cnt >= young_objs_capa) {
        young_objs_capa = young_objs_capa ? young_objs_capa * 2 : 1024;
        young_objs = (GCHeader **)realloc(young_objs, young_objs_capa * sizeof(GCHeader *));
        if (!young_objs) abort();
    }
    young_objs[young_objs_cnt++] = h;
}

const char *aro_gc_backend_name = "mark_gen";

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
        fprintf(stderr, "[baruby_gc=mark_gen] STRESS\n");
    }
}

// ---------------------------------------------------------------------------
// Slab management
// ---------------------------------------------------------------------------

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
        GCHeader *h = (GCHeader *)slot;
        HDR_SET_KIND(h, KIND_FREE);
        /* size / marked / old / dirty already 0 from mmap zero. */
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[class_idx];
        freelist[class_idx] = fs;
        slot -= sb;
    }
}

static GCHeader *
slab_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size, int class_idx)
{
    if (!freelist[class_idx]) new_page(gc, class_idx);
    FreeSlot *fs = freelist[class_idx];
    freelist[class_idx] = fs->next;
    GCHeader *h = (GCHeader *)fs - 1;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    /* marked/old/dirty already 0 by free_slot's invariant + mmap zero. */
    young_push(gc, h);
    /* iter 35 fairness fix: charge `sizeof(GCHeader) + ALIGN8(payload)`
     * (= "alloc bytes") instead of bare payload_size, matching the
     * bump-pointer gen backends' nursery_top measure.  Without this,
     * 16 MiB threshold meant different things across backends. */
    young_bytes += sizeof(GCHeader) + ALIGN8(payload_size);
    return h;
}

static GCHeader *
large_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size)
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
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    /* marked / old / dirty already 0 from mmap zero. */
    young_push(gc, h);
    young_bytes += sizeof(GCHeader) + ALIGN8(payload_size);
    return h;
}

static void
free_slot(ASTroGC *gc, GCHeader *h)
{
    size_t total = sizeof(GCHeader) + ALIGN8(h->size);
    int cls = size_class_for(total);
    if (cls >= 0) {
        HDR_SET_KIND(h, KIND_FREE);
        h->size = 0;
        /* Clear all gen bits so slab_alloc invariant holds (free slot
         * has marked=old=dirty=0).  Caller skips redundant resets. */
        HDR_CLR_MARKED(h);
        HDR_CLR_OLD(h);
        HDR_CLR_DIRTY(h);
        FreeSlot *fs = (FreeSlot *)(h + 1);
        fs->next = freelist[cls];
        freelist[cls] = fs;
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

static void minor_gc(CTX *c, VALUE *sp_top);
static void major_gc(CTX *c, VALUE *sp_top);

// iter 45: cold-split.  Pull the actual collect dispatch out into a
// noinline cold helper so maybe_collect's hot body shrinks to 1 branch,
// helping aro_gc_alloc stay inliner-budget friendly for baruby_ary_new /
// baruby_str_new.
static void __attribute__((noinline, cold))
maybe_collect_slow(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (old_alloc_since_major > old_major_threshold) {
        major_gc(c, sp_top);
        old_alloc_since_major = 0;
    } else {
        minor_gc(c, sp_top);
    }
}

static inline void
maybe_collect(CTX *c, size_t add, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || young_bytes + add > young_threshold, 0)) {
        maybe_collect_slow(c, sp_top);
    }
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    maybe_collect(c, sizeof(GCHeader) + ALIGN8(payload_size), sp_top);
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    GCHeader *h = (cls >= 0) ? slab_alloc(gc, kind, payload_size, cls)
                           : large_alloc(gc, kind, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    maybe_collect(c, sizeof(GCHeader) + ALIGN8(payload_size), sp_top);
    size_t slot_total = sizeof(GCHeader) + ALIGN8(payload_size);
    int cls = size_class_for(slot_total);
    GCHeader *h = (cls >= 0) ? slab_alloc(gc, KIND_PAYLOAD_BYTE, payload_size, cls)
                           : large_alloc(gc, KIND_PAYLOAD_BYTE, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

/* iter 36 remset overflow guard.  Object-level dirty + push to explicit
 * array can grow unbounded under adversarial workloads (long-lived old
 * table with sparse writes to young rows).  Cap entries; when overflow,
 * dirty bits stay set in headers but we skip the push.  Minor GC detects
 * the overflow flag and heap-walks all pages for dirty olds (O(heap),
 * slower but bounded — never silently drops dirty entries). */
#define MAX_REMSET_ENTRIES (1u << 17)  /* 128 K entries = 1 MiB ptr array */
/* remset_overflow storage moved to ASTroGC.remset_overflow (aliased above). */

static void
remset_push(ASTroGC *gc, GCHeader *h)
{
    if (remset_overflow) return;   /* bit already set; minor will heap-walk */
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

static void scan_outgoing(ASTroGC *gc, GCHeader *h);  /* forward decl for visitor */
static void
remset_visit_minor(ASTroGC *gc, GCHeader *h)
{
    HDR_CLR_DIRTY(h);
    scan_outgoing(gc, h);
}

/* Heap-walk fallback: enumerate dirty old objects across all pages + large
 * list.  Used when remset overflowed.  O(heap) but bounded. */
static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, GCHeader *))
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        size_t sb = size_class_bytes[cls];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[cls]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                GCHeader *h = (GCHeader *)slot;
                if (HDR_KIND(h) == KIND_FREE) continue;
                if (HDR_OLD(h) && HDR_DIRTY(h)) visit(gc, h);
            }
        }
    }
    for (LargeObj *lo = large_head; lo; lo = lo->next) {
        GCHeader *h = (GCHeader *)(lo + 1);
        if (HDR_OLD(h) && HDR_DIRTY(h)) visit(gc, h);
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

// ---------------------------------------------------------------------------
// Mark phase
// ---------------------------------------------------------------------------

static void
gray_push(ASTroGC *gc, GCHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (GCHeader **)realloc(gray_buf, gray_capa * sizeof(GCHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void
mark_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (HDR_MARKED(h)) return;
    // Minor: don't traverse already-old objects via root scan; the
    // remset is responsible for those.  Major: mark everything.
    if (in_minor && HDR_OLD(h)) return;
    HDR_SET_MARKED(h);
    gray_push(gc, h);
}

static void
mark_edge(void *ctx, void **slot)
{
    mark_value((ASTroGC *)ctx, (VALUE)*slot);
}

static void
scan_outgoing(ASTroGC *gc, GCHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)((h)+1), HDR_KIND(h), (h)->size, gc, mark_edge);
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(gc, h);
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
sweep_young(ASTroGC *gc, bool clear_marked)
{
    young_bytes = 0;
    for (size_t i = 0; i < young_objs_cnt; i++) {
        GCHeader *h = young_objs[i];
        if (HDR_MARKED(h)) {
            // Promote: stays in place.  In minor, clear marked (no
            // follow-up scan).  In major, keep marked so the subsequent
            // sweep_old_pages doesn't free us; it'll clear it then.
            if (clear_marked) HDR_CLR_MARKED(h);
            HDR_SET_OLD(h);
            old_bytes += h->size;
            old_alloc_since_major += sizeof(GCHeader) + ALIGN8(h->size); /* iter 36 fairness: occupancy not payload */
        } else {
            gc->common.stats.heap_bytes -= h->size;
            free_slot(gc, h);
        }
    }
    young_objs_cnt = 0;
}

// Major: walk slab pages + large objects, free unmarked OLD slots.
// Young slots have already been handled by sweep_young.
static void
sweep_old_pages(ASTroGC *gc)
{
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        size_t sb = size_class_bytes[cls];
        size_t n_slots = (PAGE_SIZE - PAGE_HDR_BYTES) / sb;
        for (Page *p = page_head[cls]; p; p = p->next) {
            char *slot = (char *)p + PAGE_HDR_BYTES;
            for (size_t i = 0; i < n_slots; i++, slot += sb) {
                GCHeader *h = (GCHeader *)slot;
                if (HDR_KIND(h) == KIND_FREE) continue;
                if (!HDR_OLD(h)) continue;
                if (HDR_MARKED(h)) {
                    HDR_CLR_MARKED(h);
                    HDR_CLR_DIRTY(h);
                } else {
                    old_bytes -= h->size;
                    gc->common.stats.heap_bytes -= h->size;
                    free_slot(gc, h);
                }
            }
        }
    }
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (!HDR_OLD(h)) { link = &lo->next; continue; }
        if (HDR_MARKED(h)) {
            HDR_CLR_MARKED(h);
            HDR_CLR_DIRTY(h);
            link = &lo->next;
        } else {
            *link = lo->next;
            old_bytes -= h->size;
            gc->common.stats.heap_bytes -= h->size;
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
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(gc, *p);
    process_gray(gc);

    // Process remset: old objects with heap writes since last minor.
    if (remset_overflow) {
        // Slow path: heap-walk for dirty olds.  scan_outgoing also clears
        // dirty inline so we don't double-process.
        remset_heap_walk(gc, remset_visit_minor);
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            GCHeader *h = remset_buf[i];
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
major_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;
    remset_cnt = 0;

    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(gc, *p);
    process_gray(gc);
    aro_gc_phase_end(tmark, &gc->common.stats.mark_seconds);

    // In major, keep marked=true on promoted young objects so the
    // subsequent sweep_old_pages doesn't free them as unmarked-old.
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
    major_gc(c, sp_top);
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
    GCHeader *h = (GCHeader *)p - 1;
    return h->size;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (size_class_for(sizeof(GCHeader) + ALIGN8(n)) >= 0)
#define ARO_GC_INPLACE_PAGE_SIZE         PAGE_SIZE
#define ARO_GC_INPLACE_MREMAP_FLAGS      0
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (young_bytes += (d))
#undef large_head
#include "gc_inplace_mremap.h"
#define large_head (gc->large_head)
