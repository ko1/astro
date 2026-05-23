#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
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

const char *aro_gc_backend_name = "copy_scramble";

/* copy_scramble — debug / audit backend.  Identical to copy except that
 * every heap-pointer VALUE slot (= sample's sp[], BaArrayItems.data[])
 * is XOR-scrambled with a per-GC-cycle random R.  Sample decodes via the
 * ARO_OBJ macro (gc.h); the framework's VISIT_EDGE_VAL wrapper (gc.h)
 * handles the decode/forward/re-encode dance inside collect.
 *
 * Detection goal: replace BARUBY_GC_STRESS for catching mark/move 漏れ
 * with less overhead than GC-per-alloc.  Three classes of bug surface
 * as SEGV at the next deref:
 *   (1) GC missed scanning a root or edge → slot keeps OLD-R encoding
 *       → decode with NEW R → garbage addr.
 *   (2) Sample forgot ARO_OBJ on a deref (= raw cast) → uses scrambled
 *       bits directly as addr → garbage.
 *   (3) Sample forgot ARO_VAL on a store (= raw addr in VALUE slot) →
 *       next GC's VISIT_EDGE_VAL decodes raw as if encoded → garbage.
 *
 * Typed-ptr fields (= BaArray.items / BaString.bytes) remain RAW (=
 * unscrambled).  These are explicit struct fields, less prone to
 * "forgot to scan" bugs than ad-hoc VALUE slots.  Forwarding them
 * uses the unchanged VISIT_EDGE_PTR macro. */

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
#define HDR_IS_FORWARDED(h) (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)
#define HDR_IS_MARKED(h)    (((h)->gc_flags & HDR_MARKED) != 0)
#define HDR_SET_MARKED(h)   ((h)->gc_flags |= HDR_MARKED)
#define HDR_CLR_MARKED(h)   ((h)->gc_flags &= (uint16_t)~HDR_MARKED)

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

    /* Active semispace (where allocations go).  In stress mode each GC
     * mmaps a fresh to-space; in non-stress mode we alternate the
     * `space0` / `space1` pair. */
    char *active_base;
    char *active_top;
    char *active_end;
    char *space0;
    char *space1;
    int   active_idx;

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
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

/* Pick a fresh scramble R: random 64-bit value with low 3 bits forced
 * to 0 (= preserve heap-pointer 8-alignment AND fixnum LSB tag).  Use
 * /dev/urandom for portability; fall back to a time-based seed.  R == 0
 * is rejected (would degrade to identity = no detection). */
static uintptr_t
scramble_pick_R(void)
{
    uintptr_t r = 0;
    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp) {
        if (fread(&r, sizeof(r), 1, fp) != 1) r = 0;
        fclose(fp);
    }
    if (r == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        r = (uintptr_t)ts.tv_nsec ^ ((uintptr_t)ts.tv_sec << 20);
        r = (r << 8) ^ (uintptr_t)getpid();
    }
    r &= ~(uintptr_t)7u;  /* clear low 3 bits */
    if (r == 0) r = (uintptr_t)1 << 8;   /* extreme fallback */
    return r;
}

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    gc->ctx = c;
    gc->gc_threshold = GC_THRESHOLD_MIN;
    c->astro_gc = gc;             /* CTX → ASTroGC を bind */
    /* Initial R.  Sample writes its first VALUE-slots with ARO_VAL(c, raw)
     * which XORs against scramble_R, so we need R set before any alloc. */
    ARO_GC_COMMON(c)->scramble_R = scramble_pick_R();
    ARO_GC_COMMON(c)->scramble_R_old = ARO_GC_COMMON(c)->scramble_R;
    if (getenv("BARUBY_GC_STRESS")) {
        ARO_GC_COMMON(c)->stress = 1;
        gc->region_bytes = STRESS_REGION_BYTES;
        gc->active_base = mmap_region(gc->region_bytes);
        gc->active_top  = gc->active_base;
        gc->active_end  = gc->active_base + gc->region_bytes;
        fprintf(stderr, "[baruby_gc] STRESS mode: collect on every alloc, "
                        "old space munmap'd each GC\n");
    } else {
        gc->region_bytes = REGION_BYTES;
        gc->space0 = mmap_region(gc->region_bytes);
        gc->space1 = mmap_region(gc->region_bytes);
        gc->active_idx  = 0;
        gc->active_base = gc->space0;
        gc->active_top  = gc->space0;
        gc->active_end  = gc->space0 + gc->region_bytes;
    }
}

// ----------------------------------------------------------------------------
// Allocation
// ----------------------------------------------------------------------------

static void gc_collect_internal(CTX *c);

static void __attribute__((noinline, cold))
gc_bump_slow(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    gc_collect_internal(c);
    if (gc->active_top + total > gc->active_end) {
        fprintf(stderr, "baruby_gc: OOM (need %zu, have %zu)\n",
                total, (size_t)(gc->active_end - gc->active_top));
        abort();
    }
}

/* Bump in semispace.  Returns payload pointer (= AroObjectHeader at
 * offset 0).  Caller (sample) sets head.flags after return. */
static inline void *
gc_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (__builtin_expect(ARO_GC_COMMON(c)->stress
                         || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold
                         || (gc->active_top + aligned) > gc->active_end, 0)) {
        gc_bump_slow(c, aligned);
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
    if (__builtin_expect(ARO_GC_COMMON(c)->stress
                         || gc->bytes_since_gc + gc->common.external_bytes + payload_size > gc->gc_threshold, 0)) {
        gc_collect_internal(c);
    }
    LargeObj *lo = (LargeObj *)malloc(sizeof(LargeObj) + aligned);
    if (!lo) { fprintf(stderr, "baruby_gc=copy: large OOM (%zu)\n", payload_size); abort(); }
    lo->next = gc->large_head;
    gc->large_head = lo;
    lo->next_gray = NULL;
    void *payload = large_payload(lo);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    h->flags    = 0;
    h->gc_flags = 0;
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
    /* Zero the post-head region so a GC scan immediately after alloc
     * sees no stale heap-pointer bits.  Head was init'd to zero above. */
    memset((char *)payload + sizeof(AroObjectHeader), 0,
           aligned - sizeof(AroObjectHeader));

    ARO_GC_COMMON(c)->stats.total_bytes += payload_size;
    ARO_GC_COMMON(c)->stats.heap_bytes  += payload_size;
    return payload;
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
    if (!new_lo) { perror("baruby_gc=copy: realloc large"); abort(); }
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
static void *
forward_payload(ASTroGC *gc, void *old_payload)
{
    if (!old_payload) return NULL;
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
    *slot = (void *)(VALUE)forward_payload(gc, (void *)v);
}

static void
gc_collect_internal(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    char *from_base = gc->active_base;
    char *from_top_pre = gc->active_top;

    /* R rotation: VISIT_EDGE_VAL macro reads scramble_R_old (= pre-GC R,
     * used to decode incoming slots) and scramble_R (= new R, used to
     * encode outgoing slots).  After the collect finishes, scramble_R is
     * the new sample-visible R; scramble_R_old keeps its value but
     * sample never reads it outside GC. */
    ARO_GC_COMMON(c)->scramble_R_old = ARO_GC_COMMON(c)->scramble_R;
    ARO_GC_COMMON(c)->scramble_R     = scramble_pick_R();
    if (getenv("BARUBY_GC_SCRAMBLE_TRACE")) {
        fprintf(stderr, "[scramble] R_old=%016lx R_new=%016lx\n",
                (unsigned long)ARO_GC_COMMON(c)->scramble_R_old,
                (unsigned long)ARO_GC_COMMON(c)->scramble_R);
    }

    /* Determine the to-space. */
    char *next_to_base;
    if (ARO_GC_COMMON(c)->stress) {
        next_to_base = mmap_region(gc->region_bytes);
    } else {
        next_to_base = (gc->active_idx == 0) ? gc->space1 : gc->space0;
    }

    gc->to_base = next_to_base;
    gc->to_top  = next_to_base;
    gc->from_base_cur = from_base;

    ARO_GC_COMMON(c)->stats.heap_bytes = 0;

    /* (1) Root scan via sample-provided AROH_VISIT_ROOTS.  Root slots
     * are VALUE-typed (= scrambled in this backend), so the macro routes
     * through VISIT_EDGE_VAL which decodes with scramble_R_old, calls
     * forward_edge with the raw addr, then re-encodes with the new
     * scramble_R before writing back. */
    struct timespec tcheney = aro_gc_phase_begin();
    AROH_VISIT_ROOTS(c, gc, forward_edge);

    /* (2a) Cheney scan-loop in to-space.  Hot loop, run unconditionally.
     * SCAN category calls sample's SCAN_EDGES which dispatches via
     * ObjectHeader.type (OBJ_ARRAY / OBJ_STRING / OBJ_VALUE_ARRAY).
     * BYTE / FREE skip.  forward_edge uses AROH_IS_GC_OBJECT to filter values. */
    char *scan = gc->to_base;
    while (scan < gc->to_top) {
        AroObjectHeader *h = (AroObjectHeader *)scan;
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

    /* Finalize pass: after mark/forward, before sweep large + swap. */
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

    /* (4) Swap active. */
    if (!ARO_GC_COMMON(c)->stress) {
        gc->active_idx = 1 - gc->active_idx;
    }
    gc->active_base = next_to_base;
    gc->active_top  = gc->to_top;
    gc->active_end  = next_to_base + gc->region_bytes;

    /* (5) Retire the old active.  Stress mode unmaps it outright. */
    if (ARO_GC_COMMON(c)->stress) {
        if (munmap(from_base, gc->region_bytes) != 0) {
            perror("gc_collect: munmap retired"); abort();
        }
    }
    (void)from_top_pre;

    gc->bytes_since_gc = 0;
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

/* Same shape as gc_copy.c finalize_check.  Scramble is GC-wise identical
 * to Cheney copy (the XOR mask applies to VALUE storage only; the
 * registered payload pointer is raw). */
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
    if (ARO_GC_COMMON(c)->stress) {
        /* stress mode: only the current active region is mapped. */
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
