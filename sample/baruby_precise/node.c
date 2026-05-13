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
    astro_cs_init("code_store", ".", 0);
}

// --- Heap allocators (precise GC) ---
//
// All allocators take a trailing `sp_top` argument: the caller's current
// scratch top.  Before any alloc that might trigger collection, we set
// c->sp = sp_top so the mark phase scans the correct VALUE range.
//
// Auxiliary buffers (items / bytes) are tracked separately via
// baruby_gc_alloc_payload — they're not VALUE roots in their own right,
// just plain memory that the owning object's sweep frees.

// Two-alloc allocators use sp_top[0] as a temporary root for the header
// so the second alloc's GC doesn't lose it.  The caller must pass an
// sp_top that has at least 1 slot of headroom (= sp_top[0] is writable
// and inside c->env..c->sp scan range).
VALUE
baruby_ary_new(uint32_t capa, VALUE *sp_top)
{
    sp_top[0] = (VALUE)baruby_gc_alloc(OBJ_ARRAY, sizeof(BaArray), sp_top + 1);
    BaArray *a = (BaArray *)sp_top[0];
    a->hdr.type = OBJ_ARRAY;
    a->hdr.flags = 0;
    a->len = 0;
    a->capa = capa;
    if (capa) {
        VALUE *items = (VALUE *)baruby_gc_alloc(KIND_PAYLOAD_VAL, sizeof(VALUE) * capa, sp_top + 1);
        a = (BaArray *)sp_top[0];   // reload after potential GC
        a->items = items;
    } else {
        a->items = NULL;
    }
    return sp_top[0];
}

VALUE
baruby_ary_new_from(const VALUE *items, uint32_t n, VALUE *sp_top)
{
    VALUE v = baruby_ary_new(n ? n : 1, sp_top);
    BaArray *a = VAL2ARY(v);
    if (n) memcpy(a->items, items, sizeof(VALUE) * n);
    a->len = n;
    return v;
}

// baruby_ary_push: takes a pointer to the caller's sp slot holding `av`,
// so we can re-read the post-move array address after the realloc-alloc
// triggers GC.  Passing `av` by value would lose this update.
void
baruby_ary_push(VALUE *av_ref, VALUE x, VALUE *sp_top)
{
    BaArray *a = VAL2ARY(*av_ref);
    fprintf(stderr, "[ary_push] a=%p (=*av_ref %lx) items=%p len/capa=%u/%u x=%lx\n",
            (void*)a, (long)*av_ref, (void*)a->items, a->len, a->capa, (long)x);
    if (a->len < a->capa) {
        a->items[a->len++] = x;
        return;
    }
    // Grow path: realloc_payload may move the owning BaArray.
    uint32_t new_capa = a->capa ? a->capa * 2 : 4;
    VALUE *new_items = (VALUE *)baruby_gc_realloc_payload(
        a->items, sizeof(VALUE) * new_capa, sp_top);
    // Reload after potential GC move.
    a = VAL2ARY(*av_ref);
    a->items = new_items;
    a->capa = new_capa;
    a->items[a->len++] = x;
}

VALUE
baruby_str_new(const char *bytes, uint32_t len, VALUE *sp_top)
{
    // Buffer source bytes BEFORE alloc may move/overwrite them.  The
    // caller may pass a pointer into a heap String's bytes payload;
    // after our internal alloc triggers GC, the source moves and the
    // OLD address points at unrelated memory.  Save on the C heap.
    char *buf = NULL;
    if (len) {
        buf = (char *)malloc(len);
        memcpy(buf, bytes, len);
    }
    sp_top[0] = (VALUE)baruby_gc_alloc(OBJ_STRING, sizeof(BaString), sp_top + 1);
    BaString *s = (BaString *)sp_top[0];
    s->hdr.type = OBJ_STRING;
    s->hdr.flags = 0;
    s->len = len;
    s->capa = len + 1;
    char *new_bytes = (char *)baruby_gc_alloc(KIND_PAYLOAD_BYTE, s->capa, sp_top + 1);
    s = (BaString *)sp_top[0];
    s->bytes = new_bytes;
    if (len) memcpy(s->bytes, buf, len);
    s->bytes[len] = '\0';
    if (buf) free(buf);
    return sp_top[0];
}

VALUE
baruby_str_new_cstr(const char *cstr, VALUE *sp_top)
{
    return baruby_str_new(cstr, (uint32_t)strlen(cstr), sp_top);
}

