#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"

extern size_t node_cnt;

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "arawk: out of memory\n");
        exit(EXIT_FAILURE);
    }
    // Zero the whole NODE: ALLOC_<name> sets operand fields and most
    // flags, but `head.flags.has_hash_opt` and `head.hash_opt` would
    // otherwise be uninitialised and HOPT() can spuriously hit the
    // cache with garbage.
    memset(n, 0, size);
    node_cnt++;
    return n;
}

__attribute__((unused)) static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

#include "astro_node.c"
#include "astro_code_store.c"

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.plain) {
        return n;
    }
    (void)astro_cs_load(n, NULL);
    return n;
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
