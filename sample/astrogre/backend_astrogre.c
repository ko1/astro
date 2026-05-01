/*
 * Backend implementation: in-house astrogre engine.
 *
 * Wraps astrogre_pattern + astrogre_search behind the generic
 * backend_ops_t interface so the grep CLI can switch backends at
 * runtime.
 */

#include <stdlib.h>
#include "backend.h"
#include "node.h"
#include "context.h"
#include "parse.h"

/* prism flag bits we need (mirrored to avoid pulling prism.h here) */
#define PR_FLAGS_IGNORE_CASE 4
#define PR_FLAGS_EXTENDED    8
#define PR_FLAGS_MULTI_LINE  16

struct backend_pattern { astrogre_pattern *p; };

/* Exposed so main.c can introspect the underlying astrogre_pattern
 * for fast-path detection (pure-literal short-circuit, etc).
 * Returns NULL for non-astrogre backends. */
astrogre_pattern *
astrogre_backend_pattern_get(backend_pattern_t *bp)
{
    return bp ? bp->p : NULL;
}

static backend_pattern_t *
agre_compile(const char *pat, size_t len, backend_flags_t f)
{
    uint32_t flags = 0;
    if (f.case_insensitive) flags |= PR_FLAGS_IGNORE_CASE;
    if (f.multiline)        flags |= PR_FLAGS_MULTI_LINE;
    if (f.extended)         flags |= PR_FLAGS_EXTENDED;

    astrogre_pattern *p = f.fixed_string
        ? astrogre_parse_fixed(pat, len, flags)
        : astrogre_parse(pat, len, flags);
    if (!p) return NULL;

    backend_pattern_t *bp = (backend_pattern_t *)calloc(1, sizeof(*bp));
    bp->p = p;
    return bp;
}

static bool
agre_search(backend_pattern_t *bp, const char *str, size_t len, backend_match_t *out)
{
    astrogre_match_t m;
    bool r = astrogre_search(bp->p, str, len, &m);
    if (out) {
        out->matched = r;
        out->start   = r ? m.starts[0] : 0;
        out->end     = r ? m.ends[0]   : 0;
    }
    return r;
}

static bool
agre_search_from(backend_pattern_t *bp, const char *str, size_t len, size_t start, backend_match_t *out)
{
    astrogre_match_t m;
    bool r = astrogre_search_from(bp->p, str, len, start, &m);
    if (out) {
        out->matched = r;
        out->start   = r ? m.starts[0] : 0;
        out->end     = r ? m.ends[0]   : 0;
    }
    return r;
}

static void
agre_free(backend_pattern_t *bp)
{
    if (!bp) return;
    astrogre_pattern_free(bp->p);
    free(bp);
}

static void
agre_aot_compile(backend_pattern_t *bp, bool verbose)
{
    if (bp) astrogre_pattern_aot_compile(bp->p, verbose);
}

static bool
agre_has_fast_scan(backend_pattern_t *bp)
{
    return bp && astrogre_pattern_has_prefilter(bp->p);
}

const backend_ops_t backend_astrogre_ops = {
    .name          = "astrogre",
    .compile       = agre_compile,
    .search        = agre_search,
    .search_from   = agre_search_from,
    .free          = agre_free,
    .aot_compile   = agre_aot_compile,
    .has_fast_scan = agre_has_fast_scan,
};
