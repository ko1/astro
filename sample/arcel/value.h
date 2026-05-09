#ifndef ARCEL_VALUE_H
#define ARCEL_VALUE_H

#include "context.h"

/* ---- arena -------------------------------------------------------- */

void   arcel_arena_init(arcel_arena *a);
void   arcel_arena_reset(arcel_arena *a);
void   arcel_arena_free(arcel_arena *a);

/* Slow path — overflow into a fresh chunk.  Out-of-line so the
 * inlined bump fast path stays small. */
void  *arcel_arena_alloc_grow(arcel_arena *a, size_t bytes, size_t align);

/* Bump-allocator fast path: 1 add, 1 cmp, 1 store on the success
 * path.  All eval-time allocations (concat strings, list/map results,
 * error messages) hit this. */
static inline void *
arcel_arena_alloc(arcel_arena *const a, const size_t bytes, const size_t align)
{
    arcel_arena_chunk *const c = a->current;
    const size_t off = (c->used + (align - 1)) & ~(align - 1);
    if (__builtin_expect(off + bytes <= c->cap, 1)) {
        c->used = off + bytes;
        return c->buf + off;
    }
    return arcel_arena_alloc_grow(a, bytes, align);
}

/* Convenience: copy a byte buffer into the arena and return a stable
 * pointer (used by string concat, type conv to string, etc.). */
const char *arcel_arena_strdup(arcel_arena *a, const char *p, uint32_t len);

/* Format a printf-style message into the arena and return a stable
 * pointer.  Used to construct error messages (`V_ERR("...")`) whose
 * lifetime matches the current eval. */
