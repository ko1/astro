#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"  /* sample が提供する SCAN_EDGES / VISIT_ROOTS macro 取得用 (= framework は CTX 中身 access 禁止、 macro 経由のみ) */
#include "astro_debug.h"
#include "gc.h"

// ----------------------------------------------------------------------------
// Semispace (Cheney) moving GC.
//
// iter 62: framework abstraction.  Process-scope state lives in `struct
// ASTroGC`, heap-allocated in `aro_gc_init` and reachable only via
// `c->astro_gc` (= `ARO_GC_INSTANCE(c)`).  No module-static instance
// pointer exists, so multiple instances can coexist (bind each to its
// own CTX).  Helpers thread `ASTroGC *gc` (or `CTX *c`) explicitly.
// ----------------------------------------------------------------------------

const char *aro_gc_backend_name = "copy";

/* iter 75 Step C+: fwd は payload offset sizeof(AroObjectHeader) に
 * overlay (= 8 B head のみ、 gc_fwd field 不要)。 from-space は次の
 * collect で捨てるので、 sample data の先頭 8 B を fwd で上書きしても
 * 問題ない (前提: sample alloc は最低 16 B = head + 8 B payload)。
 *
 * 大 obj は移動しないので overlay は使えず、 別途 HDR_MARKED bit で
 * 印を付ける (= 旧 design の "fwd = self" を bit 化)。 */
_Static_assert(sizeof(AroObjectHeader) == 8,
               "Cheney: head must be 8 bytes (fwd overlay, no dedicated gc_fwd)");

#define HDR_FORWARDED  (uint16_t)0x0001u   /* small obj copied to to-space */
#define HDR_MARKED     (uint16_t)0x0002u   /* large obj marked alive */
#define HDR_LARGE      (uint16_t)0x0004u   /* large obj (malloc'd, non-moving) */
#define HDR_IS_FORWARDED(h) (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)
#define HDR_IS_MARKED(h)    (((h)->gc_flags & HDR_MARKED) != 0)
#define HDR_SET_MARKED(h)   ((h)->gc_flags |= HDR_MARKED)
#define HDR_CLR_MARKED(h)   ((h)->gc_flags &= (uint16_t)~HDR_MARKED)
#define HDR_IS_LARGE(h)     (((h)->gc_flags & HDR_LARGE) != 0)

static inline void *
fwd_overlay_get(AroObjectHeader *h)
{
    return *(void **)((char *)h + sizeof(AroObjectHeader));
}

static inline void
fwd_overlay_set(AroObjectHeader *h, void *new_payload)
{
    *(void **)((char *)h + sizeof(AroObjectHeader)) = new_payload;
}

#define REGION_BYTES  ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virtual per semispace, lazy-paged */
/* Stress mode mmaps a fresh to-space every GC and retires the old one;
 * a 64 GiB region would accumulate TiBs of virtual address space, so
 * stress mode uses a much smaller region. */
#define STRESS_REGION_BYTES  ((size_t)64u << 20)  /* 64 MiB */

/* PURGE mode: round-robin through a 64 GiB virtual arena, allocating
 * one PURGE_PLANE_BYTES plane per GC cycle.  After each collect the
 * just-retired plane is `mprotect PROT_NONE` + `madvise DONTNEED` so
 * stale pointers into from-space SEGV deterministically (= replaces
 * the old per-GC mmap/munmap PURGE which had to reuse the same virtual
 * address immediately).  64 GiB / 64 MiB = 1024 planes in rotation
 * before any virtual address gets reused.
 *
 * Trade-off vs old PURGE: marginally higher RSS during steady state
 * (= 1 active plane physical-committed, others MADV_DONTNEED'd zero-
 * faulted on touch), but per-GC cost is mprotect+madvise (= O(plane)
 * bookkeeping, no kernel VMA tree manipulation). */
#define PURGE_PLANE_BYTES    STRESS_REGION_BYTES
#define PURGE_ARENA_BYTES    ARO_GC_REGION_VIRT_BYTES
#define ALIGN8(n)     (((n) + 7u) & ~(size_t)7u)

/* Adaptive GC trigger.  Match the mark / immix policy: trigger at
 * `bytes_since_gc > gc_threshold`, gc_threshold = max(16 MiB, 2 × live_post_cheney). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

/* Large-object threshold.  Payloads >= this go to a separately-malloc'd
 * non-moving region.  Win: large dead payloads (e.g., sieve's 128 MiB
 * BaArray.items after a doubling) get free'd promptly (glibc's M_MMAP_THRESHOLD
 * = 128 KiB causes such chunks to be mmap'd, so free → munmap → physical
 * release) instead of sitting in from-space until the next collect.
 * Matches `gc_mark.c`'s slab class max (4096 B). */
