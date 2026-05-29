#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "node.h"
#include "astro_code_store.h"

// =====================================================================
// anlox runtime glue: node allocation, code-store wiring, EVAL/OPTIMIZE/
// INIT.  The value model and builtins live in value.c; EVAL is the inline
// in node.h.
// =====================================================================

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)GC_MALLOC(size);
    if (!n) { fprintf(stderr, "anlox: out of memory\n"); exit(1); }
    memset(n, 0, size);
    return n;
}

#include "astro_node.c"
#include "astro_code_store.c"
#include "astro_build.c"

NODE *
OPTIMIZE(NODE *n)
{
    if (!OPTION.no_compiled_code) astro_cs_load(n, NULL);
    return n;
}

void code_repo_add(const char *name, NODE *body, bool force) { (void)name; (void)body; (void)force; }

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
    GC_INIT();
    astro_cs_init("code_store", ".", 0);
}

// AOT: the program root, every function body, and every variable-arity
// child (statement / argument nodes) are reached via a runtime dispatcher
// read, so each was collected as a code-store entry by the parser.
void
lox_aot_specialize(NODE *root)
{
    (void)root;
    for (int i = 0; i < lox_n_entries; i++) astro_cs_compile(lox_entries[i], NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    for (int i = 0; i < lox_n_entries; i++) astro_cs_load(lox_entries[i], NULL);
}
