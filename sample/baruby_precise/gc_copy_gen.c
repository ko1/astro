// gc_copy_gen.c — backend #6: copying GC with nursery + tenured semi-space.
//
// Layout:
//   - Nursery: one bump region (16 MiB).  All new allocs go here.
//   - Tenured: two mmap'd semi-space regions (256 MiB each).  Promoted
//     objects live here.  Major GC alternates between them.
//
// Minor GC:
//   1. Scan roots c->env..sp_top — for each young VALUE, forward to tenured.
//   2. Scan dirty tenured (remset proxy) — for each young VALUE, forward.
//   3. Cheney scan-loop over freshly-tenured objects, forwarding their refs.
//   4. Reset nursery_top.
//
// Major GC:
//   1. Cheney over from-tenured → to-tenured.
//   2. Also forwards anything in nursery (= promote first).
//   3. Swap active tenured.
//   4. Reset nursery_top.
//
// Promotion: on first survival.  Simplest and matches mark_gen.
//
// Write barrier: caller invokes aro_gc_wb() on every heap-pointer write.
// If holder is old, set holder.dirty.  Minor GC scans dirty tenured.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "context.h"
#include "astro_debug.h"
#include "gc.h"

#define NURSERY_BYTES  ((size_t)16u  << 20)    /* 16 MiB (tuning knob, not program limit) */
#define TENURED_BYTES  ARO_GC_REGION_VIRT_BYTES /* 64 GiB virtual per semispace, lazy-paged */
#define ALIGN8(n)      (((n) + 7u) & ~(size_t)7u)

typedef struct GCHeader {
    uint32_t kind;
    uint32_t size;
    void    *fwd;       // NULL → live; non-NULL → forwarding pointer
    bool     old;       // false → nursery; true → tenured
    bool     dirty;     // tenured object written to since last minor (remset entry)
    // 6 bytes padding to keep payload 8-aligned
} GCHeader;

static char *nursery_base = NULL;
static char *nursery_top  = NULL;
static char *nursery_end  = NULL;

static char *tenured_base = NULL;
static char *tenured_top  = NULL;
static char *tenured_end  = NULL;
static char *tenured_alt_base = NULL;  // the "other" tenured region for major Cheney

static CTX *gc_ctx = NULL;
static VALUE *sp_high_water = NULL;

// Explicit remembered set: tenured objects that have been written to since
// the last minor GC.  Without this, every minor would scan the whole
// tenured region looking for dirty bits.  Reset by both minor (after
// processing) and major (full trace makes it obsolete).
static GCHeader **remset_buf  = NULL;
static size_t     remset_cnt  = 0;
static size_t     remset_capa = 0;

AroGcStats aro_gc_stats = {0, 0, 0, 0, 0, 0.0, 0.0};
int aro_gc_stress = 0;
const char *aro_gc_backend_name = "copy_gen";

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

    tenured_base     = mmap_region(TENURED_BYTES);
    tenured_top      = tenured_base;
    tenured_end      = tenured_base + TENURED_BYTES;
    tenured_alt_base = mmap_region(TENURED_BYTES);

    if (getenv("BARUBY_GC_STRESS")) {
        aro_gc_stress = 1;
        fprintf(stderr, "[baruby_gc=copy_gen] STRESS mode: collect on every alloc\n");
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
                fprintf(stderr, "baruby_gc=copy_gen: OOM tenured (need %zu)\n", total);
                abort();
            }
        }
        GCHeader *h = (GCHeader *)tenured_top;
        h->kind  = (uint32_t)kind;
        h->size  = (uint32_t)payload_size;
        h->fwd   = NULL;
        h->old   = true;    // direct to tenured = "old" from the start
        h->dirty = false;
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
        }
        if (nursery_top + total > nursery_end) {
            major_gc(sp_top);
            if (nursery_top + total > nursery_end) {
                fprintf(stderr, "baruby_gc=copy_gen: OOM (need %zu)\n", total);
                abort();
            }
        }
    }
    GCHeader *h = (GCHeader *)nursery_top;
    h->kind  = (uint32_t)kind;
    h->size  = (uint32_t)payload_size;
    h->fwd   = NULL;
    h->old   = false;
    h->dirty = false;
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
    AroGcKind kind = (AroGcKind)oldh->kind;
    size_t old_size = oldh->size;
    size_t copy_bytes = old_size < new_size ? old_size : new_size;
    // Root `old` via sp_top[0] so GC sees it and updates the pointer
    // if it moves the source.  Pass sp_top+1 to inner alloc so the
    // slot is in scan range.  This is universal across moving and
    // non-moving GCs: non-moving keeps sp_top[0] unchanged; moving
    // updates it to the new payload location.  Earlier approach
    // (reading oldh->fwd after alloc) had a latent race: if oldh
    // was at nursery_base when minor fired, the next alloc would
    // overwrite oldh's bytes and the fwd field would be gone.
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
    if (hh->old && !hh->dirty) {
        hh->dirty = true;
        remset_push(hh);
    }
}

