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

static struct nuq_obj *
obj_alloc(enum nuq_type t)
{
    struct nuq_obj *o = (struct nuq_obj *)GC_malloc(sizeof(*o));
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

VALUE
nuq_make_string(const char *s, size_t len)
{
    struct nuq_obj *o = obj_alloc(NUQ_T_STRING);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.bytes = buf;
    o->str.len = len;
    return NUQ_OBJ_VAL(o);
}

VALUE
nuq_make_string_take(char *s, size_t len)
{
    struct nuq_obj *o = obj_alloc(NUQ_T_STRING);
    o->str.bytes = s;
    o->str.len = len;
    return NUQ_OBJ_VAL(o);
}

VALUE
nuq_make_array(size_t cap)
{
    struct nuq_obj *o = obj_alloc(NUQ_T_ARRAY);
    if (cap <= NUQ_ARR_INLINE) {
        /* fast path: inline storage, no second allocation */
        o->arr.items = o->arr.inline_buf;
        o->arr.capa  = NUQ_ARR_INLINE;
    } else {
        o->arr.items = (VALUE *)GC_malloc(cap * sizeof(VALUE));
        o->arr.capa  = cap;
    }
    o->arr.len = 0;
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

/* (Re)build idx[] sized for current len.  load factor target 0.5. */
static void
obj_idx_rebuild(struct nuq_obj *const o)
{
    uint32_t cap = 32;
    while ((size_t)cap < o->obj.len * 2) cap *= 2;
    uint32_t *const idx = (uint32_t *)GC_malloc_atomic(cap * sizeof(uint32_t));
    memset(idx, 0, cap * sizeof(uint32_t));
    const uint32_t mask = cap - 1;
    for (size_t i = 0; i < o->obj.len; i++) {
        obj_idx_put_raw(idx, mask, nuq_key_hash(o->obj.keys[i]), (uint32_t)(i + 1));
    }
    o->obj.idx = idx;
    o->obj.idx_mask = mask;
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
    struct nuq_obj *o = obj_alloc(NUQ_T_OBJECT);
    if (cap < 4) cap = 4;
    o->obj.keys = (VALUE *)GC_malloc(cap * sizeof(VALUE));
    o->obj.vals = (VALUE *)GC_malloc(cap * sizeof(VALUE));
    o->obj.len = 0;
    o->obj.capa = cap;
    o->obj.idx = NULL;
    o->obj.idx_mask = 0;
    return NUQ_OBJ_VAL(o);
}

void
nuq_array_push(VALUE arr, VALUE v)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    if (UNLIKELY(o->arr.len == o->arr.capa)) {
        size_t nc = o->arr.capa * 2;
        if (o->arr.items == o->arr.inline_buf) {
            /* migrate inline → heap */
            VALUE *heap = (VALUE *)GC_malloc(nc * sizeof(VALUE));
            memcpy(heap, o->arr.inline_buf, o->arr.capa * sizeof(VALUE));
            o->arr.items = heap;
        } else {
            o->arr.items = (VALUE *)GC_realloc(o->arr.items, nc * sizeof(VALUE));
        }
        o->arr.capa = nc;
    }
    o->arr.items[o->arr.len++] = v;
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
    struct nuq_obj *const o = NUQ_PTR(obj);
    const struct nuq_obj *const ks = NUQ_PTR(key);
    uint32_t h = 0;
    /* All object keys in jq/nuq are strings.  Defensive fallback to
     * linear nuq_eq for anything else (e.g., number key during a
     * non-standard construction). */
    if (UNLIKELY(ks->type != NUQ_T_STRING)) {
        for (size_t i = 0; i < o->obj.len; i++) {
            if (nuq_eq(o->obj.keys[i], key)) { o->obj.vals[i] = val; return; }
        }
        goto append;
    }
    h = nuq_str_hash(ks->str.bytes, ks->str.len);
    if (o->obj.idx != NULL) {
        const int64_t pos = obj_idx_lookup(o, ks->str.bytes, ks->str.len, h);
        if (pos >= 0) { o->obj.vals[pos] = val; return; }
    } else {
        /* Linear scan while small. */
        for (size_t i = 0; i < o->obj.len; i++) {
            const struct nuq_obj *const ki = NUQ_PTR(o->obj.keys[i]);
            if (ki->type == NUQ_T_STRING &&
                ki->str.len == ks->str.len &&
                memcmp(ki->str.bytes, ks->str.bytes, ks->str.len) == 0) {
                o->obj.vals[i] = val;
                return;
            }
        }
    }
append:
    if (o->obj.len == o->obj.capa) {
        size_t nc = o->obj.capa * 2;
        o->obj.keys = (VALUE *)GC_realloc(o->obj.keys, nc * sizeof(VALUE));
        o->obj.vals = (VALUE *)GC_realloc(o->obj.vals, nc * sizeof(VALUE));
        o->obj.capa = nc;
    }
    o->obj.keys[o->obj.len] = key;
    o->obj.vals[o->obj.len] = val;
    o->obj.len++;
    if (LIKELY(ks->type == NUQ_T_STRING)) {
        if (o->obj.idx == NULL) {
            if (o->obj.len > NUQ_OBJ_HASH_MIN) obj_idx_rebuild(o);
        } else {
            /* Maintain load factor ≤ 0.5. */
            if (o->obj.len * 2 > (size_t)o->obj.idx_mask + 1) obj_idx_rebuild(o);
            else obj_idx_put_raw(o->obj.idx, o->obj.idx_mask, h, (uint32_t)o->obj.len);
        }
    } else {
        /* Non-string key; idx (if any) gets stale.  Easiest: drop it
         * and rebuild on next reach of the threshold.  These keys are
         * rare enough that this branch shouldn't matter in practice. */
        o->obj.idx = NULL;
        o->obj.idx_mask = 0;
    }
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
    const size_t fit_lim = is_string ? 28 : 26;
    const size_t trunc_lim = is_string ? 25 : 24;
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
      case NUQ_T_STRING: return nuq_make_int((int64_t)o->str.len);
      case NUQ_T_ARRAY:  return nuq_make_int((int64_t)o->arr.len);
      case NUQ_T_OBJECT: return nuq_make_int((int64_t)o->obj.len);
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
        VALUE r = nuq_make_array(o->arr.len);
        for (size_t i = 0; i < o->arr.len; i++)
            nuq_array_push(r, o->arr.items[i]);
        return r;
      }
      case NUQ_T_OBJECT: {
        /* Source keys are already unique — bypass nuq_object_set's
         * collision check.  If the source has an idx[] (len past the
         * threshold), rebuild on the clone too so subsequent
         * `clone(big) + small` (the kv-bench / object-add pattern)
         * stays O(small) per merge instead of degrading to linear. */
        VALUE r = nuq_make_object(o->obj.len);
        struct nuq_obj *ro = NUQ_PTR(r);
        for (size_t i = 0; i < o->obj.len; i++) {
            ro->obj.keys[ro->obj.len] = o->obj.keys[i];
            ro->obj.vals[ro->obj.len] = o->obj.vals[i];
            ro->obj.len++;
        }
        if (o->obj.idx != NULL) obj_idx_rebuild(ro);
        return r;
      }
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
bool
nuq_contains(VALUE a, VALUE b)
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
    }
    return false;
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