#define LARGE_THRESHOLD      4096u

/* Large object wrapper.  Lives in a separately-malloc'd buffer; not in
 * either semispace.  Payload (= sample struct starting with
 * AroObjectHeader) immediately follows the next-pointer pair —
 * `large_payload(lo) = lo + 1`.
 *
 * "Forwarded marker" for large objects (= non-moving): set
 * head.gc_fwd to the payload itself.  Sweep clears it to NULL on
 * survivors and free()s the entry for unmarked. */
typedef struct LargeObj {
    struct LargeObj *next;       /* live list, threaded through aro_gc_init's gc->large_head */
    struct LargeObj *next_gray;  /* gray queue during collect; NULL when not in queue */
    /* payload follows: large_payload(lo) = (void *)(lo + 1) */
} LargeObj;

static inline LargeObj *
large_from_payload(void *p)
{
    return (LargeObj *)((char *)p - sizeof(LargeObj));
}

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

static inline AroObjectHeader *
large_head(LargeObj *lo)
{
    return (AroObjectHeader *)large_payload(lo);
}

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// Heap-allocated in aro_gc_init; lifetime = lifetime of the owning CTX.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    /* Common header — must be first field.  See gc.h AroGcCommonState. */
    AroGcCommonState common;

    /* Active semispace (where allocations go).  Non-PURGE mode
     * alternates the `space0` / `space1` pair; PURGE mode bumps through
     * planes in the 64 GiB `purge_arena_*` reservation. */
    char *active_base;
    char *active_top;
    char *active_end;
    char *space0;
    char *space1;
    int   active_idx;

    /* PURGE mode: 64 GiB virtual arena reserved at init (MAP_NORESERVE
     * PROT_NONE).  Each GC mprotect-enables the next plane and
     * mprotect-disables the just-retired one — virtual address reuse
     * lag = arena/plane planes (= 1024 by default). */
    char *purge_arena_base;
    char *purge_arena_end;

    /* Adaptive trigger state */
    size_t bytes_since_gc;
    size_t gc_threshold;

    /* Reserved region size — stress mode uses a much smaller region. */
    size_t region_bytes;

    /* CTX bind (= back-pointer for callbacks that only have ASTroGC *). */
    CTX *ctx;

    /* Cheney scratch (used during gc_collect_internal only) */
    char *to_top;
    char *to_base;
    char *from_base_cur;

    /* Large-object lists.  `large_head` is the live list, threaded by
     * LargeObj.next.  `large_gray` is the scan queue during collect. */
    LargeObj *large_head;
    LargeObj *large_gray;
} ASTroGC;

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

