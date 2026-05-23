// gc_none.c — backend #1: no garbage collection.
//
// Every alloc goes through libc malloc.  Memory is never reclaimed; the
// process leaks until exit.  Useful as a baseline that strips out all
// GC bookkeeping cost — anything slower than this is GC overhead.
//
// The realloc path uses libc realloc directly.  No write barrier.  No
// stress mode (there's nothing to stress).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* iter 75 Step C: AroObjectHeader at payload offset 0.  gc_none has
 * no scan / walk so head fields are sample-only (flags) plus gc_size
 * (preserved for symmetry with other backends). */

/* ASTroGC: process-scope GC instance.  gc_none has no backend-specific
 * state — only the common header (stats / stress / timer) is needed. */
typedef struct ASTroGC {
    AroGcCommonState common;
} ASTroGC;

const char *aro_gc_backend_name = "none";

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    if (getenv("BARUBY_GC_STRESS")) {
        fprintf(stderr, "[baruby_gc=none] STRESS mode requested but "
                        "ignored — no collector to stress\n");
    }
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    void *p = calloc(1, payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    ((AroObjectHeader *)p)->gc_size = (uint32_t)payload_size;
    ARO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ARO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return p;
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    void *p = malloc(payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    /* malloc doesn't zero — but we need a valid head.gc_size.  Sample
     * writes head.flags immediately after return. */
    AroObjectHeader *h = (AroObjectHeader *)p;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    ARO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ARO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return p;
}

/* iter 76: sample-side realloc helper uses our in-place hook for the
 * happy path.  gc_none has no compaction / scan, so libc realloc is
 * always sufficient (and cheap).  Returning a non-NULL result here
 * short-circuits the sample's alloc+memcpy fallback. */
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    void *p = realloc(old, new_size ? new_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    ((AroObjectHeader *)p)->gc_size = (uint32_t)new_size;
    ARO_GC_COMMON(c)->stats.total_bytes += new_size;
    return p;
}

/* sample's realloc helper reads payload size via this accessor.  In the
 * gc_none path it's never actually called (in_place wins first), but
 * provided so linking succeeds. */
size_t
aro_gc_size_of(void *payload)
{
    return ((AroObjectHeader *)payload)->gc_size;
}

void
aro_gc_collect(CTX *c)
{
    (void)c;
    // no-op
}

/* gc_none never reclaims, so every registered payload is conservatively
 * "alive forever" — return payload itself.  finalize_walk is never
 * actually called (aro_gc_collect is a no-op), but we provide the symbol
 * so the framework links cleanly. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    (void)c;
    return payload;
}

/* gc_none never reclaims; on fini we deliberately do NOT free the
 * objects we handed out — they're leaked-by-design (= libc-malloc
 * lifetime).  But the ASTroGC instance itself we release. */
void
aro_gc_fini(CTX *c)
{
    if (!c->astro_gc) return;
    aro_gc_finalize_fini(c);
    free(c->astro_gc);
    c->astro_gc = NULL;
}
