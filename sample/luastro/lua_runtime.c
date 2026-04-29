// luastro runtime — strings, tables, closures, metatables, errors.
//
// This TU is hand-written (not generated).  It implements all of the
// data structures and helpers declared in context.h that the EVAL
// bodies in node.def call into.

#include <math.h>
#include <stdarg.h>
#include <inttypes.h>
#include <ctype.h>
#include "context.h"
#include "node.h"

extern struct luastro_option OPTION;

// =====================================================================
// Out-of-line double-box fallback (see context.h for the inline path).
// =====================================================================
//
// Inline flonum covers `b62 ∈ {3,4}` doubles (magnitudes ≈ 2^-255 to
// 2^256, both signs) — see context.h.  Anything outside that range
// (zeros, denormals, ±Inf, NaN, very tiny / very large) goes through
// these helpers and lives on the heap as a `LuaHeapDouble`.
//
// +0.0 is by far the most common heap-boxed double in real workloads
// (mandelbrot's `x, y = 0.0, 0.0` initial state, `x*x + y*y` momentarily
// hitting zero, etc.).  We pin a single shared `LuaHeapDouble` for
// +0.0 at first encounter and reuse it forever — no realloc, no GC
// churn.  -0.0 falls through to the per-call alloc path because
// distinguishing it cheaply would require a sign check on every box.

// Pinned +0.0 cell — allocated once and never freed.  We don't put it
// on the GC's all-objects list at all (so sweep can't touch it) but it
// still has GCHead.type = LUA_TFLOAT so the heap-type byte read works.
static struct LuaHeapDouble G_LUA_ZERO_DOUBLE = { .gc = { .type = LUA_TFLOAT } };

LuaValue
luav_box_double(double d)
{
    if (d == 0.0 && !signbit(d)) {
        return (LuaValue)(uintptr_t)&G_LUA_ZERO_DOUBLE;
    }
    struct LuaHeapDouble *hd = (struct LuaHeapDouble *)calloc(1, sizeof(*hd));
    luastro_gc_register(hd, LUA_TFLOAT);
    hd->value = d;
    return (LuaValue)(uintptr_t)hd;
}

double
luav_unbox_double(LuaValue v)
{
    if (LV_IS_HEAP_OF(v, LUA_TFLOAT)) {
        return ((struct LuaHeapDouble *)(uintptr_t)v)->value;
    }
    return 0.0;
}

#include "lua_gc.c"

// =====================================================================
// LuaString — interned (every distinct byte sequence has exactly one
// LuaString *).  Equality is pointer identity, hash is precomputed.
// =====================================================================

const char *lua_str_data(const struct LuaString *s) { return s ? s->data : ""; }
size_t      lua_str_len (const struct LuaString *s) { return s ? s->len  : 0;  }
uint64_t    lua_str_hash(const struct LuaString *s) { return s ? s->hash : 0;  }

bool
lua_str_eq(const struct LuaString *a, const struct LuaString *b)
{
    return a == b;          // interned: pointer equality is value equality
}

// FNV-1a — cheap and consistent with our hash_cstr helper.
static uint64_t
lua_strhash_bytes(const char *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Open-addressing hash set keyed by (hash, len, bytes).
struct LuaStrPool {
    struct LuaString **slots;
    uint32_t cap;
    uint32_t cnt;
};

static struct LuaStrPool *G_STRPOOL = NULL;

static void
strpool_grow(struct LuaStrPool *p)
{
    uint32_t old_cap = p->cap;
    struct LuaString **old = p->slots;
    uint32_t cap = old_cap ? old_cap * 2 : 64;
    p->slots = (struct LuaString **)calloc(cap, sizeof(struct LuaString *));
    p->cap = cap;
    p->cnt = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        struct LuaString *s = old[i];
        if (!s) continue;
        uint32_t pos = (uint32_t)s->hash & (cap - 1);
        while (p->slots[pos]) pos = (pos + 1) & (cap - 1);
        p->slots[pos] = s;
        p->cnt++;
    }
    free(old);
}

struct LuaString *
lua_str_intern_n(const char *bytes, size_t len)
{
    if (!G_STRPOOL) {
        G_STRPOOL = (struct LuaStrPool *)calloc(1, sizeof(struct LuaStrPool));
    }
    struct LuaStrPool *p = G_STRPOOL;
    if (p->cap == 0 || p->cnt * 2 >= p->cap) strpool_grow(p);

    uint64_t h = lua_strhash_bytes(bytes, len);
    uint32_t pos = (uint32_t)h & (p->cap - 1);
    for (;;) {
        struct LuaString *s = p->slots[pos];
        if (!s) break;
        if (s->hash == h && s->len == len && memcmp(s->data, bytes, len) == 0) return s;
        pos = (pos + 1) & (p->cap - 1);
    }
    struct LuaString *s = (struct LuaString *)calloc(1, sizeof(struct LuaString) + len + 1);
    luastro_gc_register(s, LUA_TSTRING);
    s->hash = h;
    s->len  = (uint32_t)len;
    memcpy(s->data, bytes, len);
    s->data[len] = '\0';
    p->slots[pos] = s;
    p->cnt++;
    return s;
}

struct LuaString *
lua_str_intern(const char *cstr)
{
    return lua_str_intern_n(cstr, strlen(cstr));
}

