/*
 * value.c — JSON value model for nuq.
 *
 * VALUE is a tagged 64-bit integer (see context.h):
 *   - low bit 1: 62-bit signed fixnum
 *   - low bit 0: pointer to `struct nuq_obj` (one of: null, bool, double,
 *     string, array, object).  null/true/false are statically-allocated
 *     singletons so equality and printing are constant-time.
 *
 * Memory: Boehm GC (libgc).  Strings store raw bytes (UTF-8 in / out
 * but we don't validate); the `bytes` field always has a NUL terminator
 * past the last byte for cheap C-string interop.
 */
#include "context.h"

struct nuq_obj NUQ_NULL_OBJ      = { .type = NUQ_T_NULL };
struct nuq_obj NUQ_NULL_ERR_OBJ  = { .type = NUQ_T_NULL };     /* distinct ptr */
struct nuq_obj NUQ_TRUE_OBJ  = { .type = NUQ_T_BOOL, .b = true };
struct nuq_obj NUQ_FALSE_OBJ = { .type = NUQ_T_BOOL, .b = false };

/* ---- per-run arena with copying GC ----
 *
 * Bump-pointer alloc through a chunk list.  When the from-space
 * exceeds `NUQ_GC_THRESHOLD`, an in-run minor collection (Cheney's
 * algorithm) copies live objects to a fresh to-space.  Roots:
 *   - CTX state (input / pool / var_stack / error / funcs[].var_snap)
 *   - Transient `nuq_gc_roots[]` stack: helpers pin their VALUE locals
 *     across allocator calls, GC walks + updates these so post-GC
 *     C code can re-fetch raw `struct nuq_obj *` from VALUE.
 *
 * Forwarding: copied obj's old slot has type set to NUQ_T_FORWARD with
 * the new location stored in the union.  Buffers (items[] / keys[] /
 * vals[] / idx[] / str.bytes) are copied as part of their owning obj's
 * forwarding step, allocated contiguously after the new obj header so
 * Cheney scan can walk obj-by-obj.  Boehm-managed objects (literals,
 * AST kname_value, --argjson values, module data) are detected by
 * arena membership and skipped. */

bool  nuq_alloc_perm = true;     /* startup: parse / setup is permanent */
char *nuq_arena_cur  = NULL;
char *nuq_arena_end  = NULL;

VALUE *nuq_gc_roots[NUQ_GC_ROOTS_CAP];

void
nuq_gc_push_overflow(void)
{
    fprintf(stderr,
            "nuq: GC root pin stack overflow (>%u). "
            "This is a bug — a helper is pinning without unpinning.\n",
            (unsigned)NUQ_GC_ROOTS_CAP);
    abort();
}
size_t nuq_gc_roots_top = 0;
struct nuq_gc_arr nuq_gc_arrs[NUQ_GC_ARR_CAP];
size_t nuq_gc_arrs_top = 0;
int    nuq_gc_defer = 0;

#define NUQ_ARENA_CHUNK   (1u << 20)   /* 1 MiB chunks */
#define NUQ_GC_THRESHOLD  (16u << 20)  /* initial GC trigger at 16 MB */
/* Next-trigger threshold; bumped after each GC to ~2x the surviving
 * live set so workloads with large persistent state (e.g. `[paths]`
 * collecting 1M paths) don't enter a quadratic copy cycle where every
 * alloc-bump immediately re-fires GC. */
static size_t arena_gc_threshold = NUQ_GC_THRESHOLD;

struct nuq_chunk {
    struct nuq_chunk *next;
    size_t            capa;        /* size of `data` */
    size_t            used;        /* bytes used (0 if currently active) */
    char              data[];
};

static struct nuq_chunk *arena_first   = NULL;
static struct nuq_chunk *arena_current = NULL;
static size_t            arena_total   = 0;
/* Spare list — chunks recycled after reset/GC. */
static struct nuq_chunk *arena_spare   = NULL;

/* In-flight GC state. */
static struct nuq_chunk *gc_to_first    = NULL;
static struct nuq_chunk *gc_to_current  = NULL;
static char             *gc_to_cur      = NULL;
static char             *gc_to_end      = NULL;
static size_t            gc_to_total    = 0;
static bool              gc_in_progress = false;

extern struct CTX_struct *nuq_active_ctx;

static struct nuq_chunk *
arena_take_chunk(size_t bytes)
{
    if (bytes < NUQ_ARENA_CHUNK) bytes = NUQ_ARENA_CHUNK;
    /* Reuse from spare if any chunk is large enough.  We keep at most
     * `NUQ_SPARE_KEEP` spare chunks across GC cycles so growing-acc
     * workloads can bring chunk count down between cycles instead of
     * accumulating progressively-larger leftovers. */
    struct nuq_chunk **slot = &arena_spare;
    while (*slot) {
        if ((*slot)->capa >= bytes) {
            struct nuq_chunk *c = *slot;
            *slot = c->next;
            c->next = NULL;
            c->used = 0;
            return c;
        }
        slot = &(*slot)->next;
    }
    /* Plain malloc — Boehm doesn't need to see arena contents.  All
     * Boehm-managed values (literals, --arg*, module data, AST) stay
     * reachable from their own GC-rooted side tables.  Our copying GC
     * relocates arena→arena pointers; arena→Boehm references are
     * still valid because the Boehm targets aren't going anywhere. */
    struct nuq_chunk *c = (struct nuq_chunk *)malloc(
        sizeof(struct nuq_chunk) + bytes);
    if (!c) abort();
    c->next = NULL;
    c->capa = bytes;
    c->used = 0;
    return c;
}

#define NUQ_SPARE_KEEP 3
static void
arena_trim_spare(void)
{
    /* After GC, free everything beyond a small reserve so memory
     * tracks live working set, not high-water mark. */
    struct nuq_chunk *cur = arena_spare;
    int kept = 0;
    while (cur && kept < NUQ_SPARE_KEEP) { cur = cur->next; kept++; }
    while (cur) {
        struct nuq_chunk *next = cur->next;
        free(cur);
        cur = next;
    }
    /* Also drop the kept tail beyond NUQ_SPARE_KEEP — done by
     * truncating the list. */
    cur = arena_spare;
    for (int i = 0; cur && i < NUQ_SPARE_KEEP - 1; i++) cur = cur->next;
    if (cur) cur->next = NULL;
}

static bool
in_arena(const void *p)
{
    if (!p) return false;
    /* During GC, `arena_first` points at from-space. */
    for (struct nuq_chunk *c = arena_first; c; c = c->next) {
        const char *cp = (const char *)p;
        if (cp >= c->data && cp < c->data + c->capa) return true;
    }
    return false;
}

/* Forward declaration. */
static void nuq_arena_collect(void);

void *
nuq_arena_alloc_slow(size_t sz)
{
    /* Trigger GC if from-space crossed threshold — checked BEFORE the
     * big-alloc branch so deeply-recursive `acc + [$i]` style code
     * with growing arrays doesn't bypass the GC trigger by always
     * taking the dedicated-chunk path. */
    if (!gc_in_progress && nuq_gc_defer == 0 && nuq_active_ctx && arena_total > arena_gc_threshold) {
        nuq_arena_collect();
        char *p = nuq_arena_cur;
        char *next = p + sz;
        if (next <= nuq_arena_end) {
            nuq_arena_cur = next;
            return p;
        }
        /* Fall through to chunk grow / big alloc on the post-GC arena. */
    }
    /* Big alloc: dedicated chunk. */
    if (sz > NUQ_ARENA_CHUNK / 2) {
        struct nuq_chunk *c = arena_take_chunk(sz);
        c->next = arena_first;
        arena_first = c;
        if (!arena_current) arena_current = c;
        arena_total += c->capa;
        return c->data;
    }
    /* Take a fresh chunk — all chunks count NUQ_ARENA_CHUNK bytes
     * toward the GC-trigger threshold (we're past the bump pointer
     * of the previous chunk, so the previous chunk is full enough). */
    if (arena_current && arena_current->next) {
        arena_current = arena_current->next;
    } else {
        struct nuq_chunk *c = arena_take_chunk(NUQ_ARENA_CHUNK);
        if (arena_current) arena_current->next = c;
        else arena_first = c;
        arena_current = c;
    }
    nuq_arena_cur = arena_current->data;
    nuq_arena_end = nuq_arena_cur + arena_current->capa;
    arena_total += arena_current->capa;
    char *p = nuq_arena_cur;
    nuq_arena_cur += sz;
    return p;
}

