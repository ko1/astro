/*
 * astrogre top-level matcher.
 *
 * The for-each-start-position search loop is now `node_grep_search`
 * inside the AST itself; what's left here is plumbing — set up the
 * CTX, dispatch the root, copy out the captures.
 *
 * `astrogre_search_from` is the resume entry point used by grep --color
 * and -o; it just sets c.pos to the requested offset before
 * dispatching, since node_grep_search reads c.pos as the loop start.
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
    /* Explicit field init.  CTX = {0} would zero ~600 bytes per call
     * (mainly the 32-entry capture arrays); but starts[]/ends[] are
     * only read after valid[] is true, and valid[] is reset by
     * node_grep_search at every search-loop iteration.  rep_top is
     * also reset there.  So skipping the bulk zero is safe AND saves
     * ~50 ns / call — measurable on grep -c paths that call search_from
     * once per matching line. */
    CTX c;
    c.str = (const uint8_t *)str;
    c.str_len = len;
    c.case_insensitive = p->case_insensitive;
    c.multiline = p->multiline;
    c.encoding = p->encoding;
    c.n_groups = p->n_groups;
    c.rep_cont_sentinel = astrogre_rep_cont_singleton();
    c.pos = start_from;
    c.rep_top = NULL;

    bool r = (bool)EVAL(&c, p->root);

    if (out) {
        out->matched = r;
        if (r) {
            out->n_groups = p->n_groups;
            /* Only copy slots that node_grep_search actually marked
             * valid; clear the rest so callers don't see ghosts from
             * a previous reuse of the `out` struct. */
            for (int i = 0; i <= p->n_groups; i++) {
                out->valid[i] = c.valid[i];
                if (c.valid[i]) {
                    out->starts[i] = c.starts[i];
                    out->ends[i]   = c.ends[i];
                }
            }
            for (int i = p->n_groups + 1; i < ASTROGRE_MAX_GROUPS; i++) {
                out->valid[i] = false;
            }
        }
    }
    return r;
}

bool
astrogre_search(astrogre_pattern *p, const char *str, size_t len, astrogre_match_t *out)
{
    return astrogre_search_from(p, str, len, 0, out);
}
