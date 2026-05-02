/*
 * Aho-Corasick implementation.  See aho_corasick.h for the role this
 * plays in the prefilter cascade.
 *
 * Trie: each state has an `int32_t children[256]` array, a failure
 * link, and an `output_len` (length of the pattern that ends here, or
 * 0 if none).  Failure links are folded INTO the children table at
 * build time — for any byte where the state has no real child, we
 * store the goto target the failure-link walk would have arrived at.
 * That makes the scan loop branchless: one indirect load per byte
 * with no inner `while`.
 */

#include "aho_corasick.h"
#include <stdlib.h>

typedef struct ac_state {
    int32_t children[256];   /* >= 0 transition target; never -1 after build */
    int32_t fail;            /* failure-link state index */
    int32_t output_len;      /* > 0 if a pattern ends here */
} ac_state_t;

struct ac_t {
    ac_state_t *states;
    int         n_states;
    int         cap_states;
    int         min_pat_len;      /* shortest pattern, kept for diagnostics */
};

static int
ac_new_state(ac_t *ac)
{
    if (ac->n_states == ac->cap_states) {
        ac->cap_states = ac->cap_states ? ac->cap_states * 2 : 16;
        ac->states = (ac_state_t *)realloc(ac->states,
                                           sizeof(ac_state_t) * (size_t)ac->cap_states);
    }
    ac_state_t *s = &ac->states[ac->n_states];
    for (int i = 0; i < 256; i++) s->children[i] = -1;
    s->fail = 0;
    s->output_len = 0;
    return ac->n_states++;
}

ac_t *
astrogre_ac_build(const char *const *needles, const uint32_t *lens, int n)
{
    ac_t *ac = (ac_t *)calloc(1, sizeof(*ac));
    ac->min_pat_len = INT32_MAX;
    ac_new_state(ac);  /* state 0 = root */

    /* Phase 1: insert each needle into the trie. */
    for (int p = 0; p < n; p++) {
        int s = 0;
        const uint8_t *bytes = (const uint8_t *)needles[p];
        for (uint32_t i = 0; i < lens[p]; i++) {
            const uint8_t c = bytes[i];
            int next = ac->states[s].children[c];
            if (next < 0) {
                next = ac_new_state(ac);
                /* Re-fetch ac->states after realloc may have moved it. */
                ac->states[s].children[c] = next;
            }
            s = next;
        }
        if (ac->states[s].output_len < (int32_t)lens[p]) {
            ac->states[s].output_len = (int32_t)lens[p];
        }
        if ((int)lens[p] < ac->min_pat_len) ac->min_pat_len = (int)lens[p];
    }
    if (ac->min_pat_len == INT32_MAX) ac->min_pat_len = 0;

    /* Phase 2: BFS from root to compute failure links + fold them
     * into `children` so the scan loop is branchless. */
    int *queue = (int *)malloc(sizeof(int) * (size_t)ac->n_states);
    int qhead = 0, qtail = 0;
    /* Depth-1 children fail to root; missing children of root fall
     * back to root itself (so the scan loop doesn't need a special
     * case for "no transition from root"). */
    for (int c = 0; c < 256; c++) {
        int child = ac->states[0].children[c];
        if (child < 0) {
            ac->states[0].children[c] = 0;
        } else {
            ac->states[child].fail = 0;
            queue[qtail++] = child;
        }
    }
    while (qhead < qtail) {
        int u = queue[qhead++];
        for (int c = 0; c < 256; c++) {
            int v = ac->states[u].children[c];
            if (v < 0) {
                /* Goto failure: no real child for this byte → take
                 * the transition our failure-link state would have
                 * taken.  Fold into children[c] so the scan loop
                 * doesn't need to walk fail links at runtime. */
                ac->states[u].children[c] = ac->states[ac->states[u].fail].children[c];
            } else {
                ac->states[v].fail = ac->states[ac->states[u].fail].children[c];
                /* Inherit output_len if our failure state has a
                 * longer pattern ending there (overlap case —
                 * `cat` ending inside `concatenate`). */
                if (ac->states[ac->states[v].fail].output_len > ac->states[v].output_len) {
                    ac->states[v].output_len = ac->states[ac->states[v].fail].output_len;
                }
                queue[qtail++] = v;
            }
        }
    }
    free(queue);
    return ac;
}

void
astrogre_ac_free(ac_t *ac)
{
    if (!ac) return;
    free(ac->states);
    free(ac);
}

bool
astrogre_ac_scan(void *ac_handle, const uint8_t *hay, size_t end,
                 size_t *io_pos, int32_t *io_state, size_t *out_match_start)
{
    const ac_t *ac = (const ac_t *)ac_handle;
    int32_t state = *io_state;
    size_t pos = *io_pos;
    while (pos < end) {
        state = ac->states[state].children[hay[pos]];
        const int32_t olen = ac->states[state].output_len;
        pos++;
        if (olen > 0) {
            *out_match_start = pos - (size_t)olen;
            *io_pos          = pos;
            *io_state        = state;
            return true;
        }
    }
    *io_pos = pos;
    *io_state = state;
    return false;
}
