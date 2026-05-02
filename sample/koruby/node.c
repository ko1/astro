#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "context.h"
#include "object.h"
#include "node.h"

/* Hash helpers + HASH / DUMP / hash_node / alloc_dispatcher_name come
 * from runtime/astro_node.c.  We forward-declare the host hooks the
 * shared runtime expects, then include it. */
static __attribute__((noinline)) NODE *node_allocate(size_t size);
static void dispatch_info(CTX *c, NODE *n, bool end);

#include "../../runtime/astro_node.c"

void clear_hash(NODE *n) {
    while (n) {
        n->head.flags.has_hash_value = false;
        n = n->head.parent;
    }
}

/* node allocation via Boehm GC.  Forward-declared above the
 * runtime/astro_node.c include so the runtime helpers can reach it. */
extern size_t korb_node_cnt;
size_t korb_node_cnt = 0;

static __attribute__((noinline)) NODE *
node_allocate(size_t size) {
    NODE *n = (NODE *)korb_xmalloc(size);
    if (!n) { perror("node_allocate"); exit(1); }
    korb_node_cnt++;
    return n;
}

/* code repo */
struct code_repo {
    uint32_t size, capa;
    struct code_entry {
        const char *name;
        NODE *body;
    } *entries;
};
struct code_repo code_repo;

NODE *code_repo_find(node_hash_t h) {
    if (!h) return NULL;
    for (uint32_t i = 0; i < code_repo.size; i++) {
        if (HASH(code_repo.entries[i].body) == h) return code_repo.entries[i].body;
    }
    return NULL;
}

void code_repo_add(const char *name, NODE *body, bool force) {
    if (!body) return;
    if (!force && code_repo_find(HASH(body))) return;
    if (code_repo.size >= code_repo.capa) {
        code_repo.capa = code_repo.capa ? code_repo.capa * 2 : 8;
        code_repo.entries = korb_xrealloc(code_repo.entries, code_repo.capa * sizeof(*code_repo.entries));
    }
    code_repo.entries[code_repo.size].name = name;
    code_repo.entries[code_repo.size].body = body;
    code_repo.size++;
}

/* specialized code repo */
struct specialized_code {
    node_hash_t hash;
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;
};

static struct sc_repo {
    uint32_t size, capa;
    struct specialized_code *entries;
} sc_repo;

static struct specialized_code *sc_repo_search(NODE *n, node_hash_t h) {
    for (uint32_t i = 0; i < sc_repo.size; i++) {
        if (sc_repo.entries[i].hash == h) return &sc_repo.entries[i];
    }
    return NULL;
}

static struct specialized_code *sc_repo_new_entry(void) {
    if (sc_repo.size < sc_repo.capa) return &sc_repo.entries[sc_repo.size++];
    sc_repo.capa = sc_repo.capa ? sc_repo.capa * 2 : 8;
    sc_repo.entries = korb_xrealloc(sc_repo.entries, sc_repo.capa * sizeof(*sc_repo.entries));
    return sc_repo_new_entry();
}

static void sc_repo_add(NODE *n, node_hash_t h) {
    struct specialized_code *sc = sc_repo_new_entry();
    sc->hash = h;
    sc->dispatcher_name = n->head.dispatcher_name;
    sc->dispatcher = n->head.dispatcher;
}

void sc_repo_clear(void) { sc_repo.size = 0; }

/* alloc_dispatcher_name, astro_fprintf_cstr, astro_fprint_cstr come
 * from runtime/astro_node.c. */

static void dispatch_info(CTX *c, NODE *n, bool end) { (void)c; (void)n; (void)end; }

/* OPTIMIZE / SPECIALIZE */

static void fill_with_sc(NODE *n, struct specialized_code *sc) {
    n->head.dispatcher_name = sc->dispatcher_name;
    n->head.dispatcher = sc->dispatcher;
    n->head.flags.is_specialized = true;
}

/* Code store: AOT lookup goes through the shared runtime/.  Pulled in
 * after astro_node.c so the runtime can use hash_merge / hash_node /
 * alloc_dispatcher_name etc. that astro_node.c provides. */
#include "../../runtime/astro_code_store.h"

NODE *OPTIMIZE(NODE *n) {
    if (!n) return n;
    if (OPTION.no_compiled_code) return n;
    /* First try the runtime code store (dlopen'd all.so).  AOT-only —
     * pass file=NULL so PGC lookup is skipped. */
    if (astro_cs_load(n, NULL)) return n;
    node_hash_t h = hash_node(n);
    struct specialized_code *sc = sc_repo_search(n, h);
    if (sc) fill_with_sc(n, sc);
    return n;
}

/* SPECIALIZE comes from runtime/astro_code_store.c (included below). */

void node_replace(NODE *parent, NODE *old, NODE *new_node) {
    if (!parent || !parent->head.kind->replacer) return;
    parent->head.kind->replacer(parent, old, new_node);
    clear_hash(parent);
    if (new_node) new_node->head.parent = parent;
}

void korb_swap_dispatcher(NODE *n, const struct NodeKind *new_kind) {
    n->head.kind = new_kind;
    n->head.dispatcher = new_kind->default_dispatcher;
    n->head.dispatcher_name = new_kind->default_dispatcher_name;
    n->head.flags.has_hash_value = false;
    if (n->head.parent) clear_hash(n->head.parent);
}

/* include generated files */
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

/* Pulled in last — uses HASH/HORG/HOPT and the static helpers from
 * astro_node.c above. */
#include "../../runtime/astro_code_store.c"

/* try to include specialized */
#if __has_include("node_specialized.c")
#include "node_specialized.c"
#endif

#ifndef NODE_SPECIALIZED_INCLUDED
static struct specialized_code sc_entries[] = {{0}};
static uint32_t sc_entries_count = 0;
#define SC_ENTRIES_COUNT 0
#else
#define SC_ENTRIES_COUNT (sizeof(sc_entries)/sizeof(sc_entries[0]))
#endif

void INIT(void) {
    sc_repo.size = SC_ENTRIES_COUNT;
    sc_repo.capa = sc_repo.size == 0 ? 4 : sc_repo.size * 2;
    sc_repo.entries = korb_xmalloc(sc_repo.capa * sizeof(struct specialized_code));
    if (sc_repo.size > 0) memcpy(sc_repo.entries, sc_entries, sc_repo.size * sizeof(struct specialized_code));
}
