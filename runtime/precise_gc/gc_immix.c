// gc_immix.c — backend #12: Immix mark-region (non-moving v1, no evacuation).
//
// Heap is a contiguous mmap'd arena, partitioned into 32 KiB BLOCKS, each
// further divided into 128 B LINES (= 256 lines per block).  Allocation
// bump-allocates within a "hole" — a contiguous run of unmarked lines.
// When the current hole exhausts, find the next hole in the same block;
// when the block exhausts, advance to the next block.  GC trigger when
// no block has a usable hole for the requested size.
//
// Object size categories:
//   - small:  total slot (header + aligned payload) ≤ LINE_BYTES (128 B).
//             Fits within a single line.
//   - medium: total > LINE_BYTES but ≤ BLOCK_BYTES / 2.  Spans multiple
//             lines.  Requires a hole of sufficient line count.
//   - large:  total > BLOCK_BYTES / 2.  Bypass arena entirely, mmap one
//             region per object (LargeObj list, same as gc_mark.c).
//
// Mark phase: standard precise mark from roots via gray queue.  On
// marking an object, also set the line_marks bits for every line the
// object spans.  Conservative line mark: mark the line containing the
// header and the line containing the last payload byte, plus all lines
// in between.
//
// Sweep phase: per-block, examine line_marks.  Unmarked lines = free,
// marked lines = retained.  No object movement.  Holes are recomputed
// lazily by the allocator on its next pass (no separate free-list).
//
// Block states (informational, used as a hint for the alloc scanner):
//   FREE        — no marks (whole block is one big hole)
//   RECYCLABLE  — has at least one free line and at least one marked line
//   USED        — all lines marked (no holes; skip during allocation)
//
// Note: line-mark conservatism (marking trailing line as full when only
// partial) is a known Immix property; future v2 could implement
// "conservative-line-mark + per-line line-spans" to recover those bytes.
// v1 prioritizes correctness + simple implementation.

#define _GNU_SOURCE      /* mremap, MREMAP_MAYMOVE */
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
#define MEDIUM_MAX       (BLOCK_BYTES / 2u)            /* > this → large */
#define ARENA_BYTES      ARO_GC_REGION_VIRT_BYTES      /* 64 GiB virtual, lazy-paged */
#define N_BLOCKS         (ARENA_BYTES / BLOCK_BYTES)   /* virtual block count */
#define ALIGN8(n)        (((n) + 7u) & ~(size_t)7u)

/* mark_epoch: 0 = never marked; otherwise the GC cycle counter value when
 * this object was last marked.  We compare against cur_epoch in mark_value
 * to know "already marked this cycle".  Avoids the need to walk the heap
 * at sweep end to clear stale marked bits — incrementing cur_epoch
 * implicitly invalidates all prior marks. */
/* iter 75 Step C: framework GCHeader 廃止、 ASTroObjectHeader at offset 0。
 * mark_epoch を gc_flags low 8 bits に詰める (8 epoch beats wrap-handling
 * vs 16, plenty for typical workloads). */
_Static_assert(sizeof(ASTroObjectHeader) == 8, "non-moving GC: head must be 8 B");

#define HDR_EPOCH(h)       ((uint8_t)((h)->gc_flags & 0x00ffu))
#define HDR_SET_EPOCH(h,e) ((h)->gc_flags = (uint16_t)(((h)->gc_flags & 0xff00u) | ((e) & 0xffu)))

enum { BLK_FREE = 0, BLK_RECYCLABLE = 1, BLK_USED = 2 };

typedef struct BlockMeta {
    uint8_t state;
    uint8_t line_marks[LINES_PER_BLOCK];   /* 256 bytes (bool-as-byte for speed) */
} BlockMeta;

/* Large objects (> MEDIUM_MAX): own mmap region each.  Same layout as gc_mark.c. */
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

/* Adaptive threshold: same heuristic as gc_mark.c.  Starts at 16 MiB. */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2

// ----------------------------------------------------------------------------
// ASTroGC: process-scope GC instance.  See docs/gc_design.md §3.
// ----------------------------------------------------------------------------
typedef struct ASTroGC {
    AroGcCommonState common;   /* MUST be first field */
    uint8_t cur_epoch;            /* skips 0 so fresh allocs (mark_epoch=0) are "unmarked" */
    char       *arena_base;
    BlockMeta  *blocks;
    size_t      block_cursor;
    size_t      line_cursor;
    char       *cur_ptr;
    char       *cur_end;
    size_t      max_touched_block;
    LargeObj   *large_head;
    size_t      bytes_since_gc;
    size_t      gc_threshold;
    CTX        *ctx;
    struct ASTroObjectHeader **gray_buf;
    size_t            gray_cnt;
    size_t            gray_capa;
} ASTroGC;

