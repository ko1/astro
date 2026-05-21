// gc_common.c — shared GC framework helpers used by every backend.
//
// Currently the only resident is `aro_gc_realloc_payload`, which 14 of
// the 16 backends used to implement with the same 12-line body.  The
// only backend-specific bits are header layout, which we delegate to
// the per-backend accessors `aro_gc_kind_of` / `aro_gc_size_of`
// declared in gc.h.
//
// gc_none and gc_bump-style backends that don't want this default can
// implement their own aro_gc_realloc_payload (the linker picks the
// .o-local definition first).  See gc_none.c.

#include <stdio.h>
#include <string.h>
#include "context.h"
#include "gc.h"

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    VALUE *sp_top = c->sp;
    if (!old) return aro_gc_alloc(c, KIND_PAYLOAD_VAL, new_size);

    AroGcKind kind = aro_gc_kind_of(old);
    size_t old_size = aro_gc_size_of(old);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    /* Park `old` in sp_top[0] so GC scans it; bump c->sp by 1 so the
     * inner alloc sees the parked slot as in-range.  For moving GCs
     * this also lets the collector forward the slot if the source
     * payload migrates during the inner alloc's GC trigger.  After
     * restoring c->sp, read back through sp_top[0] (which now points
     * at the possibly-moved source) for the memcpy. */
    sp_top[0] = (VALUE)old;
    c->sp = sp_top + 1;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size)
        : aro_gc_alloc(c, kind, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}