struct LuaString *
lua_str_concat(struct LuaString *a, struct LuaString *b)
{
    size_t la = lua_str_len(a), lb = lua_str_len(b);
    char *buf = (char *)malloc(la + lb);
    memcpy(buf, a->data, la);
    memcpy(buf + la, b->data, lb);
    struct LuaString *r = lua_str_intern_n(buf, la + lb);
    free(buf);
    return r;
}

// =====================================================================
// LuaTable — hybrid array + hash.
// =====================================================================
//
// The array part holds integer keys 1..arr_cnt directly (1-indexed,
// stored 0-indexed internally).  Other keys go through the hash part.
// A new entry with integer key k > arr_cnt grows the array if k is
// roughly contiguous (k <= 2 * arr_cnt + 4); otherwise it goes to the
// hash so we don't waste memory on sparse tables.

static uint64_t
lua_value_hash(LuaValue v)
{
    if (LV_IS_INT(v))   return (uint64_t)LV_AS_INT(v) * 0x9E3779B97F4A7C15ULL;
    if (LV_IS_FLOAT(v)) {
        // Hash by the actual double bit pattern so inline and heap-boxed
        // representations of the same value hash to the same slot.
        double f = LV_AS_FLOAT(v); uint64_t b; memcpy(&b, &f, 8);
        return b * 0x9E3779B97F4A7C15ULL;
    }
    if (LV_IS_STR(v))   return lua_str_hash(LV_AS_STR(v));
    if (LV_IS_BOOL(v))  return LV_AS_BOOL(v) ? 0x1234 : 0x5678;
    if (LV_IS_NIL(v))   return 0;
    return (uint64_t)v * 0x9E3779B97F4A7C15ULL;
}

extern bool
lua_eq_raw(LuaValue a, LuaValue b)
{
    if (a == b) return true;
    // int/float cross-type equality: 1 == 1.0
    if (LV_IS_INT(a) && LV_IS_FLOAT(b))
        return (double)LV_AS_INT(a) == LV_AS_FLOAT(b);
    if (LV_IS_FLOAT(a) && LV_IS_INT(b))
        return LV_AS_FLOAT(a) == (double)LV_AS_INT(b);
    if (LV_IS_FLOAT(a) && LV_IS_FLOAT(b))
        return LV_AS_FLOAT(a) == LV_AS_FLOAT(b);
    return false;
}

static LuaValue
lua_value_normalize_key(LuaValue k)
{
    // Per Lua 5.4, integer-valued floats normalize to integer keys.
    if (LV_IS_FLOAT(k)) {
        double f = LV_AS_FLOAT(k);
        int64_t i = (int64_t)f;
        if ((double)i == f && !isnan(f)) return LUAV_INT(i);
    }
    return k;
}

struct LuaTable *
lua_table_new(uint32_t nseq, uint32_t nhash)
{
    struct LuaTable *t = (struct LuaTable *)calloc(1, sizeof(struct LuaTable));
    luastro_gc_register(t, LUA_TTABLE);
    if (nseq) {
        t->array = (LuaValue *)calloc(nseq, sizeof(LuaValue));
        for (uint32_t i = 0; i < nseq; i++) t->array[i] = LUAV_NIL;
        t->arr_cap = nseq;
    }
    if (nhash) {
        // Minimum cap of 8 ensures the open-addressing probe always finds
        // a sentinel-nil slot to terminate (we never let load factor > 0.5).
        uint32_t cap = 8;
        while (cap < nhash * 2) cap *= 2;
        t->hash = (struct LuaTabEntry *)calloc(cap, sizeof(struct LuaTabEntry));
        t->hash_cap = cap;
    }
    return t;
}

static void
lua_table_array_grow(struct LuaTable *t, uint32_t need)
{
    if (need <= t->arr_cap) return;
    uint32_t cap = t->arr_cap ? t->arr_cap : 4;
    while (cap < need) cap *= 2;
    t->array = (LuaValue *)realloc(t->array, cap * sizeof(LuaValue));
    for (uint32_t i = t->arr_cap; i < cap; i++) t->array[i] = LUAV_NIL;
    t->arr_cap = cap;
}

static void
lua_table_hash_grow(struct LuaTable *t)
{
    uint32_t old_cap = t->hash_cap;
    struct LuaTabEntry *old = t->hash;
    uint32_t cap = old_cap ? old_cap * 2 : 8;
    t->hash = (struct LuaTabEntry *)calloc(cap, sizeof(struct LuaTabEntry));
    t->hash_cap = cap;
    t->hash_cnt = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (!LV_IS_NIL(old[i].key)) {
            uint32_t pos = (uint32_t)lua_value_hash(old[i].key) & (cap - 1);
            while (!LV_IS_NIL(t->hash[pos].key)) pos = (pos + 1) & (cap - 1);
            t->hash[pos] = old[i];
            t->hash_cnt++;
        }
    }
    free(old);
}

static struct LuaTabEntry *
lua_table_hash_find(struct LuaTable *t, LuaValue k)
{
    if (t->hash_cap == 0) return NULL;
    uint32_t pos = (uint32_t)lua_value_hash(k) & (t->hash_cap - 1);
    for (;;) {
        struct LuaTabEntry *e = &t->hash[pos];
        if (LV_IS_NIL(e->key)) return NULL;
        if (lua_eq_raw(e->key, k)) return e;
        pos = (pos + 1) & (t->hash_cap - 1);
    }
}