void
nuq_arena_reset(void)
{
    /* End-of-run wholesale reset: chunks become spares. */
    if (arena_first) {
        struct nuq_chunk *tail = arena_first;
        while (tail->next) tail = tail->next;
        tail->next = arena_spare;
        arena_spare = arena_first;
    }
    arena_first = NULL;
    arena_current = NULL;
    nuq_arena_cur = NULL;
    nuq_arena_end = NULL;
    arena_total = 0;
    arena_gc_threshold = NUQ_GC_THRESHOLD;
    nuq_gc_roots_top = 0;
    nuq_gc_arrs_top = 0;
    nuq_gc_defer = 0;
}

/* ---- copying GC ---- */

static void *
gc_to_alloc(size_t sz)
{
    sz = (sz + 7) & ~(size_t)7;
    if (gc_to_cur + sz <= gc_to_end) {
        char *p = gc_to_cur;
        gc_to_cur += sz;
        gc_to_total += sz;
        return p;
    }
    /* Move on to a new chunk; freeze the previous chunk's used size
     * so the Cheney scan walks only the populated portion. */
    if (gc_to_current) {
        gc_to_current->used = (size_t)(gc_to_cur - gc_to_current->data);
    }
    size_t need = sz > NUQ_ARENA_CHUNK ? sz : NUQ_ARENA_CHUNK;
    struct nuq_chunk *c = arena_take_chunk(need);
    c->next = NULL;
    c->used = 0;
    if (gc_to_current) gc_to_current->next = c;
    else gc_to_first = c;
    gc_to_current = c;
    gc_to_cur = c->data + sz;
    gc_to_end = c->data + c->capa;
    gc_to_total += sz;
    return c->data;
}

