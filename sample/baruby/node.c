#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "astro_jit.h"

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

// --- Heap allocators ---

VALUE
baruby_ary_new(uint32_t capa)
{
    BaArray *a = (BaArray *)malloc(sizeof(BaArray));
    a->hdr.type = OBJ_ARRAY;
    a->hdr.flags = 0;
    a->len = 0;
    a->capa = capa;
    a->items = capa ? (VALUE *)malloc(sizeof(VALUE) * capa) : NULL;
    return (VALUE)a;
}

VALUE
baruby_ary_new_from(const VALUE *items, uint32_t n)
{
    VALUE v = baruby_ary_new(n ? n : 1);
    BaArray *a = VAL2ARY(v);
    if (n) memcpy(a->items, items, sizeof(VALUE) * n);
    a->len = n;
    return v;
}

void
baruby_ary_push(VALUE av, VALUE x)
{
    BaArray *a = VAL2ARY(av);
    if (a->len == a->capa) {
        uint32_t new_capa = a->capa ? a->capa * 2 : 4;
        a->items = (VALUE *)realloc(a->items, sizeof(VALUE) * new_capa);
        a->capa = new_capa;
    }
    a->items[a->len++] = x;
}

VALUE
baruby_str_new(const char *bytes, uint32_t len)
{
    BaString *s = (BaString *)malloc(sizeof(BaString));
    s->hdr.type = OBJ_STRING;
    s->hdr.flags = 0;
    s->len = len;
    s->capa = len + 1;
    s->bytes = (char *)malloc(s->capa);
    if (len) memcpy(s->bytes, bytes, len);
    s->bytes[len] = '\0';
    return (VALUE)s;
}

VALUE
baruby_str_new_cstr(const char *cstr)
{
    return baruby_str_new(cstr, (uint32_t)strlen(cstr));
}

VALUE
baruby_str_concat(VALUE av, VALUE bv)
{
    const BaString *a = VAL2STR(av);
    const BaString *b = VAL2STR(bv);
    uint32_t total = a->len + b->len;
    BaString *r = (BaString *)malloc(sizeof(BaString));
    r->hdr.type = OBJ_STRING;
    r->hdr.flags = 0;
    r->len = total;
    r->capa = total + 1;
    r->bytes = (char *)malloc(r->capa);
    memcpy(r->bytes, a->bytes, a->len);
    memcpy(r->bytes + a->len, b->bytes, b->len);
    r->bytes[total] = '\0';
    return (VALUE)r;
}

void
baruby_print_value(FILE *fp, VALUE v)
{
    if (v == VAL_FALSE) {
        fputs("false", fp);
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
        fwrite(s->bytes, 1, s->len, fp);
        fputc('"', fp);
    }
    else {
        fprintf(fp, "<unknown:0x%lx>", (unsigned long)v);
    }
}