LuaValue
lua_table_geti(struct LuaTable *t, int64_t i)
{
    if (i >= 1 && (uint64_t)i <= t->arr_cnt) return t->array[i - 1];
    LuaValue k = LUAV_INT(i);
    struct LuaTabEntry *e = lua_table_hash_find(t, k);
    return e ? e->val : LUAV_NIL;
}

LuaValue
lua_table_get_str(struct LuaTable *t, struct LuaString *key)
{
    LuaValue k = LUAV_STR(key);
    struct LuaTabEntry *e = lua_table_hash_find(t, k);
    return e ? e->val : LUAV_NIL;
}

LuaValue
lua_table_get(struct LuaTable *t, LuaValue key)
{
    key = lua_value_normalize_key(key);
    if (LV_IS_INT(key)) return lua_table_geti(t, LV_AS_INT(key));
    if (LV_IS_NIL(key)) return LUAV_NIL;
    struct LuaTabEntry *e = lua_table_hash_find(t, key);
    return e ? e->val : LUAV_NIL;
}

static void
lua_table_array_set(struct LuaTable *t, int64_t i, LuaValue v)
{
    lua_table_array_grow(t, (uint32_t)i);
    t->array[i - 1] = v;
    if ((uint32_t)i > t->arr_cnt && !LV_IS_NIL(v)) t->arr_cnt = (uint32_t)i;
    // shrink-to-fit on nil at the boundary
    while (t->arr_cnt > 0 && LV_IS_NIL(t->array[t->arr_cnt - 1])) t->arr_cnt--;
}

static void
lua_table_hash_set(struct LuaTable *t, LuaValue k, LuaValue v)
{
    // Keep at least one empty sentinel slot so probe loops terminate.
    if (t->hash_cap == 0 || (t->hash_cnt + 1) * 2 > t->hash_cap) lua_table_hash_grow(t);
    uint32_t pos = (uint32_t)lua_value_hash(k) & (t->hash_cap - 1);
    for (;;) {
        struct LuaTabEntry *e = &t->hash[pos];
        if (LV_IS_NIL(e->key)) {
            if (LV_IS_NIL(v)) return;        // setting nil on absent key is a no-op
            e->key = k;
            e->val = v;
            t->hash_cnt++;
            return;
        }
        if (lua_eq_raw(e->key, k)) {
            e->val = v;
            // We don't physically delete on nil to avoid breaking
            // open-addressing chains; lookup treats nil-val keys as absent
            // via lua_table_get's check below.
            return;
        }
        pos = (pos + 1) & (t->hash_cap - 1);
    }
}

void
lua_table_seti(struct LuaTable *t, int64_t i, LuaValue v)
{
    // Promote to array part if (a) strictly contiguous OR (b) modestly
    // close to it (≤ 2× arr_cap + 4).  The (b) case lets benchmarks
    // like sieve — which fill primes[2]..primes[N] without ever
    // touching primes[1] — actually use the array part instead of
    // dropping every entry into the open-addressing hash.
    if (i >= 1) {
        uint64_t cap = t->arr_cap;
        if ((uint64_t)i <= (uint64_t)t->arr_cnt + 1 ||
            (uint64_t)i <= cap * 2 + 4) {
            lua_table_array_set(t, i, v);
            return;
        }
    }
    lua_table_hash_set(t, LUAV_INT(i), v);
}

void
lua_table_set_str(struct LuaTable *t, struct LuaString *key, LuaValue v)
{
    lua_table_hash_set(t, LUAV_STR(key), v);
}

void
lua_table_set(struct LuaTable *t, LuaValue key, LuaValue v)
{
    key = lua_value_normalize_key(key);
    if (LV_IS_NIL(key)) return;
    if (LV_IS_FLOAT(key) && isnan(LV_AS_FLOAT(key))) return;
    if (LV_IS_INT(key)) { lua_table_seti(t, LV_AS_INT(key), v); return; }
    lua_table_hash_set(t, key, v);
}

int64_t
lua_table_len(struct LuaTable *t)
{
    return (int64_t)t->arr_cnt;
}

struct LuaTable *
lua_table_metatable(struct LuaTable *t) { return t ? t->metatable : NULL; }

void
lua_table_set_metatable(struct LuaTable *t, struct LuaTable *mt) { t->metatable = mt; }