static char *
mmap_region(size_t bytes)
{
#ifdef KORB_WASI
    // WASI の mmap エミュレーションは MAP_NORESERVE を受け付けない。
    // wasm の線形メモリはどのみち遅延コミットされないので、素の確保でよい。
    // wasm の線形メモリは memory.grow で増えた分が仕様上ゼロなので、calloc の
    // memset は要らない (mmap(MAP_ANONYMOUS) がゼロを保証するのと同じ理屈)。
    char *p = (char *)malloc(bytes);
    if (p == NULL) { perror("alloc"); abort(); }
#else
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
#endif
    return p;
}

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    gc->ctx = c;
    gc->gc_threshold = GC_THRESHOLD_MIN;
    c->astro_gc = gc;             /* CTX → ASTroGC を bind */
    /* ASTRO_GC_* is the canonical name; BARUBY_GC_* kept as a fallback
     * (shared runtime — other precise samples still use the old name). */
    const char *stress_env = getenv("ASTRO_GC_STRESS");
    if (!stress_env) stress_env = getenv("BARUBY_GC_STRESS");
    bool stress = stress_env != NULL;
    bool purge  = (getenv("ASTRO_GC_PURGE") != NULL) || (getenv("BARUBY_GC_PURGE") != NULL);
    ARO_GC_COMMON(c)->stress = stress;
    ARO_GC_COMMON(c)->purge  = purge;
    if (stress) {
        /* BARUBY_GC_STRESS=N (N > 1) → GC every N allocs.  N=1 or any
         * non-numeric value keeps the every-alloc behavior.  Lets large
         * workloads finish under STRESS+PURGE without timing out while
         * still surfacing stale-ptr bugs. */
        uint64_t interval = 1;
        if (stress_env && *stress_env) {
            char *endp = NULL;
            uint64_t n = strtoull(stress_env, &endp, 10);
            if (endp && *endp == '\0' && n >= 1) interval = n;
        }
        ARO_GC_COMMON(c)->stress_interval = interval;
        ARO_GC_COMMON(c)->stress_count = 0;
        /* gc_threshold = 0 only when stress_interval == 1 (every alloc).
         * For larger intervals, use the normal adaptive threshold so the
         * heap doesn't fill up between forced GCs.  Without this, every
         * alloc triggers GC via the threshold check even when stress_fire_now
         * returns false — defeating the interval. */
        if (interval <= 1) {
            gc->gc_threshold = 0;
        }
    }
    /* PURGE: reserve a 64 GiB MAP_NORESERVE / PROT_NONE arena once, then
     * mprotect-enable a fresh plane each GC and PROT_NONE the retired
     * one.  Stale ptr deref deterministically SEGVs until 1024 planes
     * worth of GC have passed (= virtual address reuse lag). */
    if (purge) {
        char *arena = (char *)mmap(NULL, PURGE_ARENA_BYTES, PROT_NONE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,
                                   -1, 0);
        if (arena == MAP_FAILED) { perror("mmap PURGE arena"); abort(); }
        gc->purge_arena_base = arena;
        gc->purge_arena_end  = arena + PURGE_ARENA_BYTES;
        gc->region_bytes = PURGE_PLANE_BYTES;
        if (mprotect(arena, PURGE_PLANE_BYTES, PROT_READ|PROT_WRITE) != 0) {
            perror("mprotect PURGE initial plane"); abort();
        }
        gc->active_base = arena;
        gc->active_top  = arena;
        gc->active_end  = arena + PURGE_PLANE_BYTES;
        /* space0/space1 unused under PURGE. */
    } else {
        gc->region_bytes = stress ? STRESS_REGION_BYTES : REGION_BYTES;
        gc->space0 = mmap_region(gc->region_bytes);
        gc->space1 = mmap_region(gc->region_bytes);
        gc->active_idx  = 0;
        gc->active_base = gc->space0;
        gc->active_top  = gc->space0;
        gc->active_end  = gc->space0 + gc->region_bytes;
    }
    if (stress || purge) {
        char stress_buf[64] = "";
        if (stress) {
            uint64_t iv = ARO_GC_COMMON(c)->stress_interval;
            if (iv <= 1) snprintf(stress_buf, sizeof(stress_buf), " STRESS (GC every alloc)");
            else snprintf(stress_buf, sizeof(stress_buf), " STRESS (GC every %llu allocs)", (unsigned long long)iv);
        }
        fprintf(stderr, "[aro_gc=copy]%s%s\n",
                stress_buf,
                purge  ? " PURGE (round-robin mprotect)" : "");
    }
}

// ----------------------------------------------------------------------------
// Allocation
// ----------------------------------------------------------------------------

static void gc_collect_internal(CTX *c);

static void __attribute__((noinline, cold))
gc_bump_cold(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    gc_collect_internal(c);
    if (gc->active_top + total > gc->active_end) {
        fprintf(stderr, "aro_gc: OOM (need %zu, have %zu)\n",
                total, (size_t)(gc->active_end - gc->active_top));
        abort();
    }
}

/* Bump in semispace.  Returns payload pointer (= AroObjectHeader at
 * offset 0).  Caller (sample) sets head.flags after return. */
static inline bool
stress_fire_now(CTX *c)
{
    if (!ARO_GC_COMMON(c)->stress) return false;
    uint64_t iv = ARO_GC_COMMON(c)->stress_interval;
    if (iv <= 1) return true;
    if (++ARO_GC_COMMON(c)->stress_count >= iv) {
        ARO_GC_COMMON(c)->stress_count = 0;
        return true;
    }
    return false;
}

static inline void *
gc_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(stress_fire_now(c)
                         || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold
                         || (gc->active_top + aligned) > gc->active_end, 0)) {
        gc_bump_cold(c, aligned);
    }
    void *payload = gc->active_top;
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    gc->active_top += aligned;
    gc->bytes_since_gc += payload_size;
    return payload;
}

