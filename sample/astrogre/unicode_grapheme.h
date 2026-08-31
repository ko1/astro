#ifndef ASTROGRE_UNICODE_GRAPHEME_H
#define ASTROGRE_UNICODE_GRAPHEME_H 1

/* Extended grapheme cluster segmentation (UAX #29 GB1..GB999) — the
 * `\X` matcher.  Inline for the same reason as agre_cpset_contains: a
 * code-store SD is a standalone .so and must carry its own copy. */

#include "unicode_gcb.h"

/* Byte offset just past the extended grapheme cluster starting at `pos`.
 * Always advances at least one byte (a malformed byte is its own
 * cluster), so callers can't loop forever. */
static inline size_t
agre_grapheme_end(const uint8_t *restrict s, size_t len, size_t pos)
{
    uint32_t cp;
    int n = agre_utf8_decode(s + pos, len - pos, &cp);
    if (n == 0) return pos + 1;
    pos += (size_t)n;

    int prev = agre_gcb_class(cp);
    /* GB11 needs "ExtPict Extend* ZWJ ×": remember whether the run so far
     * started with an Extended_Pictographic. */
    bool pict = (prev & AGRE_GCB_EXTPICT) != 0;
    /* GB12/GB13: regional indicators only pair up, so join on odd counts. */
    unsigned ri = (AGRE_GCB_CLASS(prev) == AGRE_GCB_RI);

    while (pos < len) {
        n = agre_utf8_decode(s + pos, len - pos, &cp);
        if (n == 0) break;
        const int cur = agre_gcb_class(cp);
        const int a = AGRE_GCB_CLASS(prev), b = AGRE_GCB_CLASS(cur);
        bool join;
        if (a == AGRE_GCB_CR && b == AGRE_GCB_LF) join = true;             /* GB3 */
        else if (a == AGRE_GCB_CR || a == AGRE_GCB_LF || a == AGRE_GCB_CONTROL ||
                 b == AGRE_GCB_CR || b == AGRE_GCB_LF || b == AGRE_GCB_CONTROL)
            join = false;                                                  /* GB4/GB5 */
        else if (a == AGRE_GCB_L && (b == AGRE_GCB_L || b == AGRE_GCB_V ||
                                     b == AGRE_GCB_LV || b == AGRE_GCB_LVT))
            join = true;                                                   /* GB6 */
        else if ((a == AGRE_GCB_LV || a == AGRE_GCB_V) && (b == AGRE_GCB_V || b == AGRE_GCB_T))
            join = true;                                                   /* GB7 */
        else if ((a == AGRE_GCB_LVT || a == AGRE_GCB_T) && b == AGRE_GCB_T)
            join = true;                                                   /* GB8 */
        else if (b == AGRE_GCB_EXTEND || b == AGRE_GCB_ZWJ) join = true;   /* GB9 */
        else if (b == AGRE_GCB_SPACINGMARK) join = true;                   /* GB9a */
        else if (a == AGRE_GCB_PREPEND) join = true;                       /* GB9b */
        else if (a == AGRE_GCB_ZWJ && pict && (cur & AGRE_GCB_EXTPICT)) join = true;  /* GB11 */
        else if (a == AGRE_GCB_RI && b == AGRE_GCB_RI && (ri & 1)) join = true;       /* GB12/13 */
        else join = false;                                                 /* GB999 */
        if (!join) break;

        pos += (size_t)n;
        if (cur & AGRE_GCB_EXTPICT) pict = true;
        else if (b != AGRE_GCB_EXTEND && b != AGRE_GCB_ZWJ) pict = false;
        ri = (b == AGRE_GCB_RI) ? ri + 1 : 0;
        prev = cur;
    }
    return pos;
}

#endif /* ASTROGRE_UNICODE_GRAPHEME_H */
