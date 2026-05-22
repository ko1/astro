// gc_mark_bump_gen.c — backend #11: bump-allocated nursery + linked-list
// mark&sweep tenured.
//
// Goal: isolate "nursery alloc strategy" from "tenured strategy" in the
// design space:
//   - mark_gen:        malloc nursery + linked-list mark&sweep tenured
//   - mark_compact_gen: bump   nursery + bump-region mark+compact tenured
//   - mark_bump_gen:   bump   nursery + linked-list mark&sweep tenured (this)
//
// Comparing mark_bump_gen vs mark_gen shows the cost of malloc-per-alloc
// in the nursery.  Comparing it vs mark_compact_gen shows the cost of
// mark&sweep tenured (no compaction → fragmentation, cache locality loss)
// against compacted tenured.
//
// Layout:
//   - Nursery: one bump region (16 MiB).  Headers are inline.
//   - Tenured: doubly-linked list of malloc'd { GCHeader, payload } blocks.
//
// Minor GC:
//   1. Scan roots c->env..sp_top — for each nursery VALUE, promote.
//   2. Scan remset (dirty tenured) — promote any nursery refs.
//   3. Cheney scan-loop over freshly-promoted-into-list — for each, forward
//      its outgoing refs.
//   4. Reset nursery_top.
//
// Promotion: bump-alloc a fresh tenured slot, memcpy from nursery.
// Old nursery slot's `fwd` set to new payload pointer for forwarding.
//
// Major GC: mark + region-walk sweep.  Walks tenured region linearly
// from base to top, reading header-size-prefix to find each object.
// No linked list — saves 16 B/header and gives cache-friendly sweep.
// Without compaction the region's bump pointer never resets, so live
// + dead objects accumulate until 1 GiB OOM (fine for short benches).
//
// Write barrier: when writing a heap pointer into an old object, mark it
// dirty + push to remset.  Minor GC walks the remset (not the full old
// list) to find young roots from tenured.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define NURSERY_BYTES  ((size_t)16u  << 20)    /* 16 MiB (tuning knob, not program limit) */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C+: fwd overlay (8 B head). */
_Static_assert(sizeof(ASTroObjectHeader) == 8, "head must be 8 bytes");

#define HDR_MARKED_BIT   (uint16_t)0x0001u
#define HDR_OLD_BIT      (uint16_t)0x0002u
#define HDR_DIRTY_BIT    (uint16_t)0x0004u
#define HDR_FREE_BIT     (uint16_t)0x0008u
#define HDR_FORWARDED    (uint16_t)0x0010u   /* nursery → tenured copied */
#define HDR_IS_FORWARDED(h)  (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)

static inline void *
fwd_overlay_get(ASTroObjectHeader *h)
{
    return *(void **)((char *)h + sizeof(ASTroObjectHeader));
}

static inline void
fwd_overlay_set(ASTroObjectHeader *h, void *new_payload)
{
    *(void **)((char *)h + sizeof(ASTroObjectHeader)) = new_payload;
}
#define HDR_MARKED(h)      (((h)->gc_flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->gc_flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->gc_flags &= (uint16_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->gc_flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->gc_flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->gc_flags &= (uint16_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->gc_flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->gc_flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->gc_flags &= (uint16_t)~HDR_DIRTY_BIT)
#define HDR_IS_FREE(h)     (((h)->gc_flags & HDR_FREE_BIT) != 0)
#define HDR_SET_FREE(h)    ((h)->gc_flags |= HDR_FREE_BIT)
// NB: no linked list — tenured objects live in a contiguous mmap region,
// sweep iterates by region walk (header-size-prefix), not by chasing
// next pointers.  Saves 16 bytes/header and turns sweep into a
// sequential scan (cache-friendly).