/* Allocate a LargeObj in malloc heap and link into gc->large_head. */
static void *
large_alloc(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(stress_fire_now(c)
                         || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold, 0)) {
        gc_collect_internal(c);
    }
    LargeObj *lo = (LargeObj *)malloc(sizeof(LargeObj) + aligned);
    if (!lo) { fprintf(stderr, "aro_gc=copy: large OOM (%zu)\n", payload_size); abort(); }
    lo->next = gc->large_head;
    gc->large_head = lo;
    lo->next_gray = NULL;
    void *payload = large_payload(lo);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = HDR_LARGE;   /* O(1) discriminator in forward_payload */
    h->gc_size  = (uint32_t)payload_size;
    gc->bytes_since_gc += payload_size;
    return payload;
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    void *payload = __builtin_expect(payload_size >= LARGE_THRESHOLD, 0)
        ? large_alloc(c, payload_size, aligned)
        : gc_bump    (c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Zero the post-head region so a GC scan immediately after alloc sees no
     * stale heap-pointer bits.  `aligned` and the header are both 8-multiples,
     * so the region is a whole number of 64-bit words.  Most objects are small
     * (the variable-size buffers are separate allocs), so inline the word stores
     * for <=8 words to skip the __memset PLT call; memset only the large tail. */
    uint64_t *const zw = (uint64_t *)((char *)payload + sizeof(AroObjectHeader));
    const size_t znw = (aligned - sizeof(AroObjectHeader)) / sizeof(uint64_t);
    switch (znw) {
        case 8: zw[7] = 0;  /* fallthrough */
        case 7: zw[6] = 0;  /* fallthrough */
        case 6: zw[5] = 0;  /* fallthrough */
        case 5: zw[4] = 0;  /* fallthrough */
        case 4: zw[3] = 0;  /* fallthrough */
        case 3: zw[2] = 0;  /* fallthrough */
        case 2: zw[1] = 0;  /* fallthrough */
        case 1: zw[0] = 0;  /* fallthrough */
        case 0: break;
        default: memset(zw, 0, znw * sizeof(uint64_t));
    }

    ARO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ARO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

/* Audit query: is `p` a moved-out (stale) heap payload — an arena pointer NOT in
 * the current active (to-space) live region [active_base, active_top)?  A copy
 * GC can answer this exactly: a live object is in the active region; a handle
 * left over from before a collection points to a retired plane (PURGE) or the
 * inactive space (non-PURGE).  Used by a sample's construction-time check to
 * catch an already-collected value being wrapped.  Does NOT dereference p;
 * immediates / non-arena (libc-immortal) pointers report not-stale. */
int
aro_gc_addr_stale(CTX *c, const void *p)
{
    if (p == NULL || ((uintptr_t)p & 7u)) return 0;
    const ASTroGC *const gc = ARO_GC_INSTANCE(c);
    if (!gc || !gc->active_base) return 0;
    const char *const cp = (const char *)p;
    if (cp >= gc->active_base && cp < gc->active_top) return 0;   /* live */
    if (gc->purge_arena_base &&
        cp >= gc->purge_arena_base && cp < gc->purge_arena_end)
        return 1;   /* PURGE: in the super-arena but not the active plane = retired */
    {   /* non-PURGE: the inactive space holds stale (pre-swap) copies */
        const char *const inactive = (gc->active_idx == 0) ? gc->space1 : gc->space0;
        if (inactive && cp >= inactive && cp < inactive + gc->region_bytes)
            return 1;
    }
    return 0;   /* outside arena = libc-immortal, not stale */
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    void *payload = __builtin_expect(payload_size >= LARGE_THRESHOLD, 0)
        ? large_alloc(c, payload_size, aligned)
        : gc_bump    (c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Byte payloads: skip post-head zero-fill (caller writes data
     * immediately).  Head was init'd to zero by the bump/large path. */

    ARO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ARO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
}

/* In-place realloc for large objs.  Returns NULL to fall through to the
 * default alloc + memcpy path for: stress mode, small `old`, shrink to
 * small.  Otherwise realloc(3) the underlying LargeObj — glibc's
 * M_MMAP_THRESHOLD (≥128 KiB) chunks use mremap, so the buffer may be
 * resized in-place (or relocated, but always without our memcpy).
 *
 * Caller (gc_common.c::aro_gc_realloc_payload) handles NULL by retrying
 * with the parking + alloc + memcpy fallback. */
void *
aro_gc_realloc_in_place(CTX *c, void *old, size_t new_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (ARO_GC_COMMON(c)->stress) return NULL;
    if (new_size < LARGE_THRESHOLD) return NULL;
    char *p = (char *)old;
    if (p >= gc->active_base && p < gc->active_end) return NULL;

    /* Walk large_head to find lo's slot in the linked list (so we can
     * patch `next` if realloc moves the LargeObj header).  N is small
     * for our workloads (1-3 live large objs in flight); the O(N) walk
     * runs only once per realloc, well outside the per-alloc hot path. */
    LargeObj **link = &gc->large_head;
    while (*link && large_payload(*link) != old) link = &(*link)->next;
    if (!*link) return NULL;
    LargeObj *lo = *link;

    AroObjectHeader *oldh = large_head(lo);
    size_t old_size = oldh->gc_size;
    size_t old_aligned = ALIGN8(old_size);
    size_t new_aligned = ALIGN8(new_size);
    LargeObj *new_lo = (LargeObj *)realloc(lo, sizeof(LargeObj) + new_aligned);
    if (!new_lo) { perror("aro_gc=copy: realloc large"); abort(); }
    *link = new_lo;
    AroObjectHeader *newh = large_head(new_lo);
    newh->gc_size = (uint32_t)new_size;

    /* Zero the freshly-grown tail unconditionally.  Sample-side SCAN_EDGES
     * for OBJ_BYTE_DATA short-circuits, so even pre-existing stale bytes
     * never look like pointers to GC.  Cost: one memset per realloc grow
     * — well outside any hot path. */
    if (new_aligned > old_aligned) {
        memset((char *)large_payload(new_lo) + old_aligned, 0,
               new_aligned - old_aligned);
    }

    /* stats: charge the growth (mirrors the alloc path).  Adaptive
     * trigger sees the delta, just like a fresh alloc would. */
    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        ARO_GC_COMMON(c)->stats.total_bytes += delta;
        ARO_GC_COMMON(c)->stats.heap_bytes  += delta;
        gc->bytes_since_gc += delta;
    }
    return large_payload(new_lo);
}

// ----------------------------------------------------------------------------
// Cheney-style copy collector
// ----------------------------------------------------------------------------

/* Forward an old payload pointer: copy to to-space if not already done,
 * return new payload address.  `gc` carries the active from/to-space
 * bounds (set by gc_collect_internal).
 *
 * Three paths:
 *   (1) From-space arena (small obj, not yet copied) → cheney copy to to-space
 *   (2) Already-forwarded (fwd non-NULL) → return fwd (self-ptr for large,
 *       to-space ptr for small).  Single check handles both.
 *   (3) Large obj (outside from-space arena), not yet marked → mark by
 *       setting fwd=self-payload, enqueue on large_gray for content scan,
 *       return same payload (non-moving). */
/* True iff p points into a RETIRED (mprotect(PROT_NONE)'d) purge plane — a
 * pointer from a past cycle whose object has moved, so dereferencing p would
 * SEGV.  Lets sample code defensively skip a slot that escaped root-forwarding
 * (e.g. an orphaned cref->klass) instead of crashing.  False when PURGE is off
 * or p is current/from/to-space or a libc-immortal address. */
bool aro_gc_addr_retired(CTX *c, const void *p)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc || !gc->purge_arena_base) return false;
    const char *cp = (const char *)p;
    if (cp < gc->purge_arena_base || cp >= gc->purge_arena_end) return false;
    /* Between collections every live object resides in the current active
     * plane [active_base, active_end).  Anything else inside the purge
     * super-arena is a retired (mprotect PROT_NONE) plane — a stale ref. */
    return !(cp >= gc->active_base && cp < gc->active_end);
}

