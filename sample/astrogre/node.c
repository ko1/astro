#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"

/* --- Hash helpers (used by generated node_hash.c) --- */

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
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static node_hash_t
hash_uint64(uint64_t u)
{
    node_hash_t x = (node_hash_t)u;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static node_hash_t
hash_node(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) return n->head.hash_value;
    return HASH(n);
}

/* --- Allocation --- */

size_t astrogre_node_cnt;

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        fprintf(stderr, "astrogre: out of memory\n");
        exit(EXIT_FAILURE);
    }
    astrogre_node_cnt++;
    return n;
}

/* --- General node operations --- */

node_hash_t
HASH(NODE *n)
{
    if (n == NULL) return 0;
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

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *
OPTIMIZE(NODE *n)
{
    return n;
}

/* SPECIALIZE / dispatcher-name hooks (unused for now: we only ship the
 * plain interpreter mode for v1). */

void
SPECIALIZE(FILE *fp, NODE *n)
{
    (void)fp; (void)n;
}

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

static const char *
alloc_dispatcher_name(NODE *n)
{
    char buff[128], *name;
    snprintf(buff, sizeof(buff), "SD_%lx", (unsigned long)hash_node(n));
    name = malloc(strlen(buff) + 1);
    strcpy(name, buff);
    return name;
}

static void
astro_fprintf_cstr(FILE *fp, const char *s)
{
    if (s == NULL) { fputs("\"\"", fp); return; }
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '\\': fputs("\\\\", fp); break;
        case '"':  fputs("\\\"", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (*p < 0x20 || *p == 0x7f) fprintf(fp, "\\x%02x", *p);
            else                          fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

/* Used by SPECIALIZE_* (we don't generate code, but the function is
 * referenced).  Same shape as astro_node.c's astro_fprint_cstr. */
__attribute__((unused)) static void
astro_fprint_cstr(FILE *fp, const char *s)
{
    fprintf(fp, "        \"");
    for (; *s; s++) {
        switch (*s) {
        case '"':  fprintf(fp, "\\\""); break;
        case '\\': fprintf(fp, "\\\\"); break;
        case '\n': fprintf(fp, "\\n"); break;
        case '\r': fprintf(fp, "\\r"); break;
        case '\t': fprintf(fp, "\\t"); break;
        default:   fputc(*s, fp);
        }
    }
    fprintf(fp, "\"");
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

/* --- Generated code --- */
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
    /* nothing for now */
}
