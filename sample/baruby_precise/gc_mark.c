// gc_mark.c — backend #2: non-moving mark & sweep.
//
// Every alloc is a libc malloc with a 32-byte header prepended.  All live
// objects are linked into a doubly-linked list via prev/next so sweep can
// walk them and free unmarked ones.
//
// Mark phase: scan the VALUE stack c->env..sp_top; for each IS_PTR(v) value,
// recover the GCHeader by `(GCHeader*)v - 1`, mark it, and push to a gray
// queue.  Process the gray queue: each item dispatches on `kind` to mark its
// outgoing references.  The items / bytes payloads are themselves linked
// objects (KIND_PAYLOAD_VAL / _BYTE) so they are marked the same way.
//
// Sweep phase: walk the list, free unmarked objects, clear marked bit on
// the rest.
//
// Stress mode: collect on every alloc (threshold = 0).  No mprotect tricks
// because objects are not moved.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

typedef struct GCHeader {
    struct GCHeader *prev, *next;
    uint32_t  kind;
    uint32_t  size;
    bool      marked;
} GCHeader;

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

static GCHeader head_node;             // sentinel of live object list
static size_t   bytes_since_gc = 0;
// Adaptive threshold: after each GC, reset to max(MIN, 2 * live_bytes).
// Without this, mark&sweep on a 200 MB live heap with a fixed 4 MB
// threshold triggers ~50 GCs (each O(heap)), giving 89% GC time on
// binary_trees.  Adaptive keeps total GC work near O(allocation),
// not O(allocation × live).
#define GC_THRESHOLD_MIN     (4u * 1024u * 1024u)
#define GC_THRESHOLD_FACTOR  2
static size_t   gc_threshold   = GC_THRESHOLD_MIN;
static CTX     *gc_ctx         = NULL;

// Gray work list for iterative tracing.
static GCHeader **gray_buf  = NULL;
static size_t     gray_cnt  = 0;
static size_t     gray_capa = 0;

BarubyGCStats baruby_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0};
int baruby_gc_stress = 0;
const char *baruby_gc_backend_name = "mark";

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
    head_node.prev = head_node.next = &head_node;
    if (getenv("BARUBY_GC_STRESS")) {
        baruby_gc_stress = 1;
        gc_threshold = 0;
        fprintf(stderr, "[baruby_gc=mark] STRESS mode: collect on every alloc\n");
    }
}

// Allocate a fresh object, link into list head.
static GCHeader *
list_alloc(BarubyGCKind kind, size_t payload_size)
{
    size_t aligned = ALIGN8(payload_size);
    GCHeader *h = (GCHeader *)malloc(sizeof(GCHeader) + aligned);
    if (!h) { fprintf(stderr, "baruby_gc=mark: OOM\n"); abort(); }
    h->kind   = (uint32_t)kind;
    h->size   = (uint32_t)payload_size;
    h->marked = false;
    h->prev   = &head_node;
    h->next   = head_node.next;
    head_node.next->prev = h;
    head_node.next       = h;
    return h;
}

static void gc_collect_internal(VALUE *sp_top);

void *
baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top)
{
    ASTRO_ASSERT(kind == KIND_OBJ_ARRAY || kind == KIND_OBJ_STRING ||
                 kind == KIND_PAYLOAD_VAL);
    if (baruby_gc_stress || bytes_since_gc + payload_size > gc_threshold) {
        gc_collect_internal(sp_top);
    }
    GCHeader *h = list_alloc(kind, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    memset(payload, 0, ALIGN8(payload_size));
    bytes_since_gc += payload_size;
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top)
{
    if (baruby_gc_stress || bytes_since_gc + payload_size > gc_threshold) {
        gc_collect_internal(sp_top);
    }
    GCHeader *h = list_alloc(KIND_PAYLOAD_BYTE, payload_size);
    void *payload = (void *)(h + 1);
    ASTRO_ASSERT(((uintptr_t)payload & 7u) == 0);
    bytes_since_gc += payload_size;
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    return payload;
}

void *
baruby_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top)
{
    if (!old) {
        return baruby_gc_alloc(KIND_PAYLOAD_VAL, new_size, sp_top);
    }
    GCHeader *oldh = (GCHeader *)old - 1;
    BarubyGCKind kind = (BarubyGCKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;

    // Root `old` via sp_top[0] so the GC inside the alloc keeps it
    // marked.  Non-moving GC: sp_top[0] unchanged after GC.  Uniform
    // with moving-GC realloc_payload — caller doesn't need to know.
    sp_top[0] = (VALUE)old;
    void *newp = (kind == KIND_PAYLOAD_BYTE)
        ? baruby_gc_alloc_byte(new_size, sp_top + 1)
        : baruby_gc_alloc(kind, new_size, sp_top + 1);
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
    if (h->marked) return;
    h->marked = true;
    gray_push(h);
}

static void
process_gray(void)
{
    while (gray_cnt > 0) {
        GCHeader *h = gray_buf[--gray_cnt];
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
            ASTRO_ASSERT(0 && "process_gray: unknown kind");
        }
    }
}

// ---------------------------------------------------------------------------
// Sweep phase
// ---------------------------------------------------------------------------

static void
sweep(void)
{
    GCHeader *h = head_node.next;
    while (h != &head_node) {
        GCHeader *next = h->next;
        if (h->marked) {
            h->marked = false;
        } else {
            h->prev->next = h->next;
            h->next->prev = h->prev;
            baruby_gc_stats.heap_bytes -= h->size;
            free(h);
        }
        h = next;
    }
}

static void
gc_collect_internal(VALUE *sp_top)
{
    struct timespec t0 = baruby_gc_time_begin();
    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) mark_value(*p);
    process_gray();
    sweep();

    baruby_gc_stats.gc_count++;
    bytes_since_gc = 0;
    // Re-tune threshold based on post-sweep live size.  stress mode
    // overrides with 0 so we keep firing every alloc.
    if (!baruby_gc_stress) {
        size_t live = baruby_gc_stats.heap_bytes;
        size_t next = live * GC_THRESHOLD_FACTOR;
        gc_threshold = next < GC_THRESHOLD_MIN ? GC_THRESHOLD_MIN : next;
    }
    c->sp = sp_top;
    baruby_gc_time_end(t0);
}

void
baruby_gc_collect(VALUE *sp_top)
{
    gc_collect_internal(sp_top);
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return baruby_gc_stats.heap_bytes;  }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }
size_t baruby_gc_minor_count(void) { return baruby_gc_stats.minor_count; }
size_t baruby_gc_major_count(void) { return baruby_gc_stats.major_count; }
double baruby_gc_total_seconds(void) { return baruby_gc_stats.total_seconds; }
double baruby_gc_max_pause_seconds(void) { return baruby_gc_stats.max_pause_seconds; }
