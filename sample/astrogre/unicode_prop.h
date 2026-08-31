#ifndef ASTROGRE_UNICODE_PROP_H
#define ASTROGRE_UNICODE_PROP_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Inclusive codepoint range.  Tables are sorted by `lo` and never overlap,
 * so membership is a binary search. */
typedef struct {
    uint32_t lo, hi;
} agre_cprange_t;

/* One named property: a slice [off, off+len) of agre_uni_pool. */
typedef struct {
    const char *name;
    uint32_t off, len;
} agre_uni_prop_t;

/* unicode_tables.c (generated) */
extern const agre_cprange_t agre_uni_pool[];
extern const unsigned agre_uni_pool_n;
extern const agre_uni_prop_t agre_uni_props[];
extern const unsigned agre_uni_props_n;
extern const agre_uni_prop_t agre_posix_props[];
extern const unsigned agre_posix_props_n;

#define AGRE_CP_MAX 0x10FFFFu

/* Immutable set handed to node_re_uniclass.  `r` points either into
 * agre_uni_pool or at a parse-time-built array owned by the pattern. */
typedef struct agre_cpset {
    const agre_cprange_t *r;
    uint32_t n;
} agre_cpset_t;

/* Growable range set used while parsing a character class. */
typedef struct {
    agre_cprange_t *r;
    uint32_t n, cap;
} agre_cpsetb_t;

/* `\p{NAME}` / `[[:name:]]` lookup.  `name` need not be NUL-terminated.
 * Property names are matched case-insensitively ignoring `-`, `_` and
 * spaces, as Onigmo does.  Returns false for an unknown name. */
bool agre_uni_prop_find(const char *name, size_t len, agre_cpset_t *out);
bool agre_posix_prop_find(const char *name, size_t len, agre_cpset_t *out);

/* Inline so a code-store SD gets its own copy: libastrogre.so's version
 * script exports only astrogre_*, so an out-of-line callee would not
 * resolve from the dlopen'd all.so. */
static inline bool
agre_cpset_contains(const agre_cpset_t *restrict s, uint32_t cp)
{
    if (s == NULL) return false;
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (cp < s->r[mid].lo)      hi = mid;
        else if (cp > s->r[mid].hi) lo = mid + 1;
        else                        return true;
    }
    return false;
}

/* Builder.  All of these keep the set sorted and disjoint. */
void agre_cpsetb_free(agre_cpsetb_t *b);
bool agre_cpsetb_add(agre_cpsetb_t *b, uint32_t lo, uint32_t hi);
bool agre_cpsetb_add_set(agre_cpsetb_t *b, const agre_cpset_t *s);
/* Complement over [0, AGRE_CP_MAX]. */
bool agre_cpsetb_complement(agre_cpsetb_t *b);
/* b &= s */
void agre_cpsetb_intersect(agre_cpsetb_t *b, const agre_cpset_t *s);
/* Content hash — feeds the node's structural hash so two uniclass nodes
 * with different sets never collide in the code store. */
uint64_t agre_cpset_hash(const agre_cpset_t *s);

/* Decode one UTF-8 codepoint.  Returns its byte length, or 0 for a
 * truncated / malformed sequence. */
static inline int
agre_utf8_decode(const uint8_t *p, size_t avail, uint32_t *out)
{
    if (avail == 0) return 0;
    const uint8_t b = p[0];
    if (b < 0x80) { *out = b; return 1; }
    uint32_t cp;
    int len;
    if      ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; len = 2; }
    else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; len = 3; }
    else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; len = 4; }
    else return 0;
    if (avail < (size_t)len) return 0;
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
    }
    *out = cp;
    return len;
}

#endif /* ASTROGRE_UNICODE_PROP_H */
