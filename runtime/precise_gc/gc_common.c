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
#include "context.h"  /* CTX_struct + sample-provided ASTRO_GC_VISIT_ROOTS contract macro (= 必須) */
#include "gc.h"

/* Default in-place realloc hook — returns NULL so the caller falls
 * through to the alloc + memcpy path.  Backends that track large objs
 * on a malloc-backed list (gc_copy / gc_mark_compact) override this.
 *
 * `c` is opaque here: framework treats CTX as a void *-equivalent.
 * Backends that override read backend state via ASTRO_GC_INSTANCE. */
__attribute__((weak))
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    (void)c; (void)old; (void)new_size;
    return NULL;
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
    ASTroObjectHeader *h = (ASTroObjectHeader *)payload;
    h->gc_size  = (uint32_t)new_size;
    h->gc_flags = 0;
#ifdef ASTRO_GC_HAS_FWD
    h->gc_fwd = NULL;
#endif
}

/* ---------------------------------------------------------------------------
 * Finalizer machinery (= weak ref + post-mark sweep pass).  See gc.h doc.
 * --------------------------------------------------------------------------- */

void
aro_gc_finalize_register(CTX *c, void *payload)
{
    AroGcCommonState *const cs = ASTRO_GC_COMMON(c);
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
    AroGcCommonState *const cs = ASTRO_GC_COMMON(c);
    size_t write = 0;
    for (size_t i = 0; i < cs->finalize_count; i++) {
        void *const p = cs->finalize_list[i];
        void *const new_p = aro_gc_finalize_check(c, p);
        if (new_p) {
            cs->finalize_list[write++] = new_p;
        } else {
            ASTRO_GC_FINALIZE(p);
        }
    }
    cs->finalize_count = write;
}

void
aro_gc_finalize_fini(CTX *c)
{
    AroGcCommonState *const cs = ASTRO_GC_COMMON(c);
    free(cs->finalize_list);
    cs->finalize_list  = NULL;
    cs->finalize_count = 0;
    cs->finalize_cap   = 0;
}