static void
gc_forward_value(VALUE *vp)
{
    VALUE v = *vp;
    if (NUQ_IS_FIX(v)) return;
    struct nuq_obj *o = NUQ_PTR(v);
    if (!o) return;
    if (!in_arena(o)) return;     /* Boehm-managed or static singleton */

    if (o->type == NUQ_T_FORWARD) {
        *vp = NUQ_OBJ_VAL(o->forward);
        return;
    }

    /* Compute total to-space footprint — must be allocated in a single
     * gc_to_alloc call so Cheney scan can walk obj-then-buffers as a
     * contiguous block.  Splitting across chunks would put a buffer
     * at a chunk start where the scan expects an obj header. */
    size_t obj_sz = (sizeof(*o) + 7) & ~(size_t)7;
    size_t buf_sz = 0;
    switch (o->type) {
      case NUQ_T_STRING:
        buf_sz = (o->str.len + 1 + 7) & ~(size_t)7;
        break;
      case NUQ_T_ARRAY:
        if (o->arr.items != o->arr.inline_buf)
            buf_sz = (o->arr.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        break;
      case NUQ_T_OBJECT:
        buf_sz = (o->obj.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        buf_sz += (o->obj.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        if (o->obj.idx)
            buf_sz += (((size_t)o->obj.idx_mask + 1) * sizeof(uint32_t) + 7) & ~(size_t)7;
        break;
      default: break;
    }
    char *block = (char *)gc_to_alloc(obj_sz + buf_sz);
    struct nuq_obj *new_o = (struct nuq_obj *)block;
    *new_o = *o;
    char *bp = block + obj_sz;

    switch (o->type) {
      case NUQ_T_STRING: {
        size_t n = o->str.len + 1;
        memcpy(bp, o->str.bytes, n);
        new_o->str.bytes = bp;
        bp += (n + 7) & ~(size_t)7;
        break;
      }
      case NUQ_T_ARRAY: {
        if (o->arr.items == o->arr.inline_buf) {
            new_o->arr.items = new_o->arr.inline_buf;
        } else {
            size_t n = o->arr.capa * sizeof(VALUE);
            memcpy(bp, o->arr.items, o->arr.len * sizeof(VALUE));
            new_o->arr.items = (VALUE *)bp;
            bp += (n + 7) & ~(size_t)7;
        }
        break;
      }
      case NUQ_T_OBJECT: {
        size_t kn = o->obj.capa * sizeof(VALUE);
        memcpy(bp, o->obj.keys, o->obj.len * sizeof(VALUE));
        new_o->obj.keys = (VALUE *)bp;
        bp += (kn + 7) & ~(size_t)7;
        memcpy(bp, o->obj.vals, o->obj.len * sizeof(VALUE));
        new_o->obj.vals = (VALUE *)bp;
        bp += (kn + 7) & ~(size_t)7;
        if (o->obj.idx) {
            size_t in = ((size_t)o->obj.idx_mask + 1) * sizeof(uint32_t);
            memcpy(bp, o->obj.idx, in);
            new_o->obj.idx = (uint32_t *)bp;
            bp += (in + 7) & ~(size_t)7;
        }
        break;
      }
      default: break;
    }

    /* Install forwarding. */
    o->type = NUQ_T_FORWARD;
    o->forward = new_o;
    *vp = NUQ_OBJ_VAL(new_o);
}

static void
gc_scan_obj(struct nuq_obj *o)
{
    switch (o->type) {
      case NUQ_T_ARRAY:
        for (size_t i = 0; i < o->arr.len; i++)
            gc_forward_value(&o->arr.items[i]);
        break;
      case NUQ_T_OBJECT:
        for (size_t i = 0; i < o->obj.len; i++) {
            gc_forward_value(&o->obj.keys[i]);
            gc_forward_value(&o->obj.vals[i]);
        }
        break;
      default: break;
    }
}

static size_t
gc_obj_total_size(const struct nuq_obj *o)
{
    size_t sz = (sizeof(*o) + 7) & ~(size_t)7;
    switch (o->type) {
      case NUQ_T_STRING:
        sz += (o->str.len + 1 + 7) & ~(size_t)7;
        break;
      case NUQ_T_ARRAY:
        if (o->arr.items != o->arr.inline_buf)
            sz += (o->arr.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        break;
      case NUQ_T_OBJECT:
        sz += (o->obj.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        sz += (o->obj.capa * sizeof(VALUE) + 7) & ~(size_t)7;
        if (o->obj.idx)
            sz += (((size_t)o->obj.idx_mask + 1) * sizeof(uint32_t) + 7) & ~(size_t)7;
        break;
      default: break;
    }
    return sz;
}

static void
nuq_arena_collect(void)
{
    if (gc_in_progress || !nuq_active_ctx) return;
    gc_in_progress = true;
    if (getenv("NUQ_GC_TRACE")) {
        static int cnt = 0;
        fprintf(stderr, "[gc#%d] from=%zu MB\n", ++cnt, arena_total >> 20);
    }

    /* Stash the from-space, set up empty to-space. */
    struct nuq_chunk *from_first = arena_first;
    /* `in_arena` checks against `arena_first`; we keep it pointing
     * at from-space for the duration of forwarding. */
    /* arena_first stays = from_first */
    gc_to_first = NULL;
    gc_to_current = NULL;
    gc_to_cur = NULL;
    gc_to_end = NULL;
    gc_to_total = 0;

    struct CTX_struct *c = nuq_active_ctx;

    /* Roots: CTX. */
    gc_forward_value(&c->input);
    gc_forward_value(&c->error);
    for (size_t i = 0; i < c->pool_top; i++)
        gc_forward_value(&c->pool[i]);
    for (size_t i = 0; i < c->var_top; i++)
        gc_forward_value(&c->var_stack[i].value);
    for (size_t i = 0; i < c->func_cnt; i++) {
        struct nuq_func_def *fd = c->funcs[i];
        if (fd && fd->var_snap) {
            for (size_t j = 0; j < fd->var_snap_cnt; j++)
                gc_forward_value(&fd->var_snap[j].value);
        }
    }

    /* Roots: transient C-helper pins. */
    for (size_t i = 0; i < nuq_gc_roots_top; i++)
        gc_forward_value(nuq_gc_roots[i]);
    /* Roots: transient pinned arrays (NODE_DEF snapshot buffers). */
    for (size_t i = 0; i < nuq_gc_arrs_top; i++) {
        struct nuq_gc_arr *a = &nuq_gc_arrs[i];
        for (size_t j = 0; j < a->cnt; j++)
            gc_forward_value(&a->base[j]);
    }

    /* Cheney scan. */
    struct nuq_chunk *scan_chunk = gc_to_first;
    char *scan_ptr = scan_chunk ? scan_chunk->data : NULL;
    while (scan_chunk) {
        for (;;) {
            char *chunk_end = (scan_chunk == gc_to_current)
                              ? gc_to_cur
                              : scan_chunk->data + scan_chunk->used;
            while (scan_ptr < chunk_end) {
                struct nuq_obj *o = (struct nuq_obj *)scan_ptr;
                size_t sz = gc_obj_total_size(o);
                sz = (sz + 7) & ~(size_t)7;
                gc_scan_obj(o);
                scan_ptr += sz;
            }
            if (scan_chunk == gc_to_current && scan_ptr < gc_to_cur) continue;
            break;
        }
        scan_chunk = scan_chunk->next;
        scan_ptr = scan_chunk ? scan_chunk->data : NULL;
    }

    /* Recycle from-space chunks back to spare; trim oversized list
     * after each GC so growing-acc workloads don't pile up unused
     * chunks of stale sizes. */
    if (from_first) {
        struct nuq_chunk *tail = from_first;
        while (tail->next) tail = tail->next;
        tail->next = arena_spare;
        arena_spare = from_first;
    }
    arena_trim_spare();

    /* Promote to-space. */
    arena_first = gc_to_first;
    arena_current = gc_to_current;
    nuq_arena_cur = gc_to_cur;
    nuq_arena_end = gc_to_end;
    arena_total = gc_to_total;

    /* Adaptive threshold: aim for ~2x live so steady-state alloc rate
     * gives one GC per doubling of arena, not per fixed 16 MB. */
    size_t want = gc_to_total * 2;
    if (want < NUQ_GC_THRESHOLD) want = NUQ_GC_THRESHOLD;
    arena_gc_threshold = want;

    gc_to_first = NULL;
    gc_to_current = NULL;
    gc_in_progress = false;
}

static struct nuq_obj *
obj_alloc(enum nuq_type t)
{
    struct nuq_obj *o = (struct nuq_obj *)nuq_value_alloc(sizeof(*o));
    o->type = t;
    return o;
}

VALUE
nuq_make_double(double d)
{
    /* Try fixnum if it's an integer in range. */
    if (LIKELY(d == (double)(int64_t)d)) {
        int64_t i = (int64_t)d;
        if (i >= NUQ_FIX_MIN && i <= NUQ_FIX_MAX) return NUQ_FIX(i);
    }
    struct nuq_obj *o = obj_alloc(NUQ_T_DOUBLE);
    o->dbl = d;
    return NUQ_OBJ_VAL(o);
}

VALUE
nuq_make_int_slow(int64_t v)
{
    if (LIKELY(v >= NUQ_FIX_MIN && v <= NUQ_FIX_MAX)) return NUQ_FIX(v);
    return nuq_make_double((double)v);
}

/* Single-allocation constructors: obj + out-of-line buffers in one
 * `nuq_value_alloc` call so the GC trigger that the call may run is
 * a no-op for partially-constructed objects (there are none — the
 * obj header is created and populated before any other work happens
 * that could fire GC).  Layout matches what GC's to-space copy
 * produces, so Cheney scan can walk obj-then-buffer cleanly. */
#define NUQ_HDR_SZ ((sizeof(struct nuq_obj) + 7u) & ~(size_t)7u)

VALUE
nuq_make_string(const char *s, size_t len)
{
    size_t buf_sz = (len + 1 + 7) & ~(size_t)7;
    char *block = (char *)nuq_value_alloc(NUQ_HDR_SZ + buf_sz);
    struct nuq_obj *o = (struct nuq_obj *)block;
    o->type = NUQ_T_STRING;
    o->str.bytes = block + NUQ_HDR_SZ;
    o->str.len = len;
    memcpy(o->str.bytes, s, len);
    o->str.bytes[len] = '\0';
    return NUQ_OBJ_VAL(o);
}

VALUE
nuq_make_string_take(char *s, size_t len)
{
    /* `s` is presumed to already live in the same arena as the obj
     * we're about to allocate.  After the alloc, s may be stale (GC
     * could fire and not have known to update it).  To stay safe,
     * copy s into the obj's combined buffer.  Marginal cost — repeat
     * / fmt callers that build a buffer up front already alloc
     * separately.  The combined-alloc path is the new contract. */
    size_t buf_sz = (len + 1 + 7) & ~(size_t)7;
    char *block = (char *)nuq_value_alloc(NUQ_HDR_SZ + buf_sz);
    struct nuq_obj *o = (struct nuq_obj *)block;
    o->type = NUQ_T_STRING;
    o->str.bytes = block + NUQ_HDR_SZ;
    o->str.len = len;
    memcpy(o->str.bytes, s, len);
    o->str.bytes[len] = '\0';
    return NUQ_OBJ_VAL(o);
}

VALUE
nuq_make_array(size_t cap)
{
    if (cap <= NUQ_ARR_INLINE) {
        struct nuq_obj *o = (struct nuq_obj *)nuq_value_alloc(sizeof(*o));
        o->type = NUQ_T_ARRAY;
        o->arr.items = o->arr.inline_buf;
        o->arr.capa  = NUQ_ARR_INLINE;
        o->arr.len   = 0;
        return NUQ_OBJ_VAL(o);
    }
    size_t buf_sz = (cap * sizeof(VALUE) + 7) & ~(size_t)7;
    char *block = (char *)nuq_value_alloc(NUQ_HDR_SZ + buf_sz);
    struct nuq_obj *o = (struct nuq_obj *)block;
    o->type = NUQ_T_ARRAY;
    o->arr.items = (VALUE *)(block + NUQ_HDR_SZ);
    o->arr.capa  = cap;
    o->arr.len   = 0;
    return NUQ_OBJ_VAL(o);
}

/* Object hash-index threshold — below this, plain linear scan is
 * faster (cache-friendly, no extra alloc).  Past this, build an
 * open-addressing index keyed by string-hash. */
#define NUQ_OBJ_HASH_MIN 16

static uint32_t
nuq_str_hash(const char *bytes, size_t n)
{
    /* FNV-1a 32-bit. */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)bytes[i]) * 16777619u;
    return h;
}

static inline uint32_t
nuq_key_hash(VALUE k)
{
    const struct nuq_obj *const ks = NUQ_PTR(k);
    return nuq_str_hash(ks->str.bytes, ks->str.len);
}

/* Insert (key_idx_plus_one) into idx[] for hash h, assuming free slot
 * exists.  Used by rebuild and by set on growth. */
static void
obj_idx_put_raw(uint32_t *const idx, uint32_t mask, uint32_t h, uint32_t kpos1)
{
    uint32_t pos = h & mask;
    while (idx[pos] != 0) pos = (pos + 1) & mask;
    idx[pos] = kpos1;
}

/* (Re)build idx[] sized for current len.  Takes VALUE not raw ptr —
 * the alloc may GC; we re-fetch o afterwards. */
static void
obj_idx_rebuild_v(VALUE obj)
{
    NUQ_GC_PIN1(obj);
    struct nuq_obj *o = NUQ_PTR(obj);
    uint32_t cap = 32;
    while ((size_t)cap < o->obj.len * 2) cap *= 2;
    uint32_t *const idx = (uint32_t *)nuq_value_alloc_atomic(cap * sizeof(uint32_t));
    /* Re-fetch o after the alloc (GC may have moved it). */
    o = NUQ_PTR(obj);
    memset(idx, 0, cap * sizeof(uint32_t));
    const uint32_t mask = cap - 1;
    for (size_t i = 0; i < o->obj.len; i++) {
        obj_idx_put_raw(idx, mask, nuq_key_hash(o->obj.keys[i]), (uint32_t)(i + 1));
    }
    o->obj.idx = idx;
    o->obj.idx_mask = mask;
    NUQ_GC_UNPIN(1);
}

/* Look up bytes/len in idx; returns keys[]-index or -1 if not found. */
static int64_t
obj_idx_lookup(const struct nuq_obj *const o, const char *bytes, size_t n, uint32_t h)
{
    const uint32_t *const idx = o->obj.idx;
    const uint32_t mask = o->obj.idx_mask;
    uint32_t pos = h & mask;
    for (;;) {
        const uint32_t slot = idx[pos];
        if (slot == 0) return -1;
        const struct nuq_obj *const ks = NUQ_PTR(o->obj.keys[slot - 1]);
        if (ks->str.len == n && memcmp(ks->str.bytes, bytes, n) == 0)
            return (int64_t)(slot - 1);
        pos = (pos + 1) & mask;
    }
}

VALUE
nuq_make_object(size_t cap)
{
    if (cap < 4) cap = 4;
    size_t kn = (cap * sizeof(VALUE) + 7) & ~(size_t)7;
    char *block = (char *)nuq_value_alloc(NUQ_HDR_SZ + kn + kn);
    struct nuq_obj *o = (struct nuq_obj *)block;
    o->type = NUQ_T_OBJECT;
    o->obj.keys = (VALUE *)(block + NUQ_HDR_SZ);
    o->obj.vals = (VALUE *)(block + NUQ_HDR_SZ + kn);
    o->obj.len = 0;
    o->obj.capa = cap;
    o->obj.idx = NULL;
    o->obj.idx_mask = 0;
    return NUQ_OBJ_VAL(o);
}

/* Slow path for nuq_array_push — invoked when inline fast path
 * (header) hits capa.  arr / v are pinned; we re-fetch `o` after
 * the new-buffer alloc since GC may have moved the array. */
void
nuq_array_push_slow(VALUE arr, VALUE v)
{
    NUQ_GC_PIN2(arr, v);
    struct nuq_obj *o = NUQ_PTR(arr);
    size_t old_capa = o->arr.capa;
    size_t nc = old_capa * 2;
    if (nc < 4) nc = 4;
    VALUE *new_items = (VALUE *)nuq_value_alloc(nc * sizeof(VALUE));
    /* GC may have moved arr; re-fetch.  Either old items[] migrated
     * to to-space (heap-array path) or it was inline_buf inside obj
     * (we then read inline_buf from the relocated obj). */
    o = NUQ_PTR(arr);
    memcpy(new_items, o->arr.items, old_capa * sizeof(VALUE));
    o->arr.items = new_items;
    o->arr.capa = nc;
    o->arr.items[o->arr.len++] = v;
    NUQ_GC_UNPIN(2);
}

VALUE
nuq_array_get(VALUE arr, int64_t idx)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    int64_t len = (int64_t)o->arr.len;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return NUQ_NULL;
    return o->arr.items[idx];
}

size_t
nuq_array_len(VALUE arr)
{
    return NUQ_PTR(arr)->arr.len;
}

void
nuq_object_set(VALUE obj, VALUE key, VALUE val)
{
    NUQ_GC_PIN3(obj, key, val);
    struct nuq_obj *o = NUQ_PTR(obj);
    const struct nuq_obj *ks = NUQ_PTR(key);
    uint32_t h = 0;
    /* All object keys in jq/nuq are strings.  Defensive fallback to
     * linear nuq_eq for anything else. */
    if (UNLIKELY(ks->type != NUQ_T_STRING)) {
        for (size_t i = 0; i < o->obj.len; i++) {
            if (nuq_eq(o->obj.keys[i], key)) {
                o->obj.vals[i] = val;
                NUQ_GC_UNPIN(3);
                return;
            }
        }
        goto append;
    }
    h = nuq_str_hash(ks->str.bytes, ks->str.len);
    if (o->obj.idx != NULL) {
        const int64_t pos = obj_idx_lookup(o, ks->str.bytes, ks->str.len, h);
        if (pos >= 0) {
            o->obj.vals[pos] = val;
            NUQ_GC_UNPIN(3);
            return;
        }
    } else {
        for (size_t i = 0; i < o->obj.len; i++) {
            const struct nuq_obj *const ki = NUQ_PTR(o->obj.keys[i]);
            if (ki->type == NUQ_T_STRING &&
                ki->str.len == ks->str.len &&
                memcmp(ki->str.bytes, ks->str.bytes, ks->str.len) == 0) {
                o->obj.vals[i] = val;
                NUQ_GC_UNPIN(3);
                return;
            }
        }
    }
append:
    if (o->obj.len == o->obj.capa) {
        size_t old_capa = o->obj.capa;
        size_t nc = old_capa * 2;
        /* Allocate fresh keys + vals buffers, copy, repoint o.
         * Each alloc may GC — re-fetch o after each. */
        VALUE *new_keys = (VALUE *)nuq_value_alloc(nc * sizeof(VALUE));
        o = NUQ_PTR(obj);
        memcpy(new_keys, o->obj.keys, old_capa * sizeof(VALUE));
        o->obj.keys = new_keys;
        VALUE *new_vals = (VALUE *)nuq_value_alloc(nc * sizeof(VALUE));
        o = NUQ_PTR(obj);
        memcpy(new_vals, o->obj.vals, old_capa * sizeof(VALUE));
        o->obj.vals = new_vals;
        o->obj.capa = nc;
        /* `ks` cached pointer also needs refresh after the allocs. */
        ks = NUQ_PTR(key);
    }
    o->obj.keys[o->obj.len] = key;
    o->obj.vals[o->obj.len] = val;
    o->obj.len++;
    if (LIKELY(ks->type == NUQ_T_STRING)) {
        if (o->obj.idx == NULL) {
            if (o->obj.len > NUQ_OBJ_HASH_MIN) {
                obj_idx_rebuild_v(obj);
                /* o stale after rebuild call; not used after this. */
            }
        } else {
            if (o->obj.len * 2 > (size_t)o->obj.idx_mask + 1) {
                obj_idx_rebuild_v(obj);
            } else {
                obj_idx_put_raw(o->obj.idx, o->obj.idx_mask, h, (uint32_t)o->obj.len);
            }
        }
    } else {
        o->obj.idx = NULL;
        o->obj.idx_mask = 0;
    }
    NUQ_GC_UNPIN(3);
}

void
nuq_object_set_cstr(VALUE obj, const char *key, VALUE val)
{
    nuq_object_set(obj, nuq_make_string(key, strlen(key)), val);
}

VALUE
nuq_object_get(VALUE obj, VALUE key)
{
    const struct nuq_obj *const o = NUQ_PTR(obj);
    const struct nuq_obj *const ks = NUQ_PTR(key);
    if (UNLIKELY(ks->type != NUQ_T_STRING)) {
        for (size_t i = 0; i < o->obj.len; i++) {
            if (nuq_eq(o->obj.keys[i], key)) return o->obj.vals[i];
        }
        return NUQ_NULL;
    }
    if (o->obj.idx != NULL) {
        const uint32_t h = nuq_str_hash(ks->str.bytes, ks->str.len);
        const int64_t pos = obj_idx_lookup(o, ks->str.bytes, ks->str.len, h);
        return pos >= 0 ? o->obj.vals[pos] : NUQ_NULL;
    }
    for (size_t i = 0; i < o->obj.len; i++) {
        const struct nuq_obj *const ki = NUQ_PTR(o->obj.keys[i]);
        if (ki->type == NUQ_T_STRING &&
            ki->str.len == ks->str.len &&
            memcmp(ki->str.bytes, ks->str.bytes, ks->str.len) == 0)
            return o->obj.vals[i];
    }
    return NUQ_NULL;
}

VALUE
nuq_object_get_cstr(VALUE obj, const char *key)
{
    const size_t klen = strlen(key);
    const struct nuq_obj *const o = NUQ_PTR(obj);
    if (o->obj.idx != NULL) {
        const uint32_t h = nuq_str_hash(key, klen);
        const int64_t pos = obj_idx_lookup(o, key, klen, h);
        return pos >= 0 ? o->obj.vals[pos] : NUQ_NULL;
    }
    for (size_t i = 0; i < o->obj.len; i++) {
        const struct nuq_obj *const ks = NUQ_PTR(o->obj.keys[i]);
        if (ks->type == NUQ_T_STRING && ks->str.len == klen &&
            memcmp(ks->str.bytes, key, klen) == 0)
            return o->obj.vals[i];
    }
    return NUQ_NULL;
}

bool
nuq_object_has(VALUE obj, VALUE key)
{
    const struct nuq_obj *const o = NUQ_PTR(obj);
    const struct nuq_obj *const ks = NUQ_PTR(key);
    if (UNLIKELY(ks->type != NUQ_T_STRING)) {
        for (size_t i = 0; i < o->obj.len; i++) {
            if (nuq_eq(o->obj.keys[i], key)) return true;
        }
        return false;
    }
    if (o->obj.idx != NULL) {
        const uint32_t h = nuq_str_hash(ks->str.bytes, ks->str.len);
        return obj_idx_lookup(o, ks->str.bytes, ks->str.len, h) >= 0;
    }
    for (size_t i = 0; i < o->obj.len; i++) {
        const struct nuq_obj *const ki = NUQ_PTR(o->obj.keys[i]);
        if (ki->type == NUQ_T_STRING && ki->str.len == ks->str.len &&
            memcmp(ki->str.bytes, ks->str.bytes, ks->str.len) == 0)
            return true;
    }
    return false;
}

size_t
nuq_object_len(VALUE obj)
{
    return NUQ_PTR(obj)->obj.len;
}

const char *
nuq_string_cstr(VALUE s)
{
    return NUQ_PTR(s)->str.bytes;
}

size_t
nuq_string_len(VALUE s)
{
    return NUQ_PTR(s)->str.len;
}

bool
nuq_eq_slow(VALUE a, VALUE b)
{
    /* Inline `nuq_eq` (in node.h) handled the trivial fastpaths
     * (same VALUE / both fixnum) — anything reaching here is a
     * heterogeneous or heap-comparison. */
    /* numeric coerce: int <-> double */
    if (NUQ_IS_FIX(a) && NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_DOUBLE)
        return (double)NUQ_FIX_VAL(a) == NUQ_PTR(b)->dbl;
    if (NUQ_IS_FIX(b) && NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_DOUBLE)
        return (double)NUQ_FIX_VAL(b) == NUQ_PTR(a)->dbl;
    if (NUQ_IS_FIX(a) || NUQ_IS_FIX(b)) return false;

    struct nuq_obj *oa = NUQ_PTR(a);
    struct nuq_obj *ob = NUQ_PTR(b);
    if (oa->type != ob->type) return false;
    switch (oa->type) {
      case NUQ_T_NULL:   return true;
      case NUQ_T_BOOL:   return oa->b == ob->b;
      case NUQ_T_DOUBLE: return oa->dbl == ob->dbl;
      case NUQ_T_STRING:
        return oa->str.len == ob->str.len &&
               memcmp(oa->str.bytes, ob->str.bytes, oa->str.len) == 0;
      case NUQ_T_ARRAY:
        if (oa->arr.len != ob->arr.len) return false;
        for (size_t i = 0; i < oa->arr.len; i++) {
            if (!nuq_eq(oa->arr.items[i], ob->arr.items[i])) return false;
        }
        return true;
      case NUQ_T_OBJECT:
        /* jq compares objects by key set ignoring order */
        if (oa->obj.len != ob->obj.len) return false;
        for (size_t i = 0; i < oa->obj.len; i++) {
            VALUE k = oa->obj.keys[i];
            if (!nuq_object_has(b, k)) return false;
            if (!nuq_eq(oa->obj.vals[i], nuq_object_get(b, k))) return false;
        }
        return true;
      case NUQ_T_FORWARD: abort(); /* live values can't be forwarding ptrs */
    }
    return false;
}

/* jq type ordering: null < false < true < number < string < array < object */
static int
nuq_type_rank(VALUE v)
{
    if (NUQ_IS_FIX(v)) return 3;
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:   return 0;
      case NUQ_T_BOOL:   return o->b ? 2 : 1;
      case NUQ_T_DOUBLE: return 3;
      case NUQ_T_STRING: return 4;
      case NUQ_T_ARRAY:  return 5;
      case NUQ_T_OBJECT: return 6;
      case NUQ_T_FORWARD: abort();
    }
    return 7;
}

static double
to_double(VALUE v)
{
    if (NUQ_IS_FIX(v)) return (double)NUQ_FIX_VAL(v);
    return NUQ_PTR(v)->dbl;
}

int
nuq_cmp_slow(VALUE a, VALUE b)
{
    /* Inline `nuq_cmp` (in node.h) folded the both-fixnum case. */
    int ra = nuq_type_rank(a), rb = nuq_type_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;

    switch (ra) {
      case 0: return 0;        /* null */
      case 1: case 2: return 0; /* same bool */
      case 3: {
        double da = to_double(a), db = to_double(b);
        return da < db ? -1 : (da > db ? 1 : 0);
      }
      case 4: {
        struct nuq_obj *sa = NUQ_PTR(a), *sb = NUQ_PTR(b);
        size_t m = sa->str.len < sb->str.len ? sa->str.len : sb->str.len;
        int c = memcmp(sa->str.bytes, sb->str.bytes, m);
        if (c) return c < 0 ? -1 : 1;
        if (sa->str.len != sb->str.len)
            return sa->str.len < sb->str.len ? -1 : 1;
        return 0;
      }
      case 5: {
        struct nuq_obj *sa = NUQ_PTR(a), *sb = NUQ_PTR(b);
        size_t m = sa->arr.len < sb->arr.len ? sa->arr.len : sb->arr.len;
        for (size_t i = 0; i < m; i++) {
            int c = nuq_cmp(sa->arr.items[i], sb->arr.items[i]);
            if (c) return c;
        }
        if (sa->arr.len != sb->arr.len)
            return sa->arr.len < sb->arr.len ? -1 : 1;
        return 0;
      }
      case 6: {
        /* jq sorts object keys then compares pairwise */
        VALUE ak = nuq_keys(a, true);
        VALUE bk = nuq_keys(b, true);
        int c = nuq_cmp(ak, bk);
        if (c) return c;
        struct nuq_obj *kao = NUQ_PTR(ak);
        for (size_t i = 0; i < kao->arr.len; i++) {
            VALUE va = nuq_object_get(a, kao->arr.items[i]);
            VALUE vb = nuq_object_get(b, kao->arr.items[i]);
            c = nuq_cmp(va, vb);
            if (c) return c;
        }
        return 0;
      }
    }
    return 0;
}

bool
nuq_truthy_slow(VALUE v)
{
    if (NUQ_IS_FIX(v)) return true;       /* 0 is truthy in jq */
    struct nuq_obj *o = NUQ_PTR(v);
    if (o->type == NUQ_T_NULL) return false;
    if (o->type == NUQ_T_BOOL) return o->b;
    return true;
}

const char *
nuq_type_name(VALUE v)
{
    if (NUQ_IS_FIX(v)) return "number";
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:   return "null";
      case NUQ_T_BOOL:   return "boolean";
      case NUQ_T_DOUBLE: return "number";
      case NUQ_T_STRING: return "string";
      case NUQ_T_ARRAY:  return "array";
      case NUQ_T_OBJECT: return "object";
      case NUQ_T_FORWARD: abort();
    }
    return "unknown";
}