const char *arcel_arena_msg(arcel_arena *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ---- list / map builders ----------------------------------------- */

arcel_list *arcel_list_new(arcel_arena *a, uint32_t len);
arcel_map  *arcel_map_new (arcel_arena *a, uint32_t len);

/* ---- value comparison & arithmetic ------------------------------- */

/* CEL strict equality: same logical type & equal payload.  null == null
 * is true; mixed numerics (int vs uint vs double) compare numerically
 * if values fit; everything else of different tag is false.
 *
 * The same-tag fast path is inlined here so the SD specializer can
 * fold `memcmp(k.s.p, "<literal>", N)` into single-instruction cmps
 * once both sides are known.  The cross-tag numeric path falls
 * through to a non-inline helper to keep SD code size bounded. */
bool arcel_eq_slow(VALUE a, VALUE b);
static inline bool
arcel_eq(VALUE a, VALUE b)
{
    if (a.tag == b.tag) {
        switch (a.tag) {
            case AC_NULL:   return true;
            case AC_BOOL:   return a.b == b.b;
            case AC_INT:    return a.i == b.i;
            case AC_UINT:   return a.u == b.u;
            case AC_DOUBLE: return a.d == b.d;
            case AC_STRING:
            case AC_BYTES:
                return a.s.len == b.s.len &&
                       (a.s.len == 0 || memcmp(a.s.p, b.s.p, a.s.len) == 0);
            default:        return arcel_eq_slow(a, b);
        }
    }
    return arcel_eq_slow(a, b);
}

/* Total ordering result: -1, 0, 1, or `INT_MIN` for "incomparable"
 * (different non-numeric types).  CEL only defines ordering on same
 * type plus the int/uint/double trio. */
int  arcel_cmp(VALUE a, VALUE b);

VALUE arcel_add(CTX *c, VALUE a, VALUE b);
VALUE arcel_sub(CTX *c, VALUE a, VALUE b);
VALUE arcel_mul(CTX *c, VALUE a, VALUE b);
VALUE arcel_div(CTX *c, VALUE a, VALUE b);
VALUE arcel_mod(CTX *c, VALUE a, VALUE b);
VALUE arcel_neg(CTX *c, VALUE a);

/* Membership: `x in xs`.  Inlined so that `x in [a, b, c]` (cel-spec
 * idiom for set membership) folds the per-element arcel_eq through —
 * with all the inner comparisons specialized via the inline arcel_eq
 * fast path. */
VALUE arcel_in_err(CTX *c);

static inline VALUE
arcel_in(CTX *const c, const VALUE x, const VALUE xs)
{
    if (x.tag  == AC_ERR) return x;
    if (xs.tag == AC_ERR) return xs;
    if (xs.tag == AC_LIST) {
        const arcel_list *const l = xs.list;
        const uint32_t n = l->len;
        for (uint32_t i = 0; i < n; i++) {
            if (arcel_eq(x, l->items[i])) return V_BOOL(true);
        }
        return V_BOOL(false);
    }
    if (xs.tag == AC_MAP) {
        const arcel_map *const m = xs.map;
        const uint32_t n = m->len;
        for (uint32_t i = 0; i < n; i++) {
            if (arcel_eq(x, m->entries[i].key)) return V_BOOL(true);
        }
        return V_BOOL(false);
    }
    return arcel_in_err(c);
}

/* size(x): string codepoints, bytes length, list length, map size.
 * Inlined for the common scalar paths; UTF-8 codepoint count for
 * strings is the only sub-loop and is short. */
VALUE arcel_size_err(CTX *c);
uint32_t arcel_utf8_count(const char *p, uint32_t len);

static inline VALUE
arcel_size(CTX *const c, const VALUE x)
{
    switch (x.tag) {
        case AC_STRING: return V_INT((int64_t)arcel_utf8_count(x.s.p, x.s.len));
        case AC_BYTES:  return V_INT((int64_t)x.s.len);
        case AC_LIST:   return V_INT((int64_t)x.list->len);
        case AC_MAP:    return V_INT((int64_t)x.map->len);
        case AC_ERR:    return x;
        default:        return arcel_size_err(c);
    }
}

/* String operations.  Args already evaluated.  startsWith / endsWith
 * inline so memcmp can constant-fold against AST-literal prefixes /
 * suffixes; contains uses memmem which is already a hardware-tuned
 * intrinsic. */
VALUE arcel_str_op_err(CTX *c, const char *which);

static inline VALUE
arcel_starts_with(CTX *const c, const VALUE recv, const VALUE prefix)
{
    if (recv.tag   == AC_ERR) return recv;
    if (prefix.tag == AC_ERR) return prefix;
    if (recv.tag != AC_STRING || prefix.tag != AC_STRING) return arcel_str_op_err(c, "startsWith");
    if (prefix.s.len > recv.s.len) return V_BOOL(false);
    if (prefix.s.len == 0) return V_BOOL(true);
    return V_BOOL(memcmp(recv.s.p, prefix.s.p, prefix.s.len) == 0);
}

static inline VALUE
arcel_ends_with(CTX *const c, const VALUE recv, const VALUE suffix)
{
    if (recv.tag   == AC_ERR) return recv;
    if (suffix.tag == AC_ERR) return suffix;
    if (recv.tag != AC_STRING || suffix.tag != AC_STRING) return arcel_str_op_err(c, "endsWith");
    if (suffix.s.len > recv.s.len) return V_BOOL(false);
    if (suffix.s.len == 0) return V_BOOL(true);
    return V_BOOL(memcmp(recv.s.p + recv.s.len - suffix.s.len, suffix.s.p, suffix.s.len) == 0);
}

static inline VALUE
arcel_contains(CTX *const c, const VALUE recv, const VALUE needle)
{
    if (recv.tag   == AC_ERR) return recv;
    if (needle.tag == AC_ERR) return needle;
    if (recv.tag != AC_STRING || needle.tag != AC_STRING) return arcel_str_op_err(c, "contains");
    if (needle.s.len == 0) return V_BOOL(true);
    if (needle.s.len > recv.s.len) return V_BOOL(false);
    return V_BOOL(memmem(recv.s.p, recv.s.len, needle.s.p, needle.s.len) != NULL);
}

VALUE arcel_matches(CTX *c, VALUE recv, VALUE pattern);

/* Field / index access.  Returns AC_ERR on missing key / out-of-bounds /
 * type mismatch.  Inlined: the SD specializer embeds `name` as a C
 * literal and the gcc optimizer folds the inner memcmp into native
 * 1/2/4/8-byte cmps based on the literal's length. */
VALUE arcel_field_err_no_key  (CTX *c, const char *name, uint32_t name_len);
VALUE arcel_field_err_overload(CTX *c, int tag);
/* Slow path for AC_OBJECT — calls into the descriptor's `field`
 * callback.  Out-of-line so the inlined arcel_field stays small even
 * when the embedder's adapter is heavy. */
VALUE arcel_field_object(CTX *c, VALUE recv, const char *name, uint32_t name_len);

static inline VALUE
arcel_field(CTX *const c, const VALUE recv, const char *const name, const uint32_t name_len)
{
    if (recv.tag == AC_ERR) return recv;
    if (recv.tag == AC_MAP) {
        const arcel_map *const m = recv.map;
        const uint32_t n = m->len;
        for (uint32_t i = 0; i < n; i++) {
            const VALUE k = m->entries[i].key;
            if (k.tag == AC_STRING && k.s.len == name_len &&
                (name_len == 0 || memcmp(k.s.p, name, name_len) == 0)) {
                return m->entries[i].val;
            }
        }
        return arcel_field_err_no_key(c, name, name_len);
    }
    if (recv.tag == AC_OBJECT) {
        return arcel_field_object(c, recv, name, name_len);
    }
    return arcel_field_err_overload(c, recv.tag);
}

/* Index access — inlined.  When the key is an AST literal (`x["team"]`)
 * the SD specializer can fold the per-entry compare against that
 * literal value just like field access. */
VALUE arcel_index_err     (CTX *c);
VALUE arcel_index_oob     (CTX *c, int64_t i);
VALUE arcel_index_no_key  (CTX *c);

static inline VALUE
arcel_index(CTX *const c, const VALUE recv, const VALUE key)
{
    if (recv.tag == AC_ERR) return recv;
    if (key.tag  == AC_ERR) return key;
    if (recv.tag == AC_LIST) {
        int64_t i;
        if      (key.tag == AC_INT)    i = key.i;
        else if (key.tag == AC_UINT)   i = (int64_t)key.u;
        else if (key.tag == AC_DOUBLE) {
            if (key.d != (double)(int64_t)key.d) return arcel_index_err(c);
            i = (int64_t)key.d;
        } else {
            return arcel_index_err(c);
        }
        if (i < 0 || (uint64_t)i >= recv.list->len) return arcel_index_oob(c, i);
        return recv.list->items[(uint32_t)i];
    }
    if (recv.tag == AC_MAP) {
        const arcel_map *const m = recv.map;
        const uint32_t n = m->len;
        for (uint32_t i = 0; i < n; i++) {
            if (arcel_eq(m->entries[i].key, key)) return m->entries[i].val;
        }
        return arcel_index_no_key(c);
    }
    return arcel_index_err(c);
}

/* CEL `has(...)` — try field access, return true if present.  The
 * receiver's tag drives behaviour: map → key existence, message →
 * field set (only relevant for proto, which we don't support). */
VALUE arcel_has_field(CTX *c, VALUE recv, const char *name, uint32_t name_len);

/* Type conversions (cel-spec strict rules). */
VALUE arcel_to_int   (CTX *c, VALUE x);
VALUE arcel_to_uint  (CTX *c, VALUE x);
VALUE arcel_to_double(CTX *c, VALUE x);
VALUE arcel_to_bool  (CTX *c, VALUE x);
VALUE arcel_to_string(CTX *c, VALUE x);
VALUE arcel_to_bytes (CTX *c, VALUE x);

/* type(x): the CEL type of x, returned as a string token like "int",
 * "list(string)" — we just return the simple form for now.  Named
 * `_cel_type_of` because the embedding API in arcel.h uses
 * `arcel_type_of` for the C-side enum tag accessor. */
VALUE arcel_cel_type_of(CTX *c, VALUE x);

/* ---- formatting --------------------------------------------------- */

/* Print value as a JSON literal that matches celgo_ref's output, so
 * the test harness can byte-compare.  For unrepresentable cases
 * (timestamp, duration, bytes) we use the closest JSON form. */
void arcel_print_json(FILE *out, VALUE v);

/* (was: arcel_format_json that appended to an arena buffer; never
 * actually used internally.  The public arcel_format_json in arcel.h
 * shadows this name — keep the internal one out of the header to
 * avoid collision.) */

/* ---- bindings ---------------------------------------------------- */

/* Inlined for the same reason as arcel_field — `name` is an AST
 * literal that the SD bakes in as a C string constant. */
VALUE arcel_lookup_ident_slow(CTX *c, const char *name, uint32_t name_len);

static inline VALUE
arcel_lookup_ident(CTX *const c, const char *const name, const uint32_t name_len)
{
    /* macro stack first (LIFO so inner shadows outer) */
    for (int i = c->bind_top - 1; i >= 0; i--) {
        if (c->bind_stack[i].name_len == name_len &&
            (name_len == 0 || memcmp(c->bind_stack[i].name, name, name_len) == 0)) {
            return c->bind_stack[i].value;
        }
    }
    /* global bindings */
    if (c->bindings) {
        const arcel_map *const m = c->bindings;
        const uint32_t n = m->len;
        for (uint32_t i = 0; i < n; i++) {
            const VALUE k = m->entries[i].key;
            if (k.tag == AC_STRING && k.s.len == name_len &&
                (name_len == 0 || memcmp(k.s.p, name, name_len) == 0)) {
                return m->entries[i].val;
            }
        }
    }
    return arcel_lookup_ident_slow(c, name, name_len);
}

/* push/pop are tiny (a struct write + increment) but in the macro
 * body hot path: every list iteration of `xs.all(x, ...)` does one
 * pair.  Inlined so the SD body sees the direct stack writes. */
void arcel_bind_overflow_abort(void);

static inline void
arcel_push_bind(CTX *const c, const char *const name, const uint32_t name_len, const VALUE v)
{
    if (__builtin_expect(c->bind_top >= ARCEL_BIND_STACK_MAX, 0)) {
        arcel_bind_overflow_abort();
    }
    arcel_bind *const b = &c->bind_stack[c->bind_top++];
    b->name     = name;
    b->name_len = name_len;
    b->value    = v;
}

static inline void
arcel_pop_bind(CTX *const c)
{
    if (c->bind_top > 0) c->bind_top--;
}

#endif
