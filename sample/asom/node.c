// asom: ASTro SOM runtime support.
//
// Mirrors sample/calc/node.c: provides node_allocate / dispatch_info / EVAL /
// OPTIMIZE / INIT, then includes the generated infrastructure and the
// asom-specific runtime helpers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "asom_runtime.h"

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        fprintf(stderr, "asom: node allocation failed\n");
        exit(EXIT_FAILURE);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Common ASTro infrastructure
// ---------------------------------------------------------------------------

#include "astro_node.c"
#include "astro_code_store.c"

// ---------------------------------------------------------------------------
// EVAL / OPTIMIZE / HORG / HOPT
// ---------------------------------------------------------------------------

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

static unsigned int g_swap_count;
static unsigned int g_alloc_count;

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    g_alloc_count++;
    // OPTIMIZE happens at parse time inside ALLOC_node_*. Children get
    // visited too, but only entry-level nodes have their own SD_<hash>
    // public symbols (children are baked as static inline functions
    // inside the parent SD's .c file). Cache-load is best effort.
    if (astro_cs_load(n, NULL)) g_swap_count++;
    return n;
}

void asom_print_optimize_stats(void)
{
    fprintf(stderr, "asom: OPTIMIZE: swapped %u of %u nodes\n",
            g_swap_count, g_alloc_count);
}

// asom does not yet have a profile-aware hash; just delegate to the
// structural Merkle hash for both HORG (mandatory) and HOPT (called only
// when Hopt mode is enabled, which we never enable for now).
node_hash_t HORG(NODE *n) { return HASH(n); }
node_hash_t HOPT(NODE *n) { return HASH(n); }

// Dispatcher swap (type-feedback). Specialized variants opt into the
// canonical hash via @canonical=node_send1 in node.def, so swapping
// preserves Horg and the AOT cache stays valid.
static unsigned int g_swap_dispatcher_count;
void
swap_dispatcher(NODE *n, const struct NodeKind *target_kind)
{
    if (n->head.kind == target_kind) return;
    if (n->head.flags.is_specialized) return;
    n->head.dispatcher = target_kind->default_dispatcher;
    n->head.dispatcher_name = target_kind->default_dispatcher_name;
    n->head.kind = target_kind;
    g_swap_dispatcher_count++;
}

unsigned int asom_swap_dispatcher_count(void) { return g_swap_dispatcher_count; }

// ---------------------------------------------------------------------------
// code_repo — the asom front-end maintains its own (class, method) registry
// in asom_runtime.c, so this just satisfies the linker for now.
// ---------------------------------------------------------------------------

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

// ---------------------------------------------------------------------------
// Generated code
// ---------------------------------------------------------------------------

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void
INIT(void)
{
    const char *store = OPTION.code_store_dir ? OPTION.code_store_dir : "code_store";
    // src_dir tells the code store where node.h / node_eval.c live so the
    // SD_*.c shards can `#include` them. We keep them next to the asom
    // binary (cwd at startup), matching the in-tree dev layout.
    astro_cs_init(store, ".", 0);
}
