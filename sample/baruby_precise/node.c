#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "astro_jit.h"
#include "gc.h"

// --- User-provided: allocation ---

extern size_t node_cnt;

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    // Zero-init the entire NODE.  Generated ALLOC_<name> sets the operand
    // fields and most flags explicitly, but `head.flags.has_hash_opt` and
    // `head.hash_opt` are not in that list, so without zeroing them here
    // HOPT() can spuriously hit the cache with a garbage uninitialized
    // value.  abruby does the same.
    memset(n, 0, size);
    node_cnt++;
    return n;
}

// --- Optional dispatch tracing ---
//
// Older astrogen-generated node_dispatch.c calls dispatch_info();
// modern astrogen no longer emits it.  Provide a no-op stub so locally-
// regenerated and historic generated files both link.

__attribute__((unused)) static void
dispatch_info(CTX *c, NODE *n, bool end)
{
#if DEBUG_EVAL
    if (end) {
        c->rec_cnt--;
    }
    else {
        for (int i=0; i<c->rec_cnt; i++) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, "%s\n", n->head.dispatcher_name);
        c->rec_cnt++;
    }
#else
    (void)c; (void)n; (void)end;
#endif
}

// --- ASTro node infrastructure (hashes, HASH, DUMP, alloc_dispatcher_name) ---

#include "astro_node.c"

// baruby-specific hash: identifies a builtin function by its source identity
// (C symbol name) so two distinct cfunc entries don't collide in the code
// store.  Used by node_call_builtin's auto-generated hash function.

static node_hash_t
hash_builtin_func(builtin_func_t *bf)
{
    if (bf->have_src) {
        node_hash_t h = hash_cstr(bf->name);
        return hash_merge(h, hash_cstr(bf->func_name));
    }
    else {
        return 0;
    }
}

// --- Code store (SPECIALIZE, astro_cs_*) ---

#include "astro_code_store.c"

// --- baruby-specific helpers ---

// Invalidate cached hashes (HORG via head.hash_value, HOPT via
// head.hash_opt) for a node and all its ancestors.  Used after an
// in-place AST mutation that changes hash inputs — most commonly
// `callsite_resolve` patching `sp_body` (HOPT-only) or
// `baruby_update_sp_bodies_from_cc` doing the same before the PGSD
// bake.  Both caches must be invalidated because HOPT depends on
// HORG of subtree members and HORG depends on HOPT-relevant fields
// being unchanged — playing safe by clearing both is cheaper than
// reasoning per-mutation.
void
clear_hash(NODE *n)
{
    while (n) {
        n->head.flags.has_hash_value = false;
        n->head.flags.has_hash_opt   = false;
        n = n->head.parent;
    }
}

// --- User-provided: OPTIMIZE ---

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.plain) {
        return n;
    }
    else if (OPTION.jit) {
        astro_jit_submit_query(n);
        return n;
    }
    else {
        // AOT compiled — try to bind SD_<hash> from code_store/all.so
        (void)astro_cs_load(n, NULL);
    }

    return n;
}

// Render the specialized C source for a single entry node into a malloc'd
// string buffer.  Used by the JIT path (sample/baruby/astro_jit.c) to send
// SPECIALIZE output to L1 without touching the on-disk code store.