static void *
forward_payload(ASTroGC *gc, void *old_payload)
{
    if (!old_payload) return NULL;
    /* Bounds check BEFORE dereferencing oldh — under PURGE, addresses
     * outside the current from/to planes are mprotect'd PROT_NONE, and
     * reading their headers SEGVs.  Determine the payload's region:
     * - in current from-space plane: from-space obj, normal cheney path
     * - in current to-space plane: stale or already-forwarded
     * - in PURGE super-arena but not current plane: retired plane,
     *   stale ref from past cycle → return NULL (= safe to nullify slot)
     * - outside the arena entirely: libc-allocated immortal → return as-is */
    {
        char *p = (char *)old_payload;
        bool in_from = (p >= gc->from_base_cur && p < gc->from_base_cur + gc->region_bytes);
        bool in_to   = (p >= gc->to_base       && p < gc->to_base       + gc->region_bytes);
        if (!in_from && !in_to) {
            if (gc->purge_arena_base &&
                p >= gc->purge_arena_base && p < gc->purge_arena_end) {
                /* PURGE: in super-arena but not current plane = retired,
                 * PROT_NONE.  Cannot deref.  Stale ref. */
                return NULL;
            }
            /* A large object (malloc'd via large_alloc) lives OUTSIDE the
             * from/to plane ranges.  glibc places its mmap wherever the
             * kernel chooses — often just below/around the GC's reserved
             * region — so the bounds checks above can miss it and it would
             * be mistaken for a libc-immortal pointer: never marked, then
             * swept by the large_head sweep, leaving a->backing dangling
             * (reproduced by optcarrot's 256 KiB TILE_LUT Array backings).
             * The in-to branch only catches large objs that happen to land
             * inside the reserved range.  We're past the purge-retired check
             * above, so `old_payload` is mapped (libc-immortal OR a malloc'd
             * large object) and its header is safe to read: the HDR_LARGE bit
             * (set by large_alloc) discriminates in O(1) — no large_head walk,
             * which was O(large_count) per out-of-range ref and pathological
             * for AST-node-heavy programs (optcarrot 1 frame >> minutes). */
            if (gc->large_head && HDR_IS_LARGE((AroObjectHeader *)old_payload)) {
                if (!HDR_IS_MARKED((AroObjectHeader *)old_payload)) {
                    HDR_SET_MARKED((AroObjectHeader *)old_payload);
                    LargeObj *lo = large_from_payload(old_payload);
                    lo->next_gray = gc->large_gray;
                    gc->large_gray = lo;
                }
                return old_payload;
            }
            /* libc-allocated immortal. */
            return old_payload;
        }
    }
    AroObjectHeader *oldh = (AroObjectHeader *)old_payload;
    /* Small obj already forwarded → return overlay-stored ptr. */
    if (oldh->gc_flags & HDR_FORWARDED) {
        return fwd_overlay_get(oldh);
    }

    /* Large object: lives outside the from-space arena, doesn't move.
     * Mark + enqueue for content scan; return the same payload pointer. */
    if (__builtin_expect(gc->large_head != NULL, 0)) {
        char *p = (char *)old_payload;
        if (p < gc->from_base_cur || p >= gc->from_base_cur + gc->region_bytes) {
            if (HDR_IS_MARKED(oldh)) return old_payload;
            HDR_SET_MARKED(oldh);
            LargeObj *lo = large_from_payload(old_payload);
            lo->next_gray = gc->large_gray;
            gc->large_gray = lo;
            return old_payload;
        }
    }

    /* Cheney copy: from-space → to-space.  After memcpy, mark old as
     * FORWARDED and store fwd ptr in overlay slot (= payload offset 8,
     * overwriting first sample field — from-space is discarded after
     * collect, so destruction is harmless). */
    size_t aligned = ALIGN8(oldh->gc_size);
    /* old_payload in current to-space arena means it's a stale ref from a
     * prior cycle (= the to/from swap reused this memory).  Treat the
     * slot as unreachable: return NULL so *slot = NULL.  This is the
     * safest recovery — the original obj is gone, so dereferencing the
     * slot was already a UAF; converting to NULL surfaces it deterministically
     * (= NIL_P / SPECIAL_CONST_P checks skip).  Previously this branch
     * aborted; the abort masks tests whose sample-side root tracking has
     * a gap (= libc container holds the only ref → arena obj collected →
     * stale ref reappears via the libc container next visit).
     *
     * baruby_precise / ascheme_precise have fully arena-allocated
     * containers so this branch is unreachable for them; only sample-
     * specific mixed-allocation code paths hit it.  koruby_precise's
     * Phase 3 (= migrate all containers to arena) is the proper fix;
     * until then this fallback prevents abort. */
    if ((char *)old_payload >= gc->to_base &&
        (char *)old_payload < gc->to_base + gc->region_bytes) {
        return NULL;
    }
    void *new_payload = gc->to_top;
    memcpy(new_payload, old_payload, aligned);
    /* memcpy copied oldh's gc_flags (without FORWARDED yet) — fine. */
    gc->to_top += aligned;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, new_payload);
    return new_payload;
}

