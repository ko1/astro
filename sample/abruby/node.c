#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "builtin/builtin.h"

// --- User-provided: allocation ---

size_t node_cnt = 0;

static NODE *
node_allocate(size_t size)
{
    if (size < sizeof(NODE)) size = sizeof(NODE);
    NODE *n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    memset(n, 0, size);
    node_cnt++;
    return n;
}

// --- User-provided: dispatch tracing ---

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

// --- ASTro node infrastructure (hash functions, HASH, DUMP) ---

#include "astro_node.c"

// --- Code store (SPECIALIZE, astro_cs_*) ---

#include "astro_code_store.c"

// --- General node operations ---

void
clear_hash(NODE *n)
{
    while (n) {
        n->head.flags.has_hash_value = false;
        n = n->head.parent;
    }
}

// Replace old_node with new_node in the AST tree.
// Updates the parent's child pointer and invalidates hash.
void
node_replace(NODE *old_node, NODE *new_node)
{
    NODE *parent = old_node->head.parent;
    if (parent) {
        parent->head.kind->replacer(parent, old_node, new_node);
    }
    new_node->head.parent = parent;
    old_node->head.parent = NULL;
    // Ensure new node is GC-managed
    abruby_wrap_node(new_node);
    clear_hash(parent);
}

// --- User-provided: OPTIMIZE ---

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

    return n;
}

char *
SPECIALIZED_SRC(NODE *n)
{
    if (n == NULL) return NULL;

    astro_spec_dedup_clear();

    char *buf = NULL;
    size_t len = 0;

    FILE *fp = open_memstream(&buf, &len);
    if (fp == NULL) return NULL;

    (*n->head.kind->specializer)(fp, n, true);

    if (fclose(fp) != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

void
code_repo_add(const char *name, NODE *body, bool force_add)
{
    (void)name; (void)body; (void)force_add;
}

// --- Generated code ---

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"

// GC mark helpers (used by generated node_mark.c)
static void
mark_child(NODE *child)
{
    if (child && child->head.rb_wrapper) {
        rb_gc_mark(child->head.rb_wrapper);
    }
}
#define MARK(child) mark_child(child)

#include "node_replace.c"
#include "node_mark.c"
#include "node_alloc.c"

void
abruby_node_mark(void *ptr)
{
    NODE *n = (NODE *)ptr;
    if (n->head.kind && n->head.kind->marker) {
        n->head.kind->marker(n);
    }
}

struct abruby_method *
abruby_find_method(struct abruby_class *klass, const char *name)
{
    return abruby_class_find_method(klass, name);
}

void
INIT(void)
{
    // code store is initialized from Ruby via AbRuby.cs_init
}
