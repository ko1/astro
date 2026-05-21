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

/* 16-byte header (down from 24).  kind + old + dirty + marked packed in flags. */
typedef struct GCHeader {
    void    *fwd;                   // forwarding pointer (set during minor for nursery objs)
    uint8_t  flags;                 /* bits 0-2: kind, bit 3: marked, bit 4: old, bit 5: dirty */
    uint8_t  _pad[3];
    uint32_t size;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 16, "GCHeader must be 16 bytes");

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
    struct GCHeader **scan_buf;
    size_t            scan_head, scan_tail, scan_capa;
    struct GCHeader **gray_buf;
    size_t            gray_cnt, gray_capa;
    struct GCHeader **remset_buf;
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

static GCHeader *
old_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size, size_t aligned)
{
    size_t total = sizeof(GCHeader) + aligned;
    if (tenured_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_bump_gen: tenured OOM (%zu / %zu)\n",
                (size_t)(tenured_top - tenured_base), (size_t)TENURED_BYTES);
        abort();
    }
    GCHeader *h = (GCHeader *)tenured_top;
    tenured_top += total;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    h->fwd    = NULL;
    HDR_SET_OLD(h);
    HDR_CLR_DIRTY(h);
    HDR_CLR_MARKED(h);
    old_bytes += payload_size;
    old_alloc_since_major += sizeof(GCHeader) + ALIGN8(payload_size);
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

