// gc_mark_compact_gen.c — backend #9: generational hybrid.
//
// Same nursery layout as gc_copy_gen.c, but tenured uses a single-region
// mark + Lisp-2 sliding compactor (gc_mark_compact's algorithm) instead of
// a two-region semispace.  Saves half the tenured virtual address space
// at the cost of a more complex major.
//
// Layout:
//   - Nursery: one bump region (16 MiB).
//   - Tenured: single mmap'd region (512 MiB).  Survivors are appended on
//     minor; major mark+compact reclaims dead in place.
//
// Minor GC: Cheney-style copy nursery→tenured (same as copy_gen).
// Major GC: Lisp 2 sliding compactor over tenured (same as mark_compact).
// Write barrier: explicit remset, same as copy_gen / mark_gen.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define NURSERY_BYTES  ((size_t)16u  << 20)    /* 16 MiB (tuning knob, not program limit) */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

/* 16-byte header (down from 24).  kind + old + dirty + marked packed in flags. */
typedef struct GCHeader {
    uint8_t  flags;     /* bits 0-2: kind, bit 3: marked, bit 4: old, bit 5: dirty */
    uint8_t  _pad[3];
    uint32_t size;
    void    *fwd;       // forwarding pointer (minor: tenured dest; major: compacted dest)
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 16, "GCHeader must be 16 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_MARKED_BIT   0x08u
#define HDR_OLD_BIT      0x10u
#define HDR_DIRTY_BIT    0x20u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))
#define HDR_MARKED(h)      (((h)->flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->flags &= (uint8_t)~HDR_MARKED_BIT)
#define HDR_OLD(h)         (((h)->flags & HDR_OLD_BIT) != 0)
#define HDR_SET_OLD(h)     ((h)->flags |= HDR_OLD_BIT)
#define HDR_CLR_OLD(h)     ((h)->flags &= (uint8_t)~HDR_OLD_BIT)
#define HDR_DIRTY(h)       (((h)->flags & HDR_DIRTY_BIT) != 0)
#define HDR_SET_DIRTY(h)   ((h)->flags |= HDR_DIRTY_BIT)
#define HDR_CLR_DIRTY(h)   ((h)->flags &= (uint8_t)~HDR_DIRTY_BIT)

static char *nursery_base = NULL;
static char *nursery_top  = NULL;
static char *nursery_end  = NULL;

static char *tenured_base = NULL;
static char *tenured_top  = NULL;
static char *tenured_end  = NULL;

static CTX *gc_ctx = NULL;
static VALUE *sp_high_water = NULL;

// Explicit remembered set: tenured objects that have been written to since
// the last minor GC.  Without this, every minor would scan the whole
// tenured region looking for dirty bits.  Reset by both minor (after
// processing) and major (full trace makes it obsolete).
static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

/* Adaptive major threshold (iter 29).  Without this, major fired only
 * when tenured couldn't hold worst-case promotion = effectively never
 * with 64 GiB virtual. */
#define MAJOR_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define MAJOR_THRESHOLD_FACTOR  2
static size_t old_alloc_since_major = 0;
static size_t old_major_threshold = MAJOR_THRESHOLD_MIN;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "mark_compact_gen";

// Gray queue for major mark phase.
static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

static char *
mmap_region(size_t bytes)
{
    char *p = (char *)mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); abort(); }
    return p;
}

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    nursery_base = mmap_region(NURSERY_BYTES);
    nursery_top  = nursery_base;
    nursery_end  = nursery_base + NURSERY_BYTES;

    tenured_base = mmap_region(TENURED_BYTES);
    tenured_top  = tenured_base;
    tenured_end  = tenured_base + TENURED_BYTES;

    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        fprintf(stderr, "[baruby_gc=mark_compact_gen] STRESS mode: collect on every alloc\n");
    }
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void minor_gc(VALUE *sp_top);
static void major_gc(VALUE *sp_top);