// Iterate next-after-key; sets *kio and *vout, returns false at end.
// Order: array part 1..arr_cnt, then hash part in slot order.
bool
lua_table_next(struct LuaTable *t, LuaValue *kio, LuaValue *vout)
{
    LuaValue k = lua_value_normalize_key(*kio);
    uint32_t i = 0;
    if (LV_IS_NIL(k)) {
        i = 0;
    } else if (LV_IS_INT(k) && LV_AS_INT(k) >= 1 && (uint64_t)LV_AS_INT(k) <= t->arr_cnt) {
        i = (uint32_t)LV_AS_INT(k);
    } else {
        // Find in hash and then advance to next occupied slot.
        if (t->hash_cap == 0) return false;
        uint32_t pos = (uint32_t)lua_value_hash(k) & (t->hash_cap - 1);
        for (;;) {
            struct LuaTabEntry *e = &t->hash[pos];
            if (LV_IS_NIL(e->key)) return false;
            if (lua_eq_raw(e->key, k)) { pos = (pos + 1) & (t->hash_cap - 1); break; }
            pos = (pos + 1) & (t->hash_cap - 1);
        }
        // Find next non-nil-key, non-nil-val from pos.
        for (uint32_t j = 0; j < t->hash_cap; j++) {
            uint32_t p = (pos + j) & (t->hash_cap - 1);
            // We need to walk linearly from index `pos` to end of array,
            // but with open addressing we instead resume scanning from
            // the original numeric position; simpler: scan the whole
            // table from absolute slot index pos forward.
            (void)p;
            break;
        }
        // Simpler approach: scan from absolute index after the matched slot.
        // Recompute the absolute index by searching again — small tables
        // make this fine.
        uint32_t found_at = UINT32_MAX;
        uint32_t pp = (uint32_t)lua_value_hash(k) & (t->hash_cap - 1);
        for (uint32_t step = 0; step < t->hash_cap; step++) {
            struct LuaTabEntry *e = &t->hash[pp];
            if (LV_IS_NIL(e->key)) return false;
            if (lua_eq_raw(e->key, k)) { found_at = pp; break; }
            pp = (pp + 1) & (t->hash_cap - 1);
        }
        if (found_at == UINT32_MAX) return false;
        for (uint32_t s = found_at + 1; s < t->hash_cap; s++) {
            if (!LV_IS_NIL(t->hash[s].key) && !LV_IS_NIL(t->hash[s].val)) {
                *kio = t->hash[s].key;
                *vout = t->hash[s].val;
                return true;
            }
        }
        return false;
    }
    // Resume in array part.
    while (i < t->arr_cnt) {
        if (!LV_IS_NIL(t->array[i])) {
            *kio  = LUAV_INT((int64_t)i + 1);
            *vout = t->array[i];
            return true;
        }
        i++;
    }
    // Then start of hash part.
    for (uint32_t s = 0; s < t->hash_cap; s++) {
        if (!LV_IS_NIL(t->hash[s].key) && !LV_IS_NIL(t->hash[s].val)) {
            *kio  = t->hash[s].key;
            *vout = t->hash[s].val;
            return true;
        }
    }
    return false;
}

// =====================================================================
// LuaClosure — compiled Lua function with upvalue cells.
// =====================================================================

struct LuaClosure *
lua_closure_new(struct Node *body, uint32_t nparams, uint32_t nlocals,
                uint32_t nupvals, bool is_vararg, const char *name)
{
    struct LuaClosure *cl = (struct LuaClosure *)calloc(1, sizeof(*cl));
    luastro_gc_register(cl, LUA_TFUNC);
    cl->body      = body;
    cl->nparams   = nparams;
    cl->nlocals   = nlocals;
    cl->nupvals   = nupvals;
    cl->is_vararg = is_vararg;
    cl->name      = name;
    cl->upvals    = nupvals ? (LuaValue **)calloc(nupvals, sizeof(LuaValue *)) : NULL;
    return cl;
}

struct LuaClosure *
lua_closure_with_upvals(struct LuaClosure *proto, LuaValue **upvals)
{
    struct LuaClosure *cl = (struct LuaClosure *)malloc(sizeof(*cl));
    *cl = *proto;
    if (proto->nupvals) {
        cl->upvals = (LuaValue **)malloc(proto->nupvals * sizeof(LuaValue *));
        memcpy(cl->upvals, upvals, proto->nupvals * sizeof(LuaValue *));
    }
    return cl;
}

LuaValue *
lua_closure_upval(struct LuaClosure *cl, uint32_t i) { return cl->upvals[i]; }

struct LuaCFunction *
lua_cfunc_new(const char *name, lua_cfunc_ptr_t fn)
{
    struct LuaCFunction *cf = (struct LuaCFunction *)calloc(1, sizeof(*cf));
    luastro_gc_register(cf, LUA_TCFUNC);
    cf->name = name;
    cf->fn   = fn;
    return cf;
}

// =====================================================================
// Conversions
// =====================================================================

bool
lua_to_int(LuaValue v, int64_t *out)
{
    if (LV_IS_INT(v)) { *out = LV_AS_INT(v); return true; }
    if (LV_IS_FLOAT(v)) {
        double f = LV_AS_FLOAT(v);
        int64_t i = (int64_t)f;
        if ((double)i == f && !isnan(f)) { *out = i; return true; }
        return false;
    }
    if (LV_IS_STR(v)) {
        const char *s = lua_str_data(LV_AS_STR(v));
        char *end = NULL;
        long long ll = strtoll(s, &end, 0);
        if (end && *end == '\0' && end != s) { *out = (int64_t)ll; return true; }
        double f = strtod(s, &end);
        if (end && *end == '\0' && end != s) {
            int64_t i = (int64_t)f;
            if ((double)i == f) { *out = i; return true; }
        }
        return false;
    }
    return false;
}

bool
lua_to_float(LuaValue v, double *out)
{
    if (LV_IS_INT(v))   { *out = (double)LV_AS_INT(v); return true; }
    if (LV_IS_FLOAT(v)) { *out = LV_AS_FLOAT(v);       return true; }
    if (LV_IS_STR(v)) {
        const char *s = lua_str_data(LV_AS_STR(v));
        char *end = NULL;
        double f = strtod(s, &end);
        if (end && *end == '\0' && end != s) { *out = f; return true; }
        return false;
    }
    return false;
}

bool
lua_to_bool(LuaValue v) { return LV_TRUTHY(v); }

