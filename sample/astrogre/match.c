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

#define _GNU_SOURCE
#include <sys/resource.h>
#include <pthread.h>
#include "node.h"
#include "context.h"
#include "parse.h"

/* RLIMIT_STACK at process start, halved + floored — the runtime cap
 * for recursive `\g<>` calls.  See node_re_subroutine_call. */
/* Lowest stack address the current thread may safely reach.  Computed per
 * thread (cached in a TLS slot) from the pthread stack bounds, so the `\g<>`
 * recursion guard measures ACTUAL remaining headroom — not `total/2` from a
 * mid-stack snapshot, which overflowed when the caller (e.g. koruby running
 * net/http) had already consumed most of the stack. */
static __thread uintptr_t g_stack_floor = 0;   /* absolute low address + margin; 0 = uncomputed */
static __thread uintptr_t g_stack_floor_override = 0;   /* set by the host (koruby) to its own C-stack floor */

/* The host runs the matcher on its own (possibly fiber) C-stack, whose real
 * floor pthread_getattr_np cannot see.  koruby calls this on every fiber /
 * green-thread switch with its cstack_limit so the `\g<>` guard uses the
 * stack we are actually on.  Pass 0 to clear. */
void astrogre_set_stack_floor(uintptr_t floor) { g_stack_floor_override = floor; }

static uintptr_t
astrogre_stack_floor(void)
{
    if (g_stack_floor_override) return g_stack_floor_override;
    if (g_stack_floor) return g_stack_floor;
    void  *lo = NULL;
    size_t size = 0;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        if (pthread_attr_getstack(&attr, &lo, &size) != 0) { lo = NULL; size = 0; }
        pthread_attr_destroy(&attr);
    }
    if (lo == NULL || size == 0) {
        /* Fallback: derive a floor from RLIMIT_STACK below the current frame. */
        struct rlimit rl;
        size_t total = 8u * 1024u * 1024u;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) total = (size_t)rl.rlim_cur;
        char here;
        g_stack_floor = (uintptr_t)&here - (total - (total / 8));   /* keep ~1/8 in reserve */
        return g_stack_floor;
    }
    /* Keep a 256 KiB safety margin above the true floor: one astrogre
     * recursion level plus the backtrace/handler needs some room. */
    const size_t margin = 256u * 1024u;
    g_stack_floor = (uintptr_t)lo + margin;
    return g_stack_floor;
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
    c.stack_base  = astrogre_stack_floor();   /* absolute low address the guard must not cross */
    c.stack_limit = 0;                        /* unused now (guard compares against stack_base directly) */
    c.stack_overflow = false;                 /* set by the floor guard; surfaced via out->overflow */

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

    /* getenv() is a libc syscall-equivalent (binary-search through
     * environ + strncmp).  In line-by-line grep this gets called
     * ~once per matching line, which on a 35 MB log of 400k lines
     * showed up at 24% of total CPU in `perf record`.  Cache once. */
    static int memo_debug = -1;
    if (memo_debug < 0) memo_debug = (getenv("ASTROGRE_MEMO_DEBUG") != NULL);
    if (memo_debug) {
        fprintf(stderr, "[memo] eligible=%d n_branches=%d backtracks=%zu memo_alloc=%s\n",
                p->memo_eligible, p->n_branches, c.backtrack_count,
                c.memo ? "yes" : "no");
    }
    if (c.memo) free(c.memo);

    if (out) {
        out->matched = r;
        /* A floor-guard abort only counts as an "overflow" outcome when no
         * other branch found a match — a successful match is the truth. */
        out->overflow = (!r && c.stack_overflow);
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