VALUE
nuq_op_add_slow(VALUE a, VALUE b)
{
    /* Inline `nuq_op_add` (in node.h) folded fix+fix without overflow. */
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_NULL) return b;
    if (NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_NULL) return a;
    if (NUQ_IS_FIX(a) && NUQ_IS_FIX(b)) {
        /* Reached only on fix+fix overflow (inline path bailed). */
        int64_t la = NUQ_FIX_VAL(a), lb = NUQ_FIX_VAL(b);
        return nuq_make_double((double)la + (double)lb);
    }
    if (both_numeric(a, b)) return nuq_make_double(to_double_v(a) + to_double_v(b));
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_STRING &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_STRING) {
        struct nuq_obj *oa = NUQ_PTR(a), *ob = NUQ_PTR(b);
        size_t ln = oa->str.len + ob->str.len;
        char *buf = (char *)GC_malloc_atomic(ln + 1);
        memcpy(buf, oa->str.bytes, oa->str.len);
        memcpy(buf + oa->str.len, ob->str.bytes, ob->str.len);
        buf[ln] = '\0';
        return nuq_make_string_take(buf, ln);
    }
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_ARRAY &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_ARRAY) {
        struct nuq_obj *oa = NUQ_PTR(a), *ob = NUQ_PTR(b);
        VALUE r = nuq_make_array(oa->arr.len + ob->arr.len);
        for (size_t i = 0; i < oa->arr.len; i++) nuq_array_push(r, oa->arr.items[i]);
        for (size_t i = 0; i < ob->arr.len; i++) nuq_array_push(r, ob->arr.items[i]);
        return r;
    }
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_OBJECT &&
        NUQ_IS_PTR(b) && NUQ_PTR(b)->type == NUQ_T_OBJECT) {
        VALUE r = nuq_clone(a);
        struct nuq_obj *ob = NUQ_PTR(b);
        for (size_t i = 0; i < ob->obj.len; i++) nuq_object_set(r, ob->obj.keys[i], ob->obj.vals[i]);
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
        struct nuq_obj *oa = NUQ_PTR(a), *ob = NUQ_PTR(b);
        VALUE r = nuq_make_array(oa->arr.len);
        for (size_t i = 0; i < oa->arr.len; i++) {
            bool found = false;
            for (size_t j = 0; j < ob->arr.len; j++)
                if (nuq_eq(oa->arr.items[i], ob->arr.items[j])) { found = true; break; }
            if (!found) nuq_array_push(r, oa->arr.items[i]);
        }
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
        VALUE r = nuq_clone(a);
        struct nuq_obj *ob = NUQ_PTR(b);
        for (size_t i = 0; i < ob->obj.len; i++) {
            VALUE bv = ob->obj.vals[i];
            VALUE av = nuq_object_get(r, ob->obj.keys[i]);
            if (NUQ_IS_PTR(av) && NUQ_PTR(av)->type == NUQ_T_OBJECT &&
                NUQ_IS_PTR(bv) && NUQ_PTR(bv)->type == NUQ_T_OBJECT) {
                nuq_object_set(r, ob->obj.keys[i], nuq_op_mul(av, bv));
            } else {
                nuq_object_set(r, ob->obj.keys[i], bv);
            }
        }
        return r;
    }
    if (NUQ_IS_PTR(a) && NUQ_PTR(a)->type == NUQ_T_STRING && NUQ_IS_FIX(b)) {
        int64_t n = NUQ_FIX_VAL(b);
        if (n <= 0) return NUQ_NULL;
        struct nuq_obj *oa = NUQ_PTR(a);
        size_t L = oa->str.len * (size_t)n;
        char *buf = (char *)GC_malloc_atomic(L + 1);
        for (int64_t i = 0; i < n; i++) memcpy(buf + i * oa->str.len, oa->str.bytes, oa->str.len);
        buf[L] = '\0';
        return nuq_make_string_take(buf, L);
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
            nuq_helper_error("");
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
        if (lb == 0) { nuq_helper_error(""); return NUQ_NULL; }
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
        if (!isfinite(da) || !isfinite(db)) {
            if (!isfinite(da) && !isfinite(db)) return nuq_make_int(-1);
            return nuq_make_int(0);
        }
        int64_t ia = (int64_t)da;
        int64_t ib = (int64_t)db;
        if (ib == 0) { nuq_helper_error(""); return NUQ_NULL; }
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
    nuq_helper_error("undefined variable $%s", nuq_intern_lookup(id));
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
    c->funcs[c->func_cnt++] = fd;
}

struct nuq_func_def *
nuq_func_lookup(CTX *c, uint32_t name_id, int arity)
{
    /* later definitions shadow earlier — search top-down */
    for (size_t i = c->func_cnt; i > 0; i--) {
        struct nuq_func_def *fd = c->funcs[i-1];
        if (fd->name_id == name_id && fd->arity == arity) return fd;
    }
    return NULL;
}