LuaValue
lua_tostring(CTX *c, LuaValue v)
{
    char buf[64];
    (void)c;
    if (LV_IS_NIL(v))   return LUAV_STR(lua_str_intern("nil"));
    if (LV_IS_BOOL(v))  return LUAV_STR(lua_str_intern(LV_AS_BOOL(v) ? "true" : "false"));
    if (LV_IS_INT(v))   { snprintf(buf, sizeof(buf), "%" PRId64, LV_AS_INT(v)); return LUAV_STR(lua_str_intern(buf)); }
    if (LV_IS_FLOAT(v)) {
        double f = LV_AS_FLOAT(v);
        if (isnan(f)) return LUAV_STR(lua_str_intern("nan"));
        if (isinf(f)) return LUAV_STR(lua_str_intern(f < 0 ? "-inf" : "inf"));
        snprintf(buf, sizeof(buf), "%.14g", f);
        bool has_dot = false;
        for (const char *p = buf; *p; p++) if (*p == '.' || *p == 'e' || *p == 'n') { has_dot = true; break; }
        if (!has_dot) {
            size_t len = strlen(buf);
            buf[len++] = '.'; buf[len++] = '0'; buf[len] = '\0';
        }
        return LUAV_STR(lua_str_intern(buf));
    }
    if (LV_IS_STR(v))   return v;
    if (LV_IS_TBL(v))   { snprintf(buf, sizeof(buf), "table: %p", (void *)LV_AS_TBL(v));  return LUAV_STR(lua_str_intern(buf)); }
    if (LV_IS_FN(v))    { snprintf(buf, sizeof(buf), "function: %p", (void *)LV_AS_FN(v)); return LUAV_STR(lua_str_intern(buf)); }
    if (LV_IS_CF(v))    { snprintf(buf, sizeof(buf), "function: builtin %s", LV_AS_CF(v)->name); return LUAV_STR(lua_str_intern(buf)); }
    return LUAV_STR(lua_str_intern("?"));
}

const char *
lua_type_name(LuaValue v)
{
    if (LV_IS_NIL(v))    return "nil";
    if (LV_IS_BOOL(v))   return "boolean";
    if (LV_IS_INT(v))    return "number";
    if (LV_IS_FLOAT(v))  return "number";
    if (LV_IS_STR(v))    return "string";
    if (LV_IS_TBL(v))    return "table";
    if (LV_IS_FN(v))     return "function";
    if (LV_IS_CF(v))     return "function";
    if (LV_IS_THREAD(v)) return "thread";
    return "unknown";
}

void
lua_print_value(FILE *fp, LuaValue v)
{
    LuaValue s = lua_tostring(NULL, v);
    fwrite(LV_AS_STR(s)->data, 1, LV_AS_STR(s)->len, fp);
}

// =====================================================================
// Errors / pcall
// =====================================================================

void
lua_raise(CTX *c, LuaValue err)
{
    c->last_error = err;
    if (c->pcall_top) longjmp(c->pcall_top->jb, 1);
    fprintf(stderr, "luastro: uncaught error: ");
    lua_print_value(stderr, err);
    fprintf(stderr, "\n");
    exit(1);
}

void
lua_raisef(CTX *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lua_raise(c, LUAV_STR(lua_str_intern(buf)));
}

RESULT
lua_pcall(CTX *c, LuaValue fn, LuaValue *args, uint32_t argc)
{
    struct lua_pcall_frame frame;
    frame.prev    = c->pcall_top;
    frame.sp_save = c->sp;
    c->pcall_top  = &frame;

    if (setjmp(frame.jb) == 0) {
        RESULT r = lua_call(c, fn, args, argc);
        c->pcall_top = frame.prev;
        // Lua's pcall protocol: on success, returns (true, ...callee's results...).
        // f's primary return is r.value; if it returned multi-values they're
        // already in ret_info[0..n-1] (with [0] == r.value).  Shift right
        // by one and prefix `true`.
        uint32_t got = c->ret_info.result_cnt;
        if (got == 0) {
            c->ret_info.results[0] = LUAV_BOOL(true);
            c->ret_info.results[1] = r;
            c->ret_info.result_cnt = 2;
        } else {
            if (got > LUASTRO_MAX_RETS - 1) got = LUASTRO_MAX_RETS - 1;
            for (int k = (int)got - 1; k >= 0; k--) {
                c->ret_info.results[k + 1] = c->ret_info.results[k];
            }
            c->ret_info.results[0] = LUAV_BOOL(true);
            c->ret_info.result_cnt = got + 1;
        }
        return RESULT_OK(LUAV_BOOL(true));
    } else {
        c->pcall_top = frame.prev;
        c->sp        = frame.sp_save;
        c->ret_info.results[0] = LUAV_BOOL(false);
        c->ret_info.results[1] = c->last_error;
        c->ret_info.result_cnt = 2;
        return RESULT_OK(LUAV_BOOL(false));
    }
}

// =====================================================================
// Comparisons (with metamethod dispatch deferred — v1 raw)
// =====================================================================

bool
lua_eq(CTX *c, LuaValue a, LuaValue b)
{
    if (lua_eq_raw(a, b)) return true;
    if (LV_IS_TBL(a) && LV_IS_TBL(b) && LV_AS_TBL(a)->metatable) {
        struct LuaString *mk = lua_str_intern("__eq");
        LuaValue mm = lua_table_get_str(LV_AS_TBL(a)->metatable, mk);
        if (LV_IS_CALL(mm)) {
            LuaValue argv[2] = {a, b};
            RESULT r = lua_call(c, mm, argv, 2);
            return LV_TRUTHY(r);
        }
    }
    return false;
}