/* Render `<type> (<json-truncated>)` into `dst` (size `n`).
 * Used for jq-style error messages such as:
 *   "Cannot iterate over <descr>"
 *   "Cannot index <type-of-container> with <descr>"
 * jq truncates long renderings with '...'.
 */
size_t
nuq_value_descr(VALUE v, char *dst, size_t n)
{
    extern void nuq_json_print(FILE *, VALUE, int);
    const char *tn = nuq_type_name(v);
    char *json_buf = NULL; size_t jl = 0;
    FILE *fp = open_memstream(&json_buf, &jl);
    nuq_json_print(fp, v, 0);
    fclose(fp);
    /* Truncate the json rendering to keep the message short.  jq cuts
     * after ~24 bytes of CONTENT (string body / number digits) and
     * walks back to a UTF-8 codepoint boundary so multi-byte chars
     * don't break the output.  Strings up to 26 content bytes are
     * shown whole (jq picks a fit-threshold a bit larger than the
     * cut to avoid trivial truncations). */
    bool is_string = (jl > 0 && json_buf[0] == '"');
    /* jq's value-descr cuts numbers at 26 chars (preserving long
     * decimal expansions for error messages) but strings earlier. */
    const size_t fit_lim = is_string ? 28 : 28;
    const size_t trunc_lim = is_string ? 25 : 26;
    char buf[80];
    if (jl <= fit_lim) {
        size_t copy = jl < sizeof(buf) - 1 ? jl : sizeof(buf) - 1;
        memcpy(buf, json_buf, copy);
        buf[copy] = 0;
    } else {
        size_t cut = trunc_lim;
        while (cut > 0 && (((unsigned char)json_buf[cut]) & 0xC0) == 0x80) cut--;
        memcpy(buf, json_buf, cut);
        memcpy(buf + cut, "...", 3);
        if (is_string) { buf[cut + 3] = '"'; buf[cut + 4] = 0; }
        else buf[cut + 3] = 0;
    }
    free(json_buf);
    int w = snprintf(dst, n, "%s (%s)", tn, buf);
    return (w < 0) ? 0 : (size_t)w;
}