// Allocate `bytes` (header + aligned payload).  Small allocations go in
// the nursery; allocations bigger than half the nursery size go straight
// into tenured (= pretenured, like Boehm's large-object heap).  Triggers
// minor / major GC + retry on space pressure.
static GCHeader *
nursery_bump(AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    size_t total = sizeof(GCHeader) + aligned;

    // Pretenure huge allocations directly into tenured (the nursery is
    // small and can't hold a single multi-MiB payload).
    if (total > NURSERY_BYTES / 2) {
        if (tenured_top + total > tenured_end) {
            major_gc(sp_top);
            if (tenured_top + total > tenured_end) {
                fprintf(stderr, "baruby_gc=mark_compact_gen: OOM tenured (need %zu)\n", total);
                abort();
            }
        }
        GCHeader *h = (GCHeader *)tenured_top;
        HDR_SET_KIND(h, kind);
        h->size   = (uint32_t)payload_size;
        h->fwd    = NULL;
        HDR_SET_OLD(h);    // direct to tenured = "old" from the start
        HDR_CLR_DIRTY(h);
        HDR_CLR_MARKED(h);
        tenured_top += total;
        return h;
    }

    if (aro_gc_stress || nursery_top + total > nursery_end) {
        // If tenured can't safely hold the entire nursery (worst-case
        // promotion), do a major first to recover dead tenured space.
        size_t max_promotion = (size_t)(nursery_top - nursery_base);
        if (tenured_top + max_promotion > tenured_end) {
            major_gc(sp_top);
        } else {
            minor_gc(sp_top);
            if (old_alloc_since_major > old_major_threshold) {
                major_gc(sp_top);
            }
        }
        if (nursery_top + total > nursery_end) {
            major_gc(sp_top);
            if (nursery_top + total > nursery_end) {
                fprintf(stderr, "baruby_gc=mark_compact_gen: OOM (need %zu)\n", total);
                abort();
            }
        }
    }
    GCHeader *h = (GCHeader *)nursery_top;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    h->fwd    = NULL;
    HDR_CLR_OLD(h);
    HDR_CLR_DIRTY(h);
    HDR_CLR_MARKED(h);
    nursery_top += total;
    return h;
}

void *
aro_gc_alloc(AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = nursery_bump(kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
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
    GCHeader *h = nursery_bump(KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
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
    // Root old via sp_top[0] so GC tracks the source through any move
    // (minor copy-promote or major slide-compact).  See gc_copy_gen.c.
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(new_size, sp_top + 1)
        : aro_gc_alloc(kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

static void
remset_push(GCHeader *h)
{
    if (remset_cnt >= remset_capa) {
        remset_capa = remset_capa ? remset_capa * 2 : 256;
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
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        HDR_SET_DIRTY(hh);
        remset_push(hh);
    }
}

void
aro_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (HDR_OLD(hh) && !HDR_DIRTY(hh)) {
        HDR_SET_DIRTY(hh);
        remset_push(hh);
    }
}

// ---------------------------------------------------------------------------
// Cheney copy collector
// ---------------------------------------------------------------------------

static char *to_top;          // bump pointer into to-tenured
static char *to_base;
static char *from_base_cur;   // active region being copied OUT of (for range check)
static char *from_end_cur;

// Copy `oldh` (already in nursery or from-tenured) into to-tenured, returning
// the new payload pointer.  Sets oldh->fwd so future references find the
// new home.
static void *
forward_obj(GCHeader *oldh)
{
    if (oldh->fwd) return oldh->fwd;
    size_t aligned = ALIGN8(oldh->size);
    size_t total = sizeof(GCHeader) + aligned;
    if (to_top + total > tenured_end) {
        fprintf(stderr, "baruby_gc=mark_compact_gen: tenured OOM in forward_obj "
                        "(need %zu, tenured %zu / %zu)\n",
                total, (size_t)(to_top - tenured_base), TENURED_BYTES);
        abort();
    }
    GCHeader *newh = (GCHeader *)to_top;
    memcpy(newh, oldh, total);
    newh->fwd   = NULL;
    HDR_SET_OLD(newh);
    HDR_CLR_DIRTY(newh);
    to_top += total;
    void *new_payload = (void *)(newh + 1);
    oldh->fwd = new_payload;
    return new_payload;
}

// During MINOR GC: nursery objects only (in_minor=true).
// During MAJOR GC: all in from-tenured (and any nursery survivors).
static bool in_minor = false;

static inline bool
in_nursery(void *p)
{
    return (char *)p >= nursery_base && (char *)p < nursery_end;
}

static inline bool
in_from_tenured(void *p)
{
    return (char *)p >= from_base_cur && (char *)p < from_end_cur;
}

static void *
forward_payload_value(void *p)
{
    if (!p) return NULL;
    GCHeader *h = (GCHeader *)p - 1;
    if (in_minor) {
        if (!in_nursery(p)) return p;     // already tenured; nothing to do
    } else {
        // Major: forward anything in nursery OR from-tenured.
        if (!in_nursery(p) && !in_from_tenured(p)) return p;
    }
    return forward_obj(h);
}

static VALUE
forward_value(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)forward_payload_value((void *)v);
}

// Walk a freshly-copied object's outgoing references and forward them.
static void
process_object(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)forward_payload_value(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) s->bytes = (char *)forward_payload_value(s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = forward_value(items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "process_object: unknown kind");
    }
}


static void
minor_gc(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    in_minor = true;
    to_base = tenured_base;
    to_top  = tenured_top;            // append onto current tenured
    from_base_cur = nursery_base;
    from_end_cur  = nursery_top;      // tight bound on valid nursery objects

    // Maintain the high-water-mark invariant: slots above the current sp_top
    // get zeroed if they had been used at a previous deeper recursion.
    // Without this, a stale nursery pointer left behind in a higher slot
    // (after that minor moved its target to tenured AND another later minor
    // emptied the nursery) could be re-scanned next time sp_top extends past
    // it, and forward_value would treat it as a live nursery object.
    CTX *c = gc_ctx;
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    /* Minor is pure Cheney → all in reclaim_seconds. */
    struct timespec tminor = aro_gc_phase_begin();
    // (1) Roots
    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(*p);

    // (2) Dirty tenured (explicit remset): scan only the dirty entries,
    //     forwarding their young pointers.  O(|remset|) — vastly cheaper
    //     than walking the whole tenured region (which on large old heaps
    //     dominated minor GC time).
    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *h = remset_buf[i];
        if (HDR_DIRTY(h)) {
            process_object(h);
            HDR_CLR_DIRTY(h);
        }
    }
    remset_cnt = 0;

    // (3) Cheney scan-loop over freshly-tenured objects (tenured_top..to_top).
    {
        char *scan = tenured_top;
        while (scan < to_top) {
            GCHeader *h = (GCHeader *)scan;
            process_object(h);
            scan += sizeof(GCHeader) + ALIGN8(h->size);
        }
    }
    aro_gc_phase_end(tminor, &aro_gc_stats.reclaim_seconds);

    // (4) Commit: tenured_top advances to to_top; nursery emptied.
    old_alloc_since_major += (size_t)(to_top - tenured_top);
    tenured_top = to_top;
    nursery_top = nursery_base;
    in_minor = false;

    aro_gc_stats.gc_count++;
    aro_gc_stats.minor_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

// ---------------------------------------------------------------------------
// Major GC: Lisp-2 sliding compactor over tenured.
// (Same algorithm as gc_mark_compact.c, but only over the tenured region —
// the nursery is first folded into tenured via minor_gc.)
// ---------------------------------------------------------------------------

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

static void
mark_value_major(VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(h);
}

static void
scan_outgoing_major(GCHeader *h)
{
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
        ASTRO_ASSERT(0 && "scan_outgoing_major: unknown kind");
    }
}

