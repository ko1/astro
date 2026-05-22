// gc_immix_gen.c — backend #13: generational Immix (nursery + Immix tenured).
//
// Composition:
//   - Nursery: 16 MiB bump region (same shape as gc_copy_gen / mark_compact_gen)
//   - Tenured: 512 MiB Immix arena (block 32 KiB × line 128 B mark-region)
//
// Minor GC: copy of nursery survivors into tenured holes via the Immix
// bump-allocator (hole_alloc_header).  Forwarding: KIND_FREE + payload[0..8]
// stores fwd pointer (payload is dead-from-source after forwarding).
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

/* 8-byte header (down from 16).  kind + old + dirty packed in flags.
 * Layout: flags(1) + mark_epoch(1) + _pad(2) + size(4) = 8 B. */
typedef struct GCHeader {
    uint8_t  flags;         /* bits 0-2: kind, bit 3: OLD (tenured), bit 4: DIRTY (remset) */
    uint8_t  mark_epoch;
    uint8_t  _pad[2];
    uint32_t size;
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 8, "GCHeader must be 8 bytes");

#define HDR_KIND_MASK    0x07u
#define H_OLD            0x08u  /* bit 3 */
#define H_DIRTY          0x10u  /* bit 4 */
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))

enum { BLK_FREE = 0, BLK_RECYCLABLE = 1, BLK_USED = 2 };

typedef struct BlockMeta {
    uint8_t state;
    uint8_t line_marks[LINES_PER_BLOCK];
} BlockMeta;

typedef struct LargeObj {
    struct LargeObj *next;
    size_t           map_bytes;
    /* GCHeader follows */
} LargeObj;

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
    struct GCHeader **remset_buf;
    size_t            remset_cnt, remset_capa;
    /* iter 35 fairness fix: counts only promoted tenured bytes, not nursery. */
    size_t old_alloc_since_major;
    size_t major_threshold;
    CTX  *ctx;
    VALUE *sp_high_water;
    struct GCHeader **gray_buf;
    size_t            gray_cnt, gray_capa;
    bool   remset_pressure;
} ASTroGC;

#define cur_epoch             (gc->cur_epoch)
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
#define sp_high_water         (gc->sp_high_water)
#define gray_buf              (gc->gray_buf)
#define gray_cnt              (gc->gray_cnt)
#define gray_capa             (gc->gray_capa)
#define remset_pressure       (gc->remset_pressure)

const char *aro_gc_backend_name = "immix_gen";