VALUE
nuq_length(VALUE v)
{
    if (NUQ_IS_FIX(v)) {
        int64_t i = NUQ_FIX_VAL(v);
        return nuq_make_int(i < 0 ? -i : i);
    }
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:   return NUQ_FIX(0);
      case NUQ_T_BOOL:
        nuq_helper_error("");
        return NUQ_FIX(0);
      case NUQ_T_DOUBLE: {
        double d = o->dbl;
        return nuq_make_double(d < 0 ? -d : d);
      }
      case NUQ_T_STRING: {
        /* jq returns codepoint count, not byte count. */
        int64_t cp = 0;
        for (size_t i = 0; i < o->str.len; ) {
            unsigned char x = (unsigned char)o->str.bytes[i];
            if (x < 0x80) i += 1;
            else if ((x & 0xE0) == 0xC0) i += 2;
            else if ((x & 0xF0) == 0xE0) i += 3;
            else if ((x & 0xF8) == 0xF0) i += 4;
            else i += 1;
            cp++;
        }
        return nuq_make_int(cp);
      }
      case NUQ_T_ARRAY:  return nuq_make_int((int64_t)o->arr.len);
      case NUQ_T_OBJECT: return nuq_make_int((int64_t)o->obj.len);
      case NUQ_T_FORWARD: abort();
    }
    return NUQ_FIX(0);
}