VALUE
baruby_ary_plus(VALUE av, VALUE bv, VALUE *sp_top)
{
    const BaArray *a = VAL2ARY(av);
    const BaArray *b = VAL2ARY(bv);
    uint32_t total = a->len + b->len;
    VALUE rv = baruby_ary_new(total ? total : 1, sp_top);
    BaArray *r = VAL2ARY(rv);
    // Reload a / b after alloc (in case future moving GC moves them);
    // for current non-moving M&S the pointers stay valid, but the reload
    // is harmless and documents the moving-safe pattern.
    a = VAL2ARY(av);
    b = VAL2ARY(bv);
    if (a->len) memcpy(r->items,            a->items, sizeof(VALUE) * a->len);
    if (b->len) memcpy(r->items + a->len,   b->items, sizeof(VALUE) * b->len);
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
        return sa->len == sb->len && memcmp(sa->bytes, sb->bytes, sa->len) == 0;
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
    int cmp = memcmp(a->bytes, b->bytes, mn);
    if (cmp != 0) return cmp;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

VALUE
baruby_str_repeat(VALUE sv, intptr_t n, VALUE *sp_top)
{
    if (n <= 0) return baruby_str_new("", 0, sp_top);
    const BaString *s = VAL2STR(sv);
    uint64_t total = (uint64_t)s->len * (uint64_t)n;
    if (total > UINT32_MAX) total = UINT32_MAX;
    sp_top[0] = (VALUE)baruby_gc_alloc(OBJ_STRING, sizeof(BaString), sp_top + 1);
    BaString *r = (BaString *)sp_top[0];
    s = VAL2STR(sv);
    r->hdr.type  = OBJ_STRING;
    r->hdr.flags = 0;
    r->len  = (uint32_t)total;
    r->capa = r->len + 1;
    char *new_bytes = (char *)baruby_gc_alloc(KIND_PAYLOAD_BYTE, r->capa, sp_top + 1);
    r = (BaString *)sp_top[0];
    r->bytes = new_bytes;
    s = VAL2STR(sv);
    for (intptr_t i = 0; i < n; i++) {
        memcpy(r->bytes + (uint32_t)i * s->len, s->bytes, s->len);
    }
    r->bytes[r->len] = '\0';
    return sp_top[0];
}

VALUE
baruby_ary_repeat(VALUE av, intptr_t n, VALUE *sp_top)
{
    if (n <= 0) return baruby_ary_new(0, sp_top);
    const BaArray *a = VAL2ARY(av);
    uint64_t total = (uint64_t)a->len * (uint64_t)n;
    if (total > UINT32_MAX) total = UINT32_MAX;
    VALUE rv = baruby_ary_new((uint32_t)total ? (uint32_t)total : 1, sp_top);
    BaArray *r = VAL2ARY(rv);
    a = VAL2ARY(av);
    for (intptr_t i = 0; i < n; i++) {
        if (a->len) memcpy(r->items + (uint32_t)i * a->len, a->items, sizeof(VALUE) * a->len);
    }
    r->len = (uint32_t)total;
    return rv;
}

void
baruby_str_append(VALUE dstv, VALUE srcv, VALUE *sp_top)
{
    BaString *d = VAL2STR(dstv);
    const BaString *s = VAL2STR(srcv);
    uint32_t need = d->len + s->len + 1;
    if (need > d->capa) {
        uint32_t nc = d->capa ? d->capa * 2 : 8;
        while (nc < need) nc *= 2;
        d->bytes = (char *)baruby_gc_realloc_payload(d->bytes, nc, sp_top);
        d = VAL2STR(dstv);
        s = VAL2STR(srcv);
        d->capa = nc;
    }
    if (s->len) memcpy(d->bytes + d->len, s->bytes, s->len);
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
        sb_append(sb, "\"", 1);
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char ch = (unsigned char)s->bytes[i];
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
                    sb_append(sb, (const char *)&s->bytes[i], 1);
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
baruby_to_s(VALUE v, VALUE *sp_top)
{
    if (IS_STR(v))      return v;
    if (v == VAL_NIL)   return baruby_str_new_cstr("", sp_top);
    if (v == VAL_FALSE) return baruby_str_new_cstr("false", sp_top);
    if (v == VAL_TRUE)  return baruby_str_new_cstr("true", sp_top);
    if (IS_INT(v)) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%ld", (long)VAL2INT(v));
        return baruby_str_new(buf, (uint32_t)n, sp_top);
    }
    StrBuf sb = {0};
    to_s_inner(&sb, v);
    VALUE r = baruby_str_new(sb.bytes, sb.len, sp_top);
    free(sb.bytes);
    return r;
}

// baruby_str_concat: av, bv are passed by VALUE — stale after our internal
// allocs.  Buffer their content (size + bytes) up front on the C heap so we
// don't need to re-read through stale ptrs.  Cost: 2 mallocs per concat.
// Future improvement: take av_ref / bv_ref (sp slot pointers) instead.
VALUE
baruby_str_concat(VALUE av, VALUE bv, VALUE *sp_top)
{
    const BaString *a = VAL2STR(av);
    const BaString *b = VAL2STR(bv);
    uint32_t a_len = a->len, b_len = b->len;
    uint32_t total = a_len + b_len;
    char *buf_a = a_len ? (char *)malloc(a_len) : NULL;
    char *buf_b = b_len ? (char *)malloc(b_len) : NULL;
    if (a_len) memcpy(buf_a, a->bytes, a_len);
    if (b_len) memcpy(buf_b, b->bytes, b_len);

    sp_top[0] = (VALUE)baruby_gc_alloc(OBJ_STRING, sizeof(BaString), sp_top + 1);
    BaString *r = (BaString *)sp_top[0];
    r->hdr.type = OBJ_STRING;
    r->hdr.flags = 0;
    r->len = total;
    r->capa = total + 1;
    char *new_bytes = (char *)baruby_gc_alloc(KIND_PAYLOAD_BYTE, r->capa, sp_top + 1);
    r = (BaString *)sp_top[0];
    r->bytes = new_bytes;
    if (a_len) memcpy(r->bytes,         buf_a, a_len);
    if (b_len) memcpy(r->bytes + a_len, buf_b, b_len);
    r->bytes[total] = '\0';
    free(buf_a); free(buf_b);
    return sp_top[0];
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
        fputc('"', fp);
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char ch = (unsigned char)s->bytes[i];
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
