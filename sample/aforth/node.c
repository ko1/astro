#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"

/* ---------- User-provided: allocation ---------- */

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        fprintf(stderr, "aforth: out of memory\n");
        exit(EXIT_FAILURE);
    }
    return n;
}

/* ---------- ASTro shared helpers ---------- */

#include "astro_node.c"
#include "astro_code_store.c"

/* ---------- code_repo: holds (name, body) for each `: word ;` definition.
 *
 * Used by main.c during AOT to walk every entry and call astro_cs_compile,
 * and by the parser to look up call targets when emitting node_call.        */

struct aforth_code_repo aforth_code_repo;

NODE *
code_repo_find(node_hash_t h)
{
    for (uint32_t i = 0; i < aforth_code_repo.size; i++) {
        if (HASH(aforth_code_repo.entries[i].body) == h) {
            return aforth_code_repo.entries[i].body;
        }
    }
    return NULL;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    if (!force) {
        for (uint32_t i = 0; i < aforth_code_repo.size; i++) {
            if (aforth_code_repo.entries[i].body == body) return;
        }
    }
    if (aforth_code_repo.size >= aforth_code_repo.capa) {
        aforth_code_repo.capa = aforth_code_repo.capa ? aforth_code_repo.capa * 2 : 16;
        aforth_code_repo.entries = realloc(aforth_code_repo.entries,
            aforth_code_repo.capa * sizeof(*aforth_code_repo.entries));
    }
    aforth_code_repo.entries[aforth_code_repo.size].name = name;
    aforth_code_repo.entries[aforth_code_repo.size].body = body;
    aforth_code_repo.size++;
}

/* ---------- User-provided: EVAL / OPTIMIZE ---------- */

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

/* ---------- Generated code (must come AFTER #include "astro_node.c") ---------- */

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

/* ---------- INIT ---------- */

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
