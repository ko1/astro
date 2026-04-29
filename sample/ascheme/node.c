#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"
#include "astro_code_store.h"

// Allocation hook used by generated ALLOC_node_* functions.
//
// We allocate NODEs out of the GC heap so that scheme values referenced
// from NODE fields (`node_quote.v`, `node_const_str.s`, etc.) are kept
// alive by Boehm's conservative scan.  NODEs themselves never need to
// be reclaimed (they live as long as the program), but skipping GC for
// them would let referenced strings/lists/closures get freed prematurely.
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)GC_malloc(size);
    if (n == NULL) {
        fprintf(stderr, "ascheme: node allocation failed\n");
        exit(1);
    }
    return n;
}

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

#include "astro_node.c"
#include "astro_code_store.c"

// OPTIMIZE: opportunistic code-store lookup.  If `--no-compile` (default
// for the interpreter) is set, this is a no-op.
NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    astro_cs_load(n, NULL);
    return n;
}

// Code-repository hook is unused for ascheme.
void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
