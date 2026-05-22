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

/* ASTroGC: process-scope GC instance.  gc_none has no backend-specific
 * state — only the common header (stats / stress / timer) is needed.
 * The struct still gets heap-allocated + bound to c->astro_gc so the
 * uniform `ASTRO_GC_COMMON(c)` accessor works. */
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
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    (void)kind;
    void *p = calloc(1, payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return p;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    void *p = malloc(payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    ASTRO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ASTRO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return p;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    void *p = realloc(old, new_size ? new_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    ASTRO_GC_COMMON(c)->stats.total_bytes += new_size;
    return p;
}

/* gc_none has no GC scan, so byte / scan-safe paths are identical
 * (= libc realloc).  Provided just to satisfy the linker. */
void *
aro_gc_realloc_byte_payload(CTX *c, void *old, size_t new_size)
{
    return aro_gc_realloc_payload(c, old, new_size);
}

void
aro_gc_collect(CTX *c)
{
    (void)c;
    // no-op
}

/* gc_none never reclaims; on fini we deliberately do NOT free the
 * objects we handed out — they're leaked-by-design (= libc-malloc
 * lifetime).  But the ASTroGC instance itself we release. */
void
aro_gc_fini(CTX *c)
{
    if (!c->astro_gc) return;
    free(c->astro_gc);
    c->astro_gc = NULL;
}
