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

// Install a per-child dispatch NULL-check for debug builds, driven by the
// EVAL_ARG_CHECK hook that node_eval.c exposes (no-op by default).  The hook
// must expand to an expression because node_eval.c wires it into a
// comma-expression (EVAL_ARG_CHECK(n), (*disp)(c, n)); a do/while block
// wouldn't compile there.
#if ABRUBY_DEBUG
static void abruby_debug_null_dispatch(NODE *n, const char *caller);
#define EVAL_ARG_CHECK(n) \
    (UNLIKELY((n)->head.dispatcher == NULL) \
        ? (abruby_debug_null_dispatch((n), __func__), (void)0) \
        : (void)0)

static void
abruby_debug_null_dispatch(NODE *n, const char *caller)
{
    const char *kname = (n->head.kind && n->head.kind->default_dispatcher_name)
        ? n->head.kind->default_dispatcher_name : "<unknown>";
    fprintf(stderr, "\nABRUBY_BUG: NULL child dispatcher via EVAL_ARG in %s\n", caller);
    fprintf(stderr, "  node=%p kind=%s line=%d hash=%lx is_specialized=%d\n",
            (void*)n, kname, n->head.line,
            (unsigned long)(n->head.flags.has_hash_value ? n->head.hash_value : HASH(n)),
            n->head.flags.is_specialized);
    fprintf(stderr, "  ---- failing node ----\n  ");
    DUMP(stderr, n, true);
    fprintf(stderr, "\n");
    NODE *root = abruby_debug_enclosing_entry(n);
    if (root && root != n) {
        const char *rname = (root->head.kind && root->head.kind->default_dispatcher_name)
            ? root->head.kind->default_dispatcher_name : "<unknown>";
        fprintf(stderr, "  ---- enclosing entry (%s, line=%d) ----\n  ",
                rname, root->head.line);
        DUMP(stderr, root, true);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    rb_bug("ABRUBY: NULL child dispatcher (kind=%s, via %s)", kname, caller);
}
#endif

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
abruby_find_method(const struct abruby_class *klass, ID name)
{
    return abruby_class_find_method(klass, name);
}

void
INIT(void)
{
    // code store is initialized from Ruby via AbRuby.cs_init
}