VALUE
nuq_keys(VALUE v, bool sorted)
{
    if (!NUQ_IS_PTR(v)) goto bad;
    struct nuq_obj *o = NUQ_PTR(v);
    if (o->type == NUQ_T_OBJECT) {
        VALUE r = nuq_make_array(o->obj.len);
        for (size_t i = 0; i < o->obj.len; i++)
            nuq_array_push(r, o->obj.keys[i]);
        if (sorted) {
            /* simple insertion sort, n is usually small */
            struct nuq_obj *ro = NUQ_PTR(r);
            for (size_t i = 1; i < ro->arr.len; i++) {
                VALUE x = ro->arr.items[i];
                size_t j = i;
                while (j > 0 && nuq_cmp(ro->arr.items[j-1], x) > 0) {
                    ro->arr.items[j] = ro->arr.items[j-1];
                    j--;
                }
                ro->arr.items[j] = x;
            }
        }
        return r;
    }
    if (o->type == NUQ_T_ARRAY) {
        VALUE r = nuq_make_array(o->arr.len);
        for (size_t i = 0; i < o->arr.len; i++)
            nuq_array_push(r, nuq_make_int((int64_t)i));
        return r;
    }
  bad:
    nuq_helper_error("keys requires object or array, got %s", nuq_type_name(v));
    return nuq_make_array(0);
}

VALUE
nuq_values(VALUE v)
{
    if (!NUQ_IS_PTR(v)) goto bad;
    struct nuq_obj *o = NUQ_PTR(v);
    if (o->type == NUQ_T_OBJECT) {
        VALUE r = nuq_make_array(o->obj.len);
        for (size_t i = 0; i < o->obj.len; i++)
            nuq_array_push(r, o->obj.vals[i]);
        return r;
    }
    if (o->type == NUQ_T_ARRAY) {
        VALUE r = nuq_make_array(o->arr.len);
        for (size_t i = 0; i < o->arr.len; i++)
            nuq_array_push(r, o->arr.items[i]);
        return r;
    }
  bad:
    nuq_helper_error("values requires object or array, got %s", nuq_type_name(v));
    return nuq_make_array(0);
}

VALUE
nuq_clone(VALUE v)
{
    if (NUQ_IS_FIX(v)) return v;
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:
      case NUQ_T_BOOL:
      case NUQ_T_DOUBLE:
      case NUQ_T_STRING:
        return v;             /* immutable */
      case NUQ_T_ARRAY: {
        NUQ_GC_PIN1(v);
        size_t len = o->arr.len;
        VALUE r = nuq_make_array(len);
        /* GC may have fired during make_array — re-fetch raw ptrs from v. */
        struct nuq_obj *src = NUQ_PTR(v);
        struct nuq_obj *dst = NUQ_PTR(r);
        for (size_t i = 0; i < len; i++) dst->arr.items[i] = src->arr.items[i];
        dst->arr.len = len;
        NUQ_GC_UNPIN(1);
        return r;
      }
      case NUQ_T_OBJECT: {
        /* Source keys are already unique — bypass nuq_object_set's
         * collision check.  If the source has an idx[] (len past the
         * threshold), rebuild on the clone too. */
        NUQ_GC_PIN1(v);
        size_t len = o->obj.len;
        bool has_idx = (o->obj.idx != NULL);
        VALUE r = nuq_make_object(len);
        NUQ_GC_PIN1(r);
        /* Re-fetch after make_object. */
        struct nuq_obj *src = NUQ_PTR(v);
        struct nuq_obj *dst = NUQ_PTR(r);
        for (size_t i = 0; i < len; i++) {
            dst->obj.keys[i] = src->obj.keys[i];
            dst->obj.vals[i] = src->obj.vals[i];
        }
        dst->obj.len = len;
        if (has_idx) obj_idx_rebuild_v(r);
        /* dst stale after rebuild — not used below. */
        NUQ_GC_UNPIN(2);
        return r;
      }
      default:
        break;
    }
    return v;
}

/* jq's `contains(b)` semantics:
 *   string  contains string  → b is substring of a
 *   array   contains array   → every b-elem has some matching a-elem
 *                              (where "matching" recurses through contains)
 *   object  contains object  → every key in b exists in a AND
 *                              a[k] contains b[k]
 *   numbers / bools / null    → equality
 *   mismatched types          → false
 */
static int contains_depth = 0;

static bool
nuq_contains_core(VALUE a, VALUE b)
{
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) return a == b;
    if (NUQ_IS_FIX(a) || NUQ_IS_FIX(b)) {
        /* number-double mixed: defer to nuq_eq's coercion. */
        return nuq_eq(a, b);
    }
    struct nuq_obj *oa = NUQ_PTR(a), *ob = NUQ_PTR(b);
    if (oa->type != ob->type) return false;
    switch (oa->type) {
      case NUQ_T_NULL:
      case NUQ_T_BOOL:
      case NUQ_T_DOUBLE:
        return nuq_eq(a, b);
      case NUQ_T_STRING: {
        if (ob->str.len > oa->str.len) return false;
        if (ob->str.len == 0) return true;
        for (size_t i = 0; i + ob->str.len <= oa->str.len; i++)
            if (memcmp(oa->str.bytes + i, ob->str.bytes, ob->str.len) == 0) return true;
        return false;
      }
      case NUQ_T_ARRAY: {
        for (size_t j = 0; j < ob->arr.len; j++) {
            bool found = false;
            for (size_t i = 0; i < oa->arr.len; i++) {
                if (nuq_contains(oa->arr.items[i], ob->arr.items[j])) {
                    found = true; break;
                }
            }
            if (!found) return false;
        }
        return true;
      }
      case NUQ_T_OBJECT: {
        for (size_t i = 0; i < ob->obj.len; i++) {
            VALUE bk = ob->obj.keys[i];
            if (!nuq_object_has(a, bk)) return false;
            VALUE av = nuq_object_get(a, bk);
            if (!nuq_contains(av, ob->obj.vals[i])) return false;
        }
        return true;
      }
      case NUQ_T_FORWARD: abort();
    }
    return false;
}

bool
nuq_contains(VALUE a, VALUE b)
{
    if (++contains_depth > 10000) {
        contains_depth--;
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
            nuq_active_ctx->error = nuq_make_string("Containment check too deep", 26);
        return false;
    }
    bool r = nuq_contains_core(a, b);
    contains_depth--;
    /* Reset on outermost return so a deep error doesn't poison later
     * calls via the static counter. */
    if (contains_depth == 0 && nuq_active_ctx && nuq_active_ctx->error != NUQ_NULL) {
        /* keep error set; counter already at 0 */
    }
    return r;
}

/* ---- arithmetic ops ---- */

static bool
both_numeric(VALUE a, VALUE b)
{
    return (NUQ_IS_FIX(a) || (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_DOUBLE)) &&
           (NUQ_IS_FIX(b) || (NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_DOUBLE));
}

static double
to_double_v(VALUE v)
{
    if (NUQ_IS_FIX(v)) return (double)NUQ_FIX_VAL(v);
    return NUQ_PTR(v)->dbl;
}

/* Linearity-analysis-marked variant of `+` for arrays.
 *
 * The static linearity pass (linearity.c) marks a `node_add` as
 * `node_add_inplace` when it can prove the LHS at this site is
 * unique — i.e. no other live reference to it exists.  Under that
 * guarantee, mutating LHS by appending RHS items is observationally
 * equivalent to allocating a fresh `lhs+rhs` array but avoids the
 * O(|lhs|) copy.  The canonical win is `reduce SRC as $x ([]; . +
 * [$x])` where naive copy is O(N²) and in-place is O(N).
 *
 * Non-array combinations (string+string, object+object, numeric, null
 * absorption) don't have a useful in-place form here — strings need a
 * fresh sized buffer, objects need merge semantics — so we delegate
 * to the regular slow path. */
