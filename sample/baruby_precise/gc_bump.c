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

#define REGION_BYTES ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)    (((n) + 7u) & ~(size_t)7u)

/* iter 62: process-scope state を struct AstroGc に集約 */
typedef struct AstroGc {
    char *region_base;
    char *region_top;
    char *region_end;
} AstroGc;

static AstroGc g_astro_gc;
#define ASTRO_GC_INSTANCE() (&g_astro_gc)

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "bump";

void
aro_gc_init(CTX *c)
{
    (void)c;
    AstroGc *gc = ASTRO_GC_INSTANCE();
    gc->region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (gc->region_base == MAP_FAILED) { perror("mmap"); abort(); }
    gc->region_top = gc->region_base;
    gc->region_end = gc->region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        fprintf(stderr, "[baruby_gc=bump] STRESS mode requested but ignored — "
                        "no collector to stress\n");
    }
}

static GCHeader *
bump(AroGcKind kind, size_t payload_size, size_t aligned)
{
    AstroGc *gc = ASTRO_GC_INSTANCE();
    size_t total = sizeof(GCHeader) + aligned;
    if (gc->region_top + total > gc->region_end) {
        fprintf(stderr, "baruby_gc=bump: OOM (need %zu, virtual %p..%p)\n",
                total, (void *)gc->region_base, (void *)gc->region_end);
        abort();
    }
    GCHeader *h = (GCHeader *)gc->region_top;
    h->kind = (uint32_t)kind;
    h->size = (uint32_t)payload_size;
    gc->region_top += total;
    return h;
}

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    (void)sp_top;
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(kind, payload_size, aligned);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_PAYLOAD(payload, aligned);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    (void)sp_top;
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(KIND_PAYLOAD_BYTE, payload_size, aligned);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_BYTE_PAYLOAD(payload, aligned);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = (AroGcKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top)
        : aro_gc_alloc(kind, new_size, sp_top);
    if (copy_bytes) memcpy(newp, old, copy_bytes);
    return newp;
}

void
aro_gc_collect(VALUE *sp_top)
{
    (void)sp_top;
    // no-op
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return (size_t)(g_astro_gc.region_top - g_astro_gc.region_base); }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
