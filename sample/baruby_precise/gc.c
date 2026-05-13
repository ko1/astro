#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "gc.h"

// ----------------------------------------------------------------------------
// Semispace (Cheney) moving GC with stress mode.  See gc.h for design.
// ----------------------------------------------------------------------------

BarubyGCStats baruby_gc_stats = {0, 0, 0};
int baruby_gc_stress = 0;

#define REGION_BYTES  ((size_t)64u << 20)   // 64 MiB per semispace
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

static CTX *gc_ctx = NULL;

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

static char *
mmap_region(void)
{
    char *p = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
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

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    size_t aligned = ALIGN8(payload_size);
    size_t total   = sizeof(GCHeader) + aligned;

    if (baruby_gc_stress || (active_top + total) > active_end) {
        gc_collect_internal(sp_top);
        if (active_top + total > active_end) {
            fprintf(stderr, "baruby_gc: OOM (need %zu, have %zu)\n",
                    total, (size_t)(active_end - active_top));
            abort();
        }
    }

    GCHeader *h = (GCHeader *)active_top;
    h->kind = (uint32_t)kind;
    h->size = (uint32_t)payload_size;
    h->fwd  = NULL;
    active_top += total;

    void *payload = (void *)(h + 1);
    memset(payload, 0, aligned);

    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (old == NULL) {
        return baruby_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    }
    // Read old header BEFORE alloc (alloc may move/mprotect us).
    GCHeader *oldh = (GCHeader *)old - 1;
    size_t old_size = oldh->size;
    BarubyGCKind kind = (BarubyGCKind)oldh->kind;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    // Buffer old's content in plain heap BEFORE the alloc may invalidate it.
    // Cost is one malloc/free per realloc; acceptable for the testbed.
    void *buf = malloc(copy_bytes);
    if (!buf) { fprintf(stderr, "realloc buf OOM\n"); abort(); }
    memcpy(buf, old, copy_bytes);

    // After this alloc, `old`'s page may be mprotect'd PROT_NONE (stress).
    void *newp = baruby_gc_alloc(kind, new_size, sp_top);
    memcpy(newp, buf, copy_bytes);
    free(buf);
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
    // Validate: old_payload must be in current from-space.  Stale pointers
    // from un-cleared sp[] slots above the current frame's logical top
    // might point at unrelated memory (uninitialized region of inactive
    // space, etc.).  Treat such pointers as dead — return NULL so the
    // root slot reads as 0 (= VAL_FALSE) afterward.
    if ((char *)old_payload < from_base_cur ||
        (char *)old_payload >= from_base_cur + REGION_BYTES) {
        return NULL;
    }
    GCHeader *oldh = (GCHeader *)old_payload - 1;
    if (oldh->fwd) {
        // Validate fwd: must point into current to-space.
        if ((char *)oldh->fwd < to_base ||
            (char *)oldh->fwd >= to_base + REGION_BYTES) {
            return NULL;
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
    switch ((BarubyGCKind)h->kind) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
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
    }
}

static void
gc_collect_internal(VALUE *sp_top)
{
    char *from_base = active_base;

    // Determine the to-space.
    char *next_to_base;
    if (baruby_gc_stress) {
        next_to_base = mmap_region();
    } else {
        next_to_base = (active_idx == 0) ? space1 : space0;
    }

    to_base = next_to_base;
    to_top  = to_base;
    from_base_cur = from_base;

    // Reset live-bytes counter; we'll add as we copy.
    baruby_gc_stats.heap_bytes = 0;

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

    // (Pre-mark assertion) Every IS_PTR slot in scan range must point into
    // the current from-space.  This MUST run BEFORE the root scan loop
    // (which mutates *p in-place).
    if (baruby_gc_stress) {
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
                abort();
            }
        }
    }

    // (1) Scan VALUE stack and forward root pointers in place.
    for (VALUE *p = c->env; p < sp_top; p++) {
        *p = forward_value(*p);
    }

    char *scan = to_base;
    while (scan < to_top) {
        GCHeader *h = (GCHeader *)scan;
        process_object(h);
        baruby_gc_stats.heap_bytes += h->size;
        scan += sizeof(GCHeader) + ALIGN8(h->size);
    }

    // (3) Swap active.
    if (!baruby_gc_stress) {
        active_idx = 1 - active_idx;
    }
    active_base = next_to_base;
    active_top  = to_top;
    active_end  = next_to_base + REGION_BYTES;

    // (4) Retire the old active.
    if (baruby_gc_stress) {
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

    baruby_gc_stats.gc_count++;
    gc_ctx->sp = sp_top;
}

void
baruby_gc_collect(VALUE *sp_top)
{
    gc_collect_internal(sp_top);
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return baruby_gc_stats.heap_bytes;  }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }
