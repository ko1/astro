// gc_immix_gen.c — backend #13: generational Immix (nursery + Immix tenured).
//
// Composition:
//   - Nursery: 16 MiB bump region (same shape as gc_copy_gen / mark_compact_gen)
//   - Tenured: 512 MiB Immix arena (block 32 KiB × line 128 B mark-region)
//
// Minor GC: copy of nursery survivors into tenured holes via the Immix
// bump-allocator (hole_alloc_header).  Forwarding: HDR_FORWARDED bit +
// payload[0..8] overlay stores fwd pointer (payload is dead-from-source
// after forwarding; nursery is reset post-minor).
//
// Major GC: regular Immix mark + line-mark sweep over tenured.  Folds
// nursery via a leading minor first.
//
// Write barrier: explicit remset.  Old → young writes set dirty + push
// old header to remset; minor processes remset to forward young refs.
//
// v1: no evacuation in major (mark-region only).  Same fragmentation
// limitation as gc_immix.c — v2 may add opportunistic evacuation.

#define _GNU_SOURCE      /* mremap; must precede stdio.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define LINE_BYTES       128u
#define BLOCK_BYTES      (32u * 1024u)
#define LINES_PER_BLOCK  (BLOCK_BYTES / LINE_BYTES)   /* 256 */
#define MEDIUM_MAX       (BLOCK_BYTES / 2u)
#define ARENA_BYTES      ARO_GC_REGION_VIRT_BYTES      /* 64 GiB virtual, lazy-paged */
#define N_BLOCKS         (ARENA_BYTES / BLOCK_BYTES)
#define NURSERY_BYTES    ((size_t)16u  << 20)          /* 16 MiB (tuning knob, not program limit) */
#define ALIGN8(n)        (((n) + 7u) & ~(size_t)7u)

/* iter 75 Step C+: fwd overlay (8 B head).  mark_epoch を gc_flags の
 * low 8 bits に詰める。 OLD / DIRTY / FORWARDED は high bits。 */
_Static_assert(sizeof(AroObjectHeader) == 8, "head must be 8 bytes");

#define H_OLD            (uint16_t)0x0100u   /* gc_flags bit 8 */
#define H_DIRTY          (uint16_t)0x0200u   /* gc_flags bit 9 */
#define HDR_FORWARDED    (uint16_t)0x0400u   /* gc_flags bit 10 — nursery→tenured */
#define HDR_EPOCH(h)       ((uint8_t)((h)->gc_flags & 0x00ffu))
#define HDR_SET_EPOCH(h,e) ((h)->gc_flags = (uint16_t)(((h)->gc_flags & 0xff00u) | ((e) & 0xffu)))
#define HDR_IS_FORWARDED(h)  (((h)->gc_flags & HDR_FORWARDED) != 0)
#define HDR_SET_FORWARDED(h) ((h)->gc_flags |= HDR_FORWARDED)

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

enum { BLK_FREE = 0, BLK_RECYCLABLE = 1, BLK_USED = 2 };

typedef struct BlockMeta {
    uint8_t state;
    uint8_t line_marks[LINES_PER_BLOCK];
} BlockMeta;

typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    /* payload follows */
} LargeObj;

static inline void *
large_payload(LargeObj *lo)
{
    return (void *)(lo + 1);
}

#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    uint8_t cur_epoch;
    char       *arena_base;
    BlockMeta  *blocks;
    size_t      block_cursor, line_cursor;
    char       *cur_ptr, *cur_end;
    size_t      max_touched_block;
    LargeObj   *large_head;
    char *nursery_base, *nursery_top, *nursery_end;
    struct AroObjectHeader **remset_buf;
    size_t            remset_cnt, remset_capa;
    /* iter 35 fairness fix: counts only promoted tenured bytes, not nursery. */
    size_t old_alloc_since_major;
    size_t major_threshold;
    CTX  *ctx;
    struct AroObjectHeader **gray_buf;
    size_t            gray_cnt, gray_capa;
    bool   remset_pressure;
    bool   in_minor;        /* true while minor_gc is in progress */
} ASTroGC;

#define cur_epoch             (gc->cur_epoch)
#define in_minor              (gc->in_minor)
#define arena_base            (gc->arena_base)
#define blocks                (gc->blocks)
#define block_cursor          (gc->block_cursor)
#define line_cursor           (gc->line_cursor)
#define cur_ptr               (gc->cur_ptr)
#define cur_end               (gc->cur_end)
#define max_touched_block     (gc->max_touched_block)
#define large_head            (gc->large_head)
#define nursery_base          (gc->nursery_base)
#define nursery_top           (gc->nursery_top)
#define nursery_end           (gc->nursery_end)
#define remset_buf            (gc->remset_buf)
#define remset_cnt            (gc->remset_cnt)
#define remset_capa           (gc->remset_capa)
#define old_alloc_since_major (gc->old_alloc_since_major)
#define major_threshold       (gc->major_threshold)
#define gc_ctx                (gc->ctx)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_pressure       (gc->remset_pressure)

const char *aro_gc_backend_name = "immix_gen";

static void minor_gc(CTX *c);
static void major_gc(CTX *c);

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    cur_epoch       = 1;
    major_threshold = MAJOR_THRESHOLD_MIN;

    arena_base = (char *)mmap(NULL, ARENA_BYTES, PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (arena_base == MAP_FAILED) { perror("immix_gen mmap arena"); abort(); }
    /* block_meta lazy-paged same as gc_immix.c. */
    blocks = (BlockMeta *)mmap(NULL, N_BLOCKS * sizeof(BlockMeta),
                               PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (blocks == MAP_FAILED) { perror("immix_gen mmap blocks"); abort(); }
    block_cursor = 0;
    line_cursor  = 0;
    cur_ptr = NULL;
    cur_end = NULL;

    nursery_base = (char *)mmap(NULL, NURSERY_BYTES, PROT_READ|PROT_WRITE,
                                MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (nursery_base == MAP_FAILED) { perror("immix_gen mmap nursery"); abort(); }
    nursery_top = nursery_base;
    nursery_end = nursery_base + NURSERY_BYTES;

    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        major_threshold = 0;
        fprintf(stderr, "[baruby_gc=immix_gen] STRESS mode: collect on every alloc\n");
    }
    if (getenv("BARUBY_GC_PURGE")) ARO_GC_COMMON(c)->purge = true;
}

/* ---------------------------------------------------------------------------
 * Line / hole helpers
 * --------------------------------------------------------------------------- */

static inline size_t
block_of(ASTroGC *gc, const void *p)
{
    return (size_t)(((const char *)p - arena_base) / BLOCK_BYTES);
}

static inline size_t
line_of(ASTroGC *gc, const void *p)
{
    return (size_t)(((const char *)p - arena_base) % BLOCK_BYTES) / LINE_BYTES;
}

static inline bool
in_arena(ASTroGC *gc, const void *p)
{
    return (const char *)p >= arena_base && (const char *)p < arena_base + ARENA_BYTES;
}

static inline bool
in_nursery(ASTroGC *gc, const void *p)
{
    return (const char *)p >= nursery_base && (const char *)p < nursery_end;
}

static inline void
mark_lines_for(ASTroGC *gc, const AroObjectHeader *h)
{
    const char *start = (const char *)h;
    const char *last  = start + ALIGN8(h->gc_size) - 1;
    size_t bi = block_of(gc, start);
    size_t l0 = line_of(gc, start);
    size_t l1 = line_of(gc, last);
    for (size_t l = l0; l <= l1; l++) {
        blocks[bi].line_marks[l] = 1;
    }
}

static bool
find_hole(ASTroGC *gc, size_t n_lines, char **out_start, char **out_end)
{
    if (n_lines < 1) n_lines = 1;
    for (size_t b = block_cursor; b <= max_touched_block && b < N_BLOCKS; b++) {
        if (blocks[b].state == BLK_USED) { line_cursor = 0; continue; }
        const uint8_t *lm = blocks[b].line_marks;
        size_t i = (b == block_cursor) ? line_cursor : 0;
        while (i < LINES_PER_BLOCK) {
            while (i < LINES_PER_BLOCK && lm[i]) i++;
            size_t hole_start = i;
            while (i < LINES_PER_BLOCK && !lm[i]) i++;
            size_t hole_lines = i - hole_start;
            if (hole_lines >= n_lines) {
                char *bbase = arena_base + b * BLOCK_BYTES;
                *out_start = bbase + hole_start * LINE_BYTES;
                *out_end   = bbase + i * LINE_BYTES;
                block_cursor = b;
                line_cursor  = i;
                if (b > max_touched_block) max_touched_block = b;
                return true;
            }
        }
        line_cursor = 0;
    }
    /* No hole in touched blocks: extend by touching the next virtual block. */
    if (max_touched_block + 1 < N_BLOCKS) {
        max_touched_block++;
        block_cursor = max_touched_block;
        char *bbase = arena_base + max_touched_block * BLOCK_BYTES;
        *out_start = bbase;
        *out_end   = bbase + BLOCK_BYTES;
        line_cursor = LINES_PER_BLOCK;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Tenured allocation (used for pretenure + minor-GC promotion)
 * --------------------------------------------------------------------------- */

static void *
large_alloc(ASTroGC *gc, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("immix_gen mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = H_OLD;       /* epoch 0 + tenured */
    h->gc_size  = (uint32_t)payload_size;
    old_alloc_since_major += ALIGN8(payload_size);
    return (void *)h;
}

static AroObjectHeader *hole_alloc_header(ASTroGC *gc, size_t payload_size)
{
    size_t total = ALIGN8(payload_size);
    if (cur_ptr + total <= cur_end) {
        AroObjectHeader *h = (AroObjectHeader *)cur_ptr;
        cur_ptr += total;
        h->flags    = 0;
        h->gc_flags = H_OLD;
        h->gc_size  = (uint32_t)payload_size;
        return h;
    }
    size_t need_lines = (total + LINE_BYTES - 1) / LINE_BYTES;
    char *hs, *he;
    if (find_hole(gc, need_lines, &hs, &he)) {
        cur_ptr = hs;
        cur_end = he;
        AroObjectHeader *h = (AroObjectHeader *)cur_ptr;
        cur_ptr += total;
        h->flags    = 0;
        h->gc_flags = H_OLD;
        h->gc_size  = (uint32_t)payload_size;
        return h;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Nursery allocation
 * --------------------------------------------------------------------------- */

// iter 43: cold-path split for inliner-budget friendliness.
static void __attribute__((noinline, cold))
nursery_collect_slow(CTX *c, size_t total)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    /* major XOR minor (see gc_copy_gen.c rationale) */
    if (old_alloc_since_major > major_threshold
        || gc->common.external_bytes > major_threshold) {
        major_gc(c);
    } else {
        minor_gc(c);
    }
    if (nursery_top + total > nursery_end) {
        major_gc(c);
        if (nursery_top + total > nursery_end) {
            fprintf(stderr, "baruby_gc=immix_gen: OOM nursery (need %zu)\n", total);
            abort();
        }
    }
}

static inline void *
nursery_bump(CTX *c, size_t payload_size, size_t aligned)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t total = aligned;

    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        return large_alloc(gc, payload_size);
    }

    /* external_bytes pressure → major via nursery_collect_slow.
     * See gc_mark_gen.c for matmul livelock rationale. */
    if (__builtin_expect(gc->common.stress
                         || (size_t)(nursery_top - nursery_base) + total > NURSERY_BYTES
                         || gc->common.external_bytes > major_threshold
                         || remset_pressure, 0)) {
        nursery_collect_slow(c, total);
    }
    AroObjectHeader *h = (AroObjectHeader *)nursery_top;
    h->flags    = 0;
    h->gc_flags = 0;            /* fresh nursery slot: not OLD, epoch 0 */
    h->gc_size  = (uint32_t)payload_size;
    nursery_top += total;
    return (void *)h;
}

void *
aro_gc_alloc_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(AroObjectHeader), 0,
           aligned - sizeof(AroObjectHeader));
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte_raw(CTX *c, size_t payload_size)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(c, payload_size, aligned);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    /* Byte payloads skip post-head zero-fill. */
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* ---------------------------------------------------------------------------
 * Write barrier
 * --------------------------------------------------------------------------- */

/* iter 38 v2 remset overflow guard.  When remset approaches cap, set a
 * pressure flag.  The alloc path checks this at every safepoint and
 * drains the remset by forcing a minor before allowing more growth.
 * Hot path stays a single push (no enumeration list, no tenured_objs[]).
 *
 * Soft cap: pressure flag at MAX-1 — leaves room for the LAST WB to
 * still record its entry (so minor finds all dirty olds).  Hard cap at
 * MAX: only reached when many WBs happen between two alloc safepoints
 * (adversarial alloc-less loop).  Aborts with diagnostic.
 *
 * In practice, every Ruby iteration creates at least one young object
 * via the parser-emitted alloc paths, so the soft cap fires far before
 * the hard cap. */
#define MAX_REMSET_ENTRIES (1u << 17)
#define REMSET_PRESSURE_THRESH (MAX_REMSET_ENTRIES - 1u)
/* remset_pressure storage moved to ASTroGC.remset_pressure (aliased above). */

static void
remset_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (__builtin_expect(remset_cnt >= MAX_REMSET_ENTRIES, 0)) {
        fprintf(stderr,
            "baruby_gc=immix_gen: remset hard-cap (%u) hit between alloc "
            "safepoints — alloc-less adversarial workload.\n",
            MAX_REMSET_ENTRIES);
        abort();
    }
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
        if (remset_capa > MAX_REMSET_ENTRIES) remset_capa = MAX_REMSET_ENTRIES;
        remset_buf = (AroObjectHeader **)realloc(remset_buf, remset_capa * sizeof(AroObjectHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
    if (__builtin_expect(remset_cnt >= REMSET_PRESSURE_THRESH, 0)) {
        remset_pressure = true;
    }
}

/* WB body — caller verified holder is old + not-yet-dirty. */
void __attribute__((noinline, cold))
aro_gc_remember(CTX *c, AroObjectHeader *h)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    h->gc_flags |= H_DIRTY;
    remset_push(gc, h);
}

/* ---------------------------------------------------------------------------
 * Mark / forwarding
 * --------------------------------------------------------------------------- */

static void
gray_push(ASTroGC *gc, AroObjectHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (AroObjectHeader **)realloc(gray_buf, gray_capa * sizeof(AroObjectHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static void *
forward_payload_nursery(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    if (!in_nursery(gc, p)) return p;
    AroObjectHeader *oldh = (AroObjectHeader *)p;
    if (HDR_IS_FORWARDED(oldh)) return fwd_overlay_get(oldh);
    size_t size = oldh->gc_size;
    AroObjectHeader *newh = hole_alloc_header(gc, size);
    if (!newh) {
        fprintf(stderr, "baruby_gc=immix_gen: tenured arena OOM during promotion\n");
        abort();
    }
    void *newp = (void *)newh;
    size_t bytes = ALIGN8(size);
    if (bytes) memcpy(newp, p, bytes);
    /* memcpy overwrote newh's head with oldh's — restore framework state. */
    newh->gc_flags = H_OLD;
    HDR_SET_FORWARDED(oldh);
    fwd_overlay_set(oldh, newp);
    old_alloc_since_major += ALIGN8(size);
    gray_push(gc, newh);
    return newp;
}

static void
fwd_edge_minor(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    VALUE v = (VALUE)*slot;
    if (AROH_IS_GC_OBJECT(v)) *slot = (void *)(VALUE)forward_payload_nursery(gc, (void *)v);
}

static void
process_object_minor(ASTroGC *gc, AroObjectHeader *h)
{
    AROH_SCAN_EDGES((void *)h, h->gc_size, gc, fwd_edge_minor);
}

/* ---------------------------------------------------------------------------
 * Minor GC
 * --------------------------------------------------------------------------- */

static void
minor_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);
    in_minor = true;

    AROH_VISIT_ROOTS(c, gc, fwd_edge_minor);

    for (size_t i = 0; i < remset_cnt; i++) {
        AroObjectHeader *const h = remset_buf[i];
        if (h->gc_flags & H_DIRTY) {
            process_object_minor(gc, h);
            h->gc_flags &= (uint16_t)~H_DIRTY;
        }
    }
    remset_cnt = 0;
    remset_pressure = false;

    while (gray_cnt > 0) {
        AroObjectHeader *h = gray_buf[--gray_cnt];
        process_object_minor(gc, h);
    }

    /* Finalize pass: live nursery entries get HDR_FORWARDED + overlay to
     * new tenured addr; dead nursery → NULL; tenured entries untouched
     * (= conservatively live). */
    aro_gc_finalize_walk(c);

    nursery_top = nursery_base;
    in_minor = false;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    aro_gc_time_end(c, t0);
}

/* ---------------------------------------------------------------------------
 * Major GC: full Immix mark + line-mark sweep over tenured.
 * --------------------------------------------------------------------------- */

static void
mark_value_major(ASTroGC *gc, VALUE v)
{
    if (!AROH_IS_GC_OBJECT(v)) return;
    AroObjectHeader *h = (AroObjectHeader *)v;
    if (HDR_EPOCH(h) == cur_epoch) return;
    HDR_SET_EPOCH(h, cur_epoch);
    if (in_arena(gc, h)) mark_lines_for(gc, h);
    gray_push(gc, h);
}

static void
mark_edge_major(void *ctx, void **slot)
{
    mark_value_major((ASTroGC *)ctx, (VALUE)*slot);
}

static void
process_gray_major(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        AroObjectHeader *h = gray_buf[--gray_cnt];
        AROH_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge_major);
    }
}

static void
sweep_major(ASTroGC *gc)
{
    size_t live = 0;
    for (size_t b = 0; b <= max_touched_block; b++) {
        const uint8_t *lm = blocks[b].line_marks;
        size_t marked = 0;
        for (size_t i = 0; i < LINES_PER_BLOCK; i++) marked += lm[i];
        if (marked == 0) {
            blocks[b].state = BLK_FREE;
        } else if (marked == LINES_PER_BLOCK) {
            blocks[b].state = BLK_USED;
        } else {
            blocks[b].state = BLK_RECYCLABLE;
        }
        live += marked * LINE_BYTES;
    }
    gc->common.stats.heap_bytes = live;
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        AroObjectHeader *h = (AroObjectHeader *)large_payload(lo);
        if (HDR_EPOCH(h) == cur_epoch) {
            gc->common.stats.heap_bytes += h->gc_size;
            link = &lo->next;
        } else {
            *link = lo->next;
            munmap(lo, lo->map_bytes);
        }
    }
    block_cursor = 0;
    line_cursor  = 0;
    cur_ptr = NULL;
    cur_end = NULL;
}

static void
major_gc(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    if (nursery_top != nursery_base) {
        minor_gc(c);
    }
    remset_cnt = 0;

    for (size_t b = 0; b <= max_touched_block; b++) {
        memset(blocks[b].line_marks, 0, LINES_PER_BLOCK);
    }

    AROH_VISIT_ROOTS(c, gc, mark_edge_major);
    process_gray_major(gc);

    /* Finalize pass: live = HDR_EPOCH == cur_epoch, dead = anything else.
     * Must run BEFORE the cur_epoch bump below. */
    aro_gc_finalize_walk(c);

    sweep_major(gc);

    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    cur_epoch = (cur_epoch == 255) ? 1 : (uint8_t)(cur_epoch + 1);
    old_alloc_since_major = 0;
    if (!gc->common.stress) {
        size_t next = gc->common.stats.heap_bytes * MAJOR_THRESHOLD_FACTOR;
        major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    major_gc(c);
}


/* Liveness for finalizable entry:
 *   HDR_FORWARDED on entry → live (promoted nursery), return overlay.
 *   in_minor:
 *     in_nursery without FWD → dead.
 *     else (= tenured) → live (not touched by minor).
 *   else (major):
 *     HDR_EPOCH == cur_epoch → live; else dead. */
void *
aro_gc_finalize_check(CTX *c, void *payload)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    AroObjectHeader *h = (AroObjectHeader *)payload;
    if (HDR_IS_FORWARDED(h)) return fwd_overlay_get(h);
    if (in_minor) {
        return in_nursery(gc, payload) ? NULL : payload;
    }
    return HDR_EPOCH(h) == cur_epoch ? payload : NULL;
}

void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ARO_GC_INSTANCE(c);
    if (!gc) return;
    aro_gc_finalize_fini(c);
    if (arena_base)   munmap(arena_base, ARENA_BYTES);
    if (blocks)       munmap(blocks, N_BLOCKS * sizeof(BlockMeta));
    if (nursery_base) munmap(nursery_base, NURSERY_BYTES);
    aro_gc_free_large_chain_mmap(large_head);
    free(gray_buf);
    free(remset_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    AroObjectHeader *h = (AroObjectHeader *)p;
    return h->gc_size;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (ALIGN8(n) <= MEDIUM_MAX)
#define ARO_GC_INPLACE_PAGE_SIZE         sysconf(_SC_PAGESIZE)
#define ARO_GC_INPLACE_MREMAP_FLAGS      0
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (old_alloc_since_major += (d))
#undef large_head
#include "gc_inplace_mremap.h"
#define large_head (gc->large_head)
