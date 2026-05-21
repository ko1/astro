#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"
#include "astro_code_store.h"

// --- Node allocation ------------------------------------------------------
//
// NODEs are allocated with calloc and never freed individually.  Each NODE is
// wrapped in a Ruby T_DATA (arjsv_node_type, defined in arjsv.c) so the GC
// frees them when no Schema references them anymore.

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        rb_raise(rb_eNoMemError, "arjsv: NODE allocation failed");
    }
    return n;
}

// --- ASTro common runtime (HASH, DUMP, hash helpers) ---------------------

#include "astro_node.c"

// --- Code store -----------------------------------------------------------

#include "astro_code_store.c"

// --- EVAL / OPTIMIZE ------------------------------------------------------

int
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

// code_repo: arjsv has no named entries (each Schema is its own root, compiled
// directly via astro_cs_compile).
void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

// --- Generated ------------------------------------------------------------

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"

// GC mark helper used by generated node_mark.c.  Must be visible before the
// include below — node_mark.c references MARK as a function-like macro.
static void
arjsv_mark_child_node(NODE *child)
{
    if (child && child->head.rb_wrapper) {
        rb_gc_mark(child->head.rb_wrapper);
    }
}
#define MARK(child) arjsv_mark_child_node(child)

#include "node_mark.c"
#if defined(__has_include) && __has_include("node_emit_ast.c")
#include "node_emit_ast.c"
#endif
#include "node_alloc.c"

// --- INIT -----------------------------------------------------------------

void
INIT(void)
{
    // store_dir relative to where the loaded process runs.  Tests cd into
    // the sample dir; the dlopen path resolves relative to cwd.
    astro_cs_init("code_store", ".", 0);
}
