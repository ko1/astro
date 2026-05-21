// gc_mark_compact.c — backend #8: mark + compact in a single region.
//
// Layout: one mmap'd region (1 GiB virtual, lazy-paged).  Bump alloc.
// On collection: classic "Lisp 2" sliding compactor —
//
//   1. Mark live (BFS from roots through the gray queue).
//   2. Forward-address pass: walk region linearly; each marked header
//      records its packed-destination address into ->fwd.
//   3. Update-pointers pass: walk region; for each live, rewrite outgoing
//      pointers (a->items, s->bytes, items[i] VALUEs) using the target's
//      ->fwd field.  Roots are updated the same way.
//   4. Slide pass: walk region; for each live, memmove from src to ->fwd.
//      Destination is always ≤ source (compaction), and src_{i+1} > dst_i
//      so no overlap between iterations.  region_top reset to the end of
//      the last slid object.
//
// vs gc_mark.c (linked-list mark&sweep): no per-object malloc/free, no
// linked-list traversal during sweep.  Cost: each major collection moves
// all live objects, so pointer-rewrite + memmove dominates when |live|
// is large.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

/* 16-byte header (down from 24).  kind + marked packed in flags. */
typedef struct GCHeader {
    uint8_t  flags;     /* bits 0-2: kind, bit 3: marked */
    uint8_t  _pad[3];
    uint32_t size;
    char    *fwd;     // packed destination address during compaction
} GCHeader;
_Static_assert(sizeof(struct GCHeader) == 16, "GCHeader must be 16 bytes");

#define HDR_KIND_MASK    0x07u
#define HDR_MARKED_BIT   0x08u
#define HDR_KIND(h)        ((AroGcKind)((h)->flags & HDR_KIND_MASK))
#define HDR_SET_KIND(h, k) ((h)->flags = (uint8_t)(((h)->flags & ~HDR_KIND_MASK) | ((k) & HDR_KIND_MASK)))
#define HDR_MARKED(h)      (((h)->flags & HDR_MARKED_BIT) != 0)
#define HDR_SET_MARKED(h)  ((h)->flags |= HDR_MARKED_BIT)
#define HDR_CLR_MARKED(h)  ((h)->flags &= (uint8_t)~HDR_MARKED_BIT)

#define REGION_BYTES ARO_GC_REGION_VIRT_BYTES   /* 64 GiB virtual, lazy-paged */
#define ALIGN8(n)    (((n) + 7u) & ~(size_t)7u)

static char *region_base = NULL;
static char *region_top  = NULL;
static char *region_end  = NULL;
static CTX  *gc_ctx      = NULL;
static VALUE *sp_high_water = NULL;

/* Adaptive GC trigger: max(16 MiB, 2 × live_post_compact).  Previously
 * region-fill basis only (= effectively never with 64 GiB virtual). */
#define GC_THRESHOLD_MIN     (16u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2
static size_t bytes_since_gc = 0;
static size_t gc_threshold   = GC_THRESHOLD_MIN;

static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "mark_compact";

void
aro_gc_init(CTX *c)
{
    gc_ctx = c;
    region_base = (char *)mmap(NULL, REGION_BYTES, PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
    if (region_base == MAP_FAILED) { perror("mmap"); abort(); }
    region_top = region_base;
    region_end = region_base + REGION_BYTES;
    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        fprintf(stderr, "[baruby_gc=mark_compact] STRESS mode: collect on every alloc\n");
    }
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static void gc_collect_internal(VALUE *sp_top);

// iter 43: cold-path split (see gc_copy.c for rationale).
static void __attribute__((noinline, cold))
bump_slow(size_t total, VALUE *sp_top)
{
    gc_collect_internal(sp_top);
    if (region_top + total > region_end) {
        fprintf(stderr, "baruby_gc=mark_compact: OOM (need %zu)\n", total);
        abort();
    }
}

static inline GCHeader *
bump(AroGcKind kind, size_t payload_size, size_t aligned, VALUE *sp_top)
{
    size_t total = sizeof(GCHeader) + aligned;
    if (__builtin_expect(aro_gc_stress
                         || bytes_since_gc + payload_size > gc_threshold
                         || region_top + total > region_end, 0)) {
        bump_slow(total, sp_top);
    }
    GCHeader *h = (GCHeader *)region_top;
    HDR_SET_KIND(h, kind);
    h->size   = (uint32_t)payload_size;
    HDR_CLR_MARKED(h);
    h->fwd    = NULL;
    region_top += total;
    bytes_since_gc += payload_size;
    return h;
}

void *
aro_gc_alloc(CTX *c, AroGcKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(kind, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, aligned);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
aro_gc_alloc_byte(CTX *c, size_t payload_size, VALUE *sp_top)
{
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = bump(KIND_PAYLOAD_BYTE, payload_size, aligned, sp_top);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    aro_gc_stats.total_bytes += payload_size;
    aro_gc_stats.heap_bytes  += payload_size;
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
    // Root old via sp_top[0] — major slide-compact updates roots in
    // its fwd-pointer phase, so sp_top[0] reflects the post-slide
    // location.  Uniform with other backends.
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? aro_gc_alloc_byte(c, new_size, sp_top + 1)
        : aro_gc_alloc(c, kind, new_size, sp_top + 1);
    if (copy_bytes) memcpy(newp, (void *)sp_top[0], copy_bytes);
    return newp;
}

// ---------------------------------------------------------------------------
// Mark phase
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
mark_value(VALUE v)
{
    if (!IS_PTR(v)) return;
    GCHeader *h = (GCHeader *)v - 1;
    if (HDR_MARKED(h)) return;
    HDR_SET_MARKED(h);
    gray_push(h);
}

static void
scan_outgoing(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) mark_value((VALUE)a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (!BSTR_IS_SSO(s) && s->bytes) mark_value((VALUE)s->bytes);
        break;
      }
      case KIND_PAYLOAD_VAL: {
        VALUE *items = (VALUE *)payload;
        size_t n = h->size / sizeof(VALUE);
        for (size_t i = 0; i < n; i++) mark_value(items[i]);
        break;
      }
      case KIND_PAYLOAD_BYTE:
      case KIND_FREE:
        break;
      default:
        ASTRO_ASSERT(0 && "scan_outgoing: unknown kind");
    }
}

