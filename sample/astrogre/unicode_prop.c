/* Unicode property lookup + the range-set algebra the character-class
 * parser needs (union / complement / intersection).  The data itself is
 * in the generated unicode_tables.c. */

#include <stdlib.h>
#include <string.h>

#include "unicode_prop.h"

/* Onigmo folds case and ignores `-` / `_` / space in property names. */
static char
prop_norm(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (char)(ch + 32);
    return ch;
}

static int
prop_name_cmp(const char *key, size_t key_len, const char *ent)
{
    size_t i = 0;
    for (;;) {
        while (i < key_len && (key[i] == '-' || key[i] == '_' || key[i] == ' ')) i++;
        const int a = i < key_len ? (unsigned char)prop_norm(key[i]) : 0;
        const int b = (unsigned char)*ent;
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;
        i++;
        ent++;
    }
}

static bool
prop_find(const agre_uni_prop_t *tab, unsigned tab_n,
          const char *name, size_t len, agre_cpset_t *out)
{
    unsigned lo = 0, hi = tab_n;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const int c = prop_name_cmp(name, len, tab[mid].name);
        if (c == 0) {
            out->r = agre_uni_pool + tab[mid].off;
            out->n = tab[mid].len;
            return true;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return false;
}

bool
agre_uni_prop_find(const char *name, size_t len, agre_cpset_t *out)
{
    return prop_find(agre_uni_props, agre_uni_props_n, name, len, out);
}

bool
agre_posix_prop_find(const char *name, size_t len, agre_cpset_t *out)
{
    return prop_find(agre_posix_props, agre_posix_props_n, name, len, out);
}

void
agre_cpsetb_free(agre_cpsetb_t *b)
{
    free(b->r);
    b->r = NULL;
    b->n = b->cap = 0;
}

static bool
cpsetb_grow(agre_cpsetb_t *b, uint32_t need)
{
    if (need <= b->cap) return true;
    uint32_t cap = b->cap ? b->cap * 2 : 16;
    while (cap < need) cap *= 2;
    agre_cprange_t *r = (agre_cprange_t *)realloc(b->r, cap * sizeof(*r));
    if (r == NULL) return false;
    b->r = r;
    b->cap = cap;
    return true;
}

/* Insert keeping the array sorted and disjoint.  Class members arrive in
 * near-sorted order (whole property tables at a time), so a linear scan
 * from the tail is effectively O(1) per property range. */
bool
agre_cpsetb_add(agre_cpsetb_t *b, uint32_t lo, uint32_t hi)
{
    if (lo > hi) return true;
    if (hi > AGRE_CP_MAX) hi = AGRE_CP_MAX;

    uint32_t i = b->n;
    while (i > 0 && b->r[i - 1].lo > lo) i--;
    /* [i-1] may now overlap/abut the new range from the left. */
    if (i > 0 && b->r[i - 1].hi + 1 >= lo) {
        i--;
        if (b->r[i].lo < lo) lo = b->r[i].lo;
        if (b->r[i].hi > hi) hi = b->r[i].hi;
    }
    uint32_t j = i;
    while (j < b->n && b->r[j].lo <= hi + 1) {
        if (b->r[j].hi > hi) hi = b->r[j].hi;
        j++;
    }
    /* Replace [i, j) with the single merged range. */
    if (j == i) {
        if (!cpsetb_grow(b, b->n + 1)) return false;
        memmove(b->r + i + 1, b->r + i, (b->n - i) * sizeof(*b->r));
        b->n++;
    } else if (j > i + 1) {
        memmove(b->r + i + 1, b->r + j, (b->n - j) * sizeof(*b->r));
        b->n -= (j - i - 1);
    }
    b->r[i].lo = lo;
    b->r[i].hi = hi;
    return true;
}

bool
agre_cpsetb_add_set(agre_cpsetb_t *b, const agre_cpset_t *s)
{
    for (uint32_t i = 0; i < s->n; i++) {
        if (!agre_cpsetb_add(b, s->r[i].lo, s->r[i].hi)) return false;
    }
    return true;
}

bool
agre_cpsetb_complement(agre_cpsetb_t *b)
{
    agre_cprange_t *out = (agre_cprange_t *)malloc((b->n + 1) * sizeof(*out));
    if (out == NULL) return false;
    uint32_t n = 0, next = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        if (b->r[i].lo > next) {
            out[n].lo = next;
            out[n].hi = b->r[i].lo - 1;
            n++;
        }
        if (b->r[i].hi >= AGRE_CP_MAX) { next = AGRE_CP_MAX; goto done; }
        next = b->r[i].hi + 1;
    }
    out[n].lo = next;
    out[n].hi = AGRE_CP_MAX;
    n++;
  done:
    free(b->r);
    b->r = out;
    b->n = n;
    b->cap = b->n ? b->n : 1;
    return true;
}

void
agre_cpsetb_intersect(agre_cpsetb_t *b, const agre_cpset_t *s)
{
    uint32_t w = 0, i = 0, j = 0;
    agre_cprange_t *out = (agre_cprange_t *)malloc(((size_t)b->n + s->n + 1) * sizeof(*out));
    if (out == NULL) { b->n = 0; return; }
    while (i < b->n && j < s->n) {
        const uint32_t lo = b->r[i].lo > s->r[j].lo ? b->r[i].lo : s->r[j].lo;
        const uint32_t hi = b->r[i].hi < s->r[j].hi ? b->r[i].hi : s->r[j].hi;
        if (lo <= hi) { out[w].lo = lo; out[w].hi = hi; w++; }
        if (b->r[i].hi < s->r[j].hi) i++; else j++;
    }
    free(b->r);
    b->r = out;
    b->n = w;
    b->cap = (uint32_t)(b->n + 1);
}

uint64_t
agre_cpset_hash(const agre_cpset_t *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < s->n; i++) {
        h = (h ^ s->r[i].lo) * 0x100000001b3ULL;
        h = (h ^ s->r[i].hi) * 0x100000001b3ULL;
    }
    /* Never 0 — the node's operand doubles as "set present". */
    return h | 1ULL;
}