#define MAJOR_THRESHOLD_MIN  (16u * 1024u * 1024u)

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    char *nursery_base, *nursery_top, *nursery_end;
    /* Tenured: bump-allocated within a single mmap'd region. */
    char *tenured_base, *tenured_top, *tenured_end;
    size_t old_bytes;
    size_t old_alloc_since_major;
    size_t old_major_threshold;
    CTX  *ctx;
    VALUE *sp_high_water;
    bool  in_minor;
    /* Cheney scan queue for freshly-promoted-during-minor. */
    struct ASTroObjectHeader **scan_buf;
    size_t            scan_head, scan_tail, scan_capa;
    struct ASTroObjectHeader **gray_buf;
    size_t            gray_cnt, gray_capa;
    struct ASTroObjectHeader **remset_buf;
    size_t            remset_cnt, remset_capa;
    bool              remset_overflow;
} ASTroGC;

#define nursery_base          (gc->nursery_base)
#define nursery_top           (gc->nursery_top)
#define nursery_end           (gc->nursery_end)
#define tenured_base          (gc->tenured_base)
#define tenured_top           (gc->tenured_top)
#define tenured_end           (gc->tenured_end)
#define old_bytes             (gc->old_bytes)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define old_major_threshold   (gc->old_major_threshold)
#define gc_ctx                (gc->ctx)
#define sp_high_water         (gc->sp_high_water)
#define in_minor              (gc->in_minor)
#define scan_buf              (gc->scan_buf)
#define scan_head             (gc->scan_head)
#define scan_tail             (gc->scan_tail)
#define scan_capa             (gc->scan_capa)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define remset_overflow       (gc->remset_overflow)

const char *aro_gc_backend_name = "mark_bump_gen";

static char *
mmap_region(size_t bytes)
{
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    old_major_threshold = MAJOR_THRESHOLD_MIN;
    nursery_base = mmap_region(NURSERY_BYTES);
    nursery_top  = nursery_base;
    nursery_end  = nursery_base + NURSERY_BYTES;
    tenured_base = mmap_region(TENURED_BYTES);
    tenured_top  = tenured_base;
    tenured_end  = tenured_base + TENURED_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        fprintf(stderr, "[baruby_gc=mark_bump_gen] STRESS mode: collect on every alloc\n");
    }
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(CTX *c, VALUE *sp_top);
static void major_gc(CTX *c, VALUE *sp_top);

static ASTroObjectHeader *old_alloc(ASTroGC *gc, size_t payload_size, size_t aligned)
{
    size_t total = aligned;
    if (tenured_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_bump_gen: tenured OOM (%zu / %zu)\n",
                (size_t)(tenured_top - tenured_base), (size_t)TENURED_BYTES);
        abort();
    }
    ASTroObjectHeader *h = (ASTroObjectHeader *)tenured_top;
    tenured_top += total;
    h->flags    = 0;
    h->gc_flags = HDR_OLD_BIT;
    h->gc_size  = (uint32_t)payload_size;
    old_bytes += payload_size;
    old_alloc_since_major += ALIGN8(payload_size);
    return h;
}

static void __attribute__((noinline, cold))
nursery_collect_slow(CTX *c, size_t total, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (old_alloc_since_major > old_major_threshold) {
        major_gc(c, sp_top);
    } else {
        minor_gc(c, sp_top);
    }
    if (nursery_top + total > nursery_end) {
        major_gc(c, sp_top);
        if (nursery_top + total > nursery_end) {
            fprintf(stderr, "baruby_gc=mark_bump_gen: OOM (need %zu)\n", total);
            abort();
        }
    }
}

static inline ASTroObjectHeader *
nursery_bump(CTX *c, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(payload_size >= NURSERY_BYTES / 2, 0)) {
        return old_alloc(gc, payload_size, aligned);
    }

    if (__builtin_expect(gc->common.stress || nursery_top + total > nursery_end, 0)) {
        nursery_collect_slow(c, total, sp_top);
    }
    ASTroObjectHeader *h = (ASTroObjectHeader *)nursery_top;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    nursery_top += total;
    return h;
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    ASTroObjectHeader *h = nursery_bump(c, payload_size, aligned, sp_top);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(ASTroObjectHeader), 0,
           aligned - sizeof(ASTroObjectHeader));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    ASTroObjectHeader *h = nursery_bump(c, payload_size, aligned, sp_top);
    void *payload = (void *)h;
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

