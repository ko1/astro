#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"

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
    node_hash_t h = 14695981039346656037ULL; // FNV offset basis for 64-bit
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
    for (int i=0; i<sc_repo.size; i++) {
        if (sc_repo.entries[i].hash == h) {
            // fprintf(stderr, "found:%d\n", i);
            return &sc_repo.entries[i];
        }
    }
    return NULL;
};

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
sc_repo_add(NODE *n, node_hash_t h) {
    struct specialized_code *sc = sc_repo_new_entry();
    sc->hash = h;
    sc->dispatcher_name = n->head.dispatcher_name;
    sc->dispatcher = n->head.dispatcher;

#if 0
    fprintf(stderr, "add h:%lx\n", h);
    for (uint32_t i=0; i<sc_repo.size; i++) {
        fprintf(stderr, "* %u:%s\n", i, sc_repo.entries[i].dispatcher_name);
    }
#endif
}

void
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
    return name; // TODO: need free
}

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
#endif
}

// allocation

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

// general node operation functions

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

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

void
DUMP(FILE *fp, NODE *n, bool oneline)
{
    if (!n) {
        fprintf(fp, "<NULL>");
    }
    else if (n->head.flags.is_dumping) {
        // recursive
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
        fprintf(stderr, "hit!: h:%16lx %s\n", h, n->head.kind->default_dispatcher_name);
        fill_with_sc(n, sc);
    }
    else {
        fprintf(stderr, "miss: h:%16lx %s\n", h, n->head.kind->default_dispatcher_name);
    }

    return n;
}

void
SPECIALIZE(FILE *fp, NODE *n)
{
    if (n && n->head.kind->specializer) {
        node_hash_t h = n->head.hash_value;
        struct specialized_code *sc = sc_repo_search(n, h);

        if (sc) {
            // already specialized same form tree
            if (!n->head.flags.is_specialized) {
                fill_with_sc(n, sc);
            }
        }
        else if (n->head.flags.is_specializing) {
            // recursive specializing
        }
        else {
            if (false) {
                fprintf(stderr, "START specialize:%p\n", n);
                DUMP(stderr, n, true);
                fprintf(stderr, "\n");
            }

            n->head.flags.is_specializing = true;
            (*n->head.kind->specializer)(fp, n);
            n->head.flags.is_specializing = false;

            sc_repo_add(n, h);
        }
    }
    else {
        // fprintf(stderr, "no specializer for %p\n", n);
    }
}

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_alloc.c"

#include "node_specialized.c"

#ifndef NODE_SPECIALIZED_INCLUDED
static struct specialized_code sc_entries[] = {};
#endif

void
INIT(void)
{
    sc_repo.size = sizeof(sc_entries) / sizeof(sc_entries[0]);
    sc_repo.capa = sc_repo.size == 0 ? 2 : sc_repo.size * 2;
    sc_repo.entries = malloc(sc_repo.capa * sizeof(struct specialized_code));
    memcpy(sc_repo.entries, sc_entries, sizeof(sc_entries));
}