/* edge_visit callback for SCAN_EDGES and for root scan.  Unified: slot
 * may hold either a raw heap pointer (= BaArray.items / BaString.bytes,
 * always 8-aligned) or a tagged VALUE (= sp[] / KIND_PAYLOAD_VAL items).
 * AROH_IS_GC_OBJECT filters out singletons / fixnums; heap pointers pass through
 * (= 8-aligned, non-singleton).  `ctx` is `ASTroGC *gc` from SCAN_EDGES. */
static void
forward_edge(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (!AROH_IS_GC_OBJECT(v)) return;
    /* Idempotent skip: if the value is already a to-space address (= some
     * earlier visit in THIS cycle already rewrote the slot), don't re-
     * forward.  Without this, an obj reachable from two distinct walk
     * paths (= method aliasing puts the same korb_method into multiple
     * class tables in koruby) gets memcpy'd a second time at to_top,
     * producing a phantom obj and runaway scan. */
    if ((char *)v >= gc->to_base && (char *)v < gc->to_top) return;
    *slot = (void *)(VALUE)forward_payload(gc, (void *)v);
}

static void
gc_collect_internal(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    char *from_base = gc->active_base;
    char *from_top_pre = gc->active_top;

    /* Determine the to-space.  PURGE mode advances to the next plane in
     * the 64 GiB round-robin arena and mprotect-enables it (= stale
     * pointers into the just-retired plane SEGV); non-purge alternates
     * the space0 / space1 pair. */
    char *next_to_base;
    if (ARO_GC_COMMON(c)->purge) {
        next_to_base = gc->active_base + PURGE_PLANE_BYTES;
        if (next_to_base + PURGE_PLANE_BYTES > gc->purge_arena_end) {
            /* Wrap to the start of the 64 GiB arena.  By the time we
             * wrap, 1024 GC cycles have passed; any sample-held stale
             * pointer is long gone from registers / data. */
            next_to_base = gc->purge_arena_base;
        }
        if (mprotect(next_to_base, PURGE_PLANE_BYTES,
                     PROT_READ|PROT_WRITE) != 0) {
            perror("mprotect PURGE new plane"); abort();
        }
        /* Ensure zero pages on access (= MADV_DONTNEED resets the
         * mapping; next access fault gives fresh zero pages, mirroring
         * MAP_ANONYMOUS semantics).  Necessary on wrap-around where the
         * plane previously had physical pages from earlier use. */
        if (madvise(next_to_base, PURGE_PLANE_BYTES, MADV_DONTNEED) != 0) {
            perror("madvise PURGE new plane"); abort();
        }
    } else {
        next_to_base = (gc->active_idx == 0) ? gc->space1 : gc->space0;
    }

    gc->to_base = next_to_base;
    gc->to_top  = next_to_base;
    gc->from_base_cur = from_base;

    ARO_GC_COMMON(c)->stats.heap_bytes = 0;

    /* (1) Root scan: forward VALUE pointers in sample-owned root slots.
     * sample's AROH_VISIT_ROOTS macro handles any high-water /
     * dead-slot zeroing it cares about. */
    struct timespec tcheney = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, forward_edge);

    /* (2a) Cheney scan-loop in to-space.  Hot loop, run unconditionally.
     * SCAN category calls sample's SCAN_EDGES which dispatches via
     * ObjectHeader.type (OBJ_ARRAY / OBJ_STRING / OBJ_VALUE_ARRAY).
     * BYTE / FREE skip.  forward_edge uses AROH_IS_GC_OBJECT to filter values. */
    char *scan = gc->to_base;
    while (scan < gc->to_top) {
        AroObjectHeader *h = (AroObjectHeader *)scan;
        /* Sanity: gc_size should be a reasonable sample object size (= a few
         * dozen to a few hundred bytes for typical structs).  If it's
         * gigantic, scan would advance to garbage memory next iteration —
         * abort with diagnostics so the upstream corruption is visible. */
        if (UNLIKELY(h->gc_size == 0 || h->gc_size > (1u << 20))) {
            fprintf(stderr, "GC BUG: corrupt gc_size=%u at scan=%p (to_base=%p) flags=0x%x — stopping scan\n",
                    h->gc_size, (void*)scan, (void*)gc->to_base, h->flags);
            break;
        }
        AROH_SCAN_EDGES(h, h->gc_size, gc, forward_edge);
        ARO_GC_COMMON(c)->stats.heap_bytes += h->gc_size;
        scan += ALIGN8(h->gc_size);
    }

    /* (2b) Large-gray drain (only if large objs exist).  Each large gray
     * scan can produce new to-space objs (which need cheney drain) or
     * new large gray entries, so loop until both queues empty. */
    while (gc->large_gray) {
        LargeObj *lo = gc->large_gray;
        gc->large_gray = lo->next_gray;
        lo->next_gray = NULL;
        AroObjectHeader *h = large_head(lo);
        AROH_SCAN_EDGES((void *)h, h->gc_size, gc, forward_edge);
        ARO_GC_COMMON(c)->stats.heap_bytes += h->gc_size;
        /* Drain any newly-added to-space objs before processing the next
         * large gray (preserves the cheney-scan order semantics). */
        while (scan < gc->to_top) {
            AroObjectHeader *h2 = (AroObjectHeader *)scan;
            AROH_SCAN_EDGES(h2, h2->gc_size, gc, forward_edge);
            ARO_GC_COMMON(c)->stats.heap_bytes += h2->gc_size;
            scan += ALIGN8(h2->gc_size);
        }
    }

    /* Finalize pass: after mark/forward, before sweep/swap.  Backend's
     * aro_gc_finalize_check below returns fwd_overlay_get(h) for forwarded
     * small objs, payload itself for marked large objs, NULL for dead. */
    aro_gc_finalize_walk(c);

    /* (3) Sweep large_head: free unmarked, clear marker on survivors. */
    LargeObj **link = &gc->large_head;
    while (*link) {
        LargeObj *lo = *link;
        AroObjectHeader *h = large_head(lo);
        if (!HDR_IS_MARKED(h)) {
            /* unmarked → free */
            *link = lo->next;
            free(lo);
        } else {
            /* marked → clear for next cycle, keep in list */
            HDR_CLR_MARKED(h);
            link = &lo->next;
        }
    }
    aro_gc_phase_end(tcheney, &ARO_GC_COMMON(c)->stats.reclaim_seconds);

    /* (4) Swap active.  Non-purge alternates space0/space1.  Purge mode
     * uses freshly-mmap'd region (next_to_base); old gets munmap'd below. */
    if (!ARO_GC_COMMON(c)->purge) {
        gc->active_idx = 1 - gc->active_idx;
    }
    gc->active_base = next_to_base;
    gc->active_top  = gc->to_top;
    gc->active_end  = next_to_base + gc->region_bytes;

    /* (5) Retire the old active.  PURGE mprotects PROT_NONE +
     * MADV_DONTNEED (= stale ptrs deref SEGV, physical released);
     * non-purge keeps the space for the next alternation cycle. */
    if (ARO_GC_COMMON(c)->purge) {
        if (mprotect(from_base, PURGE_PLANE_BYTES, PROT_NONE) != 0) {
            perror("gc_collect: mprotect retire"); abort();
        }
        if (madvise(from_base, PURGE_PLANE_BYTES, MADV_DONTNEED) != 0) {
            perror("gc_collect: madvise retire"); abort();
        }
    }
    (void)from_top_pre;

    gc->bytes_since_gc = 0;
    /* stress keeps threshold at 0 → force GC every alloc.  Non-stress
     * adapts threshold to live set. */
    if (!ARO_GC_COMMON(c)->stress) {
        size_t live = ARO_GC_COMMON(c)->stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc->gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }

    ARO_GC_COMMON(c)->stats.gc_count++;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    gc_collect_internal(c);
}

