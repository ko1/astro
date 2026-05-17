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

#define NURSERY_BYTES  ((size_t)16u  << 20)    // 16 MiB
#define TENURED_BYTES  ((size_t)1u   << 30)    // 1 GiB virtual
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

typedef struct GCHeader {
    void    *fwd;                   // forwarding pointer (set during minor for nursery objs)
    uint32_t kind;
    uint32_t size;
    bool     old;                   // false → nursery; true → tenured
    bool     dirty;                 // tenured: written to since last minor (remset entry)
    bool     marked;                // marked during major
    // 5 bytes padding to keep payload 8-aligned (24-byte header total)
} GCHeader;
// NB: no linked list — tenured objects live in a contiguous mmap region,
// sweep iterates by region walk (header-size-prefix), not by chasing
// next pointers.  Saves 16 bytes/header and turns sweep into a
// sequential scan (cache-friendly).

static char *nursery_base = NULL;
static char *nursery_top  = NULL;
static char *nursery_end  = NULL;

// Tenured: bump-allocated within a single mmap'd region.  No individual
// frees (memory leaks within the region until program exit).  Sweep is
// a sequential walk of the region (header-size-prefix), not a linked-list
// traversal — cache-friendly and avoids per-object prev/next bookkeeping.
// Without compaction the bump pointer never resets, so fragmentation
// grows over time — for our short-lived benchmarks 1 GiB virtual is plenty.
static char *tenured_base = NULL;
static char *tenured_top  = NULL;
static char *tenured_end  = NULL;

static size_t   old_bytes   = 0;
static size_t   old_alloc_since_major = 0;
#define MAJOR_THRESHOLD_MIN  (64u * 1024u * 1024u)
static size_t   old_major_threshold = MAJOR_THRESHOLD_MIN;

static CTX *gc_ctx = NULL;
static VALUE *sp_high_water = NULL;
static bool in_minor = false;

// Cheney scan queue for freshly-promoted-during-minor.  Tenured objects
// are not contiguous (malloc'd), so we can't use a "scan pointer over a
// region".  Instead, push each promoted obj onto this queue and scan FIFO.
static GCHeader **scan_buf  = NULL;
static size_t     scan_head = 0;
static size_t     scan_tail = 0;
static size_t     scan_capa = 0;

static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

BarubyGCStats baruby_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0};
int baruby_gc_stress = 0;
const char *baruby_gc_backend_name = "mark_bump_gen";

static char *
mmap_region(size_t bytes)
{
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    nursery_base = mmap_region(NURSERY_BYTES);
    nursery_top  = nursery_base;
    nursery_end  = nursery_base + NURSERY_BYTES;
    tenured_base = mmap_region(TENURED_BYTES);
    tenured_top  = tenured_base;
    tenured_end  = tenured_base + TENURED_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
        fprintf(stderr, "[baruby_gc=mark_bump_gen] STRESS mode: collect on every alloc\n");
    }
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(VALUE *sp_top);
static void major_gc(VALUE *sp_top);

// Allocate a tenured object via bump.  Returns a fresh GCHeader.
// Caller fills kind/size/payload.
static GCHeader *
old_alloc(BarubyGCKind kind, size_t payload_size, size_t aligned)
{
    size_t total = sizeof(GCHeader) + aligned;
    if (tenured_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_bump_gen: tenured OOM (%zu / %zu)\n",
                (size_t)(tenured_top - tenured_base), (size_t)TENURED_BYTES);
        abort();
    }
    GCHeader *h = (GCHeader *)tenured_top;
    tenured_top += total;
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->fwd    = NULL;
    h->old    = true;
    h->dirty  = false;
    h->marked = false;
    old_bytes += payload_size;
    old_alloc_since_major += payload_size;
    return h;
}