bool
lua_lt(CTX *c, LuaValue a, LuaValue b)
{
    if (LV_IS_INT(a) && LV_IS_INT(b)) return LV_AS_INT(a) < LV_AS_INT(b);
    if (LV_IS_FLOAT(a) && LV_IS_FLOAT(b)) return LV_AS_FLOAT(a) < LV_AS_FLOAT(b);
    if (LV_IS_NUM(a) && LV_IS_NUM(b)) {
        double x = LV_IS_INT(a) ? (double)LV_AS_INT(a) : LV_AS_FLOAT(a);
        double y = LV_IS_INT(b) ? (double)LV_AS_INT(b) : LV_AS_FLOAT(b);
        return x < y;
    }
    if (LV_IS_STR(a) && LV_IS_STR(b))
        return strcmp(LV_AS_STR(a)->data, LV_AS_STR(b)->data) < 0;
    // __lt metamethod
    if (LV_IS_TBL(a) && LV_AS_TBL(a)->metatable) {
        LuaValue mm = lua_table_get_str(LV_AS_TBL(a)->metatable, lua_str_intern("__lt"));
        if (LV_IS_CALL(mm)) {
            LuaValue argv[2] = {a, b};
            RESULT r = lua_call(c, mm, argv, 2);
            return LV_TRUTHY(r);
        }
    }
    lua_raisef(c, "attempt to compare %s with %s", lua_type_name(a), lua_type_name(b));
    return false;
}

bool
lua_le(CTX *c, LuaValue a, LuaValue b)
{
    if (LV_IS_INT(a) && LV_IS_INT(b)) return LV_AS_INT(a) <= LV_AS_INT(b);
    if (LV_IS_FLOAT(a) && LV_IS_FLOAT(b)) return LV_AS_FLOAT(a) <= LV_AS_FLOAT(b);
    if (LV_IS_NUM(a) && LV_IS_NUM(b)) {
        double x = LV_IS_INT(a) ? (double)LV_AS_INT(a) : LV_AS_FLOAT(a);
        double y = LV_IS_INT(b) ? (double)LV_AS_INT(b) : LV_AS_FLOAT(b);
        return x <= y;
    }
    if (LV_IS_STR(a) && LV_IS_STR(b))
        return strcmp(LV_AS_STR(a)->data, LV_AS_STR(b)->data) <= 0;
    return !lua_lt(c, b, a);
}

// =====================================================================
// Arithmetic with full coercion + metatable fallback
// =====================================================================

static const char * const lua_op_names[] = {
    "__add","__sub","__mul","__div","__idiv","__mod","__pow",
    "__band","__bor","__bxor","__bnot","__shl","__shr",
};

static LuaValue
lua_metaop(CTX *c, int op, LuaValue a, LuaValue b)
{
    struct LuaString *mk = lua_str_intern(lua_op_names[op]);
    LuaValue mm = LUAV_NIL;
    if (LV_IS_TBL(a) && LV_AS_TBL(a)->metatable) mm = lua_table_get_str(LV_AS_TBL(a)->metatable, mk);
    if (LV_IS_NIL(mm) && LV_IS_TBL(b) && LV_AS_TBL(b)->metatable) mm = lua_table_get_str(LV_AS_TBL(b)->metatable, mk);
    if (LV_IS_NIL(mm)) {
        lua_raisef(c, "attempt to perform arithmetic on a %s value", lua_type_name(LV_IS_NUM(a) ? b : a));
    }
    LuaValue argv[2] = {a, b};
    return lua_call(c, mm, argv, 2);
}

LuaValue
lua_arith(CTX *c, int op, LuaValue a, LuaValue b)
{
    int64_t ai, bi;
    double  af, bf;
    bool ai_ok = lua_to_int(a, &ai), bi_ok = lua_to_int(b, &bi);
    bool af_ok = lua_to_float(a, &af), bf_ok = lua_to_float(b, &bf);

    // Integer-preserving ops when both are integers (or string-coercible).
    if (ai_ok && bi_ok && !LV_IS_FLOAT(a) && !LV_IS_FLOAT(b)) {
        switch (op) {
        case LUA_OP_ADD: return LUAV_INT(ai + bi);
        case LUA_OP_SUB: return LUAV_INT(ai - bi);
        case LUA_OP_MUL: return LUAV_INT(ai * bi);
        case LUA_OP_FLOORDIV:
            if (bi == 0) lua_raisef(c, "attempt to perform 'n//0'");
            { int64_t q = ai / bi; if ((ai ^ bi) < 0 && q * bi != ai) q -= 1; return LUAV_INT(q); }
        case LUA_OP_MOD:
            if (bi == 0) lua_raisef(c, "attempt to perform 'n%%0'");
            { int64_t r = ai % bi; if (r != 0 && (r ^ bi) < 0) r += bi; return LUAV_INT(r); }
        case LUA_OP_BAND: return LUAV_INT(ai & bi);
        case LUA_OP_BOR:  return LUAV_INT(ai | bi);
        case LUA_OP_BXOR: return LUAV_INT(ai ^ bi);
        case LUA_OP_SHL:  return LUAV_INT(bi >= 64 ? 0 : (int64_t)((uint64_t)ai << bi));
        case LUA_OP_SHR:  return LUAV_INT(bi >= 64 ? 0 : (int64_t)((uint64_t)ai >> bi));
        default: break;
        }
    }
    if (af_ok && bf_ok) {
        switch (op) {
        case LUA_OP_ADD: return LUAV_FLOAT(af + bf);
        case LUA_OP_SUB: return LUAV_FLOAT(af - bf);
        case LUA_OP_MUL: return LUAV_FLOAT(af * bf);
        case LUA_OP_DIV: return LUAV_FLOAT(af / bf);
        case LUA_OP_FLOORDIV: return LUAV_FLOAT(floor(af / bf));
        case LUA_OP_MOD: { double m = fmod(af, bf); if (m != 0 && (m < 0) != (bf < 0)) m += bf; return LUAV_FLOAT(m); }
        case LUA_OP_POW: return LUAV_FLOAT(pow(af, bf));
        default: break;
        }
    }
    return lua_metaop(c, op, a, b);
}

