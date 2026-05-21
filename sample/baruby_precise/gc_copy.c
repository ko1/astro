#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

// ----------------------------------------------------------------------------
// Semispace (Cheney) moving GC.
//
// iter 62: framework abstraction PoC.  This backend now uses the contract
// macros (ASTRO_GC_SCAN_EDGES, ASTRO_GC_INSTANCE, etc.) and consolidates
// process-scope state into `struct AstroGc`.  Other backends remain on
// the old shape (module-static + direct kind switch) until ported.
// ----------------------------------------------------------------------------

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "copy";

/* 16-byte header.  kind packed to flags byte. */
typedef struct GCHeader {
    uint8_t  flags;     /* bits 0-2: kind */
    uint8_t  _pad[3];
    uint32_t size;
    void    *fwd;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 16, "GCHeader must be 16 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))

#define REGION_BYTES  ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virtual per semispace, lazy-paged */
/* Stress mode mmaps a fresh to-space every GC and leaves the old one
 * PROT_NONE'd for stale-pointer detection.  At 64 GiB per region that
 * accumulates TiBs of virtual address space (and slows mprotect /
 * madvise per GC), so stress mode uses a much smaller region. */
#define STRESS_REGION_BYTES  ((size_t)64u << 20)  /* 64 MiB */
#define ALIGN8(n)     (((n) + 7u) & ~(size_t)7u)

/* Adaptive GC trigger.  Match the mark / immix policy: trigger at
 * `bytes_since_gc > gc_threshold`, gc_threshold = max(16 MiB, 2 × live_post_cheney). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// AstroGc: process-scope GC instance.  Allocated once at program start
// (`aro_gc_init`).  Holds all backend state that used to live in
// module-static variables.  Future framework version: multiple instances
// coexist by allocating multiple `AstroGc` and threading them through CTX.
// ----------------------------------------------------------------------------
typedef struct AstroGc {
    /* Active semispace (where allocations go).  In stress mode each GC
     * mmaps a fresh to-space; in non-stress mode we alternate the
     * `space0` / `space1` pair. */
    char *active_base;
    char *active_top;
    char *active_end;
    char *space0;
    char *space1;
    int   active_idx;

    /* Adaptive trigger state */
    size_t bytes_since_gc;
    size_t gc_threshold;

    /* Reserved region size — stress mode uses a much smaller region. */
    size_t region_bytes;

    /* CTX bind (single-instance only).  Future multi-thread / multi-
     * instance: CTX → AstroGc pointer via macro. */
    CTX *ctx;

    /* Cheney scratch (used during gc_collect_internal only) */
    char *to_top;
    char *to_base;
    char *from_base_cur;
    VALUE *sp_high_water;
} AstroGc;

/* Single process-scope GC instance for baruby_precise.  CTX::astro_gc
 * points here (= bound at aro_gc_init).  Multi-instance use case would
 * allocate multiple AstroGc and wire different CTX instances to each. */
static AstroGc g_astro_gc;

// Contract macros (ASTRO_GC_SCAN_EDGES / INIT_PAYLOAD / HEADER_* / etc) live
// in context.h so all backends share them.  See docs/gc_design.md §2.

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

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
    AstroGc *gc = &g_astro_gc;
    memset(gc, 0, sizeof(*gc));
    gc->ctx = c;
    gc->gc_threshold = GC_THRESHOLD_MIN;
    c->astro_gc = gc;             /* CTX → AstroGc を bind */
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        gc->region_bytes = STRESS_REGION_BYTES;
        gc->active_base = mmap_region(gc->region_bytes);
        gc->active_top  = gc->active_base;
        gc->active_end  = gc->active_base + gc->region_bytes;
        fprintf(stderr, "[baruby_gc] STRESS mode: collect on every alloc, "
                        "every old space PROT_NONE forever (madvise DONTNEED)\n");
    } else {
        gc->region_bytes = REGION_BYTES;
        gc->space0 = mmap_region(gc->region_bytes);
        gc->space1 = mmap_region(gc->region_bytes);
        gc->active_idx  = 0;
        gc->active_base = gc->space0;
        gc->active_top  = gc->space0;
        gc->active_end  = gc->space0 + gc->region_bytes;
    }
}

// ----------------------------------------------------------------------------
// Allocation
// ----------------------------------------------------------------------------

static void gc_collect_internal(CTX *c, VALUE *sp_top);

static void __attribute__((noinline, cold))
gc_bump_slow(CTX *c, size_t total, VALUE *sp_top)
{
    AstroGc *gc = ASTRO_GC_INSTANCE(c);
    gc_collect_internal(c, sp_top);
    if (gc->active_top + total > gc->active_end) {
        fprintf(stderr, "baruby_gc: OOM (need %zu, have %zu)\n",
                total, (size_t)(gc->active_end - gc->active_top));
        abort();
    }
}

