#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "context.h"
#include "precise_gc/gc.h"
#include "node.h"
#include "astro_code_store.h"

// Allocation hook used by generated ALLOC_node_* functions.
//
// NODEs live as long as the program; we allocate via libc malloc (rather
// than aro_gc_alloc) because the generated ALLOC_node_* prototype doesn't
// carry CTX, and NODEs never need reclamation.  The Scheme VALUEs that
// NODEs reference (e.g. node_quote.v) live on the precise GC heap and are
// reachable from the GC's roots via c->globals etc. — NODEs are read-only
// metadata that the GC doesn't have to scan.
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        fprintf(stderr, "ascheme: node allocation failed\n");
        exit(1);
    }
    return n;
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
#if defined(__has_include) && __has_include("node_emit_ast.c")
#include "node_emit_ast.c"
#endif
#include "node_alloc.c"

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
