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
/* 8-byte header (down from 16).  kind packed to 3 bits of flags.
 * Layout: flags(1) + mark_epoch(1) + _pad(2) + size(4) = 8 B. */
typedef struct GCHeader {
    uint8_t  flags;        /* bits 0-2: kind */
    uint8_t  mark_epoch;
    uint8_t  _pad[2];
    uint32_t size;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))

enum { BLK_FREE = 0, BLK_RECYCLABLE = 1, BLK_USED = 2 };

typedef struct BlockMeta {
    uint8_t state;
    uint8_t line_marks[LINES_PER_BLOCK];   /* 256 bytes (bool-as-byte for speed) */
} BlockMeta;

/* Large objects (> MEDIUM_MAX): own mmap region each.  Same layout as gc_mark.c. */
typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    /* GCHeader follows */
} LargeObj;

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
    struct GCHeader **gray_buf;
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
mark_lines_for(ASTroGC *gc, const GCHeader *h)
{
    const char *start = (const char *)h;
    const char *last  = start + sizeof(GCHeader) + ALIGN8(h->size) - 1;
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
large_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("immix mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->mark_epoch = 0;
    return (void *)(h + 1);
}

static void *
hole_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size)
{
    size_t total = sizeof(GCHeader) + ALIGN8(payload_size);
    if (cur_ptr + total <= cur_end) {
        GCHeader *h = (GCHeader *)cur_ptr;
        cur_ptr += total;
        HDR_SET_KIND(h, kind);
        h->size   = (uint32_t)payload_size;
        h->mark_epoch = 0;
        return (void *)(h + 1);
    }
    size_t need_lines = (total + LINE_BYTES - 1) / LINE_BYTES;
    char *hs, *he;
    if (find_hole(gc, need_lines, &hs, &he)) {
        cur_ptr = hs;
        cur_end = he;
        GCHeader *h = (GCHeader *)cur_ptr;
        cur_ptr += total;
        HDR_SET_KIND(h, kind);
        h->size   = (uint32_t)payload_size;
        h->mark_epoch = 0;
        return h + 1;
    }
    return NULL;
}

static void * __attribute__((noinline, cold))
hole_alloc_slow(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    gc_collect_internal(c, sp_top);
    void *payload = hole_alloc(gc, kind, payload_size);
    if (!payload) {
        fprintf(stderr, "baruby_gc=immix: OOM (need %zu)\n",
                sizeof(GCHeader) + ALIGN8(payload_size));
        abort();
    }
    return payload;
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + payload_size > gc_threshold, 0)) {
        gc_collect_internal(c, sp_top);
    }
    size_t total = sizeof(GCHeader) + ALIGN8(payload_size);
    void *payload;
    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        payload = large_alloc(gc, kind, payload_size);
    } else {
        payload = hole_alloc(gc, kind, payload_size);
        if (__builtin_expect(!payload, 0)) {
            payload = hole_alloc_slow(c, kind, payload_size, sp_top);
        }
    }
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (__builtin_expect(gc->common.stress || bytes_since_gc + payload_size > gc_threshold, 0)) {
        gc_collect_internal(c, sp_top);
    }
    size_t total = sizeof(GCHeader) + ALIGN8(payload_size);
    void *payload;
    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        payload = large_alloc(gc, KIND_PAYLOAD_BYTE, payload_size);
    } else {
        payload = hole_alloc(gc, KIND_PAYLOAD_BYTE, payload_size);
        if (__builtin_expect(!payload, 0)) {
            payload = hole_alloc_slow(c, KIND_PAYLOAD_BYTE, payload_size, sp_top);
        }
    }
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    bytes_since_gc += payload_size;
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(CTX *c, void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(c, KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = HDR_KIND(oldh);
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    /* Root old via sp_top[0] for uniformity (non-moving, but match other
     * backends so node.c can use the same pattern). */
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size, sp_top + 1)
        : aro_gc_alloc(c, kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

/* ---------------------------------------------------------------------------
 * Mark phase
 * --------------------------------------------------------------------------- */

static void
gray_push(ASTroGC *gc, GCHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (GCHeader **)realloc(gray_buf, gray_capa * sizeof(GCHeader *));
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
    GCHeader *h = (GCHeader *)v - 1;
    if (h->mark_epoch == cur_epoch) return;
    h->mark_epoch = cur_epoch;
    if (in_arena(gc, h)) mark_lines_for(gc, h);
    gray_push(gc, h);
}

static void
process_gray(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        void *payload = (void *)(h + 1);
        switch (HDR_KIND(h)) {
          case KIND_OBJ_ARRAY: {
            BaArray *a = (BaArray *)payload;
            if (a->items) mark_value(gc, (VALUE)a->items);
            break;
          }
          case KIND_OBJ_STRING: {
            BaString *s = (BaString *)payload;
            if (!BSTR_IS_SSO(s) && s->bytes) mark_value(gc, (VALUE)s->bytes);
            break;
          }
          case KIND_PAYLOAD_VAL: {
            VALUE *items = (VALUE *)payload;
            size_t n = h->size / sizeof(VALUE);
            for (size_t i = 0; i < n; i++) mark_value(gc, items[i]);
            break;
          }
          case KIND_PAYLOAD_BYTE:
          case KIND_FREE:
            break;
          default:
            ASTRO_ASSERT(0 && "immix process_gray: unknown kind");
        }
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
        GCHeader *h = (GCHeader *)(lo + 1);
        if (h->mark_epoch == cur_epoch) {
            gc->common.stats.heap_bytes += h->size;
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

    for (VALUE *p = c->env; p < sp_top; p++) mark_value(gc, *p);
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
aro_gc_collect(CTX *c, VALUE *sp_top)
{
    gc_collect_internal(c, sp_top);
}

