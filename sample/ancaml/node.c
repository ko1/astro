#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include "node.h"
#include "astro_code_store.h"

// =====================================================================
// ancaml runtime glue: node allocation, code-store wiring, and the
// EVAL / OPTIMIZE / INIT plumbing.  The value model and builtins live
// in value.c; EVAL itself is the inline in node.h.
// =====================================================================

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)GC_MALLOC(size);
    if (!n) { fprintf(stderr, "ancaml: out of memory\n"); exit(1); }
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

// Swap an application node to its tail variant in place.  The tail nodes
// share the exact struct layout of their non-tail counterparts, so swapping
// both the dispatcher and the kind is enough — and because the kind drives
// the AST emitter, a `--build`-baked executable also gets the tail node.
static void
to_tail(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    if      (d == DISPATCH_node_app1) { n->head.dispatcher = DISPATCH_node_tail_app1; n->head.kind = &kind_node_tail_app1; }
    else if (d == DISPATCH_node_app2) { n->head.dispatcher = DISPATCH_node_tail_app2; n->head.kind = &kind_node_tail_app2; }
    else if (d == DISPATCH_node_app3) { n->head.dispatcher = DISPATCH_node_tail_app3; n->head.kind = &kind_node_tail_app3; }
    else if (d == DISPATCH_node_app4) { n->head.dispatcher = DISPATCH_node_tail_app4; n->head.kind = &kind_node_tail_app4; }
    else if (d == DISPATCH_node_appn) { n->head.dispatcher = DISPATCH_node_tail_appn; n->head.kind = &kind_node_tail_appn; }
}

void
ac_mark_tail(NODE *n)
{
    const struct NodeKind *k = n->head.kind;
    if (k == &kind_node_app1 || k == &kind_node_app2 || k == &kind_node_app3 ||
        k == &kind_node_app4 || k == &kind_node_appn) { to_tail(n); return; }
    if (k == &kind_node_if)       { ac_mark_tail(n->u.node_if.thn); ac_mark_tail(n->u.node_if.els); return; }
    if (k == &kind_node_let)      { ac_mark_tail(n->u.node_let.body); return; }
    if (k == &kind_node_letrec)   { ac_mark_tail(n->u.node_letrec.body); return; }
    if (k == &kind_node_lettuple) { ac_mark_tail(n->u.node_lettuple.body); return; }
    if (k == &kind_node_seq)      { ac_mark_tail(n->u.node_seq.rest); return; }
    // any other node form is not a tail-call host
}

void
INIT(void)
{
    GC_INIT();
    astro_cs_init("code_store", ".", 0);
}

// AOT: the top-level expression, every function body, and every
// variable-arity child are reached through a runtime dispatcher read, so
// each was collected as a code-store entry by the parser.  Bake them all,
// then re-resolve so the freshly-built SDs take effect this run.
void
ac_aot_specialize(NODE *root)
{
    (void)root;
    for (int i = 0; i < ac_n_entries; i++) astro_cs_compile(ac_entries[i], NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    for (int i = 0; i < ac_n_entries; i++) astro_cs_load(ac_entries[i], NULL);
}