static void minor_gc(CTX *c, VALUE *sp_top);
static void major_gc(CTX *c, VALUE *sp_top);

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
large_alloc(ASTroGC *gc, AroGcKind kind, size_t payload_size)
{
    size_t need = sizeof(LargeObj) + sizeof(GCHeader) + ALIGN8(payload_size);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t map_bytes = (need + page - 1) & ~(page - 1);
    void *raw = mmap(NULL, map_bytes, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { perror("immix_gen mmap large"); abort(); }
    LargeObj *lo = (LargeObj *)raw;
    lo->next = large_head;
    lo->map_bytes = map_bytes;
    large_head = lo;
    GCHeader *h = (GCHeader *)(lo + 1);
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->mark_epoch = 0;
    h->flags = (uint8_t)(((uint8_t)kind & HDR_KIND_MASK) | H_OLD);
    old_alloc_since_major += sizeof(GCHeader) + ALIGN8(payload_size);
    return (void *)(h + 1);
}

static GCHeader *
hole_alloc_header(ASTroGC *gc, AroGcKind kind, size_t payload_size)
{
    size_t total = sizeof(GCHeader) + ALIGN8(payload_size);
    if (cur_ptr + total <= cur_end) {
        GCHeader *h = (GCHeader *)cur_ptr;
        cur_ptr += total;
        HDR_SET_KIND(h, kind);
        h->size = (uint32_t)payload_size;
        h->mark_epoch = 0;
        h->flags = (uint8_t)(((uint8_t)kind & HDR_KIND_MASK) | H_OLD);
        return h;
    }
    size_t need_lines = (total + LINE_BYTES - 1) / LINE_BYTES;
    char *hs, *he;
    if (find_hole(gc, need_lines, &hs, &he)) {
        cur_ptr = hs;
        cur_end = he;
        GCHeader *h = (GCHeader *)cur_ptr;
        cur_ptr += total;
        HDR_SET_KIND(h, kind);
        h->size = (uint32_t)payload_size;
        h->mark_epoch = 0;
        h->flags = (uint8_t)(((uint8_t)kind & HDR_KIND_MASK) | H_OLD);
        return h;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Nursery allocation
 * --------------------------------------------------------------------------- */

// iter 43: cold-path split for inliner-budget friendliness.
static void __attribute__((noinline, cold))
nursery_collect_slow(CTX *c, size_t total, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    minor_gc(c, sp_top);
    if (old_alloc_since_major > major_threshold) {
        major_gc(c, sp_top);
    }
    if (nursery_top + total > nursery_end) {
        major_gc(c, sp_top);
        if (nursery_top + total > nursery_end) {
            fprintf(stderr, "baruby_gc=immix_gen: OOM nursery (need %zu)\n", total);
            abort();
        }
    }
}

static inline void *
nursery_bump(CTX *c, AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t total = sizeof(GCHeader) + aligned;

    if (__builtin_expect(total > MEDIUM_MAX, 0)) {
        return large_alloc(gc, kind, payload_size);
    }

    if (__builtin_expect(gc->common.stress || nursery_top + total > nursery_end || remset_pressure, 0)) {
        nursery_collect_slow(c, total, sp_top);
    }
    GCHeader *h = (GCHeader *)nursery_top;
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->mark_epoch = 0;
    h->flags = (uint8_t)((uint8_t)kind & HDR_KIND_MASK);
    nursery_top += total;
    return (void *)(h + 1);
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(c, kind, payload_size, aligned, sp_top);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);
    gc->common.stats.total_bytes += payload_size;
    gc->common.stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size)
{
    VALUE *sp_top = c->sp;
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(c, KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
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
remset_push(ASTroGC *gc, GCHeader *h)
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
        remset_buf = (GCHeader **)realloc(remset_buf, remset_capa * sizeof(GCHeader *));
        if (!remset_buf) abort();
    }
    remset_buf[remset_cnt++] = h;
    if (__builtin_expect(remset_cnt >= REMSET_PRESSURE_THRESH, 0)) {
        remset_pressure = true;
    }
}

void
aro_gc_wb(CTX *c, void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if ((hh->flags & H_OLD) && !(hh->flags & H_DIRTY)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        hh->flags |= H_DIRTY;
        remset_push(gc, hh);
    }
}

void
aro_gc_wb_bulk(CTX *c, void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if ((hh->flags & H_OLD) && !(hh->flags & H_DIRTY)) {
        ASTroGC *gc = ASTRO_GC_INSTANCE(c);
        hh->flags |= H_DIRTY;
        remset_push(gc, hh);
    }
}

/* ---------------------------------------------------------------------------
 * Mark / forwarding
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

static void *
forward_payload_nursery(ASTroGC *gc, void *p)
{
    if (!p) return NULL;
    if (!in_nursery(gc, p)) return p;
    GCHeader *oldh = (GCHeader *)p - 1;
    if (HDR_KIND(oldh) == KIND_FREE) {
        return *(void **)p;
    }
    AroGcKind kind = HDR_KIND(oldh);
    size_t size = oldh->size;
    GCHeader *newh = hole_alloc_header(gc, kind, size);
    if (!newh) {
        fprintf(stderr, "baruby_gc=immix_gen: tenured arena OOM during promotion\n");
        abort();
    }
    void *newp = (void *)(newh + 1);
    size_t bytes = ALIGN8(size);
    if (bytes) memcpy(newp, p, bytes);
    HDR_SET_KIND(oldh, KIND_FREE);
    *(void **)p = newp;
    old_alloc_since_major += sizeof(GCHeader) + ALIGN8(size);
    gray_push(gc, newh);
    return newp;
}

static VALUE
fwd_value(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_nursery(gc, (void *)v);
}

static void
process_object_minor(ASTroGC *gc, GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload_nursery(gc, a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (!BSTR_IS_SSO(s) && s->bytes) s->bytes = (char *)forward_payload_nursery(gc, s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = fwd_value(gc, items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "immix_gen process_object_minor: unknown kind");
    }
}

/* ---------------------------------------------------------------------------
 * Minor GC
 * --------------------------------------------------------------------------- */

static void
minor_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    for (VALUE *p = c->env; p < sp_top; p++) *p = fwd_value(gc, *p);

    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *const h = remset_buf[i];
        if (h->flags & H_DIRTY) {
            process_object_minor(gc, h);
            h->flags &= (uint8_t)~H_DIRTY;
        }
    }
    remset_cnt = 0;
    remset_pressure = false;

    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        process_object_minor(gc, h);
    }

    nursery_top = nursery_base;

    gc->common.stats.gc_count++;
    gc->common.stats.minor_count++;
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

