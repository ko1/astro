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

// --- User-provided: dispatch tracing ---

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
#if DEBUG_EVAL
    if (end) {
        c->rec_cnt--;
    }
    else {
        for (int i=0; i<c->rec_cnt; i++) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, "%s\n", n->head.dispatcher_name);
        c->rec_cnt++;
    }
#else
    (void)c; (void)n; (void)end;
#endif
}

// --- Code store (hash functions, HASH, DUMP, SPECIALIZE, sc_repo, astro_cs_*) ---

#include "astro_code_store.c"

// --- User-provided: EVAL, OPTIMIZE ---

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
        return n;
    }

    if (astro_cs_load(n)) {
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

// --- code_repo: named AST entries (language-specific) ---

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
    astro_cs_init("code_store", ".");
}
