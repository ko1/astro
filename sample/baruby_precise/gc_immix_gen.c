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

static uint8_t cur_epoch = 1;

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

/* --- Tenured (Immix arena) state --- */
static char       *arena_base = NULL;
static BlockMeta  *blocks     = NULL;
static size_t      block_cursor = 0;
static size_t      line_cursor  = 0;
static char       *cur_ptr   = NULL;
static char       *cur_end   = NULL;
static size_t      max_touched_block = 0;
static LargeObj   *large_head = NULL;

/* --- Nursery state --- */
static char *nursery_base = NULL;
static char *nursery_top  = NULL;
static char *nursery_end  = NULL;

/* --- Remset (old objects holding young pointers) --- */
static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

/* --- GC trigger state ---
 * iter 35 fairness fix: was `bytes_since_major += payload_size` in alloc
 * path, counting nursery alloc.  That double-counted vs other gen backends
 * which only fire major when *promoted* (tenured) bytes grow.  Now
 * `old_alloc_since_major` is incremented during promotion in minor_gc /
 * large_alloc, matching copy_gen / mark_compact_gen / mark_bump_gen. */
static size_t old_alloc_since_major = 0;
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2
static size_t major_threshold = MAJOR_THRESHOLD_MIN;

static CTX *gc_ctx = NULL;
static VALUE *sp_high_water = NULL;

/* Gray queue (used by both minor's promotion-Cheney-scan and major's mark). */
static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "immix_gen";

static void minor_gc(VALUE *sp_top);
static void major_gc(VALUE *sp_top);

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
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
        aro_gc_stress = 1;
        major_threshold = 0;
        fprintf(stderr, "[baruby_gc=immix_gen] STRESS mode: collect on every alloc\n");
    }
}

/* ---------------------------------------------------------------------------
 * Line / hole helpers
 * --------------------------------------------------------------------------- */

static inline size_t
block_of(const void *p)
{
    return (size_t)(((const char *)p - arena_base) / BLOCK_BYTES);
}

static inline size_t
line_of(const void *p)
{
    return (size_t)(((const char *)p - arena_base) % BLOCK_BYTES) / LINE_BYTES;
}

static inline bool
in_arena(const void *p)
{
    return (const char *)p >= arena_base && (const char *)p < arena_base + ARENA_BYTES;
}

static inline bool
in_nursery(const void *p)
{
    return (const char *)p >= nursery_base && (const char *)p < nursery_end;
}

static inline void
mark_lines_for(const GCHeader *h)
{
    const char *start = (const char *)h;
    const char *last  = start + sizeof(GCHeader) + ALIGN8(h->size) - 1;
    size_t bi = block_of(start);
    size_t l0 = line_of(start);
    size_t l1 = line_of(last);
    for (size_t l = l0; l <= l1; l++) {
        blocks[bi].line_marks[l] = 1;
    }
}

static bool
find_hole(size_t n_lines, char **out_start, char **out_end)
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
large_alloc(AroGcKind kind, size_t payload_size)
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
    /* Pretenured straight into large/old → counts toward major trigger. */
    old_alloc_since_major += payload_size;
    return (void *)(h + 1);
}