static inline GCHeader *
gc_bump(CTX *c, AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    AstroGc *gc = ASTRO_GC_INSTANCE(c);
    size_t total = sizeof(GCHeader) + aligned;
    if (__builtin_expect(aro_gc_stress
                         || gc->bytes_since_gc + payload_size > gc->gc_threshold
                         || (gc->active_top + total) > gc->active_end, 0)) {
        gc_bump_slow(c, total, sp_top);
    }
    GCHeader *h = (GCHeader *)gc->active_top;
    HDR_SET_KIND(h, kind);
    ASTRO_GC_HEADER_SET_SIZE(h, payload_size);
    ASTRO_GC_HEADER_SET_FWD(h, NULL);
    gc->active_top += total;
    gc->bytes_since_gc += payload_size;
    return h;
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    ASTRO_ASSERT(sp_top >= c->env);

    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = gc_bump(c, kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_PAYLOAD(payload, aligned);

    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(sp_top >= c->env);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = gc_bump(c, KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_BYTE_PAYLOAD(payload, aligned);

    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size, VALUE *sp_top)
{
    if (old == NULL) {
        return aro_gc_alloc(c, KIND_PAYLOAD_VAL, new_size, sp_top);
    }
    GCHeader *oldh = (GCHeader *)old - 1;
    size_t old_size = ASTRO_GC_HEADER_SIZE(oldh);
    AroGcKind kind = HDR_KIND(oldh);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    /* Park `old` in a scanned sp slot so GC forwards it (and its interior
     * VALUE slots) to to-space.  Post-GC `sp_top[0]` points at the
     * forwarded copy; reading it stays safe in stress mode because the
     * forwarded copy lives in the new (readable) to-space.  Previously
     * stress mode used a malloc'd shadow buffer, but that hid interior
     * heap pointers from the scan-loop — memcpy then propagated stale
     * from-space refs into the new payload, crashing at later print. */
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size, sp_top + 1)
        : aro_gc_alloc(c, kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ----------------------------------------------------------------------------
// Cheney-style copy collector
// ----------------------------------------------------------------------------

/* Forward an old payload pointer: copy to to-space if not already done,
 * return new payload address. */
/* Internal helpers below access the AstroGc via the single global —
 * SCAN_EDGES callback signature doesn't carry CTX so we tap the static
 * directly.  For multi-instance future, the callback signature would
 * need to thread CTX (or the gc pointer) through. */

static void *
forward_payload(void *old_payload)
{
    AstroGc *gc = &g_astro_gc;
    if (!old_payload) return NULL;
    if (ASTRO_DEBUG && ((char *)old_payload < gc->from_base_cur ||
                        (char *)old_payload >= gc->from_base_cur + gc->region_bytes)) {
        fprintf(stderr,
            "[gc] FORWARD STALE PTR: %p (from-space [%p..%p), to-space [%p..%p))\n",
            old_payload, (void*)gc->from_base_cur,
            (void*)(gc->from_base_cur + gc->region_bytes),
            (void*)gc->to_base, (void*)(gc->to_base + gc->region_bytes));
        ASTRO_ASSERT(0 && "forward_payload: old_payload outside from-space");
    }
    GCHeader *oldh = (GCHeader *)old_payload - 1;
    void *existing_fwd = ASTRO_GC_HEADER_GET_FWD(oldh);
    if (existing_fwd) {
        if (ASTRO_DEBUG && ((char *)existing_fwd < gc->to_base ||
                            (char *)existing_fwd >= gc->to_base + gc->region_bytes)) {
            fprintf(stderr,
                "[gc] FORWARD STALE FWD: oldh@%p fwd=%p not in to-space [%p..%p)\n",
                (void*)oldh, existing_fwd, (void*)gc->to_base,
                (void*)(gc->to_base + gc->region_bytes));
            ASTRO_ASSERT(0 && "forward_payload: fwd outside to-space");
        }
        return existing_fwd;
    }

    size_t aligned = ALIGN8(ASTRO_GC_HEADER_SIZE(oldh));
    size_t total = sizeof(GCHeader) + aligned;

    GCHeader *newh = (GCHeader *)gc->to_top;
    memcpy(newh, oldh, total);
    ASTRO_GC_HEADER_SET_FWD(newh, NULL);
    gc->to_top += total;

    void *new_payload = (void *)(newh + 1);
    ASTRO_GC_HEADER_SET_FWD(oldh, new_payload);
    return new_payload;
}

/* edge_visit callback used by both root scan and to-space scan loops.
 * Updates `*slot` in place: if it points to from-space, forward and
 * rewrite. */
static void
forward_edge(void **slot)
{
    void *p = *slot;
    if (!p) return;
    *slot = forward_payload(p);
}

/* edge_visit callback for VALUE slots (= roots, KIND_PAYLOAD_VAL members).
 * The slot may hold a tagged immediate; only IS_PTR values get forwarded. */
static void
forward_value_edge(void **slot)
{
    VALUE v = (VALUE)*slot;
    if (!IS_PTR(v)) return;
    *slot = (void *)(VALUE)forward_payload((void *)v);
}

static void
gc_collect_internal(CTX *c, VALUE *sp_top)
{
    AstroGc *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin();
    char *from_base = gc->active_base;
    char *from_top_pre = gc->active_top;

    /* Determine the to-space. */
    char *next_to_base;
    if (aro_gc_stress) {
        next_to_base = mmap_region(gc->region_bytes);
    } else {
        next_to_base = (gc->active_idx == 0) ? gc->space1 : gc->space0;
    }

    gc->to_base = next_to_base;
    gc->to_top  = next_to_base;
    gc->from_base_cur = from_base;

    aro_gc_stats.heap_bytes = 0;

    /* Zero stale slots above sp_top up to high-water mark. */
    if (gc->sp_high_water == NULL || sp_top > gc->sp_high_water) {
        gc->sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < gc->sp_high_water; p++) *p = 0;
    }

    if (ASTRO_DEBUG && aro_gc_stress) {
        for (VALUE *p = c->env; p < sp_top; p++) {
            VALUE v = *p;
            if (!IS_PTR(v)) continue;
            char *vp = (char *)v;
            if (vp < from_base || vp >= from_base + gc->region_bytes) {
                fprintf(stderr,
                    "[gc] PRE-MARK ASSERT FAILED: slot c->env[%ld]=%lx "
                    "is not in from-space [%p..%p)\n",
                    p - c->env, (long)v, (void*)from_base,
                    (void*)(from_base + gc->region_bytes));
                ASTRO_ASSERT(0 && "pre-mark: stale heap pointer in sp range");
            }
        }
    }

    /* (1) Root scan: forward VALUE pointers in the sp[] range in place.
     * Cheney has no separate mark phase — record the whole loop in
     * reclaim_seconds. */
    struct timespec tcheney = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) {
        forward_value_edge((void **)p);
    }

    /* (2) Scan-loop in to-space: each freshly-copied object's outgoing
     * refs are forwarded.  SCAN_EDGES dispatches on kind; the edge_visit
     * callback decides whether to dereference as VALUE or raw pointer. */
    char *scan = gc->to_base;
    while (scan < gc->to_top) {
        GCHeader *h = (GCHeader *)scan;
        /* For OBJ_ARRAY/STRING the edges are raw pointers; for
         * PAYLOAD_VAL they are VALUEs.  Pick the right visitor per
         * kind. */
        switch (HDR_KIND(h)) {
          case KIND_PAYLOAD_VAL: {
              VALUE *slots = (VALUE *)(h + 1);
              size_t n = ASTRO_GC_HEADER_SIZE(h) / sizeof(VALUE);
              for (size_t i = 0; i < n; i++)
                  forward_value_edge((void **)&slots[i]);
              break;
          }
          default:
              ASTRO_GC_SCAN_EDGES(h, forward_edge);
              break;
        }
        aro_gc_stats.heap_bytes += ASTRO_GC_HEADER_SIZE(h);
        scan += sizeof(GCHeader) + ALIGN8(ASTRO_GC_HEADER_SIZE(h));
    }
    aro_gc_phase_end(tcheney, &aro_gc_stats.reclaim_seconds);

    /* (3) Swap active. */
    if (!aro_gc_stress) {
        gc->active_idx = 1 - gc->active_idx;
    }
    gc->active_base = next_to_base;
    gc->active_top  = gc->to_top;
    gc->active_end  = next_to_base + gc->region_bytes;

    /* (4) Retire the old active.  Stress mode unmaps it outright — we
     * never reuse it (each GC mmaps a fresh to-space), and leaving it
     * PROT_NONE'd would accumulate `region_bytes × gc_count` of address
     * space for the life of the process. */
    if (aro_gc_stress) {
        if (munmap(from_base, gc->region_bytes) != 0) {
            perror("gc_collect: munmap retired"); abort();
        }
    }
    (void)from_top_pre;

    gc->bytes_since_gc = 0;
    if (!aro_gc_stress) {
        size_t live = aro_gc_stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc->gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }

    aro_gc_stats.gc_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(CTX *c, VALUE *sp_top)
{
    gc_collect_internal(c, sp_top);
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return aro_gc_stats.heap_bytes;  }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_total_seconds(void)      { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void)  { return aro_gc_stats.max_pause_seconds; }
double aro_gc_mark_seconds(void)       { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void)    { return aro_gc_stats.reclaim_seconds; }
