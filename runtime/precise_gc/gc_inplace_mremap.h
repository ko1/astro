/* gc_inplace_mremap.h — template body for backends with mmap-backed
 * LargeObj (gc_mark / gc_mark_freelist / gc_immix and their gen variants).
 *
 * Each LargeObj has layout: { LargeObj *next; size_t map_bytes; [extras]; payload }
 * where the payload starts with `ASTroObjectHeader` at offset 0.  The
 * "extras" (e.g., mark/old/dirty bools in mark_bitmap_gen) sit between
 * map_bytes and the payload start, so sizeof(LargeObj) accounts for them.
 *
 * Usage from a backend's .c file:
 *
 *   // 1) include this AFTER defining these parameters as macros
 *   #define ARO_GC_INPLACE_THRESHOLD(new_size) \
 *       (size_class_for(ALIGN8(new_size)) >= 0)
 *   #define ARO_GC_INPLACE_PAGE_SIZE       PAGE_SIZE
 *   #define ARO_GC_INPLACE_MREMAP_FLAGS    MREMAP_MAYMOVE
 *   #define ARO_GC_INPLACE_BYTES_ACCT(d)   bytes_since_gc += (d)
 *   #include "gc_inplace_mremap.h"
 *
 * The header expands to a full `aro_gc_realloc_in_place` function body.
 *
 * Invariants (iter 75 Step C):
 *   - LargeObj's first 2 fields are `next` + `map_bytes`
 *   - Payload starts at (char *)lo + sizeof(LargeObj) (= `lo + 1`)
 *   - Payload's first member is `ASTroObjectHeader head` with writable
 *     `gc_size` field of type uint32_t */

void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (gc->common.stress) return NULL;
    if (ARO_GC_INPLACE_THRESHOLD(new_size)) return NULL;

    LargeObj **link = &gc->large_head;
    while (*link) {
        char *lo_payload = (char *)(*link) + sizeof(LargeObj);
        if (lo_payload == (char *)old) break;
        link = &(*link)->next;
    }
    if (!*link) return NULL;
    LargeObj *lo = *link;
    size_t need = sizeof(LargeObj) + ALIGN8(new_size);
    size_t pg = (size_t)(ARO_GC_INPLACE_PAGE_SIZE);
    size_t new_map_bytes = (need + pg - 1) & ~(pg - 1);
    size_t old_map_bytes = lo->map_bytes;
    ASTroObjectHeader *h = (ASTroObjectHeader *)((char *)lo + sizeof(LargeObj));
    size_t old_size = h->gc_size;

    if (new_map_bytes != old_map_bytes) {
        void *res = mremap(lo, old_map_bytes, new_map_bytes,
                           (ARO_GC_INPLACE_MREMAP_FLAGS));
        if (res == MAP_FAILED) return NULL;
        if (res != lo) {
            /* MREMAP_MAYMOVE relocated; patch the linked-list slot. */
            *link = (LargeObj *)res;
            lo = (LargeObj *)res;
            h  = (ASTroObjectHeader *)((char *)lo + sizeof(LargeObj));
        }
        lo->map_bytes = new_map_bytes;
    }
    h->gc_size = (uint32_t)new_size;

    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        gc->common.stats.total_bytes += delta;
        gc->common.stats.heap_bytes  += delta;
        ARO_GC_INPLACE_BYTES_ACCT(delta);
    }
    return (char *)lo + sizeof(LargeObj);
}