static void
process_gray_major(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        scan_outgoing_major(h);
    }
}

static void *
fwd_payload_compact(void *p)
{
    if (!p) return NULL;
    // In defer-fold mode, nursery survivors are reachable from roots /
    // marked tenured objects but have no tenured fwd address yet — the
    // trailing minor will fold them in.  Leave nursery pointers alone.
    if (in_nursery(p)) return p;
    GCHeader *h = (GCHeader *)p - 1;
    ASTRO_ASSERT(HDR_MARKED(h));
    ASTRO_ASSERT(h->fwd != NULL);
    return (void *)((char *)h->fwd + sizeof(GCHeader));
}

static VALUE
fwd_value_compact(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)fwd_payload_compact((void *)v);
}

static void
update_pointers_major(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)fwd_payload_compact(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) s->bytes = (char *)fwd_payload_compact(s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) items[i] = fwd_value_compact(items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "update_pointers_major: unknown kind");
    }
}

static void
major_gc(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    in_minor = false;

    // Try to fold nursery into tenured via a leading minor (mainline:
    // cheap, then mark+compact runs over tenured only).  But if tenured
    // can't hold the worst-case promotion, the minor's forward_obj would
    // overflow tenured_end.  Defer the fold to a trailing minor after
    // compact frees space.
    bool defer_fold = false;
    if (nursery_top != nursery_base) {
        size_t max_promotion = (size_t)(nursery_top - nursery_base);
        if (tenured_top + max_promotion > tenured_end) {
            defer_fold = true;
        } else {
            minor_gc(sp_top);
        }
    }
    remset_cnt = 0;

    CTX *c = gc_ctx;

    // High-water zero (in case minor wasn't run, e.g., empty nursery).
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    // (1) Mark from roots.
    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value_major(*p);
    process_gray_major();
    aro_gc_phase_end(tmark, &aro_gc_stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    // (2) Forward-address pass.
    char *fwd = tenured_base;
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (HDR_MARKED(h)) {
                h->fwd = fwd;
                fwd += total;
            } else {
                h->fwd = NULL;
            }
            p += total;
        }
    }

    // (3) Update outgoing pointers in each live object (in place).
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (HDR_MARKED(h)) update_pointers_major(h);
            p += total;
        }
    }

    // (4) Update roots.
    for (VALUE *p = c->env; p < sp_top; p++) *p = fwd_value_compact(*p);

    // (5) Slide live objects to their forwarding addresses.  Batched on
    //     contiguous-marked runs (same delta).
    {
        char *p = tenured_base;
        while (p < tenured_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (!HDR_MARKED(h)) {
                p += total;
                continue;
            }
            char *run_src = p;
            char *run_dst = h->fwd;
            char *run_p   = p;
            while (run_p < tenured_top) {
                GCHeader *rh = (GCHeader *)run_p;
                if (!HDR_MARKED(rh)) break;
                run_p += sizeof(GCHeader) + ALIGN8(rh->size);
            }
            size_t run_size = (size_t)(run_p - run_src);
            if (run_dst != run_src) memmove(run_dst, run_src, run_size);
            // Clear marked / fwd / dirty on each moved header.
            char *q = run_dst, *q_end = run_dst + run_size;
            while (q < q_end) {
                GCHeader *qh = (GCHeader *)q;
                HDR_CLR_MARKED(qh);
                qh->fwd    = NULL;
                HDR_CLR_DIRTY(qh);
                q += sizeof(GCHeader) + ALIGN8(qh->size);
            }
            p = run_p;
        }
    }
    tenured_top = fwd;

    // (6) Trailing minor fold (defer_fold only).  Nursery survivors were
    //     marked during the major's mark phase; their marked bit must be
    //     cleared before forward_obj memcpys the header into tenured
    //     (otherwise the new tenured copy enters with marked=true, and the
    //     next major would skip it).  After clearing, run a minor.  Walk
    //     all tenured rather than the now-empty remset, since any tenured
    //     object compacted above may still carry a nursery pointer that
    //     we deliberately left untouched in step (3).
    if (defer_fold) {
        char *q = nursery_base;
        while (q < nursery_top) {
            GCHeader *h = (GCHeader *)q;
            HDR_CLR_MARKED(h);
            q += sizeof(GCHeader) + ALIGN8(h->size);
        }

        in_minor = true;
        to_base = tenured_base;
        char *old_tenured_top = tenured_top;
        to_top = tenured_top;
        from_base_cur = nursery_base;
        from_end_cur  = nursery_top;

        if (sp_high_water == NULL || sp_top > sp_high_water) {
            sp_high_water = sp_top;
        } else {
            for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
        }

        for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(*p);

        // Walk pre-fold tenured for nursery refs (remset surrogate).
        {
            char *p = tenured_base;
            while (p < old_tenured_top) {
                GCHeader *h = (GCHeader *)p;
                process_object(h);
                p += sizeof(GCHeader) + ALIGN8(h->size);
            }
        }
        // Cheney scan of freshly-promoted.
        {
            char *scan = old_tenured_top;
            while (scan < to_top) {
                GCHeader *h = (GCHeader *)scan;
                process_object(h);
                scan += sizeof(GCHeader) + ALIGN8(h->size);
            }
        }
        tenured_top = to_top;
        nursery_top = nursery_base;
        in_minor = false;

        aro_gc_stats.minor_count++;
    }
    aro_gc_phase_end(treclaim, &aro_gc_stats.reclaim_seconds);

    /* Adaptive threshold update. */
    size_t live = (size_t)(tenured_top - tenured_base);
    aro_gc_stats.heap_bytes = live;
    old_alloc_since_major = 0;
    if (!aro_gc_stress) {
        size_t next = live * MAJOR_THRESHOLD_FACTOR;
        old_major_threshold = next < MAJOR_THRESHOLD_MIN ? MAJOR_THRESHOLD_MIN : next;
    }

    aro_gc_stats.gc_count++;
    aro_gc_stats.major_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(VALUE *sp_top)
{
    major_gc(sp_top);
}

size_t aro_gc_total_bytes(void) { return aro_gc_stats.total_bytes; }
size_t aro_gc_heap_bytes (void) { return (size_t)(tenured_top - tenured_base) +
                                            (size_t)(nursery_top - nursery_base); }
size_t aro_gc_count      (void) { return aro_gc_stats.gc_count;    }
size_t aro_gc_minor_count(void) { return aro_gc_stats.minor_count; }
size_t aro_gc_major_count(void) { return aro_gc_stats.major_count; }
double aro_gc_mark_seconds(void) { return aro_gc_stats.mark_seconds; }
double aro_gc_reclaim_seconds(void) { return aro_gc_stats.reclaim_seconds; }
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