/* iter 36 remset overflow guard — see gc_mark_gen.c for rationale.
 * Storage: ASTroGC.remset_overflow (aliased above). */
#define MAX_REMSET_ENTRIES (1u << 17)

static void
remset_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (remset_overflow) return;
    if (remset_cnt >= MAX_REMSET_ENTRIES) { remset_overflow = true; return; }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (ASTroObjectHeader **)realloc(remset_buf, remset_capa * sizeof(ASTroObjectHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

static void process_object(ASTroGC *gc, ASTroObjectHeader *h);
static void
remset_visit_minor(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (HDR_DIRTY(h)) {
        process_object(gc, h);
        HDR_CLR_DIRTY(h);
    }
}

static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, ASTroObjectHeader *))
{
    char *scan = tenured_base;
    while (scan < tenured_top) {
        ASTroObjectHeader *h = (ASTroObjectHeader *)scan;
        visit(gc, h);
        scan += ALIGN8(h->gc_size);
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
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
    ASTroObjectHeader *hh = (ASTroObjectHeader *)holder;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        HDR_SET_DIRTY(hh);
        remset_push(gc, hh);
    }
}

// ---------------------------------------------------------------------------
// Minor GC: promote nursery survivors to tenured (linked list).
// ---------------------------------------------------------------------------

static inline bool
in_nursery(ASTroGC *gc, void *p)
{
    return (char *)p >= nursery_base && (char *)p < nursery_end;
}

static void
scan_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (scan_tail >= scan_capa) {
        scan_capa = scan_capa ? scan_capa * 2 : 256;
        scan_buf = (ASTroObjectHeader **)realloc(scan_buf, scan_capa * sizeof(ASTroObjectHeader *));
        if (!scan_buf) abort();
    }
    scan_buf[scan_tail++] = h;
}

static void
gray_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (ASTroObjectHeader **)realloc(gray_buf, gray_capa * sizeof(ASTroObjectHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void *
promote(ASTroGC *gc, ASTroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t aligned = ALIGN8(oldh->gc_size);
    ASTroObjectHeader *newh = old_alloc(gc, oldh->gc_size, aligned);
    memcpy((void *)newh, (void *)oldh, aligned);
    /* memcpy overwrote newh's head — restore framework fields. */
    newh->gc_flags = HDR_OLD_BIT;     /* fresh tenured slot, no mark/dirty/fwd */
    void *new_payload = (void *)newh;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, new_payload);
    scan_push(gc, newh);
    return new_payload;
}

static void *
forward_payload_value(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    if (in_minor) {
        if (!in_nursery(gc, p)) return p;
        return promote(gc, h);
    }
    return p;
}

static VALUE
forward_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_value(gc, (void *)v);
}

static void
forward_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (IS_PTR(v)) *slot = (void *)(VALUE)forward_payload_value(gc, (void *)v);
}

static void
process_object(ASTroGC *gc, ASTroObjectHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge);
}

/* Keep cold (see gc_copy_gen.c iter (29)): inlining minor_gc into
 * nursery_bump bloats the alloc hot path past the inliner budget,
 * leaving an extra `call` on every fast-path alloc. */
static void __attribute__((noinline))
minor_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(gc, *p);

    if (remset_overflow) {
        remset_heap_walk(gc, remset_visit_minor);
        remset_overflow = false;
    } else {
        for (size_t i = 0; i < remset_cnt; i++) {
            ASTroObjectHeader *h = remset_buf[i];
            if (HDR_DIRTY(h)) {
                process_object(gc, h);
                HDR_CLR_DIRTY(h);
            }
        }
    }
    remset_cnt = 0;

    while (scan_head < scan_tail) {
        ASTroObjectHeader *h = scan_buf[scan_head++];
        process_object(gc, h);
    }
    scan_head = scan_tail = 0;

    nursery_top = nursery_base;
    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    gc->common.stats.heap_bytes = old_bytes;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