/* ---------------------------------------------------------------------------
 * Major GC: full Immix mark + line-mark sweep over tenured.
 * --------------------------------------------------------------------------- */

static void
mark_value_major(ASTroGC *gc, VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (h->mark_epoch == cur_epoch) return;
    h->mark_epoch = cur_epoch;
    if (in_arena(gc, h)) mark_lines_for(gc, h);
    gray_push(gc, h);
}

static void
process_gray_major(ASTroGC *gc)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        void *payload = (void *)(h + 1);
        switch (HDR_KIND(h)) {
          case KIND_OBJ_ARRAY: {
            BaArray *a = (BaArray *)payload;
            if (a->items) mark_value_major(gc, (VALUE)a->items);
            break;
          }
          case KIND_OBJ_STRING: {
            BaString *s = (BaString *)payload;
            if (!BSTR_IS_SSO(s) && s->bytes) mark_value_major(gc, (VALUE)s->bytes);
            break;
          }
          case KIND_PAYLOAD_VAL: {
            VALUE *items = (VALUE *)payload;
            size_t n = h->size / sizeof(VALUE);
            for (size_t i = 0; i < n; i++) mark_value_major(gc, items[i]);
            break;
          }
          case KIND_PAYLOAD_BYTE:
          case KIND_FREE:
            break;
          default:
            ASTRO_ASSERT(0 && "immix_gen process_gray_major: unknown kind");
        }
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
major_gc(CTX *c, VALUE *sp_top)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    struct timespec t0 = aro_gc_time_begin(c);

    if (nursery_top != nursery_base) {
        minor_gc(c, sp_top);
    }
    remset_cnt = 0;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    for (size_t b = 0; b <= max_touched_block; b++) {
        memset(blocks[b].line_marks, 0, LINES_PER_BLOCK);
    }

    for (VALUE *p = c->env; p < sp_top; p++) mark_value_major(gc, *p);
    process_gray_major(gc);

    sweep_major(gc);

    gc->common.stats.gc_count++;
    gc->common.stats.major_count++;
    cur_epoch = (cur_epoch == 255) ? 1 : (uint8_t)(cur_epoch + 1);
    old_alloc_since_major = 0;
    if (!gc->common.stress) {
        size_t next = gc->common.stats.heap_bytes * MAJOR_THRESHOLD_FACTOR;
        major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    c->sp = sp_top;
    aro_gc_time_end(c, t0);
}

void
aro_gc_collect(CTX *c)
{
    VALUE *sp_top = c->sp;
    major_gc(c, sp_top);
}


void
aro_gc_fini(CTX *c)
{
    ASTroGC *gc = ASTRO_GC_INSTANCE(c);
    if (!gc) return;
    if (arena_base)   munmap(arena_base, ARENA_BYTES);
    if (blocks)       munmap(blocks, N_BLOCKS * sizeof(BlockMeta));
    if (nursery_base) munmap(nursery_base, NURSERY_BYTES);
    LargeObj *lo = large_head;
    while (lo) {
        LargeObj *next = lo->next;
        munmap(lo, lo->map_bytes);
        lo = next;
    }
    free(gray_buf);
    free(remset_buf);
    free(gc);
    c->astro_gc = NULL;
}

AroGcKind
aro_gc_kind_of(void *p)
{
    GCHeader *h = (GCHeader *)p - 1;
    return HDR_KIND(h);
}

size_t
aro_gc_size_of(void *p)
{
    GCHeader *h = (GCHeader *)p - 1;
    return h->size;
}

/* In-place realloc for large objs via mremap.  Template-driven via
 * gc_inplace_mremap.h — see that header's docstring. */
#define large_head                       gc->large_head
#define ARO_GC_INPLACE_THRESHOLD(n)      (sizeof(GCHeader) + ALIGN8(n) <= MEDIUM_MAX)
#define ARO_GC_INPLACE_PAGE_SIZE         sysconf(_SC_PAGESIZE)
#define ARO_GC_INPLACE_MREMAP_FLAGS      0
#define ARO_GC_INPLACE_BYTES_ACCT(d)     (old_alloc_since_major += (d))
#include "gc_inplace_mremap.h"
#undef large_head