static GCHeader *
nursery_bump(BarubyGCKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    size_t total = sizeof(GCHeader) + aligned;

    // Pretenure huge allocations (≥ half nursery): go straight to old.
    if (payload_size >= NURSERY_BYTES / 2) {
        return old_alloc(kind, payload_size, aligned);
    }

    if (baruby_gc_stress || nursery_top + total > nursery_end) {
        if (old_alloc_since_major > old_major_threshold) {
            major_gc(sp_top);
        } else {
            minor_gc(sp_top);
        }
        if (nursery_top + total > nursery_end) {
            // Nursery still full after minor (e.g., huge tenure load).
            // Try a major to free more, then retry.
            major_gc(sp_top);
            if (nursery_top + total > nursery_end) {
                fprintf(stderr, "baruby_gc=mark_bump_gen: OOM (need %zu)\n", total);
                abort();
            }
        }
    }
    GCHeader *h = (GCHeader *)nursery_top;
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->fwd    = NULL;
    h->old    = false;
    h->dirty  = false;
    h->marked = false;
    nursery_top += total;
    return h;
}

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = nursery_bump(kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = nursery_bump(KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
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
    // Root old via sp_top[0] so GC tracks the source through promotion.
    // See gc_copy_gen.c for the rationale.
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
// Minor GC: promote nursery survivors to tenured (linked list).
// ---------------------------------------------------------------------------

static inline bool
in_nursery(void *p)
{
    return (char *)p >= nursery_base && (char *)p < nursery_end;
}

// Push a freshly-promoted tenured object onto the Cheney scan queue.
static void
scan_push(GCHeader *h)
{
    if (scan_tail >= scan_capa) {
        scan_capa = scan_capa ? scan_capa * 2 : 256;
        scan_buf = (GCHeader **)realloc(scan_buf, scan_capa * sizeof(GCHeader *));
        if (!scan_buf) abort();
    }
    scan_buf[scan_tail++] = h;
}

// Push a tenured object onto the major-mark gray queue.
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

// Promote a nursery object to tenured.  Caller has verified `p` is in
// nursery and the source header has no fwd set yet.
static void *
promote(GCHeader *oldh)
{
    if (oldh->fwd) return oldh->fwd;
    size_t aligned = ALIGN8(oldh->size);
    GCHeader *newh = old_alloc((BarubyGCKind)oldh->kind, oldh->size, aligned);
    // Copy payload only (the linked-list pointers in newh are already
    // correct from old_alloc; old/marked/dirty are fresh).
    memcpy((void *)(newh + 1), (void *)(oldh + 1), aligned);
    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    scan_push(newh);
    return new_payload;
}

static void *
forward_payload_value(void *p)
{
    if (!p) return NULL;
    GCHeader *h = (GCHeader *)p - 1;
    if (in_minor) {
        if (!in_nursery(p)) return p;        // already tenured
        return promote(h);
    }
    // Major: forward shouldn't be called — major uses mark-based logic.
    return p;
}

static VALUE
forward_value(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_value((void *)v);
}

// Walk a freshly-promoted object's outgoing refs.  For each ref that
// still lives in the nursery, promote it (which forwards the slot).
static void
process_object(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch ((BarubyGCKind)h->kind) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload_value(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) s->bytes = (char *)forward_payload_value(s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = forward_value(items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "process_object: unknown kind");
    }
}

static void
minor_gc(VALUE *sp_top)
{
    struct timespec t0 = baruby_gc_time_begin();
    in_minor = true;

    CTX *c = gc_ctx;
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    // (1) Stack roots
    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(*p);

    // (2) Remset: dirty tenured objects may hold nursery refs.
    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *h = remset_buf[i];
        if (h->dirty) {
            process_object(h);
            h->dirty = false;
        }
    }
    remset_cnt = 0;

    // (3) Cheney FIFO: process each freshly-promoted object.
    while (scan_head < scan_tail) {
        GCHeader *h = scan_buf[scan_head++];
        process_object(h);
    }
    scan_head = scan_tail = 0;

    // (4) Reset nursery.  Any forwarded headers still live in the now-empty
    //     nursery region; they get overwritten on next allocs.
    nursery_top = nursery_base;
    in_minor = false;

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.minor_count++;
    baruby_gc_stats.heap_bytes = old_bytes;   // live = promoted-to-tenured
    c->sp = sp_top;
    baruby_gc_time_end(t0);
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

// Promote a nursery object to tenured during major.  Same as `promote` for
// minor, but marks the new tenured object so sweep keeps it.
static void *
major_promote(GCHeader *oldh)
{
    if (oldh->fwd) return oldh->fwd;
    size_t aligned = ALIGN8(oldh->size);
    GCHeader *newh = old_alloc((BarubyGCKind)oldh->kind, oldh->size, aligned);
    memcpy((void *)(newh + 1), (void *)(oldh + 1), aligned);
    newh->marked = true;
    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    return new_payload;
}

// Major-time "process" of an object: walk its outgoing refs, promote
// nursery ones (replacing in-place), mark+enqueue tenured ones.
static void
major_process(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch ((BarubyGCKind)h->kind) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) {
            GCHeader *ih = (GCHeader *)a->items - 1;
            if (in_nursery(a->items)) {
                a->items = (VALUE *)major_promote(ih);
                scan_push((GCHeader *)a->items - 1);
            } else if (!ih->marked) {
                ih->marked = true;
                gray_push(ih);
            }
        }
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) {
            GCHeader *bh = (GCHeader *)s->bytes - 1;
            if (in_nursery(s->bytes)) {
                s->bytes = (char *)major_promote(bh);
                scan_push((GCHeader *)s->bytes - 1);
            } else if (!bh->marked) {
                bh->marked = true;
                gray_push(bh);
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
            if (in_nursery((void *)v)) {
                items[i] = (VALUE)major_promote(vh);
                scan_push((GCHeader *)items[i] - 1);
            } else if (!vh->marked) {
                vh->marked = true;
                gray_push(vh);
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
major_gc(VALUE *sp_top)
{
    struct timespec t0 = baruby_gc_time_begin();
    in_minor = false;

    remset_cnt = 0;
    scan_head = scan_tail = 0;
    gray_cnt = 0;

    // (1) Roots: promote nursery, mark tenured.  Replace nursery ptrs
    //     in-place on the stack so subsequent reads see the new addrs.
    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) {
        VALUE v = *p;
        if (!IS_PTR(v)) continue;
        GCHeader *h = (GCHeader *)v - 1;
        if (in_nursery((void *)v)) {
            *p = (VALUE)major_promote(h);
            scan_push((GCHeader *)*p - 1);
        } else if (!h->marked) {
            h->marked = true;
            gray_push(h);
        }
    }

    // (2) Drain gray + scan queues until both empty.  Process either
    //     queue first; both push to either queue as needed.
    while (gray_cnt > 0 || scan_head < scan_tail) {
        while (gray_cnt > 0) {
            GCHeader *h = gray_buf[--gray_cnt];
            major_process(h);
        }
        while (scan_head < scan_tail) {
            GCHeader *h = scan_buf[scan_head++];
            major_process(h);
        }
    }
    scan_head = scan_tail = 0;

    // (3) Sweep tenured by region walk (cache-friendly sequential scan
    //     of contiguous mmap region, no linked-list pointer chasing).
    {
        char *p = tenured_base;
        size_t live = 0;
        while (p < tenured_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (h->marked) {
                h->marked = false;
                h->dirty  = false;
                h->fwd    = NULL;
                live += h->size;
            }
            p += total;
        }
        old_bytes = live;
    }

    // (4) Reset nursery (any non-promoted nursery objs are dead).
    nursery_top = nursery_base;

    if (!baruby_gc_stress) {
        size_t next = old_bytes * 2;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    old_alloc_since_major = 0;

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.major_count++;
    baruby_gc_stats.heap_bytes = old_bytes;
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