// ---------------------------------------------------------------------------
// Major GC: Cheney-style "trace + promote + mark" in one pass.
//
// During major, both old and nursery contain potentially-live objects.
// We need to:
//   - Mark surviving tenured (for sweep to skip them).
//   - Promote surviving nursery objects to tenured (linked list).
//   - Replace all nursery pointers in survivors with their new tenured loc.
//
// Single Cheney-style pass:
//   1. For each root: if nursery → promote (sets fwd), push to scan queue.
//                     If tenured → mark, push to gray queue.
//   2. Drain gray queue (= mark phase on tenured side).  When scanning a
//      tenured object's outgoing refs:
//        - If ref → nursery: promote, replace ref with new tenured ptr,
//          push to scan queue.
//        - If ref → tenured unmarked: mark, push to gray queue.
//   3. Drain scan queue (= promote-then-scan-outgoing on newly-promoted).
//      Same logic; intermixed with gray drain until both empty.
//   4. Sweep tenured: free unmarked, clear marked/dirty/fwd.
//   5. Reset nursery_top.
//
// This is O(live) per major instead of the O(live × depth) iterated
// fixup the naive version did.
// ---------------------------------------------------------------------------

static void *
major_promote(ASTroGC *gc, ASTroObjectHeader *oldh)
{
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t aligned = ALIGN8(oldh->gc_size);
    ASTroObjectHeader *newh = old_alloc(gc, oldh->gc_size, aligned);
    memcpy((void *)newh, (void *)oldh, aligned);
    newh->gc_flags = HDR_OLD_BIT | HDR_MARKED_BIT;
    void *new_payload = (void *)newh;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, new_payload);
    return new_payload;
}

static void
major_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *vh = (ASTroObjectHeader *)v;
    if (in_nursery(gc, (void *)v)) {
        *slot = (void *)(VALUE)major_promote(gc, vh);
        scan_push(gc, (ASTroObjectHeader *)*slot - 1);
    } else if (!HDR_MARKED(vh)) {
        HDR_SET_MARKED(vh);
        gray_push(gc, vh);
    }
}

static void
major_process(ASTroGC *gc, ASTroObjectHeader *h)
{
    ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, major_edge);
}

static void
major_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = false;

    remset_cnt = 0;
    scan_head = scan_tail = 0;
    gray_cnt = 0;

    for (VALUE *p = c->env; p < sp_top; p++) {
        VALUE v = *p;
        if (!IS_PTR(v)) continue;
        ASTroObjectHeader *h = (ASTroObjectHeader *)v;
        if (in_nursery(gc, (void *)v)) {
            *p = (VALUE)major_promote(gc, h);
            scan_push(gc, (ASTroObjectHeader *)*p - 1);
        } else if (!HDR_MARKED(h)) {
            HDR_SET_MARKED(h);
            gray_push(gc, h);
        }
    }

    while (gray_cnt > 0 || scan_head < scan_tail) {
        while (gray_cnt > 0) {
            ASTroObjectHeader *h = gray_buf[--gray_cnt];
            major_process(gc, h);
        }
        while (scan_head < scan_tail) {
            ASTroObjectHeader *h = scan_buf[scan_head++];
            major_process(gc, h);
        }
    }
    scan_head = scan_tail = 0;

    {
        char *p = tenured_base;
        size_t live = 0;
        while (p < tenured_top) {
            ASTroObjectHeader *h = (ASTroObjectHeader *)p;
            size_t total = ALIGN8(h->gc_size);
            if (HDR_MARKED(h)) {
                HDR_CLR_MARKED(h);
                HDR_CLR_DIRTY(h);
                /* FORWARDED bit is meaningful only on from-objects;
                 * tenured survivors after major_promote don't have it. */
                live += h->gc_size;
            }
            p += total;
        }
        old_bytes = live;
    }

    nursery_top = nursery_base;

    if (!gc->common.stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;

    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    gc->common.stats.heap_bytes = old_bytes;
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
    if (nursery_base) munmap(nursery_base, NURSERY_BYTES);
    if (tenured_base) munmap(tenured_base, TENURED_BYTES);
    free(scan_buf);
    free(gray_buf);
    free(remset_buf);
    free(gc);
    c->astro_gc = NULL;
}


size_t
aro_gc_size_of(void *p)
{
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    return h->gc_size;
}
