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
        fprintf(stderr, "astocaml: node allocation failed\n");
        exit(1);
    }
    memset(n, 0, size);
    return n;
}

#include "astro_node.c"
#include "astro_code_store.c"

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
#include "node_alloc.c"

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}

// Helpers for the tail-call post-pass in main.c.  Defined here because
// the static DISPATCH_node_* function pointers are only visible in this
// translation unit.
bool
oc_node_is_app(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    return d == DISPATCH_node_app0 || d == DISPATCH_node_app1 ||
           d == DISPATCH_node_app2 || d == DISPATCH_node_app3 ||
           d == DISPATCH_node_app4;
}

void
oc_node_to_tail(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    if      (d == DISPATCH_node_app0) n->head.dispatcher = DISPATCH_node_tail_app0;
    else if (d == DISPATCH_node_app1) n->head.dispatcher = DISPATCH_node_tail_app1;
    else if (d == DISPATCH_node_app2) n->head.dispatcher = DISPATCH_node_tail_app2;
    else if (d == DISPATCH_node_app3) n->head.dispatcher = DISPATCH_node_tail_app3;
    else if (d == DISPATCH_node_app4) n->head.dispatcher = DISPATCH_node_tail_app4;
}

// Swap a binary-op node to its `_int` specialization in place.
// All `*_int` variants share `{NODE *a, NODE *b}` struct layout
// with their generic counterparts (compiler enforces this since
// they're declared with the same operands).  Returns true if a
// known mapping fired.
bool
oc_node_to_int(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    if (d == DISPATCH_node_add) { n->head.dispatcher = DISPATCH_node_add_int; n->head.kind = &kind_node_add_int; return true; }
    if (d == DISPATCH_node_sub) { n->head.dispatcher = DISPATCH_node_sub_int; n->head.kind = &kind_node_sub_int; return true; }
    if (d == DISPATCH_node_mul) { n->head.dispatcher = DISPATCH_node_mul_int; n->head.kind = &kind_node_mul_int; return true; }
    if (d == DISPATCH_node_div) { n->head.dispatcher = DISPATCH_node_div_int; n->head.kind = &kind_node_div_int; return true; }
    if (d == DISPATCH_node_mod) { n->head.dispatcher = DISPATCH_node_mod_int; n->head.kind = &kind_node_mod_int; return true; }
    if (d == DISPATCH_node_lt)  { n->head.dispatcher = DISPATCH_node_lt_int;  n->head.kind = &kind_node_lt_int;  return true; }
    if (d == DISPATCH_node_le)  { n->head.dispatcher = DISPATCH_node_le_int;  n->head.kind = &kind_node_le_int;  return true; }
    if (d == DISPATCH_node_gt)  { n->head.dispatcher = DISPATCH_node_gt_int;  n->head.kind = &kind_node_gt_int;  return true; }
    if (d == DISPATCH_node_ge)  { n->head.dispatcher = DISPATCH_node_ge_int;  n->head.kind = &kind_node_ge_int;  return true; }
    if (d == DISPATCH_node_eq)  { n->head.dispatcher = DISPATCH_node_eq_int;  n->head.kind = &kind_node_eq_int;  return true; }
    if (d == DISPATCH_node_ne)  { n->head.dispatcher = DISPATCH_node_ne_int;  n->head.kind = &kind_node_ne_int;  return true; }
    return false;
}

// Swap unary `-` to its int specialization.
bool
oc_node_to_neg_int(NODE *n)
{
    if (n->head.dispatcher != DISPATCH_node_neg) return false;
    n->head.dispatcher = DISPATCH_node_neg_int;
    n->head.kind       = &kind_node_neg_int;
    return true;
}

// Swap `if` to its bool-cond specialization.
bool
oc_node_to_if_bool(NODE *n)
{
    if (n->head.dispatcher != DISPATCH_node_if) return false;
    n->head.dispatcher = DISPATCH_node_if_bool;
    n->head.kind       = &kind_node_if_bool;
    return true;
}

// Swap `&&` / `||` to bool-operand specializations.
bool
oc_node_to_logic_bool(NODE *n)
{
    node_dispatcher_func_t d = n->head.dispatcher;
    if (d == DISPATCH_node_and) { n->head.dispatcher = DISPATCH_node_and_bool; n->head.kind = &kind_node_and_bool; return true; }
    if (d == DISPATCH_node_or)  { n->head.dispatcher = DISPATCH_node_or_bool;  n->head.kind = &kind_node_or_bool;  return true; }
    return false;
}
