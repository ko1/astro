/*
 * astrogre top-level matcher.
 *
 * Two entry points:
 *   astrogre_search       - search the whole input from offset 0
 *   astrogre_search_from  - resume from a caller-chosen offset; used
 *                           by grep --color / -o and scan-style enum
 */

#include "node.h"
#include "context.h"
#include "parse.h"

/* The single rep_cont sentinel node used by all repeats.  Allocated
 * lazily on first request so it works regardless of whether main() has
 * called INIT(). */
static NODE *g_rep_cont = NULL;

NODE *astrogre_rep_cont_singleton(void)
{
    if (!g_rep_cont) g_rep_cont = ALLOC_node_re_rep_cont();
    return g_rep_cont;
}

bool
astrogre_search_from(astrogre_pattern *p, const char *str, size_t len,
                     size_t start_from, astrogre_match_t *out)
{
    CTX c = {0};
    c.str = (const uint8_t *)str;
    c.str_len = len;
    c.case_insensitive = p->case_insensitive;
    c.multiline = p->multiline;
    c.encoding = p->encoding;
    c.n_groups = p->n_groups;
    c.rep_cont_sentinel = astrogre_rep_cont_singleton();

    /* For \A-anchored patterns, only start==0 is meaningful.  If the
     * caller resumes from a non-zero offset on such a pattern we know
     * statically there can be no further match. */
    size_t start_max;
    if (p->anchored_bos) {
        start_max = (start_from == 0) ? 1 : 0;
    } else {
        start_max = len + 1;
    }

    for (size_t start = start_from; start < start_max; start++) {
        c.pos = start;
        for (int i = 0; i < ASTROGRE_MAX_GROUPS; i++) c.valid[i] = false;
        c.rep_top = NULL;

        if (EVAL(&c, p->root)) {
            if (out) {
                out->matched = true;
                out->n_groups = p->n_groups;
                for (int i = 0; i < ASTROGRE_MAX_GROUPS; i++) {
                    out->starts[i] = c.starts[i];
                    out->ends[i]   = c.ends[i];
                    out->valid[i]  = c.valid[i];
                }
            }
            return true;
        }
    }
    if (out) out->matched = false;
    return false;
}

bool
astrogre_search(astrogre_pattern *p, const char *str, size_t len, astrogre_match_t *out)
{
    return astrogre_search_from(p, str, len, 0, out);
}
