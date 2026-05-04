#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"
#include "astro_code_store.h"

// Allocate NODEs out of the GC heap so any VALUE references they hold
// (string literals etc.) stay alive without explicit rooting.
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)GC_malloc(size);
    if (n == NULL) {
        fprintf(stderr, "pystro: node allocation failed\n");
        exit(1);
    }
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

// Variadic-call arg table, populated by the parser.  Keeping it in a
// flat array (rather than per-call malloced lists) lets node_call_n
// fold an `args_idx` literal into a direct PYSTRO_CALL_ARGS[base + i]
// load in the SD function.
NODE **PYSTRO_CALL_ARGS = NULL;
size_t PYSTRO_CALL_ARGS_LEN = 0;
size_t PYSTRO_CALL_ARGS_CAP = 0;

size_t
pystro_call_args_reserve(NODE **args, size_t n)
{
    if (PYSTRO_CALL_ARGS_LEN + n > PYSTRO_CALL_ARGS_CAP) {
        size_t cap = PYSTRO_CALL_ARGS_CAP ? PYSTRO_CALL_ARGS_CAP * 2 : 64;
        while (cap < PYSTRO_CALL_ARGS_LEN + n) cap *= 2;
        PYSTRO_CALL_ARGS = (NODE **)GC_realloc(PYSTRO_CALL_ARGS, cap * sizeof(NODE *));
        PYSTRO_CALL_ARGS_CAP = cap;
    }
    size_t base = PYSTRO_CALL_ARGS_LEN;
    for (size_t i = 0; i < n; i++) PYSTRO_CALL_ARGS[base + i] = args[i];
    PYSTRO_CALL_ARGS_LEN += n;
    return base;
}

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