/* Bump-alloc inside a tenured hole.  Returns header (NULL if no room). */
static GCHeader *
hole_alloc_header(AroGcKind kind, size_t payload_size)
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
    if (find_hole(need_lines, &hs, &he)) {
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

static void *
nursery_bump(AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    size_t total = sizeof(GCHeader) + aligned;

    /* Pretenure: anything that won't fit in a single Immix block during
     * promotion goes straight to the large-object space.  This keeps
     * nursery objects guaranteed-promotable into a single-block hole. */
    if (total > MEDIUM_MAX) {
        return large_alloc(kind, payload_size);
    }

    if (aro_gc_stress || nursery_top + total > nursery_end) {
        minor_gc(sp_top);
        if (old_alloc_since_major > major_threshold) {
            major_gc(sp_top);
        }
        if (nursery_top + total > nursery_end) {
            major_gc(sp_top);
            if (nursery_top + total > nursery_end) {
                fprintf(stderr, "baruby_gc=immix_gen: OOM nursery (need %zu)\n", total);
                abort();
            }
        }
    }
    GCHeader *h = (GCHeader *)nursery_top;
    HDR_SET_KIND(h, kind);
    h->size = (uint32_t)payload_size;
    h->mark_epoch = 0;
    h->flags = (uint8_t)((uint8_t)kind & HDR_KIND_MASK);  /* no old/dirty, just kind */
    nursery_top += total;
    return (void *)(h + 1);
}

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(kind, payload_size, aligned, sp_top);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    size_t aligned = ALIGN8(payload_size);
    void *payload = nursery_bump(KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return aro_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    AroGcKind kind = HDR_KIND(oldh);
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

/* ---------------------------------------------------------------------------
 * Write barrier
 * --------------------------------------------------------------------------- */

/* iter 36 remset overflow guard.  Cap at 128 K entries.  Unlike mark_gen /
 * copy_gen / mark_compact_gen / mark_bump_gen we don't implement the
 * heap-walk fallback yet (immix's line-allocator makes O(heap) iteration
 * more involved — slots are variable-size within blocks, no global bump
 * pointer to walk).  Until that's wired up, abort on overflow with a
 * clear diagnostic.  In practice 128 K entries is far above any benign
 * workload's needs (binary_trees peaks at ~22). */
#define MAX_REMSET_ENTRIES (1u << 17)

static void
remset_push(GCHeader *h)
{
    if (remset_cnt >= MAX_REMSET_ENTRIES) {
        fprintf(stderr,
            "baruby_gc=immix_gen: remset overflow (>%u entries).  "
            "This backend has no heap-walk fallback yet — switch to "
            "mark_gen / copy_gen / mark_compact_gen / mark_bump_gen for "
            "remset-pressure workloads.\n",
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
}

void
aro_gc_wb(void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if ((hh->flags & H_OLD) && !(hh->flags & H_DIRTY)) {
        hh->flags |= H_DIRTY;
        remset_push(hh);
    }
}

void
aro_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if ((hh->flags & H_OLD) && !(hh->flags & H_DIRTY)) {
        hh->flags |= H_DIRTY;
        remset_push(hh);
    }
}

/* ---------------------------------------------------------------------------
 * Mark / forwarding
 * --------------------------------------------------------------------------- */

static void
gray_push(GCHeader *h)
{
    if (gray_cnt >= gray_capa) {
        gray_capa = gray_capa ? gray_capa * 2 : 256;
        gray_buf = (GCHeader **)realloc(gray_buf, gray_capa * sizeof(GCHeader *));
        if (!gray_buf) abort();
    }
    gray_buf[gray_cnt++] = h;
}

/* Forward a nursery payload pointer to its tenured copy.  Encodes the
 * fwd by setting oldh->kind = KIND_FREE and storing the new pointer in
 * the first 8 bytes of the old payload (which is dead-from-source). */
static void *
forward_payload_nursery(void *p)
{
    if (!p) return NULL;
    if (!in_nursery(p)) return p;
    GCHeader *oldh = (GCHeader *)p - 1;
    if (HDR_KIND(oldh) == KIND_FREE) {
        return *(void **)p;
    }
    AroGcKind kind = HDR_KIND(oldh);
    size_t size = oldh->size;
    GCHeader *newh = hole_alloc_header(kind, size);
    if (!newh) {
        fprintf(stderr, "baruby_gc=immix_gen: tenured arena OOM during promotion\n");
        abort();
    }
    void *newp = (void *)(newh + 1);
    size_t bytes = ALIGN8(size);
    if (bytes) memcpy(newp, p, bytes);
    HDR_SET_KIND(oldh, KIND_FREE);
    *(void **)p = newp;
    /* Track promoted bytes for fair adaptive major threshold. */
    old_alloc_since_major += size;
    /* Queue the new tenured copy for outgoing-ref forwarding. */
    gray_push(newh);
    return newp;
}

static VALUE
fwd_value(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_nursery((void *)v);
}

/* Process an in-tenured object: walk its outgoing refs, forward nursery
 * pointers to their tenured copy.  Tenured-to-tenured refs pass through. */
static void
process_object_minor(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload_nursery(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) s->bytes = (char *)forward_payload_nursery(s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = fwd_value(items[i]);
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
minor_gc(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    CTX *c = gc_ctx;

    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    /* (1) Roots: forward nursery refs (which pushes new tenured copies
     *     onto the gray queue via forward_payload_nursery). */
    for (VALUE *p = c->env; p < sp_top; p++) *p = fwd_value(*p);

    /* (2) Remset: dirty old objects holding nursery pointers. */
    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *h = remset_buf[i];
        if (h->flags & H_DIRTY) {
            process_object_minor(h);
            h->flags &= (uint8_t)~H_DIRTY;
        }
    }
    remset_cnt = 0;

    /* (3) Cheney scan via gray queue: process freshly-promoted tenured
     *     objects, forwarding their nursery outgoing refs. */
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        process_object_minor(h);
    }

    /* (4) Nursery is now empty (all live promoted; dead implicitly). */
    nursery_top = nursery_base;

    aro_gc_stats.gc_count++;
    aro_gc_stats.minor_count++;
    /* Don't tick cur_epoch during minor — major's mark uses epoch to
     * distinguish "marked in major" from "unmarked".  Minor doesn't
     * touch line_marks at all. */
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

/* ---------------------------------------------------------------------------
 * Major GC: full Immix mark + line-mark sweep over tenured.
 * --------------------------------------------------------------------------- */

static void
mark_value_major(VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (h->mark_epoch == cur_epoch) return;
    h->mark_epoch = cur_epoch;
    if (in_arena(h)) mark_lines_for(h);
    gray_push(h);
}

static void
process_gray_major(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        void *payload = (void *)(h + 1);
        switch (HDR_KIND(h)) {
          case KIND_OBJ_ARRAY: {
            BaArray *a = (BaArray *)payload;
            if (a->items) mark_value_major((VALUE)a->items);
            break;
          }
          case KIND_OBJ_STRING: {
            BaString *s = (BaString *)payload;
            if (s->bytes) mark_value_major((VALUE)s->bytes);
            break;
          }
          case KIND_PAYLOAD_VAL: {
            VALUE *items = (VALUE *)payload;
            size_t n = h->size / sizeof(VALUE);
            for (size_t i = 0; i < n; i++) mark_value_major(items[i]);
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
sweep_major(void)
{
    size_t live = 0;
    /* Only walk touched blocks. */
    for (size_t b = 0; b <= max_touched_block; b++) {
        const uint8_t *lm = blocks[b].line_marks;
        size_t marked = 0;
        for (size_t i = 0; i < LINES_PER_BLOCK; i++) marked += lm[i];
        if (marked == 0) {
            blocks[b].state = BLK_FREE;
            /* MADV_DONTNEED skipped — see gc_immix.c sweep comment. */
        } else if (marked == LINES_PER_BLOCK) {
            blocks[b].state = BLK_USED;
        } else {
            blocks[b].state = BLK_RECYCLABLE;
        }
        live += marked * LINE_BYTES;
    }
    aro_gc_stats.heap_bytes = live;
    LargeObj **link = &large_head;
    while (*link) {
        LargeObj *lo = *link;
        GCHeader *h = (GCHeader *)(lo + 1);
        if (h->mark_epoch == cur_epoch) {
            aro_gc_stats.heap_bytes += h->size;
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
major_gc(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    CTX *c = gc_ctx;

    /* Fold nursery into tenured first via a leading minor. */
    if (nursery_top != nursery_base) {
        minor_gc(sp_top);
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

    for (VALUE *p = c->env; p < sp_top; p++) mark_value_major(*p);
    process_gray_major();

    sweep_major();

    aro_gc_stats.gc_count++;
    aro_gc_stats.major_count++;
    cur_epoch = (cur_epoch == 255) ? 1 : (uint8_t)(cur_epoch + 1);
    old_alloc_since_major = 0;
    if (!aro_gc_stress) {
        size_t next = aro_gc_stats.heap_bytes * MAJOR_THRESHOLD_FACTOR;
        major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(VALUE *sp_top)
{
    major_gc(sp_top);
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return aro_gc_stats.heap_bytes;  }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
