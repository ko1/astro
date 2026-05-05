/*
 * node.c — bridges user code with ASTroGen's generated runtime.
 *
 * This file is the only place that #includes the generated node_*.c
 * blobs and the framework files (astro_node.c, astro_code_store.c).
 * It also provides the `node_allocate` symbol that the runtime needs.
 */
#include "node.h"

/* --- allocation: use libgc so we don't have to worry about lifetimes ---- */
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)GC_malloc(size);
    if (n == NULL) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return n;
}

/* --- common helpers ---- */
#include "astro_node.c"

/* --- code store --- */
#include "astro_code_store.c"

/* --- generated code --- */
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

/* --- public glue ---------------------------------------------------- */

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    astro_cs_load(n, NULL);
    return n;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

void
INIT(void)
{
    GC_init();
    astro_cs_init("code_store", ".", 0);
}
