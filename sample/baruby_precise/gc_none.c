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

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "none";

void
aro_gc_init(CTX *c)
{
    (void)c;
    if (getenv("BARUBY_GC_STRESS")) {
        fprintf(stderr, "[baruby_gc=none] STRESS mode requested but "
                        "ignored — no collector to stress\n");
    }
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    (void)kind; (void)sp_top;
    void *p = calloc(1, payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return p;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size, VALUE *sp_top)
{
    (void)sp_top;
    void *p = malloc(payload_size ? payload_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return p;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size, VALUE *sp_top)
{
    (void)sp_top;
    void *p = realloc(old, new_size ? new_size : 1);
    if (!p) { fprintf(stderr, "baruby_gc=none: OOM\n"); abort(); }
    aro_gc_stats.total_bytes += new_size;
    return p;
}

void
aro_gc_collect(CTX *c, VALUE *sp_top)
{
    (void)sp_top;
    // no-op
}

