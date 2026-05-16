// gc_bump.c — backend #10: bump-only allocator, no collection.
//
// Like `none` but uses a single mmap'd region with a bump pointer instead
// of libc malloc.  Strictly cheaper allocation (= bump + memset) and no
// GC overhead — shows the absolute floor for "rooting + WB + dispatch"
// cost in baruby_precise, isolating those from any heap-management cost.
//
// Trade-off vs `none`: no fragmentation, contiguous memory; but a single
// 4 GiB region (lazy paged) is reserved up front.  Aborts on OOM since
// there is no collector.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

typedef struct GCHeader {
    uint32_t kind;
    uint32_t size;
} GCHeader;

#define REGION_BYTES ((size_t)4u << 30)   // 4 GiB virtual; lazy-paged
#define ALIGN8(n)    (((n) + 7u) & ~(size_t)7u)

static char *region_base = NULL;
static char *region_top  = NULL;
static char *region_end  = NULL;

BarubyGCStats baruby_gc_stats = {0, 0, 0, 0, 0, 0.0};
int baruby_gc_stress = 0;
const char *baruby_gc_backend_name = "bump";

void
baruby_gc_init(CTX *c)
{
    (void)c;
    region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (region_base == MAP_FAILED) { perror("mmap"); abort(); }
    region_top = region_base;
    region_end = region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        fprintf(stderr, "[baruby_gc=bump] STRESS mode requested but ignored — "
                        "no collector to stress\n");
    }
}

static GCHeader *
bump(BarubyGCKind kind, size_t payload_size, size_t aligned)
{
    size_t total = sizeof(GCHeader) + aligned;
    if (region_top + total > region_end) {
        fprintf(stderr, "baruby_gc=bump: OOM (need %zu, virtual %p..%p)\n",
                total, (void *)region_base, (void *)region_end);
        abort();
    }
    GCHeader *h = (GCHeader *)region_top;
    h->kind = (uint32_t)kind;
    h->size = (uint32_t)payload_size;
    region_top += total;
    return h;
}

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    (void)sp_top;
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(kind, payload_size, aligned);
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
    (void)sp_top;
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(KIND_PAYLOAD_BYTE, payload_size, aligned);
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
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? baruby_gc_alloc_byte(new_size, sp_top)
        : baruby_gc_alloc(kind, new_size, sp_top);
    if (copy_bytes) memcpy(newp, old, copy_bytes);
    return newp;
}

void
baruby_gc_collect(VALUE *sp_top)
{
    (void)sp_top;
    // no-op
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return (size_t)(region_top - region_base); }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }
size_t baruby_gc_minor_count(void) { return baruby_gc_stats.minor_count; }
size_t baruby_gc_major_count(void) { return baruby_gc_stats.major_count; }
double baruby_gc_total_seconds(void) { return baruby_gc_stats.total_seconds; }