static void
process_gray(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
        scan_outgoing(h);
    }
}

// ---------------------------------------------------------------------------
// Pointer update (forward-address pass + update pass)
// ---------------------------------------------------------------------------

// Translate an old payload pointer to its compacted (new) payload pointer.
// Caller guarantees p is a payload pointer (= GCHeader *) for a live object.
static void *
fwd_payload(void *p)
{
    if (!p) return NULL;
    GCHeader *h = (GCHeader *)p - 1;
    ASTRO_ASSERT(HDR_MARKED(h));
    ASTRO_ASSERT(h->fwd != NULL);
    return (void *)(h->fwd + sizeof(GCHeader));
}

static VALUE
fwd_value(VALUE v)
{
    if (!IS_PTR(v)) return v;
    return (VALUE)fwd_payload((void *)v);
}

static void
update_pointers(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch (HDR_KIND(h)) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) a->items = (VALUE *)fwd_payload(a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (!BSTR_IS_SSO(s) && s->bytes) s->bytes = (char *)fwd_payload(s->bytes);
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
        ASTRO_ASSERT(0 && "update_pointers: unknown kind");
    }
}

// ---------------------------------------------------------------------------
// Collect (Lisp 2 style)
// ---------------------------------------------------------------------------

static void
gc_collect_internal(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    CTX *c = gc_ctx;

    // High-water-mark zeroing: slots above sp_top that were used at a
    // previous deeper recursion may still hold pointers to objects this
    // GC compacts/moves; if a later recursion's sp_top exceeds these we
    // could otherwise scan stale pointers as live roots.
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    // (1) Mark from roots.
    struct timespec tmark = aro_gc_phase_begin();
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();
    aro_gc_phase_end(tmark, &aro_gc_stats.mark_seconds);

    struct timespec treclaim = aro_gc_phase_begin();
    // (2) Forward-address pass: pack live objects to the start of the region.
    char *fwd = region_base;
    size_t live_bytes = 0;
    {
        char *p = region_base;
        while (p < region_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (HDR_MARKED(h)) {
                h->fwd = fwd;
                fwd += total;
                live_bytes += h->size;
            } else {
                h->fwd = NULL;
            }
            p += total;
        }
    }

    // (3) Update outgoing pointers inside each live object (in place).
    {
        char *p = region_base;
        while (p < region_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (HDR_MARKED(h)) update_pointers(h);
            p += total;
        }
    }

    // (4) Update roots.
    for (VALUE *p = c->env; p < sp_top; p++) *p = fwd_value(*p);

    // (5) Slide live objects to their forwarding addresses.
    //     Batch consecutive marked objects (no dead in between) into a
    //     single memmove — they all share the same src-vs-dst delta.
    {
        char *p = region_base;
        while (p < region_top) {
            GCHeader *h = (GCHeader *)p;
            size_t total = sizeof(GCHeader) + ALIGN8(h->size);
            if (!HDR_MARKED(h)) {
                p += total;
                continue;
            }
            // Find the end of this contiguous-marked run.  All objects in
            // the run share the same (src - dst) delta because bump-alloc
            // packs them contiguously and the forward pass packs survivors
            // contiguously too.
            char *run_src_start = p;
            char *run_dst_start = h->fwd;
            char *run_p = p;
            while (run_p < region_top) {
                GCHeader *rh = (GCHeader *)run_p;
                if (!HDR_MARKED(rh)) break;
                run_p += sizeof(GCHeader) + ALIGN8(rh->size);
            }
            size_t run_size = (size_t)(run_p - run_src_start);
            if (run_dst_start != run_src_start) {
                memmove(run_dst_start, run_src_start, run_size);
            }
            // Clear marked / fwd on each moved header.
            char *q = run_dst_start;
            char *q_end = run_dst_start + run_size;
            while (q < q_end) {
                GCHeader *qh = (GCHeader *)q;
                HDR_CLR_MARKED(qh);
                qh->fwd    = NULL;
                q += sizeof(GCHeader) + ALIGN8(qh->size);
            }
            p = run_p;
        }
    }
    region_top = fwd;
    aro_gc_phase_end(treclaim, &aro_gc_stats.reclaim_seconds);

    aro_gc_stats.heap_bytes = live_bytes;
    /* Adaptive threshold update. */
    bytes_since_gc = 0;
    if (!aro_gc_stress) {
        size_t next = live_bytes * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    aro_gc_stats.gc_count++;
    aro_gc_stats.major_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

void
aro_gc_collect(CTX *c, VALUE *sp_top)
{
    gc_collect_internal(sp_top);
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
