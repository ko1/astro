/*
 * Backend implementation: Onigmo (https://github.com/k-takata/Onigmo).
 *
 * Built only when WITH_ONIGMO=1 was passed to make.  The grep CLI
 * picks this with --backend=onigmo.
 *
 * We use Onigmo's "regular" (non-POSIX) API: onig_new + onig_search.
 * Encoding is fixed at UTF-8 for now to match the astrogre default;
 * /n binary mode would require swapping in ONIG_ENCODING_ASCII.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "onigmo.h"
#include "backend.h"

struct backend_pattern {
    regex_t *reg;
    OnigRegion *region;
};

static OnigEncoding s_encoding = ONIG_ENCODING_UTF8;
static int s_initialised = 0;

static void
onigmo_lazy_init(void)
{
    if (s_initialised) return;
    OnigEncoding encs[] = { ONIG_ENCODING_UTF8, ONIG_ENCODING_ASCII };
    onig_initialize(encs, sizeof(encs)/sizeof(encs[0]));
    s_initialised = 1;
}

static backend_pattern_t *
ognm_compile(const char *pat, size_t len, backend_flags_t f)
{
    onigmo_lazy_init();

    /* For -F, escape regex metacharacters to make Onigmo treat the
     * pattern literally — Onigmo doesn't have a fixed-string mode. */
    char *escaped = NULL;
    const char *use_pat = pat;
    size_t use_len = len;
    if (f.fixed_string) {
        escaped = (char *)malloc(len * 2 + 1);
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            char c = pat[i];
            if (strchr("\\.^$|()[]{}*+?-/", c)) escaped[j++] = '\\';
            escaped[j++] = c;
        }
        escaped[j] = 0;
        use_pat = escaped;
        use_len = j;
    }

    OnigOptionType opt = ONIG_OPTION_NONE;
    if (f.case_insensitive) opt |= ONIG_OPTION_IGNORECASE;
    if (f.multiline)        opt |= ONIG_OPTION_MULTILINE;
    if (f.extended)         opt |= ONIG_OPTION_EXTEND;

    regex_t *reg = NULL;
    OnigErrorInfo einfo;
    int r = onig_new(&reg,
                     (const OnigUChar *)use_pat,
                     (const OnigUChar *)use_pat + use_len,
                     opt, s_encoding, ONIG_SYNTAX_DEFAULT, &einfo);
    free(escaped);
    if (r != ONIG_NORMAL) {
        OnigUChar buf[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str(buf, r, &einfo);
        fprintf(stderr, "onigmo: compile error: %s\n", (char *)buf);
        return NULL;
    }
    backend_pattern_t *bp = (backend_pattern_t *)calloc(1, sizeof(*bp));
    bp->reg = reg;
    bp->region = onig_region_new();
    return bp;
}

static bool
ognm_search_from(backend_pattern_t *bp, const char *str, size_t len, size_t start, backend_match_t *out)
{
    if (start > len) {
        if (out) { out->matched = false; out->start = out->end = 0; }
        return false;
    }
    onig_region_clear(bp->region);
    const OnigUChar *s = (const OnigUChar *)str;
    int r = onig_search(bp->reg,
                        s, s + len,
                        s + start, s + len,
                        bp->region, ONIG_OPTION_NONE);
    if (r >= 0) {
        if (out) {
            out->matched = true;
            out->start = (size_t)bp->region->beg[0];
            out->end   = (size_t)bp->region->end[0];
        }
        return true;
    }
    if (out) { out->matched = false; out->start = out->end = 0; }
    return false;
}

static bool
ognm_search(backend_pattern_t *bp, const char *str, size_t len, backend_match_t *out)
{
    return ognm_search_from(bp, str, len, 0, out);
}

static void
ognm_free(backend_pattern_t *bp)
{
    if (!bp) return;
    if (bp->region) onig_region_free(bp->region, 1);
    if (bp->reg)    onig_free(bp->reg);
    free(bp);
}

const backend_ops_t backend_onigmo_ops = {
    .name        = "onigmo",
    .compile     = ognm_compile,
    .search      = ognm_search,
    .search_from = ognm_search_from,
    .free        = ognm_free,
    .aot_compile = NULL,    /* Onigmo doesn't participate in ASTro code store */
};
