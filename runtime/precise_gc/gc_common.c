// gc_common.c — shared GC framework helpers used by every backend.
//
// Currently the only resident is `aro_gc_realloc_payload`, which 14 of
// the 16 backends used to implement with the same 12-line body.  The
// only backend-specific bits are header layout, which we delegate to
// the per-backend accessor `aro_gc_size_of`
// declared in gc.h.
//
// gc_none and gc_bump-style backends that don't want this default can
// implement their own aro_gc_realloc_payload (the linker picks the
// .o-local definition first).  See gc_none.c.

#include <stdio.h>
#include <string.h>
#include "context.h"
#include "gc.h"

/* Default in-place realloc hook — returns NULL so the caller falls
 * through to the alloc + memcpy path.  Backends that track large objs
 * on a malloc-backed list (gc_copy / gc_mark_compact) override this. */
__attribute__((weak))
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    (void)c; (void)old; (void)new_size;
    return NULL;
}

/* aro_gc_realloc_payload — grow a scan-safe (VALUE-bearing) payload.
 * Caller is responsible for choosing between this and
 * aro_gc_realloc_byte_payload based on whether the payload contains
 * heap-pointer slots (= scan-safe) or raw bytes (= byte). framework
 * never inspects the payload's stored kind to decide. */
void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size)
{
    VALUE *sp_top = c->sp;
    if (!old) return aro_gc_alloc(c, new_size);

    /* Try backend in-place growth first.  When this succeeds (large obj
     * realloc via mremap), we skip the memcpy entirely and the underlying
     * buffer may stay at the same virtual address even at large sizes. */
    void *in_place = aro_gc_realloc_in_place(c, old, new_size);
    if (in_place) return in_place;

    size_t old_size = aro_gc_size_of(old);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    sp_top[0] = (VALUE)old;
    c->sp = sp_top + 1;
    void *newp = aro_gc_alloc(c, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

/* aro_gc_realloc_byte_payload — grow a byte (no-scan) payload.  Caller
 * fills the new bytes; framework does not zero-init the growth region. */
void *
aro_gc_realloc_byte_payload(CTX *c, void *old, size_t new_size)
{
    VALUE *sp_top = c->sp;
    if (!old) return aro_gc_alloc_byte(c, new_size);

    void *in_place = aro_gc_realloc_in_place(c, old, new_size);
    if (in_place) return in_place;

    size_t old_size = aro_gc_size_of(old);
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    sp_top[0] = (VALUE)old;
    c->sp = sp_top + 1;
    void *newp = aro_gc_alloc_byte(c, new_size);
    c->sp = sp_top;
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}