#define cur_epoch         (gc->cur_epoch)
#define arena_base        (gc->arena_base)
#define blocks            (gc->blocks)
#define block_cursor      (gc->block_cursor)
#define line_cursor       (gc->line_cursor)
#define cur_ptr           (gc->cur_ptr)
#define cur_end           (gc->cur_end)
#define max_touched_block (gc->max_touched_block)
#define large_head        (gc->large_head)
#define bytes_since_gc    (gc->bytes_since_gc)
#define gc_threshold      (gc->gc_threshold)
#define gc_ctx            (gc->ctx)
#define gray_buf          (gc->gray_buf)
#define gray_cnt          (gc->gray_cnt)
#define gray_capa         (gc->gray_capa)

const char *aro_gc_backend_name = "immix";

static void gc_collect_internal(CTX *c, VALUE *sp_top);

void
aro_gc_init(CTX *c)
{
    ASTroGC *gc = (ASTroGC *)calloc(1, sizeof(ASTroGC));
    if (!gc) { perror("calloc ASTroGC"); abort(); }
    c->astro_gc = gc;
    gc_ctx = c;
    cur_epoch    = 1;
    gc_threshold = GC_THRESHOLD_MIN;

    arena_base = (char *)mmap(NULL, ARENA_BYTES, PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (arena_base == MAP_FAILED) { perror("immix mmap arena"); abort(); }
    blocks = (BlockMeta *)mmap(NULL, N_BLOCKS * sizeof(BlockMeta),
                               PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (blocks == MAP_FAILED) { perror("immix mmap blocks"); abort(); }
    block_cursor = 0;
    line_cursor  = 0;
    cur_ptr = NULL;
    cur_end = NULL;
    if (getenv("BARUBY_GC_STRESS")) {
        gc->common.stress = true;
        gc_threshold = 0;
        fprintf(stderr, "[baruby_gc=immix] STRESS mode: collect on every alloc\n");
    }
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

/* Mark all lines that an object spans (from its payload pointer + size). */
static inline void
mark_lines_for(ASTroGC *gc, const ASTroObjectHeader *h)
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
    /* Scan up to max_touched_block+1, with the last block treated as a
     * fresh (all-free) block — letting us extend the heap one block at a
     * time without ever scanning the full virtual N_BLOCKS. */
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
    /* No hole found in touched blocks: extend by touching the next block. */
    if (max_touched_block + 1 < N_BLOCKS) {
        max_touched_block++;
        block_cursor = max_touched_block;
        line_cursor  = 0;
        char *bbase = arena_base + max_touched_block * BLOCK_BYTES;
        *out_start = bbase;
        *out_end   = bbase + BLOCK_BYTES;
        line_cursor = LINES_PER_BLOCK;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Allocation
 * --------------------------------------------------------------------------- */

static void *
large_alloc(ASTroGC *gc, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("immix mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
    h->flags    = 0;
    h->gc_flags = 0;
    h->gc_size  = (uint32_t)payload_size;
    return (void *)h;
}

static void *
hole_alloc(ASTroGC *gc, size_t payload_size)
{
    size_t total = ALIGN8(payload_size);
    if (cur_ptr + total <= cur_end) {
        ASTroObjectHeader *h = (ASTroObjectHeader *)cur_ptr;
        cur_ptr += total;
        h->flags    = 0;
        h->gc_flags = 0;
        h->gc_size  = (uint32_t)payload_size;
        return (void *)h;
    }
    size_t need_lines = (total + LINE_BYTES - 1) / LINE_BYTES;
    char *hs, *he;
    if (find_hole(gc, need_lines, &hs, &he)) {
        cur_ptr = hs;
        cur_end = he;
        ASTroObjectHeader *h = (ASTroObjectHeader *)cur_ptr;
        cur_ptr += total;
        h->flags    = 0;
        h->gc_flags = 0;
        h->gc_size  = (uint32_t)payload_size;
        return (void *)h;
    }
    return NULL;
}

static void * __attribute__((noinline, cold))
hole_alloc_slow(CTX *c, size_t payload_size, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    gc_collect_internal(c, sp_top);
    void *payload = hole_alloc(gc, payload_size);
    if (!payload) {
        fprintf(stderr, "baruby_gc=immix: OOM (need %zu)\n",
                ALIGN8(payload_size));
        abort();
    }
    return payload;
}

void *
aro_gc_alloc(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + payload_size > gc_threshold, 0)) {
        gc_collect_internal(c, sp_top);
    }
    size_t total = ALIGN8(payload_size);
    void *payload;
    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        payload = large_alloc(gc, payload_size);
    } else {
        payload = hole_alloc(gc, payload_size);
        if (__builtin_expect(!payload, 0)) {
            payload = hole_alloc_slow(c, payload_size, sp_top);
        }
    }
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset((char *)payload + sizeof(ASTroObjectHeader), 0,
           ALIGN8(payload_size) - sizeof(ASTroObjectHeader));
    bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + payload_size > gc_threshold, 0)) {
        gc_collect_internal(c, sp_top);
    }
    size_t total = ALIGN8(payload_size);
    void *payload;
    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        payload = large_alloc(gc, payload_size);
    } else {
        payload = hole_alloc(gc, payload_size);
        if (__builtin_expect(!payload, 0)) {
            payload = hole_alloc_slow(c, payload_size, sp_top);
        }
    }
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

