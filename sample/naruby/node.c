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

// naruby-specific hash: identifies a builtin function by its source identity
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

// --- naruby-specific helpers ---

void
clear_hash(NODE *n)
{
    while (n) {
        n->head.flags.has_hash_value = false;
        n = n->head.parent;
    }
}

// --- User-provided: OPTIMIZE ---

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
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
// string buffer.  Used by the JIT path (sample/naruby/astro_jit.c) to send
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
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// --- INIT ---

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