void
aro_gc_wb_bulk(void *holder, VALUE *dst, const VALUE *src, size_t n)
{
    if (n) memcpy(dst, src, n * sizeof(VALUE));
    if (holder == NULL) return;
    GCHeader *hh = (GCHeader *)holder - 1;
    if (hh->old && !hh->dirty) {
        hh->dirty = true;
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
    ASTRO_ASSERT(to_top + total <= tenured_end);
    GCHeader *newh = (GCHeader *)to_top;
    memcpy(newh, oldh, total);
    newh->fwd   = NULL;
    newh->old   = true;
    newh->dirty = false;
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
    switch ((AroGcKind)h->kind) {
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

    // (1) Roots
    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(*p);

    // (2) Dirty tenured (explicit remset): scan only the dirty entries,
    //     forwarding their young pointers.  O(|remset|) — vastly cheaper
    //     than walking the whole tenured region (which on large old heaps
    //     dominated minor GC time).
    for (size_t i = 0; i < remset_cnt; i++) {
        GCHeader *h = remset_buf[i];
        if (h->dirty) {
            process_object(h);
            h->dirty = false;
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

    // (4) Commit: tenured_top advances to to_top; nursery emptied.
    tenured_top = to_top;
    nursery_top = nursery_base;
    in_minor = false;

    aro_gc_stats.gc_count++;
    aro_gc_stats.minor_count++;
    c->sp = sp_top;
    aro_gc_time_end(t0);
}

static void
major_gc(VALUE *sp_top)
{
    struct timespec t0 = aro_gc_time_begin();
    in_minor = false;
    // Drop remset — major's Cheney moves every survivor, so old pointers
    // become stale.  Major's full trace doesn't need the remset anyway.
    remset_cnt = 0;
    // Swap tenured regions: from = current active, to = the other.
    char *new_active_base = tenured_alt_base;
    tenured_alt_base = tenured_base;   // old active becomes the alt for next major
    char *old_active_base = tenured_base;
    char *old_active_top  = tenured_top;

    from_base_cur = old_active_base;
    from_end_cur  = old_active_top;    // tight bound on valid from-tenured objects

    tenured_base = new_active_base;
    tenured_end  = new_active_base + TENURED_BYTES;
    to_base = new_active_base;
    to_top  = new_active_base;

    CTX *c = gc_ctx;

    // High-water-mark zero as in minor_gc.
    if (sp_high_water == NULL || sp_top > sp_high_water) {
        sp_high_water = sp_top;
    } else {
        for (VALUE *p = sp_top; p < sp_high_water; p++) *p = 0;
    }

    // (1) Roots
    for (VALUE *p = c->env; p < sp_top; p++) *p = forward_value(*p);

    // (2) Cheney scan-loop over new tenured.  This processes both promoted
    //     nursery survivors and copied from-tenured objects.
    {
        char *scan = to_base;
        while (scan < to_top) {
            GCHeader *h = (GCHeader *)scan;
            process_object(h);
            scan += sizeof(GCHeader) + ALIGN8(h->size);
        }
    }

    tenured_top = to_top;
    nursery_top = nursery_base;

    (void)old_active_top;   // silence unused-var when ASTRO_DEBUG=0

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
double aro_gc_total_seconds(void) { return aro_gc_stats.total_seconds; }
double aro_gc_max_pause_seconds(void) { return aro_gc_stats.max_pause_seconds; }
