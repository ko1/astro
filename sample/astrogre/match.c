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

#include <sys/resource.h>
#include "node.h"
#include "context.h"
#include "parse.h"

/* RLIMIT_STACK at process start, halved + floored — the runtime cap
 * for recursive `\g<>` calls.  See node_re_subroutine_call. */
static size_t g_stack_limit = 0;

static size_t
astrogre_stack_limit(void)
{
    if (g_stack_limit) return g_stack_limit;
    struct rlimit rl;
    size_t total = 8u * 1024u * 1024u;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        total = (size_t)rl.rlim_cur;
    }
    /* Half the reported stack — the host (Ruby, the CLI) already burned
     * some before we got here, and each recursive call drags more than
     * just its own frame onto the stack (rep_cont, body chain, ...). */
    const size_t halved = total / 2;
    const size_t floor  = 1u * 1024u * 1024u;
    g_stack_limit = halved > floor ? halved : floor;
    return g_stack_limit;
}

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
    c.scan_start = start_from;
    c.rep_top = NULL;
    c.sub_chains   = p->sub_chains;
    c.sub_chains_n = p->sub_chains_n;
    c.sub_top      = NULL;
    c.sub_depth    = 0;
    char stack_marker;
    c.stack_base  = (uintptr_t)&stack_marker;
    c.stack_limit = astrogre_stack_limit();

    /* MatchCache state.  Allocated lazily by node_re_alt /
     * node_re_rep_cont once backtrack_count exceeds memo_threshold.
     *
     * Threshold formula matches Onigmo's: `str_len × n_branches`.
     * Below this, backtracking volume is consistent with normal
     * matching (each branch may try ~str_len positions).  Above it,
     * backtracking is super-linear and almost certainly catastrophic.
     * Keeping the threshold proportional to n_branches avoids paying
     * memory on patterns that just have a lot of branches but each
     * fails quickly (alt-3-of-literals on long input etc.). */
    c.memo            = NULL;
    c.n_branches      = p->n_branches;
    c.backtrack_count = 0;
    c.memo_threshold  = (size_t)len * (size_t)(p->n_branches > 0 ? p->n_branches : 1);
    c.memo_eligible   = p->memo_eligible;

    bool r = (bool)EVAL(&c, p->root);

    if (getenv("ASTROGRE_MEMO_DEBUG")) {
        fprintf(stderr, "[memo] eligible=%d n_branches=%d backtracks=%zu memo_alloc=%s\n",
                p->memo_eligible, p->n_branches, c.backtrack_count,
                c.memo ? "yes" : "no");
    }
    if (c.memo) free(c.memo);

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