LuaValue
lua_unm(CTX *c, LuaValue a)
{
    if (LV_IS_INT(a))   return LUAV_INT(-LV_AS_INT(a));
    if (LV_IS_FLOAT(a)) return LUAV_FLOAT(-LV_AS_FLOAT(a));
    int64_t i; if (lua_to_int(a, &i))   return LUAV_INT(-i);
    double  f; if (lua_to_float(a, &f)) return LUAV_FLOAT(-f);
    if (LV_IS_TBL(a) && LV_AS_TBL(a)->metatable) {
        LuaValue mm = lua_table_get_str(LV_AS_TBL(a)->metatable, lua_str_intern("__unm"));
        if (LV_IS_CALL(mm)) {
            LuaValue argv[2] = {a, a};
            return lua_call(c, mm, argv, 2);
        }
    }
    lua_raisef(c, "attempt to perform arithmetic on a %s value", lua_type_name(a));
    return LUAV_NIL;
}

LuaValue
lua_concat(CTX *c, LuaValue a, LuaValue b)
{
    if ((LV_IS_STR(a) || LV_IS_NUM(a)) && (LV_IS_STR(b) || LV_IS_NUM(b))) {
        LuaValue sa = lua_tostring(c, a), sb = lua_tostring(c, b);
        return LUAV_STR(lua_str_concat(LV_AS_STR(sa), LV_AS_STR(sb)));
    }
    if (LV_IS_TBL(a) && LV_AS_TBL(a)->metatable) {
        LuaValue mm = lua_table_get_str(LV_AS_TBL(a)->metatable, lua_str_intern("__concat"));
        if (LV_IS_CALL(mm)) {
            LuaValue argv[2] = {a, b};
            return lua_call(c, mm, argv, 2);
        }
    }
    if (LV_IS_TBL(b) && LV_AS_TBL(b)->metatable) {
        LuaValue mm = lua_table_get_str(LV_AS_TBL(b)->metatable, lua_str_intern("__concat"));
        if (LV_IS_CALL(mm)) {
            LuaValue argv[2] = {a, b};
            return lua_call(c, mm, argv, 2);
        }
    }
    lua_raisef(c, "attempt to concatenate a %s value", lua_type_name(LV_IS_STR(a) || LV_IS_NUM(a) ? b : a));
    return LUAV_NIL;
}

int64_t
lua_len(CTX *c, LuaValue v)
{
    if (LV_IS_STR(v)) return (int64_t)LV_AS_STR(v)->len;
    if (LV_IS_TBL(v)) {
        if (LV_AS_TBL(v)->metatable) {
            LuaValue mm = lua_table_get_str(LV_AS_TBL(v)->metatable, lua_str_intern("__len"));
            if (LV_IS_CALL(mm)) {
                LuaValue argv[1] = {v};
                LuaValue r = lua_call(c, mm, argv, 1);
                int64_t i; if (lua_to_int(r, &i)) return i;
            }
        }
        return lua_table_len(LV_AS_TBL(v));
    }
    lua_raisef(c, "attempt to get length of a %s value", lua_type_name(v));
    return 0;
}

// =====================================================================
// Function call — dispatches on closure vs cfunc.
//
// The Lua closure path allocates a fresh frame on the CTX stack, copies
// args into local slots, and invokes EVAL on the body.  Multi-return
// values are stashed in c->ret_info before the dispatcher returns.
// =====================================================================

// Thread-local-ish: pointer to the current closure's upvalue table.
// Set by lua_call before dispatching the body, restored on return.
LuaValue **LUASTRO_CUR_UPVALS = NULL;

// Branch-state pseudo-register.  Set by node_break / node_return /
// node_goto, read by every loop / sequence node, and converted back to
// LUA_BR_NORMAL at function-call boundaries.  Living outside the
// LuaValue lets gcc keep return values purely in registers; without
// this split, the .br byte sat at offset 1 of every returned LuaValue
// and gcc's xmm→stack→movzbl extraction was the dominant hotspot.
uint32_t LUASTRO_BR     = LUA_BR_NORMAL;
LuaValue LUASTRO_BR_VAL = LUAV_NIL;