VALUE
nuq_op_add_inplace(VALUE a, VALUE b)
{
    if (LIKELY(NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_ARRAY &&
               NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_ARRAY)) {
        /* Runtime safety net: only mutate if `a`'s allocation lives
         * in the current per-run arena.  Boehm-managed values (e.g.
         * the input JSON tree) must not be mutated — Boehm doesn't
         * track our arena, so growing the array buffer would silently
         * become unreachable for Boehm and invalid after Cheney GC.
         * The static linearity analysis can't always tell the
         * difference (top-level `. + [x]` has dot_uses=1 in scope but
         * `.` is the Boehm input), so we double-check here. */
        if (LIKELY(in_arena(NUQ_PTR(a)))) {
            NUQ_GC_PIN2(a, b);
            size_t lb = NUQ_PTR(b)->arr.len;
            for (size_t i = 0; i < lb; i++) {
                VALUE item = NUQ_PTR(b)->arr.items[i]; /* refetch */
                nuq_array_push(a, item);
            }
            NUQ_GC_UNPIN(2);
            return a;
        }
    }
    return nuq_op_add_slow(a, b);
}

VALUE
nuq_op_add_slow(VALUE a, VALUE b)
{
    /* Inline `nuq_op_add` (in node.h) folded fix+fix without overflow. */
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_NULL) return b;
    if (NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_NULL) return a;
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b);
        return nuq_make_double((double)la + (double)lb);
    }
    if (both_numeric(a, b)) return nuq_make_double(to_double_v(a) + to_double_v(b));
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_STRING &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_STRING) {
        NUQ_GC_PIN2(a, b);
        size_t la = NUQ_PTR(a)->str.len;
        size_t lb = NUQ_PTR(b)->str.len;
        size_t ln = la + lb;
        /* Combined alloc — obj header + bytes in one block.  Allocating
         * a separate `buf` first and then make_string_take'ing it would
         * race with GC: a GC fired by the second alloc would free the
         * from-space chunk holding buf and our memcpy would read stale
         * memory. */
        size_t buf_sz = (ln + 1 + 7) & ~(size_t)7;
        char *block = (char *)nuq_value_alloc(NUQ_HDR_SZ + buf_sz);
        struct nuq_obj *o = (struct nuq_obj *)block;
        o->type = NUQ_T_STRING;
        o->str.bytes = block + NUQ_HDR_SZ;
        o->str.len = ln;
        /* a/b were pinned — refetch after alloc. */
        memcpy(o->str.bytes,      NUQ_PTR(a)->str.bytes, la);
        memcpy(o->str.bytes + la, NUQ_PTR(b)->str.bytes, lb);
        o->str.bytes[ln] = '\0';
        NUQ_GC_UNPIN(2);
        return NUQ_OBJ_VAL(o);
    }
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_ARRAY &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_ARRAY) {
        NUQ_GC_PIN2(a, b);
        size_t la = NUQ_PTR(a)->arr.len;
        size_t lb = NUQ_PTR(b)->arr.len;
        VALUE r = nuq_make_array(la + lb);
        struct nuq_obj *dst = NUQ_PTR(r);
        struct nuq_obj *oa = NUQ_PTR(a);
        for (size_t i = 0; i < la; i++) dst->arr.items[i] = oa->arr.items[i];
        struct nuq_obj *ob = NUQ_PTR(b);
        for (size_t i = 0; i < lb; i++) dst->arr.items[la + i] = ob->arr.items[i];
        dst->arr.len = la + lb;
        NUQ_GC_UNPIN(2);
        return r;
    }
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_OBJECT &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_OBJECT) {
        NUQ_GC_PIN2(a, b);
        VALUE r = nuq_clone(a);
        NUQ_GC_PIN1(r);
        size_t lb = NUQ_PTR(b)->obj.len;
        for (size_t i = 0; i < lb; i++) {
            VALUE bk = NUQ_PTR(b)->obj.keys[i];
            VALUE bv = NUQ_PTR(b)->obj.vals[i];
            nuq_object_set(r, bk, bv);
        }
        NUQ_GC_UNPIN(3);
        return r;
    }
    {
        char da[80], db[80];
        nuq_value_descr(a, da, sizeof(da));
        nuq_value_descr(b, db, sizeof(db));
        nuq_helper_error("%s and %s cannot be added", da, db);
    }
    return NUQ_NULL;
}

VALUE
nuq_op_sub_slow(VALUE a, VALUE b)
{
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b);
        return nuq_make_double((double)la - (double)lb);
    }
    if (both_numeric(a, b)) return nuq_make_double(to_double_v(a) - to_double_v(b));
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_ARRAY &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_ARRAY) {
        NUQ_GC_PIN2(a, b);
        size_t la = NUQ_PTR(a)->arr.len;
        VALUE r = nuq_make_array(la);
        NUQ_GC_PIN1(r);
        for (size_t i = 0; i < la; i++) {
            VALUE av = NUQ_PTR(a)->arr.items[i];
            bool found = false;
            size_t lb = NUQ_PTR(b)->arr.len;
            for (size_t j = 0; j < lb; j++)
                if (nuq_eq(av, NUQ_PTR(b)->arr.items[j])) { found = true; break; }
            if (!found) nuq_array_push(r, av);
        }
        NUQ_GC_UNPIN(3);
        return r;
    }
    {
        char da[80], db[80];
        nuq_value_descr(a, da, sizeof(da));
        nuq_value_descr(b, db, sizeof(db));
        nuq_helper_error("%s and %s cannot be subtracted", da, db);
    }
    return NUQ_NULL;
}

VALUE
nuq_op_mul_slow(VALUE a, VALUE b)
{
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) {
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b);
        return nuq_make_double((double)la * (double)lb);
    }
    if (both_numeric(a, b)) return nuq_make_double(to_double_v(a) * to_double_v(b));
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_OBJECT &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_OBJECT) {
        static int merge_depth = 0;
        if (++merge_depth > 10001) {
            merge_depth--;
            if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
                nuq_active_ctx->error = nuq_make_string("Object merge too deep", 21);
            return NUQ_NULL;
        }
        NUQ_GC_PIN2(a, b);
        VALUE r = nuq_clone(a);
        NUQ_GC_PIN1(r);
        size_t lb = NUQ_PTR(b)->obj.len;
        for (size_t i = 0; i < lb; i++) {
            /* Read fresh each iter — recursive op_mul / object_set
             * below may have moved b, but the pin updated &b so
             * NUQ_PTR(b) still resolves correctly. */
            VALUE bk = NUQ_PTR(b)->obj.keys[i];
            VALUE bv = NUQ_PTR(b)->obj.vals[i];
            VALUE av = nuq_object_get(r, bk);
            if (NUQ_IS_PTR(av) && NUQ_PTR(av)->type == NUQ_T_OBJECT &&
                NUQ_IS_PTR(bv) && NUQ_PTR(bv)->type == NUQ_T_OBJECT) {
                /* Pin bk across the recursive merge — bk is a transient
                 * VALUE that op_mul can move via GC. */
                NUQ_GC_PIN1(bk);
                VALUE merged = nuq_op_mul(av, bv);
                NUQ_GC_UNPIN(1);
                /* bk now reflects the post-GC location (the pin updated
                 * its slot).  merged is fresh in to-space. */
                nuq_object_set(r, bk, merged);
            } else {
                nuq_object_set(r, bk, bv);
            }
        }
        NUQ_GC_UNPIN(3);
        merge_depth--;
        return r;
    }
    /* Allow string * number (and number * string symmetrically) so jq's
     * `"abc" * 3` semantics work for floats and bool conversions. */
    {
        VALUE str = NUQ_NULL, num = NUQ_NULL;
        if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_STRING &&
            (NUQ_IS_FIX(b) || (NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_DOUBLE))) {
            str = a; num = b;
        } else if (NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_STRING &&
                   (NUQ_IS_FIX(a) || (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_DOUBLE))) {
            str = b; num = a;
        }
        if (str != NUQ_NULL) {
            double d = NUQ_IS_FIX(num) ? (double)NUQ_FIX_VAL(num) : NUQ_PTR(num)->dbl;
            if (isnan(d) || d < 0) return NUQ_NULL;
            int64_t n = (int64_t)d;     /* jq truncates toward zero */
            if (n == 0) return nuq_make_string("", 0);
            NUQ_GC_PIN1(str);
            size_t slen = NUQ_PTR(str)->str.len;
            if ((slen > 0 && (size_t)n > (size_t)(SIZE_MAX / slen)) ||
                (slen * (size_t)n) > (size_t)(64 * 1024 * 1024)) {
                NUQ_GC_UNPIN(1);
                nuq_helper_error("Repeat string result too long");
                return NUQ_NULL;
            }
            size_t L = slen * (size_t)n;
            char *buf = (char *)nuq_value_alloc_atomic(L + 1);
            const char *src = NUQ_PTR(str)->str.bytes;
            for (int64_t i = 0; i < n; i++) memcpy(buf + i * slen, src, slen);
            buf[L] = '\0';
            VALUE r = nuq_make_string_take(buf, L);
            NUQ_GC_UNPIN(1);
            return r;
        }
    }
    {
        char da[80], db[80];
        nuq_value_descr(a, da, sizeof(da));
        nuq_value_descr(b, db, sizeof(db));
        nuq_helper_error("%s and %s cannot be multiplied", da, db);
    }
    return NUQ_NULL;
}

