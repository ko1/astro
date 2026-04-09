#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "builtin/builtin.h"

// --- Interned IDs for operator methods and common names ---

ID id_op_plus, id_op_minus, id_op_mul, id_op_div;
ID id_op_lt, id_op_le, id_op_gt, id_op_ge;
ID id_op_eq, id_op_mod;
ID id_method_missing, id_initialize;

void
init_interned_ids(void)
{
    id_op_plus = rb_intern("+"); id_op_minus = rb_intern("-");
    id_op_mul = rb_intern("*");  id_op_div = rb_intern("/");
    id_op_lt = rb_intern("<");   id_op_le = rb_intern("<=");
    id_op_gt = rb_intern(">");   id_op_ge = rb_intern(">=");
    id_op_eq = rb_intern("==");  id_op_mod = rb_intern("%");
    id_method_missing = rb_intern("method_missing");
    id_initialize = rb_intern("initialize");
}

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

// --- User-provided: OPTIMIZE ---

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
        return n;
    }

    if (astro_cs_load(n)) {
        if (OPTION.verbose) {
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
abruby_find_method(struct abruby_class *klass, ID name)
{
    return abruby_class_find_method(klass, name);
}

void
INIT(void)
{
    // code store is initialized from Ruby via AbRuby.cs_init
}
