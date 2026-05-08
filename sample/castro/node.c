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
    // The SD chain inlines many distinct C statements into one giant
    // expression tree.  At -O3, gcc defaults to `-ffp-contract=fast`
    // (fuse multiply-add across statement boundaries), which yields
    // different rounding than the original C statements would produce.
    // Without this override, mandelbrot's AOT result drifts from gcc's
    // -O3 (and from castro's own interpreter): the long-bench
    // 30-iter sum returns 114 instead of 174.  Forcing
    // `-ffp-contract=on` (= C99 within-expression FMA only) makes the
    // AOT output match every other tier.
    setenv("ASTRO_EXTRA_CFLAGS", "-ffp-contract=on", 0);
    astro_cs_init("code_store", ".", 0);
}
