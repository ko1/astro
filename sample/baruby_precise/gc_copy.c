#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

// ----------------------------------------------------------------------------
// Semispace (Cheney) moving GC with stress mode.  See gc.h for design.
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
#define ALIGN8(n)     (((n) + 7u) & ~(size_t)7u)

// Active semispace (where allocations go).  In stress mode, EVERY GC
// allocates a fresh to-space via mmap and PERMANENTLY retires the old
// active (mprotect PROT_NONE + madvise DONTNEED — physical pages freed,
// virtual address stays reserved forever so stale pointers segfault
// no matter how many GCs ago they were valid).
//
// In non-stress mode, we use a fixed pair of regions and alternate
// (classic semispace).
static char *active_base = NULL;
static char *active_top  = NULL;
static char *active_end  = NULL;

static char *space0 = NULL;
static char *space1 = NULL;   // non-stress: the alternate region
static int   active_idx = 0;  // non-stress only

/* Adaptive GC trigger.  Previously copy GC'd only when active region was
 * full — with 64 GiB virtual reservation (iter 27) that effectively
 * means "never", so copy degenerated to bump-alloc.  Match the mark /
 * immix policy: trigger at `bytes_since_gc > gc_threshold`,
 * gc_threshold = max(4 MiB, 2 × live_post_cheney). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2
static size_t bytes_since_gc = 0;
static size_t gc_threshold = GC_THRESHOLD_MIN;

static CTX *gc_ctx = NULL;

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

static char *
mmap_region(void)
{
    char *p = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        // Stress: start with one fresh region.  Each GC allocates a new
        // to-space and permanently retires the old active.
        active_base = mmap_region();
        active_top  = active_base;
        active_end  = active_base + REGION_BYTES;
        fprintf(stderr, "[baruby_gc] STRESS mode: collect on every alloc, "
                        "every old space PROT_NONE forever (madvise DONTNEED)\n");
    } else {
        // Non-stress: classic 2-region semispace, alternating.
        space0 = mmap_region();
        space1 = mmap_region();
        active_idx  = 0;
        active_base = space0;
        active_top  = space0;
        active_end  = space0 + REGION_BYTES;
    }
}

// ----------------------------------------------------------------------------
// Allocation
// ----------------------------------------------------------------------------

static void gc_collect_internal(VALUE *sp_top);

// Internal bump-allocator: reserves header + payload of `aligned` bytes.
// Triggers GC + retries on OOM.  Does NOT zero the payload — caller decides.
static inline GCHeader *
gc_bump(AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    size_t total = sizeof(GCHeader) + aligned;
    if (aro_gc_stress
        || bytes_since_gc + payload_size > gc_threshold
        || (active_top + total) > active_end) {
        gc_collect_internal(sp_top);
        if (active_top + total > active_end) {
            fprintf(stderr, "baruby_gc: OOM (need %zu, have %zu)\n",
                    total, (size_t)(active_end - active_top));
            abort();
        }
    }
    GCHeader *h = (GCHeader *)active_top;
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->fwd  = NULL;
    active_top += total;
    bytes_since_gc += payload_size;
    return h;
}

// aro_gc_alloc: zero-initialized payload.  Use for KIND_OBJ_ARRAY /
// KIND_OBJ_STRING (embedded items / bytes ptr starts NULL) and
// KIND_PAYLOAD_VAL (trailing slots are VAL_FALSE for GC-safe scanning).
void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    ASTRO_ASSERT(sp_top >= gc_ctx->env);

    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = gc_bump(kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);

    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

// aro_gc_alloc_byte: raw byte payload.  GC never reads it as pointers
// so we skip the memset.  Caller MUST fill bytes[0..size-1] before any
// other alloc / GC opportunity.
void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(sp_top >= gc_ctx->env);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = gc_bump(KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);

    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (old == NULL) {
        return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    }
    // Read old header BEFORE alloc (alloc may move/mprotect us).
    GCHeader *oldh = (GCHeader *)old - 1;
    size_t old_size = oldh->size;
    AroGcKind kind = HDR_KIND(oldh);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    if (aro_gc_stress) {
        // Stress mode retires from-space with PROT_NONE + MADV_DONTNEED, so
        // we can't read `old` again after the alloc.  Fall back to the
        // malloc-buffered slow path (one extra malloc/memcpy/free per
        // realloc; only relevant when BARUBY_GC_STRESS=1).
        void *buf = malloc(copy_bytes);
        if (!buf) { fprintf(stderr, "realloc buf OOM\n"); abort(); }
        memcpy(buf, old, copy_bytes);
        void *newp = (kind == KIND_PAYLOAD_BYTE)
            ? aro_gc_alloc_byte(new_size, sp_top)
            : aro_gc_alloc(kind, new_size, sp_top);
        memcpy(newp, buf, copy_bytes);
        free(buf);
        return newp;
    }

    // Fast path (iter 36): root `old` via sp_top[0] so Cheney updates it
    // through any GC fired during alloc, then alloc, then memcpy from the
    // *post-GC* location.  No temp malloc.  Pattern matches other
    // backends' realloc_payload.  Safe because non-stress copy leaves
    // from-space pages readable (just not allocatable) until next GC.
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ----------------------------------------------------------------------------
// Cheney-style copy collector
// ----------------------------------------------------------------------------

static char *to_top;
static char *to_base;
static char *from_base_cur;   // for stale-ptr detection during scan

// Highest sp_top ever passed to an alloc.  We zero slots in
// [sp_top .. high_water] before each scan so stale heap pointers from
// returned NODE_DEFs above the current call's top don't confuse the GC.
static VALUE *sp_high_water = NULL;

// Forward an old payload pointer: copy to to-space if not already done,
// return new payload address.
static void *
forward_payload(void *old_payload)
{
    if (!old_payload) return NULL;
    // Range invariants — debug-only.  Under ASTRO_DEBUG these print
    // diagnostics and trip an ASTRO_ASSERT; in release builds the
    // entire `if (ASTRO_DEBUG ...)` block constant-folds away.
    if (ASTRO_DEBUG && ((char *)old_payload < from_base_cur ||
                        (char *)old_payload >= from_base_cur + REGION_BYTES)) {
        fprintf(stderr,
            "[gc] FORWARD STALE PTR: %p (from-space [%p..%p), to-space [%p..%p))\n",
            old_payload, (void*)from_base_cur,
            (void*)(from_base_cur + REGION_BYTES),
            (void*)to_base, (void*)(to_base + REGION_BYTES));
        ASTRO_ASSERT(0 && "forward_payload: old_payload outside from-space");
    }
    GCHeader *oldh = (GCHeader *)old_payload - 1;
    if (oldh->fwd) {
        if (ASTRO_DEBUG && ((char *)oldh->fwd < to_base ||
                            (char *)oldh->fwd >= to_base + REGION_BYTES)) {
            fprintf(stderr,
                "[gc] FORWARD STALE FWD: oldh@%p fwd=%p not in to-space [%p..%p)\n",
                (void*)oldh, oldh->fwd, (void*)to_base,
                (void*)(to_base + REGION_BYTES));
            ASTRO_ASSERT(0 && "forward_payload: fwd outside to-space");
        }
        return oldh->fwd;
    }

    size_t aligned = ALIGN8(oldh->size);
    size_t total = sizeof(GCHeader) + aligned;

    GCHeader *newh = (GCHeader *)to_top;
    memcpy(newh, oldh, total);
    newh->fwd = NULL;
    to_top += total;

    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    return new_payload;
}

static VALUE
forward_value(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload((void *)v);
}

// Walk a freshly-copied object's outgoing refs (in to-space) and forward them.
static void
process_object(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        ASTRO_ASSERT(a->hdr.type == OBJ_ARRAY);
        if (a->items) a->items = (VALUE *)forward_payload(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        ASTRO_ASSERT(s->hdr.type == OBJ_STRING);
        if (s->bytes) s->bytes = (char *)forward_payload(s->bytes);
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
        ASTRO_ASSERT(0 && "process_object: unknown GCHeader kind");
    }
}

static void
gc_collect_internal(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    char *from_base = active_base;
    char *from_top_pre = active_top;

    // Determine the to-space.
    char *next_to_base;
    if (aro_gc_stress) {
        next_to_base = mmap_region();
    } else {
        next_to_base = (active_idx == 0) ? space1 : space0;
    }

    to_base = next_to_base;
    to_top  = to_base;
    from_base_cur = from_base;

    // Reset live-bytes counter; we'll add as we copy.
    aro_gc_stats.heap_bytes = 0;

    CTX *c = gc_ctx;

    // Zero stale slots above current top up to high-water mark.  Slots
    // beyond `sp_top` belong to frames that have already returned;
    // their VALUEs are logically dead but might still look like heap
    // pointers from a prior GC.  Zero them so the scan / assertion
    // sees only live state.
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    // Pre-mark invariant: every IS_PTR slot in the scan range must point
    // into the current from-space.  Must run BEFORE the root scan loop
    // (which mutates *p in-place).  Debug + stress-mode only — under
    // !ASTRO_DEBUG the whole block constant-folds away.
    if (ASTRO_DEBUG && aro_gc_stress) {
        for (VALUE *p = c->env; p < sp_top; p++) {
            VALUE v = *p;
            if (!IS_PTR(v)) continue;
            char *vp = (char *)v;
            if (vp < from_base || vp >= from_base + REGION_BYTES) {
                fprintf(stderr,
                    "[gc] PRE-MARK ASSERT FAILED: slot c->env[%ld]=%lx "
                    "is not in from-space [%p..%p)\n",
                    p - c->env, (long)v, (void*)from_base,
                    (void*)(from_base + REGION_BYTES));
                ASTRO_ASSERT(0 && "pre-mark: stale heap pointer in sp range");
            }
        }
    }

    // (1) Scan VALUE stack and forward root pointers in place.
    /* Cheney has no separate mark phase: trace and relocate are interleaved.
     * Record the entire scan loop (including root forwarding) in
     * reclaim_seconds.  mark_seconds stays 0 for copy backends. */
    struct timespec tcheney = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) {
        *p = forward_value(*p);
    }

    char *scan = to_base;
    while (scan < to_top) {
        GCHeader *h = (GCHeader *)scan;
        process_object(h);
        aro_gc_stats.heap_bytes += h->size;
        scan += sizeof(GCHeader) + ALIGN8(h->size);
    }
    aro_gc_phase_end(tcheney, &aro_gc_stats.reclaim_seconds);

    // (3) Swap active.
    if (!aro_gc_stress) {
        active_idx = 1 - active_idx;
    }
    active_base = next_to_base;
    active_top  = to_top;
    active_end  = next_to_base + REGION_BYTES;

    // (4) Retire the old active.
    if (aro_gc_stress) {
        // PERMANENT retire: PROT_NONE + DONTNEED.  Virtual address stays
        // reserved forever, physical pages reclaimed.  Stale pointers
        // from ANY past GC into this region SIGSEGV.
        if (mprotect(from_base, REGION_BYTES, PROT_NONE) != 0) {
            perror("gc_collect: mprotect NONE"); abort();
        }
        if (madvise(from_base, REGION_BYTES, MADV_DONTNEED) != 0) {
            perror("gc_collect: madvise DONTNEED"); abort();
        }
    }
    /* Non-stress: leave from-space pages committed for fast re-use as
     * the next to-space.  Aggressive MADV_DONTNEED here would trigger
     * page faults on every collection cycle (measured -20% to -50% on
     * alloc-heavy benches).  Peak physical = 2 × live; acceptable for
     * a testbed where 64 GiB virtual is the heap cap. */
    (void)from_top_pre;

    /* Adaptive threshold: same policy as mark / immix.  Next GC fires
     * at max(MIN, 2 × live_post_cheney). */
    bytes_since_gc = 0;
    if (!aro_gc_stress) {
        size_t live = aro_gc_stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }

    aro_gc_stats.gc_count++;
    gc_ctx->sp = sp_top;
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
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
