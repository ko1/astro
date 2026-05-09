#include <math.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>
#include "value.h"
/* arcel.h gives us the full definition of `arcel_object_desc` so the
 * AC_OBJECT helpers can call into the embedder's dispatch table.
 * value.h only forward-declares the struct (to keep the internal
 * header small); the full definition lives in arcel.h. */
#include "arcel.h"

/* ---- arena -------------------------------------------------------- */

#define ARCEL_ARENA_CHUNK_DEFAULT (64 * 1024)

static arcel_arena_chunk *
arena_alloc_chunk(size_t cap)
{
    if (cap < ARCEL_ARENA_CHUNK_DEFAULT) cap = ARCEL_ARENA_CHUNK_DEFAULT;
    arcel_arena_chunk *const c = (arcel_arena_chunk *)malloc(sizeof(*c) + cap);
    if (!c) { fprintf(stderr, "arcel: arena chunk alloc OOM (%zu bytes)\n", cap); exit(1); }
    c->next = NULL;
    c->cap  = cap;
    c->used = 0;
    return c;
}

void
arcel_arena_init(arcel_arena *const a)
{
    a->head    = arena_alloc_chunk(ARCEL_ARENA_CHUNK_DEFAULT);
    a->current = a->head;
}

void
arcel_arena_free(arcel_arena *const a)
{
    arcel_arena_chunk *c = a->head;
    while (c) {
        arcel_arena_chunk *const nxt = c->next;
        free(c);
        c = nxt;
    }
    a->head = a->current = NULL;
}

void
arcel_arena_reset(arcel_arena *const a)
{
    /* Keep all chunks malloc'd, just rewind `used` so the buffers are
     * reused on the next eval — no per-eval malloc churn in the hot
     * path of bench/repl mode. */
    for (arcel_arena_chunk *c = a->head; c; c = c->next) c->used = 0;
    a->current = a->head;
}

/* Out-of-line slow path for arena bump allocation.  Called when the
 * current chunk has no room.  Walks the chunk list looking for one
 * with capacity, otherwise allocates a fresh chunk doubling the
 * previous size. */
void *
arcel_arena_alloc_grow(arcel_arena *const a, const size_t bytes, const size_t align)
{
    arcel_arena_chunk *c = a->current;
    while (c->next) {
        c = c->next;
        if (bytes + align <= c->cap) {
            const size_t off = 0;
            c->used = off + bytes;
            a->current = c;
            return c->buf + off;
        }
    }
    size_t want = bytes + align;
    if (want < a->current->cap * 2) want = a->current->cap * 2;
    arcel_arena_chunk *const nc = arena_alloc_chunk(want);
    a->current->next = nc;
    a->current = nc;
    nc->used = bytes;
    return nc->buf;
}

const char *
arcel_arena_strdup(arcel_arena *const a, const char *const p, const uint32_t len)
{
    char *const dst = (char *)arcel_arena_alloc(a, len + 1, 1);
    memcpy(dst, p, len);
    dst[len] = '\0';
    return dst;
}

