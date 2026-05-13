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

static char *space0 = NULL;
static char *space1 = NULL;
static int   active = 0;
static char *active_top = NULL;
static char *active_end = NULL;

static CTX *gc_ctx = NULL;

static char *
active_space(void)   { return active == 0 ? space0 : space1; }
static char *
inactive_space(void) { return active == 0 ? space1 : space0; }

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    space0 = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    space1 = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (space0 == MAP_FAILED || space1 == MAP_FAILED) {
        perror("baruby_gc_init: mmap"); abort();
    }
    active = 0;
    active_top = space0;
    active_end = space0 + REGION_BYTES;

    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
        // Inactive starts PROT_NONE (will be flipped to RW at collection start).
        if (mprotect(space1, REGION_BYTES, PROT_NONE) != 0) {
            perror("baruby_gc_init: initial mprotect"); abort();
        }
        fprintf(stderr, "[baruby_gc] STRESS mode: collect on every alloc, "
                        "from-space PROT_NONE after move\n");
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
    char *from_base = active_space();
    char *from_end  = active_end;

    // Make the inactive (= to-space) writable.  In stress mode it was PROT_NONE.
    char *next_to_base = inactive_space();
    if (baruby_gc_stress) {
        if (mprotect(next_to_base, REGION_BYTES, PROT_READ|PROT_WRITE) != 0) {
            perror("gc_collect: mprotect RW"); abort();
        }
    }

    to_base = next_to_base;
    to_top  = to_base;
    from_base_cur = from_base;

    // Reset live-bytes counter; we'll add as we copy.
    baruby_gc_stats.heap_bytes = 0;

    // (1) Scan VALUE stack and forward root pointers in place.
    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) {
        *p = forward_value(*p);
    }

    // (2) Cheney scan: walk objects already copied to to-space, forwarding
    //     their outgoing refs.  Each newly-copied object extends to_top,
    //     so the loop terminates when scan catches up.
    // Zero stale slots above current top up to high-water mark.
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }
    char *scan = to_base;
    while (scan < to_top) {
        GCHeader *h = (GCHeader *)scan;
        process_object(h);
        baruby_gc_stats.heap_bytes += h->size;
        scan += sizeof(GCHeader) + ALIGN8(h->size);
    }

    // (3) Swap active.
    active = 1 - active;
    active_top = to_top;
    active_end = next_to_base + REGION_BYTES;

    // (4) Stress: lock out the old from-space so stale pointer access SIGSEGVs.
    if (baruby_gc_stress) {
        if (mprotect(from_base, REGION_BYTES, PROT_NONE) != 0) {
            perror("gc_collect: mprotect NONE"); abort();
        }
    }
    (void)from_end;

    baruby_gc_stats.gc_count++;
    // Also keep c->sp current.
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