char *
SPECIALIZED_SRC(NODE *n)
{
    if (n == NULL) return NULL;

    astro_spec_dedup_clear();

    char *buf = NULL;
    size_t len = 0;

    FILE *fp = open_memstream(&buf, &len);
    if (fp == NULL) {
        return NULL;
    }

    (*n->head.kind->specializer)(fp, n, true);

    if (fclose(fp) != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

// --- Generated code ---

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_hopt.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_walk.c"
#include "node_alloc.c"

// --- Hopt (profile-aware) hash dispatch + cache wrapper.
// HORG (= structural HASH) is provided by node_hash.c via the framework;
// here we expose the per-NodeKind hopt_func as the user-facing HOPT() and
// the cached hash_node_opt() that pg_call's HOPT_ recurses through.

node_hash_t
HOPT(NODE *n)
{
    if (n == NULL) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    if (n->head.kind->hopt_func) {
        // Mark first so cyclic recursion (fib_body → pg_call → sp_body=fib_body)
        // sees has_hash_opt=true and bottoms out at the partially-computed
        // value instead of recursing forever.
        //
        // Pre-seed the cached value with a kind-specific marker so two
        // mutually-recursive bodies of different shapes don't collapse
        // to the same HOPT (= 0).  fib_body's pg_call's sp_body is itself
        // a node_pg_call, so its cycle-break value depends on
        // dispatcher_name (≈ HORG-derived).  Two distinct recursive
        // functions thus hash differently.
        n->head.flags.has_hash_opt = true;
        n->head.hash_opt = hash_cstr(n->head.kind->default_dispatcher_name);
        return n->head.hash_opt = (*n->head.kind->hopt_func)(n);
    }
    return 0;
}

node_hash_t
hash_node_opt(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    return HOPT(n);
}

// --- INIT ---

void
INIT(void)
{
    /* Iter 36 AOT fix: pass BARUBY_GC as the version so the code_store
     * cache invalidates when the binary is rebuilt under a different GC
     * backend.  Without this, a code_store baked under (e.g.) mark would
     * be reused after rebuilding under copy_gen, but the GC WB inlining
     * baked into the SDs would mismatch the new gen runtime → silent
     * misbehavior or wrong perf. */
    astro_cs_init("code_store", ".", (uint64_t)BARUBY_GC);
}

// --- Heap allocators (precise GC) ---
//
// All sample helpers take a trailing `sp` argument: the caller's current
// scratch top.  Pattern: set `c->sp = sp` (or `sp + N`) before each
// alloc-can-GC call so the framework's alloc API (which reads c->sp as
// the scan upper bound) sees the right range.  No `sp_top` param on
// the alloc API anymore — c->sp is the single source of truth.
//
// Two-alloc allocators use sp[0] as a temporary root for the header
// so the second alloc's GC doesn't lose it.  The caller must supply an
// sp with at least 1 slot of headroom (= sp[0] is writable).

VALUE
baruby_ary_new(CTX *c, uint32_t capa)
{
    // Caller has set c->sp to the scratch top.  Snapshot it for stable
    // sp[N] indexing through the rest of this function.
    VALUE *sp = c->sp;
    // First alloc scans up to sp (sp[0] uninit, must not be in range).
    sp[0] = (VALUE)aro_gc_alloc(c, OBJ_ARRAY, sizeof(BaArray));
    BaArray *a = (BaArray *)sp[0];
    a->hdr.type = OBJ_ARRAY;
    a->hdr.flags = 0;
    a->len = 0;
    a->capa = capa;
    if (capa) {
        // Second alloc: extend scan to include sp[0] (= the new BaArray).
        c->sp = sp + 1;
        VALUE *items = (VALUE *)aro_gc_alloc(c, KIND_PAYLOAD_VAL, sizeof(VALUE) * capa);
        a = (BaArray *)sp[0];   // reload after potential GC
        aro_gc_wb(c, a, (VALUE *)&a->items, (VALUE)items);
    } else {
        a->items = NULL;
    }
    return sp[0];
}

VALUE
baruby_ary_new_from(CTX *c, const VALUE *items, uint32_t n)
{
    VALUE v = baruby_ary_new(c, n ? n : 1);
    BaArray *a = VAL2ARY(v);
    aro_gc_wb_bulk(c, a->items, a->items, items, n);
    a->len = n;
    return v;
}

// baruby_ary_push: av_ref / x_ref point at caller sp slots so the post-
// move addresses survive the grow-path GC.  Passing by value would freeze
// stale pointers.
void
baruby_ary_push(CTX *c, VALUE *av_ref, VALUE *x_ref)
{
    BaArray *a = VAL2ARY(*av_ref);
    if (a->len < a->capa) {
        aro_gc_wb(c, a->items, &a->items[a->len], *x_ref);
        a->len++;
        return;
    }
    // Grow path: realloc_payload may move the owning BaArray AND x's referent.
    // Caller has set c->sp already; realloc internally bumps c->sp to park
    // the old payload during the inner alloc.
    uint32_t new_capa = a->capa ? a->capa * 2 : 4;
    VALUE *new_items = (VALUE *)aro_gc_realloc_payload(c, a->items, sizeof(VALUE) * new_capa);
    // Reload both a and x after potential GC move.
    a = VAL2ARY(*av_ref);
    aro_gc_wb(c, a, (VALUE *)&a->items, (VALUE)new_items);
    a->capa = new_capa;
    aro_gc_wb(c, a->items, &a->items[a->len], *x_ref);
    a->len++;
}

// baruby_str_new: source bytes must outlive both internal allocs.  Safe
// sources: rodata, C-stack buffers in caller, etc.  For heap sources
// (substring of existing BaString) use baruby_str_slice instead.
// iter 53: SSO — `len <= BSTR_SSO_MAX` (7) skips the second alloc.
VALUE
baruby_str_new(CTX *c, const char *bytes, uint32_t len)
{
    VALUE *sp = c->sp;
    sp[0] = (VALUE)aro_gc_alloc(c, OBJ_STRING, sizeof(BaString));
    BaString *s = (BaString *)sp[0];
    s->hdr.type = OBJ_STRING;
    s->len = len;
    if (len <= BSTR_SSO_MAX) {
        s->hdr.flags = OBJ_FLAG_SSO;
        s->capa = sizeof s->small;
        if (len) memcpy(s->small, bytes, len);
        s->small[len] = '\0';
        return sp[0];
    }
    s->hdr.flags = 0;
    s->capa = len + 1;
    c->sp = sp + 1;
    char *new_bytes = (char *)aro_gc_alloc_byte(c, s->capa);
    s = (BaString *)sp[0];   // reload — second alloc may have moved
    aro_gc_wb(c, s, (VALUE *)&s->bytes, (VALUE)new_bytes);
    memcpy(s->bytes, bytes, len);
    s->bytes[len] = '\0';
    return sp[0];
}

VALUE
baruby_str_new_cstr(CTX *c, const char *cstr)
{
    return baruby_str_new(c, cstr, (uint32_t)strlen(cstr));
}

// baruby_str_slice: new BaString from [offset, offset+len) of *src_ref.
// src_ref must point at a caller sp slot holding a String.
VALUE
baruby_str_slice(CTX *c, VALUE *src_ref, uint32_t offset, uint32_t len)
{
    VALUE *sp = c->sp;
    sp[0] = (VALUE)aro_gc_alloc(c, OBJ_STRING, sizeof(BaString));
    BaString *r = (BaString *)sp[0];
    r->hdr.type = OBJ_STRING;
    r->len = len;
    if (len <= BSTR_SSO_MAX) {
        r->hdr.flags = OBJ_FLAG_SSO;
        r->capa = sizeof r->small;
        const BaString *src = VAL2STR(*src_ref);
        if (len) memcpy(r->small, bstr_bytes(src) + offset, len);
        r->small[len] = '\0';
        return sp[0];
    }
    r->hdr.flags = 0;
    r->capa = len + 1;
    c->sp = sp + 1;
    char *new_bytes = (char *)aro_gc_alloc_byte(c, r->capa);
    r = (BaString *)sp[0];                     // reload after alloc
    aro_gc_wb(c, r, (VALUE *)&r->bytes, (VALUE)new_bytes);
    const BaString *src = VAL2STR(*src_ref);   // post-GC source
    memcpy(r->bytes, bstr_bytes(src) + offset, len);
    r->bytes[len] = '\0';
    return sp[0];
}

VALUE
baruby_ary_plus(CTX *c, VALUE *av_ref, VALUE *bv_ref)
{
    const BaArray *a = VAL2ARY(*av_ref);
    const BaArray *b = VAL2ARY(*bv_ref);
    uint32_t total = a->len + b->len;
    VALUE rv = baruby_ary_new(c, total ? total : 1);
    BaArray *r = VAL2ARY(rv);
    a = VAL2ARY(*av_ref);   // reload after alloc — may have moved
    b = VAL2ARY(*bv_ref);
    aro_gc_wb_bulk(c, r->items, r->items,          a->items, a->len);
    aro_gc_wb_bulk(c, r->items, r->items + a->len, b->items, b->len);
    r->len = total;
    return rv;
}

bool
baruby_value_eq(VALUE a, VALUE b)
{
    // Identical bits cover fixnum identity, singleton identity, and
    // ptr identity (e.g. same Array compared to itself).
    if (a == b) return true;
    // Mixed int / ptr — different by construction.
    if (IS_INT(a) || IS_INT(b)) return false;
    // Any side that's a non-ptr singleton (true / false / nil) without
    // having matched in the identity check above is a different type.
    if (!IS_PTR(a) || !IS_PTR(b)) return false;
    // Both heap objects.
    uint32_t ta = OBJ_TYPE(a), tb = OBJ_TYPE(b);
    if (ta != tb) return false;
    if (ta == OBJ_STRING) {
        const BaString *sa = VAL2STR(a), *sb = VAL2STR(b);
        return sa->len == sb->len &&
               memcmp(bstr_bytes(sa), bstr_bytes(sb), sa->len) == 0;
    }
    if (ta == OBJ_ARRAY) {
        const BaArray *aa = VAL2ARY(a), *ab = VAL2ARY(b);
        if (aa->len != ab->len) return false;
        for (uint32_t i = 0; i < aa->len; i++) {
            if (!baruby_value_eq(aa->items[i], ab->items[i])) return false;
        }
        return true;
    }
    return false;
}

int
baruby_str_cmp(VALUE av, VALUE bv)
{
    const BaString *a = VAL2STR(av);
    const BaString *b = VAL2STR(bv);
    uint32_t mn = a->len < b->len ? a->len : b->len;
    int cmp = memcmp(bstr_bytes(a), bstr_bytes(b), mn);
    if (cmp != 0) return cmp;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

VALUE
baruby_str_repeat(CTX *c, VALUE *sv_ref, intptr_t n)
{
    if (n <= 0) return baruby_str_new(c, "", 0);
    VALUE *sp = c->sp;
    const BaString *s = VAL2STR(*sv_ref);
    uint64_t total = (uint64_t)s->len * (uint64_t)n;
    if (total > UINT32_MAX) total = UINT32_MAX;
    sp[0] = (VALUE)aro_gc_alloc(c, OBJ_STRING, sizeof(BaString));
    BaString *r = (BaString *)sp[0];
    s = VAL2STR(*sv_ref);   // reload after alloc
    r->hdr.type  = OBJ_STRING;
    r->len  = (uint32_t)total;
    if (r->len <= BSTR_SSO_MAX) {
        r->hdr.flags = OBJ_FLAG_SSO;
        r->capa = sizeof r->small;
        for (intptr_t i = 0; i < n; i++) {
            memcpy(r->small + (uint32_t)i * s->len, bstr_bytes(s), s->len);
        }
        r->small[r->len] = '\0';
        return sp[0];
    }
    r->hdr.flags = 0;
    r->capa = r->len + 1;
    c->sp = sp + 1;
    char *new_bytes = (char *)aro_gc_alloc_byte(c, r->capa);
    r = (BaString *)sp[0];
    aro_gc_wb(c, r, (VALUE *)&r->bytes, (VALUE)new_bytes);
    s = VAL2STR(*sv_ref);
    for (intptr_t i = 0; i < n; i++) {
        memcpy(r->bytes + (uint32_t)i * s->len, bstr_bytes(s), s->len);
    }
    r->bytes[r->len] = '\0';
    return sp[0];
}

VALUE
baruby_ary_repeat(CTX *c, VALUE *av_ref, intptr_t n)
{
    if (n <= 0) return baruby_ary_new(c, 0);
    const BaArray *a = VAL2ARY(*av_ref);
    uint64_t total = (uint64_t)a->len * (uint64_t)n;
    if (total > UINT32_MAX) total = UINT32_MAX;
    VALUE rv = baruby_ary_new(c, (uint32_t)total ? (uint32_t)total : 1);
    BaArray *r = VAL2ARY(rv);
    a = VAL2ARY(*av_ref);   // reload after alloc
    for (intptr_t i = 0; i < n; i++) {
        aro_gc_wb_bulk(c, r->items, r->items + (uint32_t)i * a->len,
                          a->items, a->len);
    }
    r->len = (uint32_t)total;
    return rv;
}

void
baruby_str_append(CTX *c, VALUE *dst_ref, VALUE *src_ref)
{
    BaString *d = VAL2STR(*dst_ref);
    const BaString *s = VAL2STR(*src_ref);
    uint32_t need = d->len + s->len + 1;

    if (BSTR_IS_SSO(d) && need <= sizeof d->small) {
        if (s->len) memcpy(d->small + d->len, bstr_bytes(s), s->len);
        d->len += s->len;
        d->small[d->len] = '\0';
        return;
    }

    if (BSTR_IS_SSO(d)) {
        uint32_t nc = 8;
        while (nc < need) nc *= 2;
        char small_buf[sizeof d->small];
        uint32_t d_len = d->len;
        memcpy(small_buf, d->small, d_len);
        /* c->sp already set by caller. */
        char *new_bytes = (char *)aro_gc_alloc_byte(c, nc);
        d = VAL2STR(*dst_ref);   // reload after alloc
        s = VAL2STR(*src_ref);
        d->hdr.flags &= ~OBJ_FLAG_SSO;
        memcpy(new_bytes, small_buf, d_len);
        aro_gc_wb(c, d, (VALUE *)&d->bytes, (VALUE)new_bytes);
        d->capa = nc;
    }
    else if (need > d->capa) {
        uint32_t nc = d->capa ? d->capa * 2 : 8;
        while (nc < need) nc *= 2;
        char *new_bytes = (char *)aro_gc_realloc_payload(c, d->bytes, nc);
        d = VAL2STR(*dst_ref);   // reload after realloc
        s = VAL2STR(*src_ref);
        aro_gc_wb(c, d, (VALUE *)&d->bytes, (VALUE)new_bytes);
        d->capa = nc;
    }
    if (s->len) memcpy(d->bytes + d->len, bstr_bytes(s), s->len);
    d->len += s->len;
    d->bytes[d->len] = '\0';
}

// Append-based libgc-backed string builder.  We can't use
// open_memstream + libc free here — the `#define free(p) ((void)(p))`
// shadow in context.h would silently leak the libc buffer, which on
// any to_s-heavy benchmark turns into runaway memory growth.
typedef struct {
    char    *bytes;
    uint32_t len, capa;
} StrBuf;

static void
sb_append(StrBuf *sb, const char *src, uint32_t n)
{
    if (sb->len + n + 1 > sb->capa) {
        uint32_t nc = sb->capa ? sb->capa * 2 : 32;
        while (nc < sb->len + n + 1) nc *= 2;
        sb->bytes = (char *)realloc(sb->bytes, nc);
        sb->capa = nc;
    }
    memcpy(sb->bytes + sb->len, src, n);
    sb->len += n;
}

// Inspect-style serialization (matches `p` output): strings get quotes,
// arrays nested, nil → "nil".  baruby_to_s special-cases the top-level
// case where `nil.to_s == ""` (Ruby semantics).
static void
to_s_inner(StrBuf *sb, VALUE v)
{
    char tmp[32];
    if (v == VAL_NIL)   { sb_append(sb, "nil", 3);   return; }
    if (v == VAL_FALSE) { sb_append(sb, "false", 5); return; }
    if (v == VAL_TRUE)  { sb_append(sb, "true", 4);  return; }
    if (IS_INT(v)) {
        int n = snprintf(tmp, sizeof tmp, "%ld", (long)VAL2INT(v));
        sb_append(sb, tmp, (uint32_t)n);
        return;
    }
    if (IS_STR(v)) {
        const BaString *s = VAL2STR(v);
        const char *sb_bytes = bstr_bytes(s);
        sb_append(sb, "\"", 1);
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char ch = (unsigned char)sb_bytes[i];
            char buf[8];
            switch (ch) {
              case '\n': sb_append(sb, "\\n", 2); break;
              case '\t': sb_append(sb, "\\t", 2); break;
              case '\r': sb_append(sb, "\\r", 2); break;
              case '\\': sb_append(sb, "\\\\", 2); break;
              case '"':  sb_append(sb, "\\\"", 2); break;
              default:
                if (ch < 0x20 || ch == 0x7f) {
                    int n = snprintf(buf, sizeof buf, "\\x%02X", ch);
                    sb_append(sb, buf, (uint32_t)n);
                } else {
                    sb_append(sb, &sb_bytes[i], 1);
                }
            }
        }
        sb_append(sb, "\"", 1);
        return;
    }
    if (IS_ARY(v)) {
        const BaArray *a = VAL2ARY(v);
        sb_append(sb, "[", 1);
        for (uint32_t i = 0; i < a->len; i++) {
            if (i) sb_append(sb, ", ", 2);
            to_s_inner(sb, a->items[i]);
        }
        sb_append(sb, "]", 1);
        return;
    }
}

VALUE
baruby_to_s(CTX *c, VALUE v)
{
    if (IS_STR(v))      return v;
    if (v == VAL_NIL)   return baruby_str_new_cstr(c, "");
    if (v == VAL_FALSE) return baruby_str_new_cstr(c, "false");
    if (v == VAL_TRUE)  return baruby_str_new_cstr(c, "true");
    if (IS_INT(v)) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%ld", (long)VAL2INT(v));
        return baruby_str_new(c, buf, (uint32_t)n);
    }
    StrBuf sb = {0};
    to_s_inner(&sb, v);
    VALUE r = baruby_str_new(c, sb.bytes, sb.len);
    free(sb.bytes);
    return r;
}

VALUE
baruby_str_concat(CTX *c, VALUE *av_ref, VALUE *bv_ref)
{
    VALUE *sp = c->sp;
    uint32_t a_len = VAL2STR(*av_ref)->len;
    uint32_t b_len = VAL2STR(*bv_ref)->len;
    uint32_t total = a_len + b_len;

    sp[0] = (VALUE)aro_gc_alloc(c, OBJ_STRING, sizeof(BaString));
    BaString *r = (BaString *)sp[0];
    r->hdr.type = OBJ_STRING;
    r->len = total;

    if (total <= BSTR_SSO_MAX) {
        r->hdr.flags = OBJ_FLAG_SSO;
        r->capa = sizeof r->small;
        const BaString *a = VAL2STR(*av_ref);
        const BaString *b = VAL2STR(*bv_ref);
        if (a_len) memcpy(r->small,         bstr_bytes(a), a_len);
        if (b_len) memcpy(r->small + a_len, bstr_bytes(b), b_len);
        r->small[total] = '\0';
        return sp[0];
    }

    r->hdr.flags = 0;
    r->capa = total + 1;
    c->sp = sp + 1;
    char *new_bytes = (char *)aro_gc_alloc_byte(c, r->capa);
    r = (BaString *)sp[0];
    aro_gc_wb(c, r, (VALUE *)&r->bytes, (VALUE)new_bytes);

    const BaString *a = VAL2STR(*av_ref);
    const BaString *b = VAL2STR(*bv_ref);
    if (a_len) memcpy(r->bytes,         bstr_bytes(a), a_len);
    if (b_len) memcpy(r->bytes + a_len, bstr_bytes(b), b_len);
    r->bytes[total] = '\0';
    return sp[0];
}

void
baruby_print_value(FILE *fp, VALUE v)
{
    if (v == VAL_NIL) {
        fputs("nil", fp);
    }
    else if (v == VAL_FALSE) {
        fputs("false", fp);
    }
    else if (v == VAL_TRUE) {
        fputs("true", fp);
    }
    else if (IS_INT(v)) {
        fprintf(fp, "%ld", (long)VAL2INT(v));
    }
    else if (IS_ARY(v)) {
        const BaArray *a = VAL2ARY(v);
        fputc('[', fp);
        for (uint32_t i = 0; i < a->len; i++) {
            if (i) fputs(", ", fp);
            baruby_print_value(fp, a->items[i]);
        }
        fputc(']', fp);
    }
    else if (IS_STR(v)) {
        const BaString *s = VAL2STR(v);
        const char *pb = bstr_bytes(s);
        fputc('"', fp);
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char ch = (unsigned char)pb[i];
            switch (ch) {
              case '\n': fputs("\\n", fp); break;
              case '\t': fputs("\\t", fp); break;
              case '\r': fputs("\\r", fp); break;
              case '\\': fputs("\\\\", fp); break;
              case '"':  fputs("\\\"", fp); break;
              default:
                if (ch < 0x20 || ch == 0x7f) fprintf(fp, "\\x%02X", ch);
                else fputc((int)ch, fp);
            }
        }
        fputc('"', fp);
    }
    else {
        fprintf(fp, "<unknown:0x%lx>", (unsigned long)v);
    }
}
