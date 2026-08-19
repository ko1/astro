/* koruby_precise regex bridge — koruby's thin adapter over the astrogre regex
 * engine (memory: Regexp via astrogre; astrogre is now a proper library).
 *
 * Built into a SEPARATE shared object (koruby_regex.so) that links libastrogre.so
 * (which keeps astrogre's generic ASTro symbols — NODE / EVAL / HASH / node_* —
 * local, so they never clash with koruby's identically-named symbols).  koruby
 * dlopen()s koruby_regex.so RTLD_LOCAL and dlsym()s only the koruby_re_* entries.
 *
 * Unlike the old whole-match wrapper, this calls astrogre_parse / astrogre_search_from
 * directly so per-group captures + named groups are surfaced.  Compiled patterns
 * are cached (source+flags keyed) so scan/gsub loops don't recompile per call, and
 * the astrogre memory fix means an evicted pattern is fully released. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../astrogre/context.h"   /* ASTROGRE_MAX_GROUPS, astrogre_match_t via parse.h */
#include "../astrogre/parse.h"

/* Match result handed back to koruby.  Byte offsets into the whole subject;
 * starts[i]/ends[i] == -1 for a group that did not participate. */
typedef struct koruby_re_match {
    int  matched;
    int  n_groups;                       /* capture groups excluding group 0 */
    long starts[ASTROGRE_MAX_GROUPS];
    long ends[ASTROGRE_MAX_GROUPS];
} koruby_re_match;

/* ---- compiled-pattern cache (direct-mapped, source+flags keyed) ---------- */
#define KRE_CACHE_SLOTS 128
typedef struct { char *pat; size_t len; unsigned flags; astrogre_pattern *p; } kre_cache_ent;
static kre_cache_ent g_cache[KRE_CACHE_SLOTS];

static uint64_t kre_hash(const char *b, size_t n, unsigned flags) {
    uint64_t h = 1469598103934665603ULL ^ ((uint64_t)flags << 1);
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)b[i]; h *= 1099511628211ULL; }
    return h;
}

/* Compile `pat`/`flags` (or return the cached pattern).  NULL on compile error. */
static astrogre_pattern *kre_get(const char *pat, size_t patlen, unsigned flags) {
    unsigned idx = (unsigned)(kre_hash(pat, patlen, flags) % KRE_CACHE_SLOTS);
    kre_cache_ent *e = &g_cache[idx];
    if (e->p && e->len == patlen && e->flags == flags &&
        (patlen == 0 || memcmp(e->pat, pat, patlen) == 0))
        return e->p;
    astrogre_pattern *np = astrogre_parse(pat, patlen, flags);
    if (np == NULL) return NULL;
    if (e->p) { astrogre_pattern_free(e->p); free(e->pat); }   /* evict → fully released (astrogre owns the AST) */
    e->pat = (char *)malloc(patlen ? patlen : 1);
    if (patlen) memcpy(e->pat, pat, patlen);
    e->len = patlen; e->flags = flags; e->p = np;
    return np;
}

/* Search `str[start..]` for `pat` (compiled with prism-compatible `flags`).
 * Returns 1 on match (filling *out with per-group byte spans), 0 no match,
 * -1 compile error.  `out` may be NULL for a boolean match test. */
__attribute__((visibility("default")))
/* koruby calls this on fiber / green-thread switches with its own C-stack
 * floor, so astrogre's \g<> recursion guard uses the stack we run on. */
void koruby_re_set_stack_floor(const void *floor) { astrogre_set_stack_floor((uintptr_t)floor); }

int koruby_re_exec(const char *pat, size_t patlen, unsigned flags,
                   const char *str, size_t slen, size_t start, koruby_re_match *out) {
    astrogre_pattern *p = kre_get(pat, patlen, flags);
    if (p == NULL) return -1;
    astrogre_match_t m;
    m.matched = false; m.n_groups = 0;
    int ng_all = ASTROGRE_MAX_GROUPS;
    for (int i = 0; i < ng_all; i++) m.valid[i] = false;
    m.overflow = false;
    bool ok = astrogre_search_from(p, str, slen, start, &m);
    /* Honest failure: the engine bailed on a branch at the C-stack floor and
     * never found a match elsewhere.  Report -2 so the host raises RegexpError
     * rather than passing off a stack-overflow as a real "no match". */
    if (!ok && m.overflow) return -2;
    if (!ok) { if (out) { out->matched = 0; out->n_groups = 0; } return 0; }
    if (out) {
        out->matched = 1;
        out->n_groups = m.n_groups;
        int ng = m.n_groups < ASTROGRE_MAX_GROUPS ? m.n_groups : ASTROGRE_MAX_GROUPS - 1;
        for (int i = 0; i <= ng; i++) {
            if (m.valid[i]) { out->starts[i] = (long)m.starts[i]; out->ends[i] = (long)m.ends[i]; }
            else            { out->starts[i] = -1; out->ends[i] = -1; }
        }
    }
    return 1;
}

/* Number of capture groups (excluding group 0) for `pat`; -1 on compile error. */
__attribute__((visibility("default")))
int koruby_re_ngroups(const char *pat, size_t patlen, unsigned flags) {
    astrogre_pattern *p = kre_get(pat, patlen, flags);
    if (p == NULL) return -1;
    return p->n_groups;
}

/* Enumerate named captures: returns the i-th named group's name (NUL-terminated,
 * owned by the cached pattern) and stores its group number in *out_idx; NULL once
 * i is past the last named group (or on compile error). */
__attribute__((visibility("default")))
const char *koruby_re_named(const char *pat, size_t patlen, unsigned flags, int i, int *out_idx) {
    astrogre_pattern *p = kre_get(pat, patlen, flags);
    if (p == NULL) return NULL;
    if (i < 0 || i >= astrogre_pattern_n_named(p)) return NULL;
    return astrogre_pattern_named_at(p, i, out_idx);
}

/* True (1) if `pat` compiles, 0 if it does not — used by Regexp.new validation. */
__attribute__((visibility("default")))
int koruby_re_valid(const char *pat, size_t patlen, unsigned flags) {
    return kre_get(pat, patlen, flags) != NULL ? 1 : 0;
}

/* Why the last koruby_re_valid/koruby_re_exec compile failed, so the caller can
 * put it in the RegexpError instead of a fixed "invalid regular expression". */
__attribute__((visibility("default")))
const char *koruby_re_error(void) { return astrogre_last_error(); }

/* ---- legacy whole-match entry (kept for any un-migrated caller) ----------- */
__attribute__((visibility("default")))
int koruby_re_search(const char *pat, size_t patlen, const char *str, size_t slen,
                     int ci, long *ms, long *me) {
    koruby_re_match m;
    int rc = koruby_re_exec(pat, patlen, ci ? 4u : 0u, str, slen, 0, &m);
    if (rc == 1) { *ms = m.starts[0]; *me = m.ends[0]; return 1; }
    return rc == 0 ? 0 : -1;
}
