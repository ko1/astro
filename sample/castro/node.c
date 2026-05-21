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

// Set to true while load_program is running so the per-ALLOC OPTIMIZE
// hook (auto-emitted by ASTroGen into every node's ALLOC_xxx) doesn't
// fire `astro_cs_load`.  Two reasons:
//
//   1. Parse-time `node_call_static` nodes are allocated with a NULL
//      `callee` and patched in phase 3.  cs_load triggers hash_node()
//      which caches the parent's hash incorporating the *unpatched*
//      callee → after the patch the cached hash is wrong → cs_load at
//      load_all_funcs time looks up the wrong SD_<hash> symbol and
//      silently falls back to the interpreter.  This is exactly why
//      md5 / any kernel with function calls was getting interp-mode
//      perf even when the AOT cache was on disk.
//   2. We don't want N×M dlsym() calls during parse — there's nothing
//      productive to do at ALLOC time, every body gets cs_load'd at
//      the end via load_all_funcs anyway.
bool parsing_phase = false;

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    if (parsing_phase) return n;
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
#if defined(__has_include) && __has_include("node_emit_ast.c")
#include "node_emit_ast.c"
#endif
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
