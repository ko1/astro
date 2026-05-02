#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"

// --- User-provided: allocation ---

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    return n;
}

// --- ASTro infrastructure (HASH, DUMP) ---

#include "astro_node.c"

// --- Code store ---

#include "astro_code_store.c"

// Module-global tables `WASTRO_FUNCS`, `WASTRO_FUNC_CNT`,
// `WASTRO_CALL_ARGS`, `WASTRO_CALL_ARGS_CNT` are defined in main.c
// (the parser owns them).  node.def references them via the externs
// declared in node.h.

// --- User-provided: OPTIMIZE (EVAL is `static inline` in node.h) ---

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
        return n;
    }

    if (astro_cs_load(n, NULL)) {
        if (!OPTION.quiet) {
            fprintf(stderr, "hit!: h:%16lx %s ",
                    (unsigned long)hash_node(n),
                    n->head.kind->default_dispatcher_name);
            DUMP(stderr, n, true);
            fprintf(stderr, "\n");
        }
    }
    else {
        if (!OPTION.quiet) {
            fprintf(stderr, "miss: h:%16lx %s ",
                    (unsigned long)hash_node(n),
                    n->head.kind->default_dispatcher_name);
            DUMP(stderr, n, true);
            fprintf(stderr, "\n");
        }
    }

    return n;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

// --- Generated code ---

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// --- INIT ---

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
