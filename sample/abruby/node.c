#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"
#include "builtin/builtin.h"

typedef uint64_t node_hash_t;

static node_hash_t
hash_merge(node_hash_t h, node_hash_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4);
    return h;
}

static node_hash_t
hash_cstr(const char *s)
{
    node_hash_t h = 14695981039346656037ULL;
    const node_hash_t FNV_PRIME = 1099511628211ULL;

    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= FNV_PRIME;
    }

    return h;
}

static node_hash_t
hash_uint32(uint32_t ui)
{
    node_hash_t x = (node_hash_t)ui;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;

    return x;
}

static node_hash_t
hash_node(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) {
        return n->head.hash_value;
    }
    else {
        return HASH(n);
    }
}

// specialized code repo

static struct specialized_code_repo {
    uint32_t size;
    uint32_t capa;

    struct specialized_code {
        node_hash_t hash;
        const char *dispatcher_name;
        node_dispatcher_func_t dispatcher;
    } *entries;
} sc_repo;

static struct specialized_code *
sc_repo_search(NODE *n, node_hash_t h)
{
    for (uint32_t i = 0; i < sc_repo.size; i++) {
        if (sc_repo.entries[i].hash == h) {
            return &sc_repo.entries[i];
        }
    }
    return NULL;
}

static struct specialized_code *
sc_repo_new_entry(void)
{
    if (sc_repo.size < sc_repo.capa) {
        return &sc_repo.entries[sc_repo.size++];
    }
    else {
        uint32_t capa = sc_repo.capa * 2;
        sc_repo.entries = realloc(sc_repo.entries, sizeof(struct specialized_code) * capa);

        if (sc_repo.entries) {
            sc_repo.capa = capa;
            return sc_repo_new_entry();
        }
        else {
            fprintf(stderr, "no memory for capa:%u\n", capa);
            exit(1);
        }
    }
}

static void
sc_repo_add(NODE *n, node_hash_t h)
{
    struct specialized_code *sc = sc_repo_new_entry();
    sc->hash = h;
    sc->dispatcher_name = n->head.dispatcher_name;
    sc->dispatcher = n->head.dispatcher;
}

static void
sc_repo_clear(void)
{
    sc_repo.size = 0;
}

static const char *
alloc_dispatcher_name(NODE *n)
{
    char buff[128], *name;
    snprintf(buff, 128, "SD_%lx", hash_node(n));
    name = malloc(strlen(buff) + 1);
    strcpy(name, buff);
    return name;
}

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

// allocation
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

// general node operations

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
    if (parent && parent->head.kind->replacer) {
        parent->head.kind->replacer(parent, old_node, new_node);
    }
    new_node->head.parent = parent;
    old_node->head.parent = NULL;
    // Ensure new node is GC-managed
    abruby_wrap_node(new_node);
    clear_hash(parent);
}

node_hash_t
HASH(NODE *n)
{
    if (n == NULL) {
        return 0;
    }
    else if (n->head.flags.has_hash_value) {
        return n->head.hash_value;
    }
    else if (n->head.kind->hash_func) {
        n->head.flags.has_hash_value = true;
        return n->head.hash_value = (*n->head.kind->hash_func)(n);
    }
    else {
        return 0;
    }
}

void
DUMP(FILE *fp, NODE *n, bool oneline)
{
    if (!n) {
        fprintf(fp, "<NULL>");
    }
    else if (n->head.flags.is_dumping) {
        fprintf(fp, "...");
    }
    else {
        n->head.flags.is_dumping = true;
        (*n->head.kind->dumper)(fp, n, oneline);
        n->head.flags.is_dumping = false;
    }
}

static void
fill_with_sc(NODE *n, struct specialized_code *sc)
{
    n->head.dispatcher_name = sc->dispatcher_name;
    n->head.dispatcher = sc->dispatcher;
    n->head.flags.is_specialized = true;
}

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
        return n;
    }

    node_hash_t h = hash_node(n);
    struct specialized_code *sc = sc_repo_search(n, h);
    if (sc) {
        fill_with_sc(n, sc);
    }

    return n;
}

void
SPECIALIZE(FILE *fp, NODE *n)
{
    if (n && n->head.kind->specializer) {
        node_hash_t h = HASH(n);
        struct specialized_code *sc = sc_repo_search(n, h);

        if (sc) {
            if (!n->head.flags.is_specialized) {
                fill_with_sc(n, sc);
            }
        }
        else if (n->head.flags.is_specializing) {
            // recursive
        }
        else {
            n->head.flags.is_specializing = true;
            (*n->head.kind->specializer)(fp, n, false);
            n->head.flags.is_specializing = false;
            sc_repo_add(n, h);
        }
    }
}

char *
SPECIALIZED_SRC(NODE *n)
{
    if (n == NULL) return NULL;

    sc_repo_clear();

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

#include "node_replacer.c"
#include "node_mark.c"
#include "node_alloc.c"

#include "node_specialized.c"

#ifndef NODE_SPECIALIZED_INCLUDED
static struct specialized_code sc_entries[] = {};
#endif

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
    sc_repo.size = sizeof(sc_entries) / sizeof(sc_entries[0]);
    sc_repo.capa = sc_repo.size == 0 ? 2 : sc_repo.size * 2;
    sc_repo.entries = malloc(sc_repo.capa * sizeof(struct specialized_code));
    memcpy(sc_repo.entries, sc_entries, sizeof(sc_entries));
}