/* ---------------------------------------------------------------------------
 * Mark phase
 * --------------------------------------------------------------------------- */

static void
gray_push(ASTroGC *gc, ASTroObjectHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (ASTroObjectHeader **)realloc(gray_buf, gray_capa * sizeof(ASTroObjectHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

static inline bool
in_arena(ASTroGC *gc, const void *p)
{
    return (const char *)p >= arena_base && (const char *)p < arena_base + ARENA_BYTES;
}

static void
mark_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    ASTroObjectHeader *h = (ASTroObjectHeader *)v;
    if (h->gc_flags == cur_epoch) return;
    h->gc_flags = cur_epoch;
    if (in_arena(gc, h)) mark_lines_for(gc, h);
    gray_push(gc, h);
}

static void
mark_edge_immix(void *ctx, void **slot)
{
    ASTroGC *gc = (ASTroGC *)ctx;
    mark_value(gc, (VALUE)*slot);
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        ASTroObjectHeader *h = gray_buf[--gray_cnt];
        ASTRO_GC_SCAN_EDGES((void *)h, h->gc_size, gc, mark_edge_immix);
    }
}

/* ---------------------------------------------------------------------------
 * Sweep phase: classify blocks, free unmarked large objs.  In-arena lines
 * are reclaimed implicitly by the next find_hole pass (line_marks already
 * holds the survivor map from mark_lines_for).
 * --------------------------------------------------------------------------- */

static void
sweep(ASTroGC *gc)
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
        ASTroObjectHeader *h = (ASTroObjectHeader *)large_payload(lo);
        if (h->gc_flags == cur_epoch) {
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
gc_collect_internal(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    for (size_t b = 0; b <= max_touched_block; b++) {
        memset(blocks[b].line_marks, 0, LINES_PER_BLOCK);
    }

    aro_gc_visit_roots(c, gc, mark_edge_immix);
    process_gray(gc);

    sweep(gc);

    cur_epoch = (cur_epoch == 255) ? 1 : (uint8_t)(cur_epoch + 1);

    gc->common.stats.gc_count++;
    bytes_since_gc = 0;
    if (!gc->common.stress) {
        size_t live = gc->common.stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    gc_collect_internal(c, sp_top);
}


void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    if (arena_base) munmap(arena_base, ARENA_BYTES);
    if (blocks)     munmap(blocks, N_BLOCKS * sizeof(BlockMeta));
    aro_gc_free_large_chain_mmap(large_head);
    free(gray_buf);
    free(gc);
    c->astro_gc = NULL;
}

size_t
aro_gc_size_of(void *p)
{
    ASTroObjectHeader *h = (ASTroObjectHeader *)p;
    return h->gc_size;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define ARO_GC_INPLACE_THRESHOLD(n)      (ALIGN8(n) <= MEDIUM_MAX)
#define ARO_GC_INPLACE_PAGE_SIZE         sysconf(_SC_PAGESIZE)
#define ARO_GC_INPLACE_MREMAP_FLAGS      MREMAP_MAYMOVE
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (bytes_since_gc += (d))
#undef large_head
#include "gc_inplace_mremap.h"
#define large_head (gc->large_head)