/* Heap walk.  Cheney bump allocation makes the active semispace gapless:
 * [active_base, active_top) is a run of payloads, each introduced by its own
 * AroObjectHeader, with no free-list holes and no forwarding pointers (those
 * only ever live in the retired from-space).  So the walk is just "step by
 * ALIGN8(gc_size)".  Large objects sit outside the arena on `large_head`.
 *
 * Garbage that has not been collected yet is visited too — the arena is a
 * chronological log of allocations since the last collect, and nothing marks
 * the dead ones.  CRuby's rb_objspace_each_objects behaves the same way.
 *
 * `visit` must not allocate (see gc.h). */
bool
aro_gc_each_object(CTX *c, void (*visit)(void *arg, void *payload), void *arg)
{
    const ASTroGC *const gc = ARO_GC_INSTANCE(c);
    if (!gc || !gc->active_base) return false;
    char *p = gc->active_base;
    char *const end = gc->active_top;
    while (p < end) {
        AroObjectHeader *const h = (AroObjectHeader *)p;
        /* Same guard as the cheney scan loop: a bad size would step the cursor
         * into the middle of an object and every later header would be junk. */
        if (UNLIKELY(h->gc_size == 0 || h->gc_size > (1u << 20))) {
            fprintf(stderr, "GC BUG: corrupt gc_size=%u at %p during each_object walk\n",
                    h->gc_size, (void *)p);
            return false;
        }
        p += ALIGN8(h->gc_size);
        visit(arg, h);
    }
    for (const LargeObj *lo = gc->large_head; lo != NULL; lo = lo->next)
        visit(arg, large_payload((LargeObj *)(uintptr_t)lo));
    return true;
}

