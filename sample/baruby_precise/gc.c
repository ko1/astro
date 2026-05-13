#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "gc.h"

// ----------------------------------------------------------------------------
// Precise mark&sweep, single-threaded.  See gc.h for design overview.
// ----------------------------------------------------------------------------

BarubyGCStats baruby_gc_stats = {0, 0, 0};

// Doubly-linked list of all live objects.
static BarubyGCNode all_objects_head = { &all_objects_head, &all_objects_head, 0, 0 };

// Current CTX (single-threaded; set by baruby_gc_init).
static CTX *gc_ctx;

// Cumulative bytes allocated since the last GC.  When this exceeds the
// threshold, run GC.  Simpler and more robust than tracking live heap
// size (which would require old/new size tracking on every realloc).
static size_t bytes_since_gc = 0;
static size_t gc_threshold = 1u << 22;   // 4 MiB initial

// ----------------------------------------------------------------------------
// Object linkage helpers
// ----------------------------------------------------------------------------

static inline void
gc_list_insert(BarubyGCNode *node)
{
    node->prev = &all_objects_head;
    node->next = all_objects_head.next;
    all_objects_head.next->prev = node;
    all_objects_head.next = node;
}

static inline void
gc_list_remove(BarubyGCNode *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void
baruby_gc_init(CTX *c)
{
    gc_ctx = c;
}

static void gc_collect_internal(VALUE *sp_top);

void *
baruby_gc_alloc(uint32_t kind, size_t payload_size, VALUE *sp_top)
{
    if (bytes_since_gc > gc_threshold) {
        gc_collect_internal(sp_top);
        bytes_since_gc = 0;
    }
    BarubyGCNode *node = (BarubyGCNode *)malloc(sizeof(BarubyGCNode) + payload_size);
    if (!node) { fprintf(stderr, "baruby_gc: OOM (%zu bytes)\n", payload_size); abort(); }
    node->marked = 0;
    node->payload_kind = kind;
    gc_list_insert(node);
    baruby_gc_stats.total_bytes += payload_size;
    baruby_gc_stats.heap_bytes  += payload_size;
    bytes_since_gc += payload_size;
    memset(node + 1, 0, payload_size);
    return (void *)(node + 1);
}

void *
baruby_gc_alloc_payload(size_t size, VALUE *sp_top)
{
    (void)sp_top;
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "baruby_gc: OOM payload (%zu bytes)\n", size); abort(); }
    baruby_gc_stats.total_bytes += size;
    baruby_gc_stats.heap_bytes  += size;
    bytes_since_gc += size;
    return p;
}

void *
baruby_gc_realloc_payload(void *p, size_t new_size, VALUE *sp_top)
{
    (void)sp_top;
    void *np = realloc(p, new_size);
    if (!np) { fprintf(stderr, "baruby_gc: OOM realloc (%zu bytes)\n", new_size); abort(); }
    baruby_gc_stats.total_bytes += new_size;
    bytes_since_gc += new_size;
    return np;
}

void
baruby_gc_collect(VALUE *sp_top)
{
    gc_collect_internal(sp_top);
}

size_t baruby_gc_total_bytes(void) { return baruby_gc_stats.total_bytes; }
size_t baruby_gc_heap_bytes (void) { return baruby_gc_stats.heap_bytes;  }
size_t baruby_gc_count      (void) { return baruby_gc_stats.gc_count;    }

// ----------------------------------------------------------------------------
// Mark phase
// ----------------------------------------------------------------------------

static void mark_value(VALUE v);

static void
mark_object(void *payload)
{
    BarubyGCNode *node = (BarubyGCNode *)payload - 1;
    if (node->marked) return;
    node->marked = 1;

    // Recurse into containers.
    switch (node->payload_kind) {
      case OBJ_ARRAY: {
          BaArray *a = (BaArray *)payload;
          for (uint32_t i = 0; i < a->len; i++) mark_value(a->items[i]);
          break;
      }
      case OBJ_STRING:
          // No outgoing refs (bytes are raw chars).
          break;
      default:
          // Unknown kind — leave it. Shouldn't happen.
          break;
    }
}

static void
mark_value(VALUE v)
{
    if (!IS_PTR(v)) return;     // fixnum / true / false / nil — no GC tracking
    mark_object((void *)v);
}

static void
mark_phase(VALUE *sp_top)
{
    // Clear all marks.
    for (BarubyGCNode *n = all_objects_head.next; n != &all_objects_head; n = n->next) {
        n->marked = 0;
    }

    // Walk the VALUE stack: c->env .. sp_top (= current scratch top).
    CTX *c = gc_ctx;
    for (VALUE *p = c->env; p < sp_top; p++) {
        mark_value(*p);
    }

    // Walk callcache.body? cc->body is an AST NODE = immortal, not a VALUE — skip.
    // Function table bodies are AST NODEs = immortal — skip.
    //
    // (If we later add inline-cached VALUEs (e.g. last_klass) they'd be
    // additional roots here.)
}

// ----------------------------------------------------------------------------
// Sweep phase
// ----------------------------------------------------------------------------

static void
gc_finalize_object(BarubyGCNode *node)
{
    void *payload = (void *)(node + 1);
    switch (node->payload_kind) {
      case OBJ_ARRAY: {
          BaArray *a = (BaArray *)payload;
          if (a->items) {
              // Approximate: don't track exact bytes — use capa for stats only.
              baruby_gc_stats.heap_bytes -= sizeof(VALUE) * a->capa;
              free(a->items);
          }
          baruby_gc_stats.heap_bytes -= sizeof(BaArray);
          break;
      }
      case OBJ_STRING: {
          BaString *s = (BaString *)payload;
          if (s->bytes) {
              baruby_gc_stats.heap_bytes -= s->capa;
              free(s->bytes);
          }
          baruby_gc_stats.heap_bytes -= sizeof(BaString);
          break;
      }
      default: break;
    }
    free(node);
}

static void
sweep_phase(void)
{
    BarubyGCNode *n = all_objects_head.next;
    while (n != &all_objects_head) {
        BarubyGCNode *next = n->next;
        if (!n->marked) {
            gc_list_remove(n);
            gc_finalize_object(n);
        }
        n = next;
    }
}

// ----------------------------------------------------------------------------
// Driver
// ----------------------------------------------------------------------------

static void
gc_collect_internal(VALUE *sp_top)
{
    gc_ctx->sp = sp_top;
    mark_phase(sp_top);
    sweep_phase();
    baruby_gc_stats.gc_count++;
    // Threshold stays at the initial value (4 MiB).  A real
    // implementation would grow based on retained-live-set size, but
    // tracking that needs old/new size accounting on every realloc,
    // which we don't do in this MVP.
}
