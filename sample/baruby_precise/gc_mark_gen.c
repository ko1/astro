// gc_mark_gen.c — backend #3: non-moving mark&sweep with 2 generations.
//
// Heap split into a young list (nursery) and an old list (tenured).
// Allocations go into young.  On minor GC, survivors are promoted to old.
//
// Remembered set is *implicit*: we don't run a write barrier in the
// mutator, so we don't know which old objects hold young pointers.
// Instead, every minor GC scans ALL old objects' payloads for young
// pointers (= treat all old as remset).  Cost: O(|old|) per minor GC —
// fine for a testbed but limits scaling.
//
// Major GC: standard mark&sweep over both lists (clears the implicit
// remset effect since fewer young pointers exist after promotion).
//
// Stress mode: collect on every alloc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

typedef struct GCHeader {
    struct GCHeader *prev, *next;
    uint32_t kind;
    uint32_t size;
    bool     marked;
    bool     old;
    bool     dirty;   // old object written to since last minor GC (remset entry)
} GCHeader;

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

static GCHeader young_head, old_head;
static size_t   young_bytes = 0;
static size_t   old_bytes   = 0;
static size_t   young_threshold     = 4u * 1024u * 1024u;     // 4 MiB nursery
static size_t   old_alloc_since_major = 0;
static size_t   old_major_threshold = 64u * 1024u * 1024u;    // 64 MiB
static CTX     *gc_ctx = NULL;
static bool     in_minor = false;

static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

BarubyGCStats baruby_gc_stats = {0, 0, 0, 0, 0};
int baruby_gc_stress = 0;
const char *baruby_gc_backend_name = "mark_gen";

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    young_head.prev = young_head.next = &young_head;
    old_head.prev   = old_head.next   = &old_head;
    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
        young_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark_gen] STRESS\n");
    }
}

static GCHeader *
list_alloc_young(BarubyGCKind kind, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = (GCHeader *)malloc(sizeof(GCHeader) + aligned);
    if (!h) { fprintf(stderr, "baruby_gc=mark_gen: OOM\n"); abort(); }
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->marked = false;
    h->old    = false;
    h->dirty  = false;
    h->prev   = &young_head;
    h->next   = young_head.next;
    young_head.next->prev = h;
    young_head.next       = h;
    return h;
}

// ---------------------------------------------------------------------------
// Write barrier
// ---------------------------------------------------------------------------

void
baruby_gc_wb(void *holder, VALUE *slot, VALUE v)
{
    *slot = v;
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (hh->old) hh->dirty = true;
}

void
baruby_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (hh->old) hh->dirty = true;
}

static void minor_gc(VALUE *sp_top);
static void major_gc(VALUE *sp_top);

static inline void
maybe_collect(size_t add, VALUE *sp_top)
{
    if (baruby_gc_stress || young_bytes + add > young_threshold) {
        if (old_alloc_since_major > old_major_threshold) {
            major_gc(sp_top);
            old_alloc_since_major = 0;
        } else {
            minor_gc(sp_top);
        }
    }
}

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    maybe_collect(payload_size, sp_top);
    GCHeader *h = list_alloc_young(kind, payload_size);
    void *payload = (void *)(h + 1);
    memset(payload, 0, ALIGN8(payload_size));
    young_bytes += payload_size;
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    maybe_collect(payload_size, sp_top);
    GCHeader *h = list_alloc_young(KIND_PAYLOAD_BYTE, payload_size);
    void *payload = (void *)(h + 1);
    young_bytes += payload_size;
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) return baruby_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    GCHeader *oldh = (GCHeader *)old - 1;
    BarubyGCKind kind = (BarubyGCKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    char *buf = NULL;
    if (copy_bytes) {
        buf = (char *)malloc(copy_bytes);
        if (!buf) abort();
        memcpy(buf, old, copy_bytes);
    }
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? baruby_gc_alloc_byte(new_size, sp_top)
        : baruby_gc_alloc(kind, new_size, sp_top);
    if (copy_bytes) memcpy(newp, buf, copy_bytes);
    free(buf);
    return newp;
}