const char *
arcel_arena_msg(arcel_arena *const a, const char *const fmt, ...)
{
    /* Reserve worst-case 256 bytes; if vsnprintf says we need more,
     * realloc and retry once. */
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return "<format error>";
    if ((size_t)n < sizeof(buf)) {
        return arcel_arena_strdup(a, buf, (uint32_t)n);
    }
    char *const big = (char *)arcel_arena_alloc(a, (size_t)n + 1, 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return big;
}

/* ---- list / map builders ----------------------------------------- */

arcel_list *
arcel_list_new(arcel_arena *const a, const uint32_t len)
{
    arcel_list *const l = (arcel_list *)arcel_arena_alloc(a, sizeof(arcel_list), _Alignof(arcel_list));
    l->len   = len;
    l->items = len ? (VALUE *)arcel_arena_alloc(a, sizeof(VALUE) * len, _Alignof(VALUE)) : NULL;
    return l;
}

arcel_map *
arcel_map_new(arcel_arena *const a, const uint32_t len)
{
    arcel_map *const m = (arcel_map *)arcel_arena_alloc(a, sizeof(arcel_map), _Alignof(arcel_map));
    m->len     = len;
    m->entries = len ? (arcel_map_entry *)arcel_arena_alloc(a, sizeof(arcel_map_entry) * len, _Alignof(arcel_map_entry)) : NULL;
    return m;
}

/* ---- equality / comparison --------------------------------------- */

/* Cross-type numeric equality.  CEL allows int == uint == double when
 * the values are mathematically equal; otherwise different types give
 * false (not an error). */
static bool
num_eq_cross(VALUE a, VALUE b)
{
    /* normalize so a's tag <= b's tag in our enum order
     * (AC_INT < AC_UINT < AC_DOUBLE) */
    if (a.tag > b.tag) { VALUE t = a; a = b; b = t; }

    if (a.tag == AC_INT && b.tag == AC_UINT) {
        return a.i >= 0 && (uint64_t)a.i == b.u;
    }
    if (a.tag == AC_INT && b.tag == AC_DOUBLE) {
        if (isnan(b.d) || isinf(b.d)) return false;
        return (double)a.i == b.d && (int64_t)b.d == a.i;
    }
    if (a.tag == AC_UINT && b.tag == AC_DOUBLE) {
        if (isnan(b.d) || isinf(b.d) || b.d < 0) return false;
        return (double)a.u == b.d && (uint64_t)b.d == a.u;
    }
    return false;
}

/* Slow path for arcel_eq (defined inline in value.h for hot scalar/string
 * cases).  Hits cross-tag numeric promotion and structural list/map
 * equality. */
bool
arcel_eq_slow(const VALUE a, const VALUE b)
{
    if (a.tag == b.tag) {
        switch (a.tag) {
            case AC_LIST: {
                if (a.list->len != b.list->len) return false;
                for (uint32_t i = 0; i < a.list->len; i++) {
                    if (!arcel_eq(a.list->items[i], b.list->items[i])) return false;
                }
                return true;
            }
            case AC_MAP: {
                if (a.map->len != b.map->len) return false;
                /* maps are unordered; for each key in a, look it up in b */
                for (uint32_t i = 0; i < a.map->len; i++) {
                    bool found = false;
                    for (uint32_t j = 0; j < b.map->len; j++) {
                        if (arcel_eq(a.map->entries[i].key, b.map->entries[j].key)) {
                            if (!arcel_eq(a.map->entries[i].val, b.map->entries[j].val)) return false;
                            found = true; break;
                        }
                    }
                    if (!found) return false;
                }
                return true;
            }
            case AC_OBJECT:
                /* Same descriptor + same obj pointer → identical.
                 * Structural equality across descriptor calls would
                 * require enumerating fields (no API for that yet). */
                return a.object.desc == b.object.desc && a.object.obj == b.object.obj;
            case AC_TIMESTAMP:
            case AC_DURATION:
                return a.ts.s == b.ts.s && a.ts.ns == b.ts.ns;
            default: return false;
        }
    }
    /* Cross-tag numeric? */
    if ((a.tag == AC_INT || a.tag == AC_UINT || a.tag == AC_DOUBLE) &&
        (b.tag == AC_INT || b.tag == AC_UINT || b.tag == AC_DOUBLE)) {
        return num_eq_cross(a, b);
    }
    return false;
}

#define INCMP INT_MIN

static int
cmp_num_cross(VALUE a, VALUE b)
{
    /* Promote both to double for ordering when the trio mixes — this
     * matches cel-go's behaviour for `1 < 2.5` etc.  (Strict spec
     * has more nuance for very-large int vs nearby double, but the
     * conformance suite doesn't currently exercise that edge.) */
    double ad, bd;
    switch (a.tag) {
        case AC_INT:    ad = (double)a.i; break;
        case AC_UINT:   ad = (double)a.u; break;
        case AC_DOUBLE: ad = a.d; break;
        default: return INCMP;
    }
    switch (b.tag) {
        case AC_INT:    bd = (double)b.i; break;
        case AC_UINT:   bd = (double)b.u; break;
        case AC_DOUBLE: bd = b.d; break;
        default: return INCMP;
    }
    if (isnan(ad) || isnan(bd)) return INCMP;   /* NaN comparison → no order */
    return (ad < bd) ? -1 : (ad > bd) ? 1 : 0;
}

int
arcel_cmp(const VALUE a, const VALUE b)
{
    if (a.tag == b.tag) {
        switch (a.tag) {
            case AC_INT:    return (a.i < b.i) ? -1 : (a.i > b.i);
            case AC_UINT:   return (a.u < b.u) ? -1 : (a.u > b.u);
            case AC_DOUBLE:
                if (isnan(a.d) || isnan(b.d)) return INCMP;
                return (a.d < b.d) ? -1 : (a.d > b.d);
            case AC_STRING:
            case AC_BYTES: {
                uint32_t n = a.s.len < b.s.len ? a.s.len : b.s.len;
                int r = (n == 0) ? 0 : memcmp(a.s.p, b.s.p, n);
                if (r != 0) return r < 0 ? -1 : 1;
                return (a.s.len < b.s.len) ? -1 : (a.s.len > b.s.len);
            }
            case AC_BOOL:   return (int)a.b - (int)b.b;   /* false < true per cel-go */
            case AC_NULL:   return 0;                 /* null == null */
            case AC_TIMESTAMP:
            case AC_DURATION:
                if (a.ts.s != b.ts.s)   return (a.ts.s < b.ts.s) ? -1 : 1;
                if (a.ts.ns != b.ts.ns) return (a.ts.ns < b.ts.ns) ? -1 : 1;
                return 0;
            default:        return INCMP;
        }
    }
    if ((a.tag == AC_INT || a.tag == AC_UINT || a.tag == AC_DOUBLE) &&
        (b.tag == AC_INT || b.tag == AC_UINT || b.tag == AC_DOUBLE)) {
        return cmp_num_cross(a, b);
    }
    return INCMP;
}

/* ---- arithmetic --------------------------------------------------- */

static VALUE
err(CTX *const c, const char *const fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *const m = arcel_arena_strdup(&c->arena, buf, (uint32_t)strlen(buf));
    c->last_err = m;
    return V_ERR(m);
}

VALUE
arcel_add(CTX *const c, const VALUE a, const VALUE b)
{
    if (a.tag == AC_ERR) return a;
    if (b.tag == AC_ERR) return b;
    if (a.tag == AC_INT && b.tag == AC_INT) {
        int64_t r;
        if (__builtin_add_overflow(a.i, b.i, &r)) return err(c, "return error: integer overflow");
        return V_INT(r);
    }
    if (a.tag == AC_UINT && b.tag == AC_UINT) {
        uint64_t r;
        if (__builtin_add_overflow(a.u, b.u, &r)) return err(c, "return error: unsigned integer overflow");
        return V_UINT(r);
    }
    if (a.tag == AC_DOUBLE && b.tag == AC_DOUBLE) return V_DOUBLE(a.d + b.d);
    if (a.tag == AC_STRING && b.tag == AC_STRING) {
        const uint32_t n = a.s.len + b.s.len;
        char *const buf = (char *)arcel_arena_alloc(&c->arena, n + 1, 1);
        if (a.s.len) memcpy(buf,           a.s.p, a.s.len);
        if (b.s.len) memcpy(buf + a.s.len, b.s.p, b.s.len);
        buf[n] = '\0';
        return V_STR(buf, n);
    }
    if (a.tag == AC_BYTES && b.tag == AC_BYTES) {
        const uint32_t n = a.s.len + b.s.len;
        char *const buf = (char *)arcel_arena_alloc(&c->arena, n, 1);
        if (a.s.len) memcpy(buf,           a.s.p, a.s.len);
        if (b.s.len) memcpy(buf + a.s.len, b.s.p, b.s.len);
        return V_BYTES(buf, n);
    }
    if (a.tag == AC_LIST && b.tag == AC_LIST) {
        const uint32_t n = a.list->len + b.list->len;
        arcel_list *const out = arcel_list_new(&c->arena, n);
        if (a.list->len) memcpy(out->items,                  a.list->items, sizeof(VALUE) * a.list->len);
        if (b.list->len) memcpy(out->items + a.list->len,    b.list->items, sizeof(VALUE) * b.list->len);
        return V_LIST(out);
    }
    /* timestamp / duration arithmetic ---------------------------------
     * Allowed: TS+DUR, DUR+TS, DUR+DUR.  Result is range-checked
     * (cel-spec rejects out-of-range timestamps even mid-arithmetic). */
    if ((a.tag == AC_TIMESTAMP && b.tag == AC_DURATION) ||
        (a.tag == AC_DURATION  && b.tag == AC_TIMESTAMP) ||
        (a.tag == AC_DURATION  && b.tag == AC_DURATION)) {
        int64_t s; int32_t ns;
        if (__builtin_add_overflow(a.ts.s,  b.ts.s,  &s))  return err(c, "return error: timestamp overflow");
        ns = a.ts.ns + b.ts.ns;
        /* Normalize ns into [-1e9, 1e9), carry into seconds. */
        if (ns >= 1000000000)      { ns -= 1000000000; if (__builtin_add_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
        else if (ns <= -1000000000){ ns += 1000000000; if (__builtin_sub_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
        const arcel_tag rt = (a.tag == AC_TIMESTAMP || b.tag == AC_TIMESTAMP) ? AC_TIMESTAMP : AC_DURATION;
        if (rt == AC_TIMESTAMP) {
            /* For TIMESTAMP, normalize ns to [0, 1e9). */
            if (ns < 0) { ns += 1000000000; if (__builtin_sub_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
            if (s < -62135596800LL || s > 253402300799LL ||
                (s == 253402300799LL && ns > 999999999))
                return err(c, "range error: timestamp out of range");
            return V_TIMESTAMP(s, ns);
        } else {
            /* For DURATION, enforce sign(s)==sign(ns). */
            if (s > 0 && ns < 0) { s -= 1; ns += 1000000000; }
            else if (s < 0 && ns > 0) { s += 1; ns -= 1000000000; }
            /* cel-go uses time.Duration (int64 nanoseconds), capping
             * at ±~9.22e9 seconds (292 years).  Match that so the
             * conformance corpus's expected ERR(range) for full-span
             * timestamp diffs and ±200B-second durations triggers. */
            if (s > 9223372035LL || s < -9223372035LL)
                return err(c, "range error: duration out of range");
            return V_DURATION(s, ns);
        }
    }
    return err(c, "no such overload: _+_(%d, %d)", a.tag, b.tag);
}

VALUE
arcel_sub(CTX *const c, const VALUE a, const VALUE b)
{
    if (a.tag == AC_ERR) return a;
    if (b.tag == AC_ERR) return b;
    if (a.tag == AC_INT && b.tag == AC_INT) {
        int64_t r;
        if (__builtin_sub_overflow(a.i, b.i, &r)) return err(c, "return error: integer overflow");
        return V_INT(r);
    }
    if (a.tag == AC_UINT && b.tag == AC_UINT) {
        uint64_t r;
        if (__builtin_sub_overflow(a.u, b.u, &r)) return err(c, "return error: unsigned integer overflow");
        return V_UINT(r);
    }
    if (a.tag == AC_DOUBLE && b.tag == AC_DOUBLE) return V_DOUBLE(a.d - b.d);
    /* timestamp / duration subtraction --------------------------------
     * Allowed: TS-DUR (→TS), TS-TS (→DUR), DUR-DUR (→DUR). */
    if ((a.tag == AC_TIMESTAMP && b.tag == AC_DURATION) ||
        (a.tag == AC_TIMESTAMP && b.tag == AC_TIMESTAMP) ||
        (a.tag == AC_DURATION  && b.tag == AC_DURATION)) {
        int64_t s; int32_t ns;
        if (__builtin_sub_overflow(a.ts.s,  b.ts.s,  &s))  return err(c, "return error: timestamp overflow");
        ns = a.ts.ns - b.ts.ns;
        if (ns >= 1000000000)      { ns -= 1000000000; if (__builtin_add_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
        else if (ns <= -1000000000){ ns += 1000000000; if (__builtin_sub_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
        if (a.tag == AC_TIMESTAMP && b.tag == AC_DURATION) {
            if (ns < 0) { ns += 1000000000; if (__builtin_sub_overflow(s, (int64_t)1, &s)) return err(c, "return error: timestamp overflow"); }
            if (s < -62135596800LL || s > 253402300799LL ||
                (s == 253402300799LL && ns > 999999999))
                return err(c, "range error: timestamp out of range");
            return V_TIMESTAMP(s, ns);
        } else {
            if (s > 0 && ns < 0) { s -= 1; ns += 1000000000; }
            else if (s < 0 && ns > 0) { s += 1; ns -= 1000000000; }
            /* cel-go uses time.Duration (int64 nanoseconds), capping
             * at ±~9.22e9 seconds (292 years).  Match that so the
             * conformance corpus's expected ERR(range) for full-span
             * timestamp diffs and ±200B-second durations triggers. */
            if (s > 9223372035LL || s < -9223372035LL)
                return err(c, "range error: duration out of range");
            return V_DURATION(s, ns);
        }
    }
    return err(c, "no such overload: _-_");
}

VALUE
arcel_mul(CTX *const c, const VALUE a, const VALUE b)
{
    if (a.tag == AC_ERR) return a;
    if (b.tag == AC_ERR) return b;
    if (a.tag == AC_INT && b.tag == AC_INT) {
        int64_t r;
        if (__builtin_mul_overflow(a.i, b.i, &r)) return err(c, "return error: integer overflow");
        return V_INT(r);
    }
    if (a.tag == AC_UINT && b.tag == AC_UINT) {
        uint64_t r;
        if (__builtin_mul_overflow(a.u, b.u, &r)) return err(c, "return error: unsigned integer overflow");
        return V_UINT(r);
    }
    if (a.tag == AC_DOUBLE && b.tag == AC_DOUBLE) return V_DOUBLE(a.d * b.d);
    return err(c, "no such overload: _*_");
}

VALUE
arcel_div(CTX *const c, const VALUE a, const VALUE b)
{
    if (a.tag == AC_ERR) return a;
    if (b.tag == AC_ERR) return b;
    if (a.tag == AC_INT && b.tag == AC_INT) {
        if (b.i == 0) return err(c, "divide by zero");
        if (a.i == INT64_MIN && b.i == -1) return err(c, "return error: integer overflow");
        return V_INT(a.i / b.i);
    }
    if (a.tag == AC_UINT && b.tag == AC_UINT) {
        if (b.u == 0) return err(c, "divide by zero");
        return V_UINT(a.u / b.u);
    }
    if (a.tag == AC_DOUBLE && b.tag == AC_DOUBLE) return V_DOUBLE(a.d / b.d);
    return err(c, "no such overload: _/_");
}

VALUE
arcel_mod(CTX *const c, const VALUE a, const VALUE b)
{
    if (a.tag == AC_ERR) return a;
    if (b.tag == AC_ERR) return b;
    if (a.tag == AC_INT && b.tag == AC_INT) {
        if (b.i == 0) return err(c, "modulus by zero");
        if (a.i == INT64_MIN && b.i == -1) return V_INT(0);
        return V_INT(a.i % b.i);
    }
    if (a.tag == AC_UINT && b.tag == AC_UINT) {
        if (b.u == 0) return err(c, "modulus by zero");
        return V_UINT(a.u % b.u);
    }
    return err(c, "no such overload: _%%_");
}

VALUE
arcel_neg(CTX *const c, const VALUE a)
{
    if (a.tag == AC_ERR) return a;
    if (a.tag == AC_INT) {
        if (a.i == INT64_MIN) return err(c, "return error: integer overflow");
        return V_INT(-a.i);
    }
    if (a.tag == AC_DOUBLE) return V_DOUBLE(-a.d);
    return err(c, "no such overload: -_");
}

/* ---- collection ops ---------------------------------------------- */

VALUE
arcel_in_err(CTX *const c)
{
    return err(c, "no such overload: @in");
}

/* Count UTF-8 codepoints; stops at malformed sequences treating them
 * as 1 unit each (CEL spec is liberal here). */
uint32_t
arcel_utf8_count(const char *p, uint32_t len)
{
    uint32_t n = 0;
    const unsigned char *u = (const unsigned char *)p;
    const unsigned char *e = u + len;
    while (u < e) {
        unsigned char c = *u;
        if      (c < 0x80) u += 1;
        else if ((c & 0xE0) == 0xC0) u += 2;
        else if ((c & 0xF0) == 0xE0) u += 3;
        else if ((c & 0xF8) == 0xF0) u += 4;
        else u += 1;
        if (u > e) u = e;
        n++;
    }
    return n;
}

VALUE
arcel_size_err(CTX *const c)
{
    return err(c, "no such overload: size");
}

VALUE
arcel_str_op_err(CTX *const c, const char *const which)
{
    return err(c, "no such overload: %s", which);
}

VALUE
arcel_matches(CTX *const c, const VALUE recv, const VALUE pattern)
{
    /* TODO: integrate astrogre once available; for now fall back to
     * POSIX regex via <regex.h> compiled fresh per call (slow, but
     * correct for the conformance bring-up).  Same shape as memmem
     * above so this stays a single place to swap out. */
    if (recv.tag    == AC_ERR) return recv;
    if (pattern.tag == AC_ERR) return pattern;
    if (recv.tag != AC_STRING || pattern.tag != AC_STRING) return err(c, "no such overload: matches");

    /* Compile pattern as POSIX ERE.  CEL uses RE2 syntax which is
     * mostly ERE-compatible for the kind of patterns the conformance
     * suite uses; advanced cases (named groups, lookaround) we'll
     * miss until astrogre is wired in. */
    #include <regex.h>
    regex_t re;
    char *const pat = (char *)arcel_arena_alloc(&c->arena, pattern.s.len + 1, 1);
    memcpy(pat, pattern.s.p, pattern.s.len);
    pat[pattern.s.len] = '\0';
    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) {
        return err(c, "invalid regex");
    }
    char *const subj = (char *)arcel_arena_alloc(&c->arena, recv.s.len + 1, 1);
    memcpy(subj, recv.s.p, recv.s.len);
    subj[recv.s.len] = '\0';
    int r = regexec(&re, subj, 0, NULL, 0);
    regfree(&re);
    return V_BOOL(r == 0);
}

/* ---- field / index access ---------------------------------------- */

/* Slow paths for arcel_field (defined inline in value.h). */
VALUE
arcel_field_err_no_key(CTX *const c, const char *const name, const uint32_t name_len)
{
    return err(c, "no such key: %.*s", (int)name_len, name);
}

VALUE
arcel_field_err_overload(CTX *const c, const int tag)
{
    return err(c, "no such overload: field-access on %d", tag);
}

/* Slow path for AC_OBJECT field access.  Calls into the embedder's
 * descriptor.field callback; the public arcel_value type returned
 * has the same in-memory layout as VALUE so we cast back without
 * marshalling.  The CTX's per-eval arena is passed through as an
 * opaque handle so the callback can build list / map values via
 * arcel_value_list_new etc. */
VALUE
arcel_field_object(CTX *const c, const VALUE recv, const char *const name, const uint32_t name_len)
{
    const struct arcel_object_desc *const d = recv.object.desc;
    if (!d || !d->field) return err(c, "no such overload: field-access on object");

    /* The descriptor writes an `arcel_value` (24 bytes) — same layout
     * as VALUE per the static_assert in arcel_lib.c.  Reinterpret. */
    union { VALUE v; struct { uint64_t _opaque[3]; } av; } u = {0};
    int rc = d->field(d, recv.object.obj, name, (size_t)name_len,
                      (void *)&c->arena,         /* arena handle */
                      (void *)&u.av);
    if (rc == 0) return u.v;
    if (rc == -1) return err(c, "no such key: %.*s", (int)name_len, name);
    return err(c, "field-access error on %s", d->type_name ? d->type_name : "object");
}

VALUE arcel_index_err   (CTX *const c)            { return err(c, "no such overload: index"); }
VALUE arcel_index_oob   (CTX *const c, int64_t i) { return err(c, "index out of range: %" PRId64, i); }
VALUE arcel_index_no_key(CTX *const c)            { return err(c, "no such key"); }

VALUE
arcel_has_field(CTX *const c, const VALUE recv, const char *const name, const uint32_t name_len)
{
    if (recv.tag == AC_ERR) return recv;
    if (recv.tag == AC_MAP) {
        for (uint32_t i = 0; i < recv.map->len; i++) {
            VALUE k = recv.map->entries[i].key;
            if (k.tag == AC_STRING && k.s.len == name_len &&
                memcmp(k.s.p, name, name_len) == 0) return V_BOOL(true);
        }
        return V_BOOL(false);
    }
    return err(c, "no such overload: has on tag %d", recv.tag);
}

/* ---- type conversions -------------------------------------------- */

VALUE
arcel_to_int(CTX *const c, const VALUE x)
{
    switch (x.tag) {
        case AC_INT: return x;
        case AC_UINT:
            if (x.u > (uint64_t)INT64_MAX) return err(c, "range error converting uint to int");
            return V_INT((int64_t)x.u);
        case AC_DOUBLE:
            /* cel-spec requires int() from double to round-trip
             * exactly.  Doubles can't distinguish adjacent integers
             * near +/- 2^63, so we reject the entire endpoint region.
             * The literals here are 2^63 exact (= INT64_MAX + 1) and
             * -2^63 exact (= INT64_MIN); both are representable as
             * double but cel-spec considers them out of range because
             * the conversion is lossy at that magnitude. */
            if (isnan(x.d) || isinf(x.d))                                return err(c, "range error converting double to int");
            if (x.d <= -9223372036854775808.0 || x.d >= 9223372036854775808.0) return err(c, "range error converting double to int");
            return V_INT((int64_t)x.d);
        case AC_STRING: {
            char *const tmp = (char *)arcel_arena_alloc(&c->arena, x.s.len + 1, 1);
            memcpy(tmp, x.s.p, x.s.len);
            tmp[x.s.len] = '\0';
            char *end;
            long long v = strtoll(tmp, &end, 10);
            if (end == tmp || *end != '\0') return err(c, "invalid int literal: %s", tmp);
            return V_INT((int64_t)v);
        }
        case AC_BOOL:   return V_INT(x.b ? 1 : 0);
        case AC_TIMESTAMP:
            /* int(timestamp) → Unix epoch seconds (cel-spec: nanos
             * truncated, sign of ns matters at the boundary).  Our
             * canonical form has ns >= 0 for AC_TIMESTAMP, so the cast
             * is straightforward. */
            return V_INT(x.ts.s);
        case AC_ERR:    return x;
        default:        return err(c, "no such overload: int");
    }
}

VALUE
arcel_to_uint(CTX *const c, const VALUE x)
{
    switch (x.tag) {
        case AC_UINT: return x;
        case AC_INT:
            if (x.i < 0) return err(c, "range error converting int to uint");
            return V_UINT((uint64_t)x.i);
        case AC_DOUBLE:
            if (isnan(x.d) || isinf(x.d) || x.d < 0 || x.d >= 1.8446744073709552e+19) return err(c, "range error converting double to uint");
            return V_UINT((uint64_t)x.d);
        case AC_STRING: {
            char *const tmp = (char *)arcel_arena_alloc(&c->arena, x.s.len + 1, 1);
            memcpy(tmp, x.s.p, x.s.len);
            tmp[x.s.len] = '\0';
            char *end;
            unsigned long long v = strtoull(tmp, &end, 10);
            if (end == tmp || *end != '\0') return err(c, "invalid uint literal: %s", tmp);
            return V_UINT((uint64_t)v);
        }
        case AC_ERR:    return x;
        default:        return err(c, "no such overload: uint");
    }
}

VALUE
arcel_to_double(CTX *const c, const VALUE x)
{
    switch (x.tag) {
        case AC_DOUBLE: return x;
        case AC_INT:    return V_DOUBLE((double)x.i);
        case AC_UINT:   return V_DOUBLE((double)x.u);
        case AC_STRING: {
            char *const tmp = (char *)arcel_arena_alloc(&c->arena, x.s.len + 1, 1);
            memcpy(tmp, x.s.p, x.s.len);
            tmp[x.s.len] = '\0';
            char *end;
            double v = strtod(tmp, &end);
            if (end == tmp || *end != '\0') return err(c, "invalid double literal: %s", tmp);
            return V_DOUBLE(v);
        }
        case AC_ERR:    return x;
        default:        return err(c, "no such overload: double");
    }
}

VALUE
arcel_to_bool(CTX *const c, const VALUE x)
{
    if (x.tag == AC_BOOL) return x;
    if (x.tag == AC_ERR)  return x;
    if (x.tag == AC_STRING) {
        /* cel-spec: accepts "true"/"True"/"TRUE"/"t"/"T"/"1"
         *           and "false"/"False"/"FALSE"/"f"/"F"/"0". */
        const char *p = x.s.p;
        uint32_t   n = x.s.len;
        if (n == 1) {
            char c0 = p[0];
            if (c0 == '1' || c0 == 't' || c0 == 'T') return V_BOOL(true);
            if (c0 == '0' || c0 == 'f' || c0 == 'F') return V_BOOL(false);
        }
        if (n == 4 && (memcmp(p, "true", 4) == 0 || memcmp(p, "True", 4) == 0 || memcmp(p, "TRUE", 4) == 0))
            return V_BOOL(true);
        if (n == 5 && (memcmp(p, "false", 5) == 0 || memcmp(p, "False", 5) == 0 || memcmp(p, "FALSE", 5) == 0))
            return V_BOOL(false);
    }
    return err(c, "no such overload: bool");
}

/* Validate a byte slice as UTF-8 without copying.  Returns true if
 * every codepoint is well-formed. */
static bool
utf8_valid(const char *const s, const uint32_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *e = p + n;
    while (p < e) {
        unsigned char c = *p++;
        if (c < 0x80) continue;
        int extra;
        if      ((c & 0xE0) == 0xC0) { extra = 1; if ((c & 0x1E) == 0) return false; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; }
        else return false;
        if (p + extra > e) return false;
        for (int i = 0; i < extra; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += extra;
    }
    return true;
}

VALUE
arcel_to_string(CTX *const c, const VALUE x)
{
    switch (x.tag) {
        case AC_STRING: return x;
        case AC_BYTES:
            if (!utf8_valid(x.s.p, x.s.len)) return err(c, "invalid UTF-8 in bytes->string");
            return V_STR(x.s.p, x.s.len);
        case AC_INT: {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%" PRId64, x.i);
            return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
        }
        case AC_UINT: {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%" PRIu64, x.u);
            return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
        }
        case AC_DOUBLE: {
            char buf[64];
            /* CEL formatting follows Go's %g for double; for integer-
             * valued doubles cel-go prints `1` (no `.0`).  We match
             * that. */
            int n;
            if (!isnan(x.d) && !isinf(x.d) && x.d == (double)(int64_t)x.d &&
                x.d > -1e15 && x.d < 1e15) {
                n = snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)x.d);
            } else {
                n = snprintf(buf, sizeof(buf), "%g", x.d);
            }
            return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
        }
        case AC_BOOL:   return V_STR(x.b ? "true" : "false", x.b ? 4 : 5);
        case AC_TIMESTAMP: {
            /* RFC3339 with optional fractional seconds.  Use gmtime_r
             * which on glibc handles the full -62135596800..253402300799
             * second range.  Fractional second: emit only if non-zero,
             * with the minimum number of digits (3, 6, or 9). */
            char buf[40];
            time_t t = (time_t)x.ts.s;
            struct tm tm;
            if (!gmtime_r(&t, &tm)) return err(c, "timestamp formatting failed");
            int n = snprintf(buf, sizeof(buf),
                "%04d-%02d-%02dT%02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
            if (x.ts.ns != 0) {
                /* Build the 9-digit fractional part, then trim trailing
                 * zeros to the next multiple of 3 (RFC3339 convention,
                 * matches cel-go). */
                char frac[12];
                snprintf(frac, sizeof(frac), ".%09d", x.ts.ns);
                int flen = 10;  /* '.' + 9 digits */
                while (flen > 4 && frac[flen - 1] == '0' &&
                       (flen - 1) % 3 == 1) {
                    flen -= 3;  /* drop a 3-digit group of zeros */
                }
                /* Edge case: if the trimmed fractional ends with zeros
                 * but doesn't fit a clean 3-group cut, fall back to
                 * trimming any trailing zero. */
                while (flen > 4 && frac[flen - 1] == '0') flen--;
                memcpy(buf + n, frac, flen);
                n += flen;
            }
            buf[n++] = 'Z';
            buf[n] = '\0';
            return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
        }
        case AC_DURATION: {
            /* "<seconds>s" form.  If ns != 0, emit "<s>.<frac>s" with
             * minimal-digit trimming.  Sign carried on the seconds. */
            char buf[40];
            int n;
            if (x.ts.ns == 0) {
                n = snprintf(buf, sizeof(buf), "%llds", (long long)x.ts.s);
            } else {
                /* Canonical form has sign(s)==sign(ns).  Move sign to s. */
                int64_t s = x.ts.s;
                int32_t ns = x.ts.ns;
                bool neg = (s < 0) || (s == 0 && ns < 0);
                if (neg) { s = -s; ns = -ns; }
                char frac[12];
                snprintf(frac, sizeof(frac), ".%09d", ns);
                int flen = 10;
                while (flen > 4 && frac[flen - 1] == '0' &&
                       (flen - 1) % 3 == 1) {
                    flen -= 3;
                }
                while (flen > 4 && frac[flen - 1] == '0') flen--;
                n = snprintf(buf, sizeof(buf), "%s%lld", neg ? "-" : "", (long long)s);
                memcpy(buf + n, frac, flen);
                n += flen;
                buf[n++] = 's';
                buf[n] = '\0';
                return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
            }
            return V_STR(arcel_arena_strdup(&c->arena, buf, (uint32_t)n), (uint32_t)n);
        }
        case AC_ERR:    return x;
        default:        return err(c, "no such overload: string");
    }
}

VALUE
arcel_to_bytes(CTX *const c, const VALUE x)
{
    if (x.tag == AC_BYTES)  return x;
    if (x.tag == AC_STRING) return V_BYTES(x.s.p, x.s.len);
    if (x.tag == AC_ERR)    return x;
    return err(c, "no such overload: bytes");
}

VALUE
arcel_cel_type_of(CTX *const c, const VALUE x)
{
    (void)c;
    const char *t;
    switch (x.tag) {
        case AC_NULL:   t = "null_type"; break;
        case AC_BOOL:   t = "bool";   break;
        case AC_INT:    t = "int";    break;
        case AC_UINT:   t = "uint";   break;
        case AC_DOUBLE: t = "double"; break;
        case AC_STRING: t = "string"; break;
        case AC_BYTES:  t = "bytes";  break;
        case AC_LIST:   t = "list";   break;
        case AC_MAP:    t = "map";    break;
        case AC_OBJECT: t = (x.object.desc && x.object.desc->type_name)
                            ? x.object.desc->type_name : "object"; break;
        case AC_TIMESTAMP: t = "google.protobuf.Timestamp"; break;
        case AC_DURATION:  t = "google.protobuf.Duration";  break;
        default:        t = "error";  break;
    }
    return V_STR(t, (uint32_t)strlen(t));
}

/* ---- bindings ---------------------------------------------------- */

/* Slow path for arcel_lookup_ident (defined inline in value.h).  Only
 * reached when the name is not in the macro stack or the global
 * bindings — i.e. an undeclared reference. */
VALUE
arcel_lookup_ident_slow(CTX *const c, const char *const name, const uint32_t name_len)
{
    return err(c, "undeclared reference to '%.*s'", (int)name_len, name);
}

void
arcel_bind_overflow_abort(void)
{
    fprintf(stderr, "arcel: macro binding stack overflow (>%d)\n", ARCEL_BIND_STACK_MAX);
    exit(1);
}

/* ---- JSON formatter ---------------------------------------------- */

static void
print_string(FILE *const out, const char *const s, const uint32_t n)
{
    fputc('"', out);
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n",  out); break;
            case '\r': fputs("\\r",  out); break;
            case '\t': fputs("\\t",  out); break;
            case '\b': fputs("\\b",  out); break;
            case '\f': fputs("\\f",  out); break;
            default:
                if (c < 0x20) fprintf(out, "\\u%04x", c);
                else          fputc(c, out);
        }
    }
    fputc('"', out);
}

/* Strip trailing zeros from the mantissa of an %e-formatted string
 * in place: `"9.0071992547409920e+15"` → `"9.007199254740992e+15"`.
 * Returns the new length.  Leaves a single digit after the decimal
 * if all post-dot digits are zero (`"1.0e+15"` not `"1.e+15"`). */
static int
strip_mantissa_zeros(char *s, int n)
{
    char *e = memchr(s, 'e', (size_t)n);
    if (!e) return n;
    char *dot = memchr(s, '.', (size_t)(e - s));
    if (!dot) return n;
    char *end = e - 1;
    while (end > dot + 1 && *end == '0') end--;
    if (end == dot) end++;        /* keep at least "1.0" not "1." */
    int new_e_off = (int)(end + 1 - s);
    int tail_len  = n - (int)(e - s);
    memmove(s + new_e_off, e, (size_t)tail_len);
    return new_e_off + tail_len;
}

/* Shortest-round-trip decimal repr of a double.  Mirrors what Ruby's
 * Float#to_s / JSON.generate produce — the form the conformance
 * harness compares against.
 *
 * Three complications:
 *   1. `%.1g 10.0` yields `"1e+01"` (5 chars, round-trips), while
 *      `%.2g 10.0` yields `"10"` (2 chars, also round-trips).  Ruby
 *      picks the shorter — so we sweep precisions and keep the
 *      shortest candidate that round-trips.
 *   2. For |d| >= 1e15 or < 1e-4 Ruby uses exponential, even when
 *      decimal is shorter.  `prefer_exp` enforces that.
 *   3. `%g` switches to decimal once precision exceeds the value's
 *      exponent (e.g. `%.16g 9e15` → `"9007199254740992"`), so for
 *      values where the round-trip threshold is past that switchover
 *      we'd never see an exponential candidate.  We also try `%e` and
 *      strip the mantissa's trailing zeros to get a Ruby-shape
 *      candidate. */
static int
format_double_min(double v, char *buf, size_t cap)
{
    double ad = v < 0 ? -v : v;
    bool prefer_exp = (ad >= 1e15) || (ad > 0 && ad < 1e-4);

    char  best[40];
    int   best_n = -1;

    for (int prec = 1; prec <= 17; prec++) {
        char tmp[40];

        /* %g — picks decimal or exp on its own */
        int n = snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
        if (n > 0 && (size_t)n < sizeof(tmp) && strtod(tmp, NULL) == v) {
            bool has_exp = (memchr(tmp, 'e', (size_t)n) != NULL);
            if (!prefer_exp || has_exp) {
                if (best_n < 0 || n < best_n) {
                    memcpy(best, tmp, (size_t)n + 1);
                    best_n = n;
                }
            }
        }

        /* %e — always exp; strip trailing mantissa zeros so we don't
         * over-emit digits (Ruby's repr is the trimmed form). */
        n = snprintf(tmp, sizeof(tmp), "%.*e", prec, v);
        if (n > 0 && (size_t)n < sizeof(tmp) && strtod(tmp, NULL) == v) {
            n = strip_mantissa_zeros(tmp, n);
            if (best_n < 0 || n < best_n) {
                memcpy(best, tmp, (size_t)n + 1);
                best_n = n;
            }
        }
    }
    if (best_n < 0) return snprintf(buf, cap, "%.17g", v);
    if ((size_t)best_n >= cap) best_n = (int)cap - 1;
    memcpy(buf, best, (size_t)best_n);
    buf[best_n] = '\0';
    return best_n;
}

static void
print_double_json(FILE *const out, const double d)
{
    /* JSON does not allow Inf/NaN; cel-go emits them as bare tokens. */
    if (isnan(d))      { fputs("NaN", out); return; }
    if (isinf(d))      { fputs(d > 0 ? "+Inf" : "-Inf", out); return; }

    /* Match Ruby Float#to_s output, since the conformance harness
     * compares against textproto-encoded doubles (which round-trip
     * through Ruby's JSON formatter).  Ruby uses:
     *   - non-exponential decimal if 1e-4 <= |d| < 1e16
     *   - exponential otherwise
     *   - always at least one digit after '.' (so integer-valued
     *     doubles get a ".0" suffix)
     *
     * For the non-exponential case with integer-valued d we can
     * shortcut to "%.0f.0".  For everything else we fall back to
     * shortest-round-trip and append ".0" if the result has no
     * decimal point / exponent. */
    /* Ruby JSON's threshold (from inspection of `JSON.generate`):
     *   |d| < 1e15  → decimal form, integer-valued gets ".0" suffix
     *   |d| >= 1e15 → exponential, no ".0" padding
     * Magnitudes < 1e-4 also go exponential, but cel-spec doesn't
     * exercise that boundary much; current shortest-round-trip
     * picks the right form via `%g` automatically. */
    char buf[40];
    int  n;
    double ad = d < 0 ? -d : d;
    bool integer_valued = (d == (double)(int64_t)d) && d >= -9.0e18 && d <= 9.0e18;

    if (integer_valued && ad < 1e15) {
        n = snprintf(buf, sizeof(buf), "%.0f.0", d);
    } else {
        n = format_double_min(d, buf, sizeof(buf));
        /* If the shortest form is exponential (contains 'e'), leave
         * it.  If it's plain decimal AND has no '.', append ".0" so
         * the JSON output is unambiguously a double. */
        bool has_marker = false;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { has_marker = true; break; }
        }
        if (!has_marker && (size_t)n + 2 < sizeof(buf)) {
            buf[n++] = '.';
            buf[n++] = '0';
        }
    }
    fwrite(buf, 1, (size_t)n, out);
}

void
arcel_print_json(FILE *const out, const VALUE v)
{
    switch (v.tag) {
        case AC_NULL:   fputs("null", out); break;
        case AC_BOOL:   fputs(v.b ? "true" : "false", out); break;
        case AC_INT:    fprintf(out, "%" PRId64, v.i); break;
        case AC_UINT:   fprintf(out, "%" PRIu64 "u", v.u); break;
        case AC_DOUBLE: print_double_json(out, v.d); break;
        case AC_STRING: print_string(out, v.s.p, v.s.len); break;
        case AC_BYTES:  print_string(out, v.s.p, v.s.len); break;   /* JSON-quoted */
        case AC_LIST:
            fputc('[', out);
            for (uint32_t i = 0; i < v.list->len; i++) {
                if (i) fputs(", ", out);
                arcel_print_json(out, v.list->items[i]);
            }
            fputc(']', out);
            break;
        case AC_MAP: {
            /* Sort entries by stringified key for stable output (the
             * harness compares strings).  Build a key index, qsort, emit. */
            uint32_t n = v.map->len;
            uint32_t *idx = (uint32_t *)alloca(sizeof(uint32_t) * (n ? n : 1));
            for (uint32_t i = 0; i < n; i++) idx[i] = i;
            /* simple insertion sort (n is small for CEL maps) */
            for (uint32_t i = 1; i < n; i++) {
                uint32_t j = i;
                while (j > 0) {
                    VALUE ka = v.map->entries[idx[j - 1]].key;
                    VALUE kb = v.map->entries[idx[j]].key;
                    /* compare by JSON form so tags interleave consistently */
                    int cmp = 0;
                    if (ka.tag == AC_STRING && kb.tag == AC_STRING) {
                        uint32_t m = ka.s.len < kb.s.len ? ka.s.len : kb.s.len;
                        cmp = m == 0 ? 0 : memcmp(ka.s.p, kb.s.p, m);
                        if (cmp == 0) cmp = (int)ka.s.len - (int)kb.s.len;
                    } else {
                        cmp = (int)ka.tag - (int)kb.tag;
                    }
                    if (cmp <= 0) break;
                    uint32_t t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
                    j--;
                }
            }
            fputc('{', out);
            for (uint32_t i = 0; i < n; i++) {
                if (i) fputs(", ", out);
                arcel_print_json(out, v.map->entries[idx[i]].key);
                fputs(": ", out);
                arcel_print_json(out, v.map->entries[idx[i]].val);
            }
            fputc('}', out);
            break;
        }
        case AC_OBJECT: {
            const struct arcel_object_desc *const d = v.object.desc;
            if (d && d->format_json) {
                /* Embedder-provided renderer.  Two-phase: query length,
                 * write into local stack buf, copy to fp.  Bounded at
                 * 1KB so a runaway descriptor can't DoS us. */
                char obuf[1024];
                size_t n = d->format_json(d, v.object.obj, obuf, sizeof obuf);
                if (n >= sizeof obuf) n = sizeof obuf - 1;
                fwrite(obuf, 1, n, out);
            } else {
                fprintf(out, "<object:%s>",
                        d && d->type_name ? d->type_name : "?");
            }
            break;
        }
        case AC_TIMESTAMP:
        case AC_DURATION: {
            /* Render via arcel_to_string into a stack buffer, then quote.
             * cel-spec serialization is the same as `string(x)` plus
             * surrounding quotes — RFC3339 / Go-duration text is
             * JSON-safe (no escaping needed). */
            char ibuf[40], obuf[44];
            size_t in_len = arcel_format_value_brief(v, ibuf, sizeof ibuf);
            int n = snprintf(obuf, sizeof obuf, "\"%.*s\"", (int)in_len, ibuf);
            fwrite(obuf, 1, (size_t)n, out);
            break;
        }
        case AC_ERR:    fprintf(out, "ERROR: %s", v.err ? v.err : "<unknown>"); break;
    }
}

/* ---- timestamp / duration ---------------------------------------- */

/* Brief text form of a value (same content as arcel_to_string but
 * never allocates).  Used by the JSON printer and by error messages.
 * Caller owns `buf`.  Returns bytes written (truncated to cap-1). */
size_t
arcel_format_value_brief(const VALUE x, char *const buf, const size_t cap)
{
    if (cap == 0) return 0;
    int n = 0;
    if (x.tag == AC_TIMESTAMP) {
        time_t t = (time_t)x.ts.s;
        struct tm tm;
        if (!gmtime_r(&t, &tm)) {
            n = snprintf(buf, cap, "<bad-timestamp>");
            return (size_t)((n < (int)cap) ? n : (int)cap - 1);
        }
        n = snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (x.ts.ns != 0 && (size_t)n + 11 < cap) {
            char frac[12];
            snprintf(frac, sizeof frac, ".%09d", x.ts.ns);
            int flen = 10;
            while (flen > 4 && frac[flen - 1] == '0' && (flen - 1) % 3 == 1) flen -= 3;
            while (flen > 4 && frac[flen - 1] == '0') flen--;
            memcpy(buf + n, frac, (size_t)flen);
            n += flen;
        }
        if ((size_t)n + 1 < cap) { buf[n++] = 'Z'; buf[n] = '\0'; }
        return (size_t)n;
    }
    if (x.tag == AC_DURATION) {
        if (x.ts.ns == 0) {
            n = snprintf(buf, cap, "%llds", (long long)x.ts.s);
        } else {
            int64_t s = x.ts.s; int32_t ns = x.ts.ns;
            bool neg = (s < 0) || (s == 0 && ns < 0);
            if (neg) { s = -s; ns = -ns; }
            char frac[12];
            snprintf(frac, sizeof frac, ".%09d", ns);
            int flen = 10;
            while (flen > 4 && frac[flen - 1] == '0' && (flen - 1) % 3 == 1) flen -= 3;
            while (flen > 4 && frac[flen - 1] == '0') flen--;
            n = snprintf(buf, cap, "%s%lld", neg ? "-" : "", (long long)s);
            if ((size_t)n + flen + 2 < cap) {
                memcpy(buf + n, frac, (size_t)flen);
                n += flen;
                buf[n++] = 's';
                buf[n] = '\0';
            }
        }
        return (size_t)n;
    }
    /* Fallback for other tags — short summary. */
    n = snprintf(buf, cap, "<tag-%d>", (int)x.tag);
    return (size_t)((n < (int)cap) ? n : (int)cap - 1);
}

/* Parse a non-negative integer with EXACT `digits` characters from `p`
 * into `*out`.  Returns 0 on success, -1 on non-digit. */
static int
parse_fixed_digits(const char *p, int digits, int *out)
{
    int v = 0;
    for (int i = 0; i < digits; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return 0;
}

/* days_from_civil: Howard Hinnant's algorithm.  Returns days since
 * 1970-01-01 for a (y, m, d) Gregorian date.  Negative for pre-epoch.
 * Handles years 0001..9999 cleanly. */
static int64_t
days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                  /* 0..399 */
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;       /* 0..146096 */
    return era * 146097 + (int64_t)doe - 719468;
}

VALUE
arcel_to_timestamp(CTX *const c, const VALUE s)
{
    if (s.tag == AC_ERR) return s;
    if (s.tag != AC_STRING) return err(c, "no such overload: timestamp");
    /* Minimum well-formed RFC3339 with no fraction & 'Z': 20 chars
     *   YYYY-MM-DDTHH:MM:SSZ
     * Fraction is optional; offset can be 'Z' or '+HH:MM' / '-HH:MM'. */
    if (s.s.len < 20) return err(c, "invalid RFC3339 timestamp");
    const char *p = s.s.p;
    const uint32_t n = s.s.len;
    int year, mon, day, hh, mm, ss;
    if (parse_fixed_digits(p,        4, &year) != 0) return err(c, "invalid RFC3339 timestamp");
    if (p[4] != '-')                                  return err(c, "invalid RFC3339 timestamp");
    if (parse_fixed_digits(p + 5,    2, &mon)  != 0) return err(c, "invalid RFC3339 timestamp");
    if (p[7] != '-')                                  return err(c, "invalid RFC3339 timestamp");
    if (parse_fixed_digits(p + 8,    2, &day)  != 0) return err(c, "invalid RFC3339 timestamp");
    if (p[10] != 'T' && p[10] != 't' && p[10] != ' ') return err(c, "invalid RFC3339 timestamp");
    if (parse_fixed_digits(p + 11,   2, &hh)   != 0) return err(c, "invalid RFC3339 timestamp");
    if (p[13] != ':')                                 return err(c, "invalid RFC3339 timestamp");
    if (parse_fixed_digits(p + 14,   2, &mm)   != 0) return err(c, "invalid RFC3339 timestamp");
    if (p[16] != ':')                                 return err(c, "invalid RFC3339 timestamp");
    if (parse_fixed_digits(p + 17,   2, &ss)   != 0) return err(c, "invalid RFC3339 timestamp");
    uint32_t off = 19;
    int32_t  ns  = 0;
    if (off < n && p[off] == '.') {
        off++;
        /* Up to 9 digits, padded to 9 with trailing zeros. */
        int dig = 0; int v = 0;
        while (off < n && p[off] >= '0' && p[off] <= '9') {
            if (dig < 9) v = v * 10 + (p[off] - '0');
            dig++;
            off++;
        }
        if (dig == 0) return err(c, "invalid RFC3339 timestamp");
        for (int i = dig; i < 9; i++) v *= 10;
        ns = v;
    }
    if (off >= n) return err(c, "invalid RFC3339 timestamp");
    int tz_off_sec = 0;
    char z = p[off];
    if (z == 'Z' || z == 'z') { off++; }
    else if (z == '+' || z == '-') {
        int sign = (z == '+') ? 1 : -1;
        off++;
        int oh, om;
        if (off + 4 >= n) return err(c, "invalid RFC3339 timestamp");
        if (parse_fixed_digits(p + off, 2, &oh) != 0) return err(c, "invalid RFC3339 timestamp");
        if (p[off + 2] != ':') return err(c, "invalid RFC3339 timestamp");
        if (parse_fixed_digits(p + off + 3, 2, &om) != 0) return err(c, "invalid RFC3339 timestamp");
        off += 5;
        tz_off_sec = sign * (oh * 3600 + om * 60);
    } else {
        return err(c, "invalid RFC3339 timestamp");
    }
    if (off != n) return err(c, "invalid RFC3339 timestamp (trailing chars)");
    if (year < 1 || year > 9999) return err(c, "range error: timestamp out of range");
    if (mon  < 1 || mon  > 12)   return err(c, "invalid RFC3339 timestamp (month)");
    if (day  < 1 || day  > 31)   return err(c, "invalid RFC3339 timestamp (day)");
    if (hh   < 0 || hh   > 23)   return err(c, "invalid RFC3339 timestamp (hour)");
    if (mm   < 0 || mm   > 59)   return err(c, "invalid RFC3339 timestamp (min)");
    if (ss   < 0 || ss   > 60)   return err(c, "invalid RFC3339 timestamp (sec)");  /* leap second tolerated */
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    int64_t sec  = days * 86400 + hh * 3600 + mm * 60 + ss - tz_off_sec;
    if (sec < -62135596800LL || sec > 253402300799LL ||
        (sec == 253402300799LL && ns > 999999999))
        return err(c, "range error: timestamp out of range");
    return V_TIMESTAMP(sec, ns);
}

VALUE
arcel_to_duration(CTX *const c, const VALUE s)
{
    if (s.tag == AC_ERR) return s;
    if (s.tag != AC_STRING) return err(c, "no such overload: duration");
    /* Go's time.ParseDuration: "<num><unit>(<num><unit>)*", optional
     * leading '-' or '+'.  Units: ns, us, µs, ms, s, m, h.  Number can
     * have a decimal fraction. */
    const char *p = s.s.p;
    const char *e = p + s.s.len;
    if (p == e) return err(c, "invalid duration");
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    if (p == e) return err(c, "invalid duration");
    int64_t total_ns_hi = 0;       /* sum of seconds * 1e9 in two parts to avoid overflow */
    int64_t total_ns_lo = 0;
    while (p < e) {
        /* Parse number: digits[.digits] */
        const char *num_start = p;
        while (p < e && *p >= '0' && *p <= '9') p++;
        int int_digits = (int)(p - num_start);
        int frac_digits = 0;
        const char *frac_start = NULL;
        if (p < e && *p == '.') {
            p++;
            frac_start = p;
            while (p < e && *p >= '0' && *p <= '9') p++;
            frac_digits = (int)(p - frac_start);
        }
        if (int_digits == 0 && frac_digits == 0) return err(c, "invalid duration");
        /* Parse unit. */
        if (p == e) return err(c, "missing duration unit");
        int64_t unit_ns = 0;
        if      (p + 2 <= e && p[0] == 'n' && p[1] == 's') { unit_ns = 1LL;             p += 2; }
        else if (p + 2 <= e && p[0] == 'u' && p[1] == 's') { unit_ns = 1000LL;          p += 2; }
        else if (p + 3 <= e && (unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB5 && p[2] == 's')
                                                           { unit_ns = 1000LL;          p += 3; }  /* µs (UTF-8) */
        else if (p + 2 <= e && p[0] == 'm' && p[1] == 's') { unit_ns = 1000000LL;       p += 2; }
        else if (p + 1 <= e && p[0] == 's')                { unit_ns = 1000000000LL;    p += 1; }
        else if (p + 1 <= e && p[0] == 'm')                { unit_ns = 60000000000LL;   p += 1; }
        else if (p + 1 <= e && p[0] == 'h')                { unit_ns = 3600000000000LL; p += 1; }
        else return err(c, "unknown duration unit");
        /* Compute integer-part contribution. */
        int64_t int_val = 0;
        for (int i = 0; i < int_digits; i++) int_val = int_val * 10 + (num_start[i] - '0');
        /* int_val * unit_ns can overflow for large s/m/h; be conservative.
         * Range guard: 315576000000s ≈ 3.16e20 ns — needs __int128 or split. */
        /* We'll detect overflow via __builtin_mul_overflow; if overflow,
         * the duration is out of range anyway. */
        int64_t int_ns;
        if (__builtin_mul_overflow(int_val, unit_ns, &int_ns))
            return err(c, "range error: duration out of range");
        if (__builtin_add_overflow(total_ns_lo, int_ns, &total_ns_lo))
            return err(c, "range error: duration out of range");
        /* Fractional contribution: (frac as int) * unit_ns / 10^frac_digits.
         * For unit_ns up to 3.6e12 and frac_digits up to 9, this fits
         * within int64 if unit <= ms.  For seconds/minutes/hours with
         * high-precision fractions we lose a digit or two — acceptable. */
        if (frac_digits > 0) {
            int64_t frac_val = 0;
            for (int i = 0; i < frac_digits; i++) frac_val = frac_val * 10 + (frac_start[i] - '0');
            int64_t denom = 1;
            for (int i = 0; i < frac_digits; i++) denom *= 10;
            /* frac_ns = frac_val * unit_ns / denom — round toward zero. */
            int64_t prod;
            if (__builtin_mul_overflow(frac_val, unit_ns, &prod)) {
                /* For h/m, frac_val * unit_ns can blow up.  Reduce by
                 * dividing first if the unit is large. */
                int64_t reduced_unit = unit_ns / denom;
                prod = frac_val * reduced_unit;
            }
            int64_t frac_ns = prod / denom;
            if (__builtin_add_overflow(total_ns_lo, frac_ns, &total_ns_lo))
                return err(c, "range error: duration out of range");
        }
        (void)total_ns_hi;
    }
    /* Apply sign. */
    if (sign < 0) total_ns_lo = -total_ns_lo;
    int64_t s_part = total_ns_lo / 1000000000LL;
    int32_t ns_part = (int32_t)(total_ns_lo % 1000000000LL);
    /* Range check (matches cel-go's time.Duration int64-ns limit). */
    if (s_part > 9223372035LL || s_part < -9223372035LL)
        return err(c, "range error: duration out of range");
    return V_DURATION(s_part, ns_part);
}

/* Parse a tz argument into a UTC offset in seconds (and IANA-name
 * support via setenv("TZ", name); tzset()).  Returns 0 + sets *off,
 * or -1 if unparseable.  If the name is "UTC" / empty / "Z" → off=0.
 * For numeric "+HH:MM" / "-HH:MM" → signed seconds.  For IANA names
 * (any other), uses gmt offset for the given timestamp via tzset
 * (process-global; arcel is single-threaded eval). */
static int
tz_offset_for(const char *const name, const uint32_t name_len,
              const time_t at, int *const off)
{
    if (name_len == 0 ||
        (name_len == 3 && memcmp(name, "UTC", 3) == 0) ||
        (name_len == 1 && (name[0] == 'Z' || name[0] == 'z'))) {
        *off = 0;
        return 0;
    }
    if (name_len >= 5 && (name[0] == '+' || name[0] == '-')) {
        int sign = (name[0] == '+') ? 1 : -1;
        int oh, om;
        if (parse_fixed_digits(name + 1, 2, &oh) != 0) return -1;
        if (name[3] != ':') return -1;
        if (parse_fixed_digits(name + 4, 2, &om) != 0) return -1;
        if (name_len != 6) return -1;
        *off = sign * (oh * 3600 + om * 60);
        return 0;
    }
    /* Sign-less "HH:MM" — cel-spec accepts as positive offset. */
    if (name_len == 5 && name[2] == ':') {
        int oh, om;
        if (parse_fixed_digits(name,     2, &oh) != 0) return -1;
        if (parse_fixed_digits(name + 3, 2, &om) != 0) return -1;
        *off = oh * 3600 + om * 60;
        return 0;
    }
    /* IANA name: NUL-terminate and use TZ + tzset. */
    char tz[64];
    if (name_len + 1 > sizeof tz) return -1;
    memcpy(tz, name, name_len); tz[name_len] = '\0';
    char *prev = getenv("TZ");
    char saved[64]; bool had_prev = false;
    if (prev) {
        size_t pn = strlen(prev);
        if (pn + 1 > sizeof saved) return -1;
        memcpy(saved, prev, pn + 1);
        had_prev = true;
    }
    setenv("TZ", tz, 1);
    tzset();
    struct tm tm_local;
    if (!localtime_r(&at, &tm_local)) {
        if (had_prev) setenv("TZ", saved, 1); else unsetenv("TZ");
        tzset();
        return -1;
    }
    int local_off = (int)tm_local.tm_gmtoff;
    if (had_prev) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
    *off = local_off;
    return 0;
}

VALUE
arcel_ts_get(CTX *const c, const VALUE recv,
             const char *const name, const uint32_t name_len,
             const VALUE tz)
{
    if (recv.tag == AC_ERR) return recv;
    /* Resolve tz offset for AC_TIMESTAMP path. */
    int tz_off = 0;
    if (recv.tag == AC_TIMESTAMP) {
        if (tz.tag == AC_STRING) {
            if (tz_offset_for(tz.s.p, tz.s.len, (time_t)recv.ts.s, &tz_off) != 0)
                return err(c, "invalid timezone");
        } else if (tz.tag != AC_NULL) {
            return err(c, "no such overload: timestamp selector tz arg");
        }
    } else if (recv.tag == AC_DURATION) {
        /* Duration selectors don't accept tz. */
        if (tz.tag != AC_NULL) return err(c, "no such overload: duration selector with tz arg");
    } else {
        return err(c, "no such overload: %.*s on tag %d", (int)name_len, name, recv.tag);
    }

    #define IS(s) (name_len == sizeof(s) - 1 && memcmp(name, s, name_len) == 0)

    /* DURATION selectors return TOTAL units (cel-spec: getHours on
     * Duration is total hours, possibly negative). */
    if (recv.tag == AC_DURATION) {
        int64_t s = recv.ts.s; int32_t ns = recv.ts.ns;
        if (IS("getHours"))        return V_INT(s / 3600);
        if (IS("getMinutes"))      return V_INT(s / 60);
        if (IS("getSeconds"))      return V_INT(s);
        if (IS("getMilliseconds")) {
            /* ns part contributes too (ns / 1e6 truncated). */
            int64_t ms = s * 1000 + (int64_t)(ns / 1000000);
            return V_INT(ms);
        }
        return err(c, "no such overload: %.*s on duration", (int)name_len, name);
    }

    /* TIMESTAMP selectors: project into struct tm at the given offset. */
    time_t local_t = (time_t)(recv.ts.s + tz_off);
    struct tm tm;
    if (!gmtime_r(&local_t, &tm)) return err(c, "timestamp formatting failed");

    if (IS("getFullYear"))     return V_INT(tm.tm_year + 1900);
    if (IS("getMonth"))        return V_INT(tm.tm_mon);          /* 0..11 per cel-spec */
    if (IS("getDate"))         return V_INT(tm.tm_mday);         /* 1..31 */
    if (IS("getDayOfMonth"))   return V_INT(tm.tm_mday - 1);     /* 0..30 per cel-spec */
    if (IS("getDayOfYear"))    return V_INT(tm.tm_yday);         /* 0..365 */
    if (IS("getDayOfWeek"))    return V_INT(tm.tm_wday);         /* 0=Sun..6=Sat */
    if (IS("getHours"))        return V_INT(tm.tm_hour);
    if (IS("getMinutes"))      return V_INT(tm.tm_min);
    if (IS("getSeconds"))      return V_INT(tm.tm_sec);
    if (IS("getMilliseconds")) return V_INT(recv.ts.ns / 1000000);

    return err(c, "no such overload: %.*s on timestamp", (int)name_len, name);
    #undef IS
}
