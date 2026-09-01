// gc_common.c — shared GC framework helpers used by every backend.
//
// iter 76: framework は CTX-opaque 化。 旧版にあった
// `aro_gc_realloc_payload` / `aro_gc_realloc_byte_payload` は sample stack
// convention (= sample 内部 slot に park) に依存するため sample 側に移動
// (baruby_precise: node.c, ascheme_precise: main.c)。
//
// 現在ここに残るのは `aro_gc_realloc_in_place` の weak default のみ。
// 個別 backend (gc_copy / gc_mark_compact / ...) が override する。
//
// header 再初期化 (= park-then-alloc 後に new payload の framework-owned
// 部分を fresh state に戻す) は sample が `aro_gc_reset_payload_header`
// helper を呼ぶ。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"  /* CTX_struct + sample-provided AROH_VISIT_ROOTS contract macro (= 必須) */
#include "gc.h"

/* AROH contract: a sample's context.h may define AROH_GC_SAFEPOINT() to be
 * notified at every alloc (= every point a moving GC could fire) — e.g. to
 * advance a logical clock.  Samples that don't opt in get this no-op default. */
#ifndef AROH_GC_SAFEPOINT
#define AROH_GC_SAFEPOINT() ((void)0)
#endif

/* Public alloc API — thin wrapper around the backend's raw alloc.
 *
 * Sample stores the returned VALUE directly into a GC-visible slot
 * (= sp[], object field).  The return is the raw heap payload pointer
 * cast to VALUE; sample uses ARO_LOAD / direct cast to access.
 *
 * `aro_gc_alloc` corresponds to the SCAN category (= scan-safe init,
 * GC's heap walk dispatches via sample's SCAN_EDGES on the payload);
 * `aro_gc_alloc_byte` corresponds to the BYTE category (= no init, no
 * scan).  Both are inline-shallow so LTO folds them into the caller. */
VALUE
aro_gc_alloc(CTX *c, size_t payload_size)
{
    (void)c;
    AROH_GC_SAFEPOINT();   /* RESULT-audit: every alloc is a potential-GC point (no-op in release) */
    return (VALUE)(uintptr_t)aro_gc_alloc_raw(c, payload_size);
}

VALUE
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    (void)c;
    AROH_GC_SAFEPOINT();
    return (VALUE)(uintptr_t)aro_gc_alloc_byte_raw(c, payload_size);
}

/* Default in-place realloc hook — returns NULL so the caller falls
 * through to the alloc + memcpy path.  Backends that track large objs
 * on a malloc-backed list (gc_copy / gc_mark_compact) override this.
 *
 * `c` is opaque here: framework treats CTX as a void *-equivalent.
 * Backends that override read backend state via ARO_GC_INSTANCE. */
__attribute__((weak))
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    (void)c; (void)old; (void)new_size;
    return NULL;
}

/* Default heap walk — "this backend cannot walk".  Backends whose heap is a
 * gapless bump region (gc_copy) override it. */
__attribute__((weak))
bool
aro_gc_each_object(CTX *c, void (*visit)(void *arg, void *payload), void *arg)
{
    (void)c; (void)visit; (void)arg;
    return false;
}

/* Restore framework-owned head fields after a `aro_gc_alloc` + memcpy
 * realloc.  The memcpy from the OLD payload overwrites the freshly-init'd
 * head of NEW (head is at payload offset 0).  Sample calls this from its
 * realloc helper to set:
 *   - gc_size to new_size (= the alloc size)
 *   - gc_flags to 0       (= no inherited mark/old/dirty/free)
 *   - gc_fwd  to NULL     (= moving GCs: fresh state)
 * Sample's `flags` field is intentionally preserved (= same logical
 * object, just bigger). */
void
aro_gc_reset_payload_header(void *payload, size_t new_size)
{
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->gc_size  = (uint32_t)new_size;
    h->gc_flags = 0;
#ifdef ARO_GC_HAS_FWD
    h->gc_fwd = NULL;
#endif
}

/* ---------------------------------------------------------------------------
 * Finalizer machinery (= weak ref + post-mark sweep pass).  See gc.h doc.
 * --------------------------------------------------------------------------- */

void
aro_gc_finalize_register(CTX *c, void *payload)
{
    AroGcCommonState *const cs = ARO_GC_COMMON(c);
    if (cs->finalize_count == cs->finalize_cap) {
        size_t newcap = cs->finalize_cap ? cs->finalize_cap * 2 : 16;
        void **newlist = (void **)realloc(cs->finalize_list,
                                          newcap * sizeof(void *));
        if (!newlist) {
            fprintf(stderr, "aro_gc_finalize_register: realloc failed\n");
            abort();
        }
        cs->finalize_list = newlist;
        cs->finalize_cap  = newcap;
    }
    cs->finalize_list[cs->finalize_count++] = payload;
}

void
aro_gc_finalize_walk(CTX *c)
{
    AroGcCommonState *const cs = ARO_GC_COMMON(c);
    size_t write = 0;
    for (size_t i = 0; i < cs->finalize_count; i++) {
        void *const p = cs->finalize_list[i];
        void *const new_p = aro_gc_finalize_check(c, p);
        if (new_p) {
            cs->finalize_list[write++] = new_p;
        } else {
            AROH_FINALIZE(p);
        }
    }
    cs->finalize_count = write;
}

void
aro_gc_finalize_fini(CTX *c)
{
    AroGcCommonState *const cs = ARO_GC_COMMON(c);
    free(cs->finalize_list);
    cs->finalize_list  = NULL;
    cs->finalize_count = 0;
    cs->finalize_cap   = 0;
}

/* aro_gc_account_external — bookkeep external pressure only.  We do NOT
 * trigger GC here: this function is typically called from inside foreign
 * library allocators (= GMP's gmp_alloc) where the host is mid-operation
 * and holds raw refs to the live set; moving GC at this point invalidates
 * those refs and the host crashes.  Instead, each backend's `aro_gc_alloc`
 * threshold check folds `external_bytes` into the comparison so GC fires
 * at the next sample-side allocation (= a safe point). */
void
aro_gc_account_external(CTX *c, ssize_t delta)
{
    AroGcCommonState *const cs = ARO_GC_COMMON(c);
    if (delta > 0) {
        cs->external_bytes += (size_t)delta;
    } else {
        size_t down = (size_t)(-delta);
        cs->external_bytes = (down < cs->external_bytes)
                             ? cs->external_bytes - down : 0;
    }
}