// ---------------------------------------------------------------------------
// Trace + mark
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
    // Minor GC: old objects are assumed live (don't push to gray; their
    // payloads have already been scanned for young pointers by the
    // initial remset pass).
    if (in_minor && h->old) return;
    if (h->marked) return;
    h->marked = true;
    gray_push(h);
}

static void
scan_outgoing(GCHeader *h)
{
    void *payload = (void *)(h + 1);
    switch ((BarubyGCKind)h->kind) {
      case KIND_OBJ_ARRAY: {
        BaArray *a = (BaArray *)payload;
        if (a->items) mark_value((VALUE)a->items);
        break;
      }
      case KIND_OBJ_STRING: {
        BaString *s = (BaString *)payload;
        if (s->bytes) mark_value((VALUE)s->bytes);
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

// Move a marked young object to the old list.  Caller decides when to
// clear the marked bit — for minor GC we clear it here (sweep stops at
// young), but for major GC the subsequent old-sweep needs to see the
// just-promoted object still marked so it doesn't free it.
static void
promote(GCHeader *h, bool clear_marked)
{
    if (clear_marked) h->marked = false;
    // unlink from young list
    h->prev->next = h->next;
    h->next->prev = h->prev;
    // insert into old list (head)
    h->old  = true;
    h->prev = &old_head;
    h->next = old_head.next;
    old_head.next->prev = h;
    old_head.next       = h;
    old_bytes += h->size;
    old_alloc_since_major += h->size;
}

static void
free_unlink(GCHeader *h)
{
    h->prev->next = h->next;
    h->next->prev = h->prev;
    baruby_gc_stats.heap_bytes -= h->size;
    free(h);
}

static void
minor_gc(VALUE *sp_top)
{
    in_minor = true;
    // Step 1: scan only dirty old objects (the implicit remembered set).
    // Each dirty old object had a young pointer written into it since the
    // previous collection; clear the bit and trace its outgoing for young.
    for (GCHeader *h = old_head.next; h != &old_head; h = h->next) {
        if (h->dirty) {
            scan_outgoing(h);
            h->dirty = false;
        }
    }
    // Step 2: roots
    for (VALUE *p = gc_ctx->env; p < sp_top; p++) mark_value(*p);
    // Step 3: BFS through young objects
    process_gray();

    // Step 4: sweep young — promote marked (clearing marked since old
    // is not being swept in this minor), free unmarked.
    GCHeader *h = young_head.next;
    while (h != &young_head) {
        GCHeader *next = h->next;
        if (h->marked) promote(h, /*clear_marked=*/true);
        else           free_unlink(h);
        h = next;
    }
    young_bytes = 0;
    in_minor = false;

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.minor_count++;
    gc_ctx->sp = sp_top;
}

static void
major_gc(VALUE *sp_top)
{
    in_minor = false;
    // Mark from roots through everything.
    for (VALUE *p = gc_ctx->env; p < sp_top; p++) mark_value(*p);
    process_gray();

    // Sweep young: promote marked (KEEP marked bit set — the old-sweep
    // below scans the same list including these just-promoted nodes and
    // needs to see them as marked).  Free unmarked.
    {
        GCHeader *h = young_head.next;
        while (h != &young_head) {
            GCHeader *next = h->next;
            if (h->marked) promote(h, /*clear_marked=*/false);
            else           free_unlink(h);
            h = next;
        }
    }
    // Sweep old: clear marked on survivors (including the just-promoted
    // young), free unmarked.
    {
        GCHeader *h = old_head.next;
        while (h != &old_head) {
            GCHeader *next = h->next;
            if (h->marked) {
                h->marked = false;
            } else {
                old_bytes -= h->size;
                free_unlink(h);
            }
            h = next;
        }
    }
    young_bytes = 0;

    baruby_gc_stats.gc_count++;
    baruby_gc_stats.major_count++;
    gc_ctx->sp = sp_top;
}

void
baruby_gc_collect(VALUE *sp_top)
{
    major_gc(sp_top);
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return baruby_gc_stats.heap_bytes;  }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }
size_t baruby_gc_minor_count(void) { return baruby_gc_stats.minor_count; }
size_t baruby_gc_major_count(void) { return baruby_gc_stats.major_count; }