VALUE
nuq_op_div_slow(VALUE a, VALUE b)
{
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_STRING &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_STRING) {
        struct nuq_obj *oa = NUQ_PTR(a), *ob = NUQ_PTR(b);
        VALUE r = nuq_make_array(0);
        if (ob->str.len == 0) {
            for (size_t i = 0; i < oa->str.len; i++)
                nuq_array_push(r, nuq_make_string(oa->str.bytes + i, 1));
            return r;
        }
        size_t i = 0, last = 0;
        while (i + ob->str.len <= oa->str.len) {
            if (memcmp(oa->str.bytes + i, ob->str.bytes, ob->str.len) == 0) {
                nuq_array_push(r, nuq_make_string(oa->str.bytes + last, i - last));
                i += ob->str.len;
                last = i;
            } else {
                i++;
            }
        }
        nuq_array_push(r, nuq_make_string(oa->str.bytes + last, oa->str.len - last));
        return r;
    }
    if (both_numeric(a, b)) {
        double db = to_double_v(b);
        if (db == 0.0) {
            char da_d[80], db_d[80];
            nuq_value_descr(a, da_d, sizeof(da_d));
            nuq_value_descr(b, db_d, sizeof(db_d));
            nuq_helper_error("%s and %s cannot be divided because the divisor is zero", da_d, db_d);
            return NUQ_NULL;
        }
        return nuq_make_double(to_double_v(a) / db);
    }
    {
        char da[80], db[80];
        nuq_value_descr(a, da, sizeof(da));
        nuq_value_descr(b, db, sizeof(db));
        nuq_helper_error("%s and %s cannot be divided", da, db);
    }
    return NUQ_NULL;
}

VALUE
nuq_op_mod_slow(VALUE a, VALUE b)
{
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) {
        int64_t lb = NUQ_FIX_VAL(b);
        if (lb == 0) {
            char da[80], db[80];
            nuq_value_descr(a, da, sizeof(da));
            nuq_value_descr(b, db, sizeof(db));
            nuq_helper_error("%s and %s cannot be divided (remainder) because the divisor is zero", da, db);
            return NUQ_NULL;
        }
        return nuq_make_int(NUQ_FIX_VAL(a) % lb);
    }
    if (both_numeric(a, b)) {
        double da = to_double_v(a);
        double db = to_double_v(b);
        /* jq mod truncates operands to int64.  Special cases:
         *   - nan % x or x % nan → nan  (jq returns nan)
         *   - inf % inf → -1 (cast UB lookalike that jq emits)
         *   - inf % finite → 0 */
        if (isnan(da) || isnan(db)) return nuq_make_double(NAN);
        /* jq's mod truncates each operand to int64 with the platform's
         * UB cast convention.  On x86 gcc both ±inf cast to INT64_MIN
         * (all 1s + sign), and `INT64_MIN % INT64_MIN = 0`.  Negative
         * inf paired with inf reproducibly yields -1 in jq's output —
         * a quirk that we mirror here so `[(-inf) % inf] == [-1]`. */
        if (!isfinite(da) || !isfinite(db)) {
            if (!isfinite(da) && da < 0 && !isfinite(db) && db > 0)
                return nuq_make_int(-1);
            return nuq_make_int(0);
        }
        int64_t ia = (int64_t)da;
        int64_t ib = (int64_t)db;
        if (ib == 0) {
            char da[80], db[80];
            nuq_value_descr(a, da, sizeof(da));
            nuq_value_descr(b, db, sizeof(db));
            nuq_helper_error("%s and %s cannot be divided (remainder) because the divisor is zero", da, db);
            return NUQ_NULL;
        }
        return nuq_make_int(ia % ib);
    }
    nuq_helper_error("cannot modulo %s by %s", nuq_type_name(a), nuq_type_name(b));
    return NUQ_NULL;
}

VALUE
nuq_op_neg_slow(VALUE a)
{
    if (NUQ_IS_FIX(a)) return nuq_make_int(-NUQ_FIX_VAL(a));
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_DOUBLE)
        return nuq_make_double(-NUQ_PTR(a)->dbl);
    {
        char d[80]; nuq_value_descr(a, d, sizeof(d));
        nuq_helper_error("%s cannot be negated", d);
    }
    return NUQ_NULL;
}

/* ---- variable bindings ---- */

void
nuq_var_push(CTX *c, uint32_t id, VALUE v)
{
    if (c->var_top == c->var_capa) {
        c->var_capa = c->var_capa ? c->var_capa * 2 : 32;
        c->var_stack = (struct nuq_var_slot *)GC_realloc(
            c->var_stack, c->var_capa * sizeof(*c->var_stack));
    }
    c->var_stack[c->var_top].id = id;
    c->var_stack[c->var_top].value = v;
    c->var_top++;
}

void
nuq_var_pop(CTX *c, size_t to_top)
{
    c->var_top = to_top;
}

VALUE
nuq_var_get(CTX *c, uint32_t id)
{
    /* search top-down */
    for (size_t i = c->var_top; i > 0; i--) {
        if (c->var_stack[i-1].id == id) return c->var_stack[i-1].value;
    }
    /* `$__loc__` is jq's built-in source-location pseudo-var — synthesize. */
    const char *nm = nuq_intern_lookup(id);
    if (nm && strcmp(nm, "__loc__") == 0) {
        VALUE loc = nuq_make_object(2);
        nuq_object_set_cstr(loc, "file", nuq_make_string("<top-level>", 11));
        nuq_object_set_cstr(loc, "line", nuq_make_int(1));
        return loc;
    }
    nuq_helper_error("undefined variable $%s", nm);
    return NUQ_NULL;
}

/* ---- string interning ---- */

struct intern_entry { const char *name; uint32_t id; };
static struct intern_entry *intern_tab = NULL;
static size_t intern_len = 0, intern_capa = 0;

uint32_t
nuq_intern(const char *name)
{
    for (size_t i = 0; i < intern_len; i++) {
        if (strcmp(intern_tab[i].name, name) == 0) return intern_tab[i].id;
    }
    if (intern_len == intern_capa) {
        intern_capa = intern_capa ? intern_capa * 2 : 32;
        intern_tab = (struct intern_entry *)GC_realloc(
            intern_tab, intern_capa * sizeof(*intern_tab));
    }
    char *dup = (char *)GC_malloc_atomic(strlen(name) + 1);
    strcpy(dup, name);
    intern_tab[intern_len].name = dup;
    intern_tab[intern_len].id = (uint32_t)intern_len + 1;
    return intern_tab[intern_len++].id;
}

const char *
nuq_intern_lookup(uint32_t id)
{
    if (id == 0 || id > intern_len) return "?";
    return intern_tab[id-1].name;
}

/* ---- function definitions (def) ---- */

void
nuq_func_define(CTX *c, struct nuq_func_def *fd)
{
    if (c->func_cnt == c->func_capa) {
        c->func_capa = c->func_capa ? c->func_capa * 2 : 16;
        c->funcs = (struct nuq_func_def **)GC_realloc(
            c->funcs, c->func_capa * sizeof(*c->funcs));
    }
    /* If the caller hasn't already set scope_top (most defs leave it 0),
     * capture the lexical scope at define time.  Param-defs can override
     * by setting scope_top BEFORE calling this — they capture the
     * caller's scope so the closure body resolves names lexically. */
    if (fd->scope_top == 0) fd->scope_top = c->func_cnt;
    c->funcs[c->func_cnt++] = fd;
}

struct nuq_func_def *
nuq_func_lookup(CTX *c, uint32_t name_id, int arity)
{
    size_t skip_s = c->func_skip_start;
    size_t skip_e = c->func_skip_end;
    for (size_t i = c->func_cnt; i > 0; i--) {
        size_t idx = i - 1;
        if (skip_s != skip_e && idx >= skip_s && idx < skip_e) continue;
        struct nuq_func_def *fd = c->funcs[idx];
        if (fd->name_id == name_id && fd->arity == arity) return fd;
    }
    return NULL;
}