/* Liveness for a registered finalize payload, post mark/forward:
 *   - small obj: HDR_FORWARDED → live, return fwd_overlay (= new addr)
 *   - large obj: HDR_MARKED    → live, payload doesn't move (returned as-is)
 *   - else: dead → NULL (caller invokes AROH_FINALIZE). */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    (void)c;
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (h->gc_flags & HDR_FORWARDED) {
        return fwd_overlay_get(h);
    }
    if (h->gc_flags & HDR_MARKED) {
        return payload;
    }
    return NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (ARO_GC_COMMON(c)->purge) {
        /* PURGE mode: one 64 GiB arena reservation. */
        if (gc->purge_arena_base) {
            munmap(gc->purge_arena_base, PURGE_ARENA_BYTES);
        }
    } else if (ARO_GC_COMMON(c)->stress) {
        /* Stress (non-purge) mode: only the current active region is mapped. */
        if (gc->active_base) munmap(gc->active_base, gc->region_bytes);
    } else {
        if (gc->space0) munmap(gc->space0, gc->region_bytes);
        if (gc->space1) munmap(gc->space1, gc->region_bytes);
    }
    aro_gc_free_large_chain_malloc(gc->large_head);
    free(gc);
    c->astro_gc = NULL;
}

/* Stat readers are static inline in gc.h (read ARO_GC_COMMON(c)->stats directly). */

size_t
aro_gc_size_of(void *p)
{
    return ((AroObjectHeader *)p)->gc_size;
}
