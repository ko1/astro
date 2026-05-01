#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "node.h"
#include "context.h"

// =====================================================================
// Hash helpers used by node_hash.c
// =====================================================================
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
    while (*s) { h ^= (unsigned char)(*s++); h *= FNV_PRIME; }
    return h;
}
static node_hash_t
hash_uint32(uint32_t ui)
{
    node_hash_t x = (node_hash_t)ui;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
static node_hash_t hash_uint64(uint64_t ui) { return hash_uint32((uint32_t)ui) ^ hash_uint32((uint32_t)(ui >> 32)); }
static node_hash_t hash_double(double d) {
    union { double d; uint64_t u; } t; t.d = d; return hash_uint64(t.u);
}

static node_hash_t
hash_node(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) return n->head.hash_value;
    return HASH(n);
}

// Specialized-code repository (currently unused since we run interpreter only).
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
    (void)n;
    for (uint32_t i = 0; i < sc_repo.size; i++) {
        if (sc_repo.entries[i].hash == h) return &sc_repo.entries[i];
    }
    return NULL;
}
void sc_repo_clear(void) { sc_repo.size = 0; }

static const char *
alloc_dispatcher_name(NODE *n)
{
    char buff[128], *name;
    snprintf(buff, 128, "SD_%lx", (unsigned long)hash_node(n));
    name = malloc(strlen(buff) + 1);
    strcpy(name, buff);
    return name;
}

extern size_t node_cnt;
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)malloc(size);
    if (!n) { fprintf(stderr, "OOM\n"); exit(1); }
    node_cnt++;
    return n;
}

void
clear_hash(NODE *n)
{
    while (n) { n->head.flags.has_hash_value = false; n = n->head.parent; }
}

node_hash_t
HASH(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) return n->head.hash_value;
    if (n->head.kind->hash_func) {
        n->head.flags.has_hash_value = true;
        return n->head.hash_value = (*n->head.kind->hash_func)(n);
    }
    return 0;
}

void
DUMP(FILE *fp, NODE *n, bool oneline)
{
    if (!n) { fprintf(fp, "<NULL>"); return; }
    if (n->head.flags.is_dumping) { fprintf(fp, "..."); return; }
    n->head.flags.is_dumping = true;
    (*n->head.kind->dumper)(fp, n, oneline);
    n->head.flags.is_dumping = false;
}

NODE *
OPTIMIZE(NODE *n)
{
    return n;  // interpreter mode for now
}

void
SPECIALIZE(FILE *fp, NODE *n)
{
    (void)fp; (void)n;  // not used yet
}

// Minimal code repository (unused for now).
static struct code_repo {
    uint32_t size; uint32_t capa;
    struct { const char *name; NODE *body; } *entries;
} code_repo;

NODE *code_repo_find(node_hash_t h) { (void)h; return NULL; }
NODE *code_repo_find_by_name(const char *name) { (void)name; return NULL; }
void  code_repo_add(const char *name, NODE *body, bool force_add) { (void)name; (void)body; (void)force_add; }

size_t node_cnt = 0;

// EVAL function — kept for callers that take the function-pointer form.
// Hot internal callers use the EVAL macro from node.h.
#undef EVAL
RESULT
EVAL_func(CTX *c, NODE *n, JsValue *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}
#define EVAL(c, n, frame) ((*(n)->head.dispatcher)((c), (n), (frame)))

// Forward decls used by generated node_dump.c.
void astro_fprintf_cstr(FILE *fp, const char *s);
void astro_fprint_cstr(FILE *fp, const char *s);

// Regex engine — included here so it sees JsRegex etc.
#include "js_regex.c"

// =====================================================================
// Generated files
// =====================================================================
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

void
INIT(void)
{
    sc_repo.size = 0;
    sc_repo.capa = 4;
    sc_repo.entries = malloc(sizeof(*sc_repo.entries) * sc_repo.capa);
    code_repo.size = 0;
    code_repo.capa = 0;
    code_repo.entries = NULL;
}

// Helper used by node_dump.c for const char * values containing arbitrary bytes.
#ifndef ASTRO_FPRINTF_CSTR_DEFINED
#define ASTRO_FPRINTF_CSTR_DEFINED
void astro_fprintf_cstr(FILE *fp, const char *s)
{
    if (!s) { fprintf(fp, "(null)"); return; }
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"') fputs("\\\"", fp);
        else if (*s == '\\') fputs("\\\\", fp);
        else if (*s == '\n') fputs("\\n", fp);
        else fputc(*s, fp);
    }
    fputc('"', fp);
}
void astro_fprint_cstr(FILE *fp, const char *s)
{
    astro_fprintf_cstr(fp, s);
}
#endif
