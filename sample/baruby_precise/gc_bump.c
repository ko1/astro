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

/* ASTroGC: process-scope GC instance (heap alloc'd in aro_gc_init).
 * `common` MUST be first field — contract for ASTRO_GC_COMMON(c) cast. */
typedef struct ASTroGC {
    AroGcCommonState common;
    char *region_base;
    char *region_top;
    char *region_end;
} ASTroGC;

const char *aro_gc_backend_name = "bump";

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
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
bump(CTX *c, AroGcKind kind, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
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
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    (void)sp_top;
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(c, kind, payload_size, aligned);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_PAYLOAD(payload, aligned);
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    (void)sp_top;
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(c, KIND_PAYLOAD_BYTE, payload_size, aligned);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    ASTRO_GC_INIT_BYTE_PAYLOAD(payload, aligned);
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    VALUE *sp_top = c->sp;
    if (!old) return aro_gc_alloc(c, KIND_PAYLOAD_VAL, new_size);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = (AroGcKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size)
        : aro_gc_alloc(c, kind, new_size);
    if (copy_bytes) memcpy(newp, old, copy_bytes);
    return newp;
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    (void)c; (void)sp_top;
    // no-op
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    if (gc->region_base && gc->region_base != MAP_FAILED) {
        munmap(gc->region_base, REGION_BYTES);
    }
    free(gc);
    c->astro_gc = NULL;
}

