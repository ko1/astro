#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"
#include "astro_code_store.h"

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "asml: node allocation failed\n");
        exit(1);
    }
    memset(n, 0, size);
    return n;
}

#include "astro_node.c"
#include "astro_code_store.c"

VALUE
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

// Helper used by main.c's tail-call post-pass: rewrite an app dispatcher
// in place to its tail-call variant.  Only app1/app2 have tail variants
// in this sample; others fall back to plain ml_apply tail recursion.
bool
ml_node_to_tail(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    if (d == DISPATCH_node_app1) { n->head.dispatcher = DISPATCH_node_tail_app1; n->head.kind = &kind_node_tail_app1; return true; }
    if (d == DISPATCH_node_app2) { n->head.dispatcher = DISPATCH_node_tail_app2; n->head.kind = &kind_node_tail_app2; return true; }
    return false;
}

bool
ml_node_is_app(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    return d == DISPATCH_node_app0 ||
           d == DISPATCH_node_app1 ||
           d == DISPATCH_node_app2 ||
           d == DISPATCH_node_app3 ||
           d == DISPATCH_node_appn;
}
