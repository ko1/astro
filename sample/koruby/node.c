#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "context.h"
#include "object.h"
#include "node.h"

/* hash helpers */
static node_hash_t hash_merge(node_hash_t h, node_hash_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4);
    return h;
}

static node_hash_t hash_cstr(const char *s) {
    if (!s) return 0;
    node_hash_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)(*s++); h *= 1099511628211ULL; }
    return h;
}

static node_hash_t hash_uint32(uint32_t ui) {
    node_hash_t x = (node_hash_t)ui;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static node_hash_t hash_uint64(uint64_t v) {
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v;
}

static node_hash_t hash_double(double d) {
    union { double d; uint64_t u; } u; u.d = d;
    return hash_uint64(u.u);
}

static node_hash_t hash_node(NODE *n) {
    if (!n) return 0;
    if (n->head.flags.has_hash_value) return n->head.hash_value;
    return HASH(n);
}

void clear_hash(NODE *n) {
    while (n) {
        n->head.flags.has_hash_value = false;
        n = n->head.parent;
    }
}

node_hash_t HASH(NODE *n) {
    if (!n) return 0;
    if (n->head.flags.has_hash_value) return n->head.hash_value;
    if (n->head.kind->hash_func) {
        n->head.flags.has_hash_value = true;
        return n->head.hash_value = (*n->head.kind->hash_func)(n);
    }
    return 0;
}

void DUMP(FILE *fp, NODE *n, bool oneline) {
    if (!n) { fprintf(fp, "<NULL>"); return; }
    if (n->head.flags.is_dumping) { fprintf(fp, "..."); return; }
    n->head.flags.is_dumping = true;
    (*n->head.kind->dumper)(fp, n, oneline);
    n->head.flags.is_dumping = false;
}

/* node allocation via Boehm GC */
extern size_t ko_node_cnt;
size_t ko_node_cnt = 0;

static __attribute__((noinline)) NODE *
node_allocate(size_t size) {
    NODE *n = (NODE *)ko_xmalloc(size);
    if (!n) { perror("node_allocate"); exit(1); }
    ko_node_cnt++;
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
        code_repo.entries = ko_xrealloc(code_repo.entries, code_repo.capa * sizeof(*code_repo.entries));
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
    sc_repo.entries = ko_xrealloc(sc_repo.entries, sc_repo.capa * sizeof(*sc_repo.entries));
    return sc_repo_new_entry();
}

static void sc_repo_add(NODE *n, node_hash_t h) {
    struct specialized_code *sc = sc_repo_new_entry();
    sc->hash = h;
    sc->dispatcher_name = n->head.dispatcher_name;
    sc->dispatcher = n->head.dispatcher;
}

void sc_repo_clear(void) { sc_repo.size = 0; }

static const char *alloc_dispatcher_name(NODE *n) {
    char buf[128];
    snprintf(buf, sizeof(buf), "SD_%lx", (unsigned long)hash_node(n));
    char *s = ko_xmalloc_atomic(strlen(buf)+1);
    strcpy(s, buf);
    return s;
}

static void dispatch_info(CTX *c, NODE *n, bool end) { (void)c; (void)n; (void)end; }

static void astro_fprintf_cstr(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:
                if ((unsigned char)*s < 32) fprintf(fp, "\\x%02x", (unsigned char)*s);
                else fputc(*s, fp);
        }
    }
    fputc('"', fp);
}

static void astro_fprint_cstr(FILE *fp, const char *s) {
    fprintf(fp, "        ");
    astro_fprintf_cstr(fp, s);
}

/* OPTIMIZE / SPECIALIZE */

static void fill_with_sc(NODE *n, struct specialized_code *sc) {
    n->head.dispatcher_name = sc->dispatcher_name;
    n->head.dispatcher = sc->dispatcher;
    n->head.flags.is_specialized = true;
}

NODE *OPTIMIZE(NODE *n) {
    if (!n) return n;
    if (OPTION.no_compiled_code) return n;
    node_hash_t h = hash_node(n);
    struct specialized_code *sc = sc_repo_search(n, h);
    if (sc) fill_with_sc(n, sc);
    return n;
}

void SPECIALIZE(FILE *fp, NODE *n) {
    if (!n || !n->head.kind->specializer) return;
    node_hash_t h = HASH(n);
    struct specialized_code *sc = sc_repo_search(n, h);
    if (sc) {
        if (!n->head.flags.is_specialized) fill_with_sc(n, sc);
        return;
    }
    if (n->head.flags.is_specializing) return;
    n->head.flags.is_specializing = true;
    (*n->head.kind->specializer)(fp, n, false);
    n->head.flags.is_specializing = false;
    sc_repo_add(n, h);
}

void node_replace(NODE *parent, NODE *old, NODE *new_node) {
    if (!parent || !parent->head.kind->replacer) return;
    parent->head.kind->replacer(parent, old, new_node);
    clear_hash(parent);
    if (new_node) new_node->head.parent = parent;
}

void ko_swap_dispatcher(NODE *n, const struct NodeKind *new_kind) {
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
    sc_repo.entries = ko_xmalloc(sc_repo.capa * sizeof(struct specialized_code));
    if (sc_repo.size > 0) memcpy(sc_repo.entries, sc_entries, sc_repo.size * sizeof(struct specialized_code));
}