static inline GCHeader *
nursery_bump(CTX *c, AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t total = sizeof(GCHeader) + aligned;

    if (__builtin_expect(payload_size >= NURSERY_BYTES / 2, 0)) {
        return old_alloc(gc, kind, payload_size, aligned);
    }

    if (__builtin_expect(gc->common.stress || nursery_top + total > nursery_end, 0)) {
        nursery_collect_slow(c, total, sp_top);
    }
    GCHeader *h = (GCHeader *)nursery_top;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    h->fwd    = NULL;
    HDR_CLR_OLD(h);
    HDR_CLR_DIRTY(h);
    HDR_CLR_MARKED(h);
    nursery_top += total;
    return h;
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = nursery_bump(c, kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);
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
    GCHeader *h = nursery_bump(c, KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    VALUE *sp_top = c->sp;
    if (!old) return aro_gc_alloc(c, KIND_PAYLOAD_VAL, new_size);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = HDR_KIND(oldh);
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    // Root old via sp_top[0] so GC tracks the source through promotion.
    // See gc_copy_gen.c for the rationale.
    sp_top[0] = (VALUE)old;

    c->sp = sp_top + 1;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size)
        : aro_gc_alloc(c, kind, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

/* iter 36 remset overflow guard — see gc_mark_gen.c for rationale.
 * Storage: ASTroGC.remset_overflow (aliased above). */
#define MAX_REMSET_ENTRIES (1u << 17)

static void
remset_push(ASTroGC *gc, GCHeader *h)
{
    if (remset_overflow) return;
    if (remset_cnt >= MAX_REMSET_ENTRIES) { remset_overflow = true; return; }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (GCHeader **)realloc(remset_buf, remset_capa * sizeof(GCHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
}

static void process_object(ASTroGC *gc, GCHeader *h);
static void
remset_visit_minor(ASTroGC *gc, GCHeader *h)
{
    if (HDR_DIRTY(h)) {
        process_object(gc, h);
        HDR_CLR_DIRTY(h);
    }
}

static void
remset_heap_walk(ASTroGC *gc, void (*visit)(ASTroGC *, GCHeader *))
{
    char *scan = tenured_base;
    while (scan < tenured_top) {
        GCHeader *h = (GCHeader *)scan;
        visit(gc, h);
        scan += sizeof(GCHeader) + ALIGN8(h->size);
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
// Minor GC: promote nursery survivors to tenured (linked list).
// ---------------------------------------------------------------------------

static inline bool
in_nursery(ASTroGC *gc, void *p)
{
    return (char *)p >= nursery_base && (char *)p < nursery_end;
}

static void
scan_push(ASTroGC *gc, GCHeader *h)
{
    if (scan_tail >= scan_capa) {
        scan_capa = scan_capa ? scan_capa * 2 : 256;
        scan_buf = (GCHeader **)realloc(scan_buf, scan_capa * sizeof(GCHeader *));
        if (!scan_buf) abort();
    }
    scan_buf[scan_tail++] = h;
}

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

static void *
promote(ASTroGC *gc, GCHeader *oldh)
{
    if (oldh->fwd) return oldh->fwd;
    size_t aligned = ALIGN8(oldh->size);
    GCHeader *newh = old_alloc(gc, HDR_KIND(oldh), oldh->size, aligned);
    memcpy((void *)(newh + 1), (void *)(oldh + 1), aligned);
    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    scan_push(gc, newh);
    return new_payload;
}

static void *
forward_payload_value(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    GCHeader *h = (GCHeader *)p - 1;
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
process_object(ASTroGC *gc, GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload_value(gc, a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (!BSTR_IS_SSO(s) && s->bytes) s->bytes = (char *)forward_payload_value(gc, s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = forward_value(gc, items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "process_object: unknown kind");
    }
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
            GCHeader *h = remset_buf[i];
            if (HDR_DIRTY(h)) {
                process_object(gc, h);
                HDR_CLR_DIRTY(h);
            }
        }
    }
    remset_cnt = 0;

    while (scan_head < scan_tail) {
        GCHeader *h = scan_buf[scan_head++];
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
major_promote(ASTroGC *gc, GCHeader *oldh)
{
    if (oldh->fwd) return oldh->fwd;
    size_t aligned = ALIGN8(oldh->size);
    GCHeader *newh = old_alloc(gc, HDR_KIND(oldh), oldh->size, aligned);
    memcpy((void *)(newh + 1), (void *)(oldh + 1), aligned);
    HDR_SET_MARKED(newh);
    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    return new_payload;
}

static void
major_process(ASTroGC *gc, GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) {
            GCHeader *ih = (GCHeader *)a->items - 1;
            if (in_nursery(gc, a->items)) {
                a->items = (VALUE *)major_promote(gc, ih);
                scan_push(gc, (GCHeader *)a->items - 1);
            } else if (!HDR_MARKED(ih)) {
                HDR_SET_MARKED(ih);
                gray_push(gc, ih);
            }
        }
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (!BSTR_IS_SSO(s) && s->bytes) {
            GCHeader *bh = (GCHeader *)s->bytes - 1;
            if (in_nursery(gc, s->bytes)) {
                s->bytes = (char *)major_promote(gc, bh);
                scan_push(gc, (GCHeader *)s->bytes - 1);
            } else if (!HDR_MARKED(bh)) {
                HDR_SET_MARKED(bh);
                gray_push(gc, bh);
            }
        }
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) {
            VALUE v = items[i];
            if (!IS_PTR(v)) continue;
            GCHeader *vh = (GCHeader *)v - 1;
            if (in_nursery(gc, (void *)v)) {
                items[i] = (VALUE)major_promote(gc, vh);
                scan_push(gc, (GCHeader *)items[i] - 1);
            } else if (!HDR_MARKED(vh)) {
                HDR_SET_MARKED(vh);
                gray_push(gc, vh);
            }
        }
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "major_process: unknown kind");
    }
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
        GCHeader *h = (GCHeader *)v - 1;
        if (in_nursery(gc, (void *)v)) {
            *p = (VALUE)major_promote(gc, h);
            scan_push(gc, (GCHeader *)*p - 1);
        } else if (!HDR_MARKED(h)) {
            HDR_SET_MARKED(h);
            gray_push(gc, h);
        }
    }

    while (gray_cnt > 0 || scan_head < scan_tail) {
        while (gray_cnt > 0) {
            GCHeader *h = gray_buf[--gray_cnt];
            major_process(gc, h);
        }
        while (scan_head < scan_tail) {
            GCHeader *h = scan_buf[scan_head++];
            major_process(gc, h);
        }
    }
    scan_head = scan_tail = 0;

    {
        char *p = tenured_base;
        size_t live = 0;
        while (p < tenured_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (HDR_MARKED(h)) {
                HDR_CLR_MARKED(h);
                HDR_CLR_DIRTY(h);
                h->fwd    = NULL;
                live += h->size;
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

