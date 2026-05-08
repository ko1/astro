#include "context.h"
#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (!n) { fprintf(stderr, "alloc failed\n"); exit(1); }
    return n;
}

#include "astro_node.c"
#include "astro_code_store.c"

// EVAL is inline in node.h.

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    if (astro_cs_load(n, NULL)) {
        if (!OPTION.quiet) {
            fprintf(stderr, "hit!: %s\n", n->head.kind->default_dispatcher_name);
        }
    }
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
    // The SD chain inlines many expressions into one giant tree.  At
    // -O3 gcc defaults to `-ffp-contract=fast` (FMA fuses across
    // statements), which yields different rounding than the original
    // C statements would have produced.  Force `-ffp-contract=on` so
    // FMA only fires within a single C expression (= the standard C99
    // behaviour) and our AOT result matches interp / gcc -O3 of the
    // original source.  Mandelbrot diverges by ~2 iters out of 30k
    // without this — visible as "expected 174, got 114" on the
    // long-bench when the count is XOR-folded.
    setenv("ASTRO_EXTRA_CFLAGS", "-ffp-contract=on", 0);
    astro_cs_init("code_store", ".", 0);
}