// Fast-path closure invocation used by node_call_arg{0,1,2,3} EVAL
// bodies.  Defined here (out of line) so all TUs — host binary and
// dynamically loaded SD shared object — share one copy.  An inline-in-
// every-TU version had subtly different per-copy bookkeeping that
// caused c->sp to leak across SD↔host transitions in deeply recursive
// programs.
RESULT
luastro_inline_call(CTX *c, LuaValue fn, uint32_t nargs, const LuaValue *argv)
{
    if (__builtin_expect(LV_IS_FN(fn), 1)) {
        struct LuaClosure *cl = LV_AS_FN(fn);
        if (__builtin_expect(cl->nparams == nargs && !cl->is_vararg, 1)) {
            LuaValue *nf = c->sp;
            if (__builtin_expect(nf + cl->nlocals + 8 < c->stack_end, 1)) {
                c->sp = nf + cl->nlocals;
                for (uint32_t i = 0; i < nargs; i++)           nf[i] = argv[i];
                for (uint32_t i = nargs; i < cl->nlocals; i++) nf[i] = LUAV_NIL;
                LuaValue **pu = LUASTRO_CUR_UPVALS;
                LUASTRO_CUR_UPVALS = cl->upvals;
                RESULT r = (*cl->body->head.dispatcher)(c, cl->body, nf);
                LUASTRO_CUR_UPVALS = pu;
                c->sp = nf;
                if (LUASTRO_BR == LUA_BR_RETURN)      LUASTRO_BR = LUA_BR_NORMAL;
                else if (LUASTRO_BR != LUA_BR_NORMAL) r = RESULT_OK(LUAV_NIL);
                return r;
            }
        }
    }
    return lua_call(c, fn, (LuaValue *)argv, nargs);
}

RESULT
lua_call(CTX *c, LuaValue fn, LuaValue *args, uint32_t argc)
{
    // Follow __call metamethod chain on tables.
    while (LV_IS_TBL(fn)) {
        if (!LV_AS_TBL(fn)->metatable) lua_raisef(c, "attempt to call a table value");
        LuaValue mm = lua_table_get_str(LV_AS_TBL(fn)->metatable, lua_str_intern("__call"));
        if (LV_IS_NIL(mm)) lua_raisef(c, "attempt to call a table value");
        // Prepend the table as first arg.
        LuaValue *argv2 = (LuaValue *)alloca(sizeof(LuaValue) * (argc + 1));
        argv2[0] = fn;
        for (uint32_t i = 0; i < argc; i++) argv2[i + 1] = args[i];
        fn = mm;
        args = argv2;
        argc++;
    }

    if (LV_IS_CF(fn)) {
        return LV_AS_CF(fn)->fn(c, args, argc);
    }
    if (!LV_IS_FN(fn)) {
        lua_raisef(c, "attempt to call a %s value", lua_type_name(fn));
    }

    struct LuaClosure *cl = LV_AS_FN(fn);

    // Allocate frame: nlocals slots.
    if (c->sp + cl->nlocals + 8 > c->stack_end) {
        lua_raisef(c, "stack overflow");
    }
    LuaValue *frame = c->sp;
    c->sp += cl->nlocals;

    // Copy args into the first nparams local slots.  Extra args go into
    // varargs storage if the function is vararg; missing args are nil.
    uint32_t fixed = argc < cl->nparams ? argc : cl->nparams;
    for (uint32_t i = 0; i < fixed; i++)             frame[i] = args[i];
    for (uint32_t i = fixed; i < cl->nlocals; i++)   frame[i] = LUAV_NIL;

    // Save / set varargs and current-upvalue pointer.
    LuaValue *prev_va     = c->varargs;
    uint32_t  prev_va_cnt = c->varargs_cnt;
    LuaValue **prev_up    = LUASTRO_CUR_UPVALS;
    if (cl->is_vararg && argc > cl->nparams) {
        c->varargs     = args + cl->nparams;
        c->varargs_cnt = argc - cl->nparams;
    } else {
        c->varargs     = NULL;
        c->varargs_cnt = 0;
    }
    LUASTRO_CUR_UPVALS = cl->upvals;

    RESULT r = EVAL(c, cl->body, frame);

    // Close any boxed locals before frame is reused.  We only need to
    // null out the .u.p pointers — the heap cells live until refs die.
    // (No GC yet — they leak.  v1 acceptable.)
    c->sp              = frame;
    c->varargs         = prev_va;
    c->varargs_cnt     = prev_va_cnt;
    LUASTRO_CUR_UPVALS = prev_up;

    if (LUASTRO_BR == LUA_BR_RETURN) {
        LUASTRO_BR = LUA_BR_NORMAL;
    } else {
        // Fell off the end without explicit return — single nil.
        r = LUAV_NIL;
        LUASTRO_BR = LUA_BR_NORMAL;
        c->ret_info.result_cnt = 0;
    }
    return r;
}

// =====================================================================
// Context creation
// =====================================================================

CTX *
luastro_create_context(void)
{
    CTX *c = (CTX *)calloc(1, sizeof(CTX));
    // calloc gives us a zero-filled stack, which encodes LUA_TNIL since
    // LUA_TNIL == 0.  Skips the linear init loop on a 4M-slot stack.
    c->stack     = (LuaValue *)calloc(LUASTRO_STACK_SIZE, sizeof(LuaValue));
    c->stack_end = c->stack + LUASTRO_STACK_SIZE;
    c->sp        = c->stack;
    c->globals   = lua_table_new(0, 32);
    c->ret_info.result_cnt = 0;
    c->last_error = LUAV_NIL;
    return c;
}
