// gc_bump.c — backend #10: bump-only allocator, no collection.
//
// Like `none` but uses a single mmap'd region with a bump pointer instead
// of libc malloc.  Strictly cheaper allocation (= bump + memset) and no
// GC overhead — shows the absolute floor for "rooting + WB + dispatch"
// cost in baruby_precise, isolating those from any heap-management cost.
//
// Trade-off vs `none`: no fragmentation, contiguous memory; but a single
// 64 GiB region (lazy paged) is reserved up front.  Aborts on OOM since
// there is no collector.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: framework GCHeader 廃止。 ASTroObjectHeader (= sample
 * struct head field) が payload offset 0 にあり、 gc_size を保持。 */

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

/* Bump payload at region_top.  Writes head at offset 0 of payload. */
static inline void *
bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (gc->region_top + aligned > gc->region_end) {
        fprintf(stderr, "baruby_gc=bump: OOM (need %zu, virtual %p..%p)\n",
                aligned, (void *)gc->region_base, (void *)gc->region_end);
        abort();
    }
    void *payload = gc->region_top;
    ASTroObjectHeader *h = (ASTroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    gc->region_top += aligned;
    return payload;
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    void *payload = bump(c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Zero post-head region so GC scans see no stale heap-pointer bits.
     * gc_bump backend has no GC, but the contract is uniform. */
    memset((char *)payload + sizeof(ASTroObjectHeader), 0,
           aligned - sizeof(ASTroObjectHeader));
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    void *payload = bump(c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Byte payloads: skip post-head zero-fill (caller writes immediately). */
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

void
aro_gc_collect(CTX *c)
{
    (void)c;
    // no-op
}

/* gc_bump never reclaims — same contract as gc_none.  Always-alive. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    (void)c;
    return payload;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (gc->region_base && gc->region_base != MAP_FAILED) {
        munmap(gc->region_base, REGION_BYTES);
    }
    free(gc);
    c->astro_gc = NULL;
}


size_t
aro_gc_size_of(void *p)
{
    return ((ASTroObjectHeader *)p)->gc_size;
}
