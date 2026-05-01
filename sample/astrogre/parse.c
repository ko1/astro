/*
 * astrogre regex parser.
 *
 * Two layers:
 *   (1) astrogre_parse_via_prism: take Ruby source, drive prism, find a
 *       RegularExpressionNode, hand its content + flags to (2).
 *   (2) astrogre_parse:           classic recursive-descent regex parser
 *       that produces our AST.  Continuation-passing assembly is done in
 *       a second walk over an intermediate struct tree (ire_node_t) so
 *       that "tail" can be threaded right-to-left without making the
 *       lexer lookahead-heavy.
 */

#include <ctype.h>
#include "parse.h"
#include "prism.h"

/* prism flag bits we care about (mirrored to avoid relying on the enum
 * being visible here). */
#define PR_FLAGS_IGNORE_CASE 4
#define PR_FLAGS_EXTENDED    8
#define PR_FLAGS_MULTI_LINE  16
#define PR_FLAGS_ASCII_8BIT  128
#define PR_FLAGS_UTF_8       512

/* ------------------------------------------------------------------ */
/* Intermediate representation                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    IRE_LIT,
    IRE_DOT,            /* .  (respects multiline) */
    IRE_CLASS,          /* [...] */
    IRE_ALT,
    IRE_CONCAT,
    IRE_REP,
    IRE_GROUP,          /* capturing */
    IRE_NCGROUP,        /* non-capturing */
    IRE_LOOKAHEAD,
    IRE_NEG_LOOKAHEAD,
    IRE_BOS,
    IRE_EOS,
    IRE_EOS_NL,
    IRE_BOL,
    IRE_EOL,
    IRE_WB,
    IRE_NWB,
    IRE_BACKREF,
    IRE_EMPTY,
} ire_kind_t;

typedef struct ire_node {
    ire_kind_t kind;
    union {
        struct { char *bytes; uint32_t len; bool ci; } lit;
        struct { uint64_t bm[4]; } cls;
        struct { struct ire_node *l, *r; } alt;
        struct { struct ire_node **xs; size_t n; size_t cap; } cat;
        struct { struct ire_node *body; int32_t min; int32_t max; bool greedy; } rep;
        struct { int idx; struct ire_node *body; } group;
        struct { struct ire_node *body; } nc;
        struct { int idx; } backref;
    } u;
} ire_node_t;

static ire_node_t *ire_new(ire_kind_t k) {
    ire_node_t *n = (ire_node_t *)calloc(1, sizeof(*n));
    n->kind = k;
    return n;
}

static void ire_cat_push(ire_node_t *cat, ire_node_t *child) {
    if (cat->u.cat.n == cat->u.cat.cap) {
        cat->u.cat.cap = cat->u.cat.cap ? cat->u.cat.cap * 2 : 4;
        cat->u.cat.xs = (ire_node_t **)realloc(cat->u.cat.xs, sizeof(ire_node_t *) * cat->u.cat.cap);
    }
    cat->u.cat.xs[cat->u.cat.n++] = child;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct re_parser {
    const uint8_t *p;
    const uint8_t *end;
    bool case_insensitive;
    bool multiline;
    bool extended;
    agre_encoding_t encoding;

    int n_groups;     /* # of capturing groups assigned so far */
    bool error;
    char errbuf[256];
} re_parser_t;

static void re_error(re_parser_t *q, const char *msg) {
    if (q->error) return;
    q->error = true;
    snprintf(q->errbuf, sizeof(q->errbuf), "regex parse error: %s (at offset %ld)",
             msg, (long)(q->p - (q->end - (q->end - q->p) - 0)));
    /* offset arithmetic above is just for the message; not exact start-relative */
}

static int re_peek(re_parser_t *q) {
    if (q->p >= q->end) return -1;
    return *q->p;
}

static int re_get(re_parser_t *q) {
    if (q->p >= q->end) return -1;
    return *q->p++;
}

/* Skip whitespace and # comments when /x is in effect. */
static void re_skip_ws(re_parser_t *q) {
    if (!q->extended) return;
    for (;;) {
        if (q->p >= q->end) return;
        uint8_t c = *q->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            q->p++;
        } else if (c == '#') {
            while (q->p < q->end && *q->p != '\n') q->p++;
        } else {
            return;
        }
    }
}

static ire_node_t *parse_alt(re_parser_t *q);

/* Bitmap helpers */
static void bm_set(uint64_t bm[4], uint8_t b) { bm[b >> 6] |= (1ULL << (b & 63)); }
static void bm_set_range(uint64_t bm[4], uint8_t lo, uint8_t hi) {
    for (int i = lo; i <= hi; i++) bm_set(bm, (uint8_t)i);
}
static void bm_invert(uint64_t bm[4]) {
    for (int i = 0; i < 4; i++) bm[i] = ~bm[i];
}
static void bm_or(uint64_t a[4], const uint64_t b[4]) {
    for (int i = 0; i < 4; i++) a[i] |= b[i];
}

/* Apply /i case-fold expansion to a single ASCII char */
static void bm_set_ci(uint64_t bm[4], uint8_t b, bool ci) {
    bm_set(bm, b);
    if (ci) {
        if (b >= 'A' && b <= 'Z') bm_set(bm, b + 32);
        else if (b >= 'a' && b <= 'z') bm_set(bm, b - 32);
    }
}

/* \d \w \s and friends inside a class */
static void bm_add_special(uint64_t bm[4], int c) {
    switch (c) {
    case 'd': bm_set_range(bm, '0', '9'); break;
    case 'D':
        for (int i = 0; i < 256; i++) if (!(i >= '0' && i <= '9')) bm_set(bm, (uint8_t)i);
        break;
    case 'w':
        bm_set_range(bm, '0', '9');
        bm_set_range(bm, 'A', 'Z');
        bm_set_range(bm, 'a', 'z');
        bm_set(bm, '_');
        break;
    case 'W':
        for (int i = 0; i < 256; i++) {
            int wc = (i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') ||
                     (i >= 'a' && i <= 'z') || (i == '_');
            if (!wc) bm_set(bm, (uint8_t)i);
        }
        break;
    case 's':
        bm_set(bm, ' ');
        bm_set(bm, '\t');
        bm_set(bm, '\n');
        bm_set(bm, '\v');
        bm_set(bm, '\f');
        bm_set(bm, '\r');
        break;
    case 'S':
        for (int i = 0; i < 256; i++) {
            int ws = (i == ' ' || i == '\t' || i == '\n' || i == '\v' || i == '\f' || i == '\r');
            if (!ws) bm_set(bm, (uint8_t)i);
        }
        break;
    default: break;
    }
}

/* Parse a single escape inside a class.  Returns 1 if handled (already set
 * bits), 0 if it was a single character (write *out_byte). */
static int parse_class_escape(re_parser_t *q, uint64_t bm[4], uint8_t *out_byte) {
    int c = re_get(q);
    switch (c) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        bm_add_special(bm, c);
        return 1;
    case 'n': *out_byte = '\n'; return 0;
    case 't': *out_byte = '\t'; return 0;
    case 'r': *out_byte = '\r'; return 0;
    case 'f': *out_byte = '\f'; return 0;
    case 'v': *out_byte = '\v'; return 0;
    case '0': *out_byte = 0;    return 0;
    case 'a': *out_byte = '\a'; return 0;
    case 'e': *out_byte = 0x1b; return 0;
    case 'x': {
        if (q->p + 2 > q->end) { re_error(q, "bad \\x"); return 0; }
        char h[3] = { (char)q->p[0], (char)q->p[1], 0 };
        q->p += 2;
        *out_byte = (uint8_t)strtol(h, NULL, 16);
        return 0;
    }
    case '\\': *out_byte = '\\'; return 0;
    case ']':  *out_byte = ']';  return 0;
    case '-':  *out_byte = '-';  return 0;
    case '^':  *out_byte = '^';  return 0;
    case '/':  *out_byte = '/';  return 0;
    case '[':  *out_byte = '[';  return 0;
    case '.':  *out_byte = '.';  return 0;
    default:
        *out_byte = (uint8_t)c;
        return 0;
    }
}

static ire_node_t *parse_class(re_parser_t *q) {
    /* '[' already consumed */
    ire_node_t *n = ire_new(IRE_CLASS);
    bool negate = false;
    if (re_peek(q) == '^') { negate = true; q->p++; }

    /* In Ruby, `]` immediately after `[` (or `[^`) is literal */
    bool first = true;

    while (q->p < q->end) {
        int c = re_peek(q);
        if (c == ']' && !first) { q->p++; goto done; }
        first = false;

        uint8_t byte_val = 0;
        bool got_byte = false;

        if (c == '\\') {
            q->p++;
            if (parse_class_escape(q, n->u.cls.bm, &byte_val)) {
                continue;  /* set was applied */
            }
            got_byte = true;
        } else {
            re_get(q);
            byte_val = (uint8_t)c;
            got_byte = true;
        }

        if (!got_byte) continue;

        /* range? */
        if (q->p + 1 < q->end && q->p[0] == '-' && q->p[1] != ']') {
            q->p++;  /* consume '-' */
            uint8_t hi;
            int c2 = re_peek(q);
            if (c2 == '\\') {
                q->p++;
                uint64_t dummy[4] = {0};
                uint8_t hib;
                if (parse_class_escape(q, dummy, &hib)) {
                    re_error(q, "char-class escape in range");
                    return n;
                }
                hi = hib;
            } else {
                re_get(q);
                hi = (uint8_t)c2;
            }
            uint8_t lo = byte_val;
            if (lo > hi) { re_error(q, "bad range"); return n; }
            for (int i = lo; i <= hi; i++) {
                bm_set_ci(n->u.cls.bm, (uint8_t)i, q->case_insensitive);
            }
        } else {
            bm_set_ci(n->u.cls.bm, byte_val, q->case_insensitive);
        }
    }
    re_error(q, "unterminated character class");
done:
    if (negate) bm_invert(n->u.cls.bm);
    return n;
}

static ire_node_t *parse_quantifier_target(re_parser_t *q);

/* Parse {min,max} or {n} after we've already consumed '{' */
static bool parse_braces(re_parser_t *q, int32_t *out_min, int32_t *out_max) {
    int32_t mn = 0, mx = 0;
    bool have_mx = false;

    while (q->p < q->end && isdigit(*q->p)) { mn = mn * 10 + (*q->p++ - '0'); }
    if (q->p < q->end && *q->p == ',') {
        q->p++;
        if (q->p < q->end && *q->p != '}') {
            while (q->p < q->end && isdigit(*q->p)) { mx = mx * 10 + (*q->p++ - '0'); }
            have_mx = true;
        }
    } else {
        mx = mn; have_mx = true;
    }
    if (q->p >= q->end || *q->p != '}') return false;
    q->p++;

    *out_min = mn;
    *out_max = have_mx ? mx : -1;
    return true;
}

/* Parse a single atom (without quantifier).  Returns NULL only on error. */
static ire_node_t *parse_atom(re_parser_t *q) {
    re_skip_ws(q);
    int c = re_peek(q);
    if (c < 0) return ire_new(IRE_EMPTY);

    switch (c) {
    case '(': {
        q->p++;
        bool capture = true;
        bool lookahead = false, neg_lookahead = false;
        bool nc = false;
        int saved_ci = q->case_insensitive, saved_ml = q->multiline, saved_x = q->extended;
        bool saved_flags = false;

        if (q->p < q->end && *q->p == '?') {
            q->p++;
            if (q->p < q->end && *q->p == ':') {
                q->p++; capture = false; nc = true;
            } else if (q->p < q->end && *q->p == '=') {
                q->p++; capture = false; lookahead = true;
            } else if (q->p < q->end && *q->p == '!') {
                q->p++; capture = false; neg_lookahead = true;
            } else if (q->p < q->end && *q->p == '<') {
                /* (?<name>...) named capture (treat as numbered) — or
                 *  (?<=...) / (?<!...) lookbehind which we don't yet support. */
                q->p++;
                if (q->p < q->end && (*q->p == '=' || *q->p == '!')) {
                    re_error(q, "lookbehind not supported");
                    return NULL;
                }
                /* skip name */
                while (q->p < q->end && *q->p != '>') q->p++;
                if (q->p < q->end) q->p++;
                capture = true;
            } else {
                /* (?ixm-ixm:...) or (?ixm) inline flag setting */
                saved_flags = true;
                bool turning_off = false;
                while (q->p < q->end && *q->p != ':' && *q->p != ')') {
                    char fc = *q->p++;
                    if (fc == '-') turning_off = true;
                    else if (fc == 'i') q->case_insensitive = !turning_off;
                    else if (fc == 'm') q->multiline = !turning_off;
                    else if (fc == 'x') q->extended = !turning_off;
                }
                if (q->p < q->end && *q->p == ':') {
                    q->p++;
                    capture = false; nc = true;
                } else if (q->p < q->end && *q->p == ')') {
                    q->p++;
                    /* flag-only group: empty match.  Flags persist. */
                    saved_flags = false;  /* leave updated */
                    return ire_new(IRE_EMPTY);
                } else {
                    re_error(q, "bad (? group");
                    return NULL;
                }
            }
        }

        int idx = 0;
        if (capture) idx = ++q->n_groups;
        ire_node_t *body = parse_alt(q);
        if (re_get(q) != ')') re_error(q, "expected )");

        if (saved_flags) {
            q->case_insensitive = saved_ci;
            q->multiline = saved_ml;
            q->extended = saved_x;
        }

        if (capture) {
            ire_node_t *g = ire_new(IRE_GROUP);
            g->u.group.idx = idx;
            g->u.group.body = body;
            return g;
        } else if (lookahead) {
            ire_node_t *g = ire_new(IRE_LOOKAHEAD);
            g->u.nc.body = body;
            return g;
        } else if (neg_lookahead) {
            ire_node_t *g = ire_new(IRE_NEG_LOOKAHEAD);
            g->u.nc.body = body;
            return g;
        } else if (nc) {
            ire_node_t *g = ire_new(IRE_NCGROUP);
            g->u.nc.body = body;
            return g;
        }
        return body;
    }
    case '[': q->p++; return parse_class(q);
    case '.': q->p++; return ire_new(IRE_DOT);
    case '^': q->p++; return ire_new(IRE_BOL);
    case '$': q->p++; return ire_new(IRE_EOL);
    case '\\': {
        q->p++;
        int e = re_get(q);
        switch (e) {
        case 'A': return ire_new(IRE_BOS);
        case 'z': return ire_new(IRE_EOS);
        case 'Z': return ire_new(IRE_EOS_NL);
        case 'b': return ire_new(IRE_WB);
        case 'B': return ire_new(IRE_NWB);
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
            ire_node_t *n = ire_new(IRE_CLASS);
            bm_add_special(n->u.cls.bm, e);
            return n;
        }
        case 'k': {
            /* \k<name> backref by name — treat all named refs as group 1
             * since we don't track names; this is a deliberate v1 limitation. */
            if (q->p < q->end && *q->p == '<') {
                q->p++;
                while (q->p < q->end && *q->p != '>') q->p++;
                if (q->p < q->end) q->p++;
            }
            ire_node_t *n = ire_new(IRE_BACKREF);
            n->u.backref.idx = 1;
            return n;
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            ire_node_t *n = ire_new(IRE_BACKREF);
            n->u.backref.idx = e - '0';
            return n;
        }
        case 'n': case 't': case 'r': case 'f': case 'v':
        case 'a': case 'e': case '0':
        case '\\': case '/': case '.': case '^': case '$':
        case '(': case ')': case '[': case ']':
        case '{': case '}': case '|': case '*': case '+':
        case '?': case '-': case ' ':
        case 'x': {
            uint8_t b;
            switch (e) {
            case 'n': b = '\n'; break;
            case 't': b = '\t'; break;
            case 'r': b = '\r'; break;
            case 'f': b = '\f'; break;
            case 'v': b = '\v'; break;
            case 'a': b = '\a'; break;
            case 'e': b = 0x1b; break;
            case '0': b = 0;    break;
            case 'x': {
                if (q->p + 2 > q->end) { re_error(q, "bad \\x"); return NULL; }
                char h[3] = { (char)q->p[0], (char)q->p[1], 0 };
                q->p += 2;
                b = (uint8_t)strtol(h, NULL, 16);
                break;
            }
            default: b = (uint8_t)e; break;
            }
            ire_node_t *n = ire_new(IRE_LIT);
            n->u.lit.bytes = (char *)malloc(1);
            n->u.lit.bytes[0] = (char)b;
            n->u.lit.len = 1;
            n->u.lit.ci = q->case_insensitive;
            return n;
        }
        default: {
            /* Unknown escape — treat as literal char, like Ruby. */
            ire_node_t *n = ire_new(IRE_LIT);
            n->u.lit.bytes = (char *)malloc(1);
            n->u.lit.bytes[0] = (char)e;
            n->u.lit.len = 1;
            n->u.lit.ci = q->case_insensitive;
            return n;
        }
        }
    }
    case '|': case ')':
        return ire_new(IRE_EMPTY);
    case '*': case '+': case '?': case '{':
        re_error(q, "quantifier with no operand");
        return NULL;
    default: {
        /* Plain literal byte (or first byte of multi-byte UTF-8 sequence) */
        re_get(q);
        ire_node_t *n = ire_new(IRE_LIT);
        size_t cap = 4;
        n->u.lit.bytes = (char *)malloc(cap);
        n->u.lit.bytes[0] = (char)c;
        n->u.lit.len = 1;
        n->u.lit.ci = q->case_insensitive;
        /* For multi-byte UTF-8: gobble continuation bytes too, so they
         * stay together as one literal token (so a quantifier following
         * binds to the whole codepoint, matching Ruby semantics). */
        int adv = 0;
        if (q->encoding == AGRE_ENC_UTF8) {
            uint8_t b = (uint8_t)c;
            if      (b < 0x80)         adv = 0;
            else if ((b & 0xE0)==0xC0) adv = 1;
            else if ((b & 0xF0)==0xE0) adv = 2;
            else if ((b & 0xF8)==0xF0) adv = 3;
        }
        for (int i = 0; i < adv && q->p < q->end; i++) {
            if (n->u.lit.len + 1 >= cap) {
                cap *= 2;
                n->u.lit.bytes = (char *)realloc(n->u.lit.bytes, cap);
            }
            n->u.lit.bytes[n->u.lit.len++] = (char)*q->p++;
        }
        return n;
    }
    }
}

/* Try to consume a quantifier *after* an atom; if found, wrap in IRE_REP. */
static ire_node_t *parse_quantifier(re_parser_t *q, ire_node_t *atom) {
    re_skip_ws(q);
    int c = re_peek(q);
    int32_t mn, mx; bool greedy = true;
    if (c == '*')      { q->p++; mn = 0; mx = -1; }
    else if (c == '+') { q->p++; mn = 1; mx = -1; }
    else if (c == '?') { q->p++; mn = 0; mx = 1; }
    else if (c == '{') {
        const uint8_t *save = q->p;
        q->p++;
        if (!parse_braces(q, &mn, &mx)) {
            q->p = save;
            return atom;
        }
    } else {
        return atom;
    }
    if (re_peek(q) == '?') { q->p++; greedy = false; }
    else if (re_peek(q) == '+') { q->p++; /* possessive — degrade to greedy v1 */ }

    ire_node_t *r = ire_new(IRE_REP);
    r->u.rep.body = atom;
    r->u.rep.min = mn;
    r->u.rep.max = mx;
    r->u.rep.greedy = greedy;
    return r;
}

static ire_node_t *parse_concat(re_parser_t *q) {
    ire_node_t *cat = ire_new(IRE_CONCAT);
    while (q->p < q->end) {
        re_skip_ws(q);
        int c = re_peek(q);
        if (c < 0 || c == '|' || c == ')') break;
        ire_node_t *atom = parse_atom(q);
        if (q->error || !atom) return cat;
        atom = parse_quantifier(q, atom);
        if (atom->kind == IRE_EMPTY) continue;

        /* Coalesce consecutive literals (saves nodes and improves locality). */
        if (atom->kind == IRE_LIT && cat->u.cat.n > 0) {
            ire_node_t *prev = cat->u.cat.xs[cat->u.cat.n - 1];
            if (prev->kind == IRE_LIT && prev->u.lit.ci == atom->u.lit.ci) {
                prev->u.lit.bytes = (char *)realloc(prev->u.lit.bytes, prev->u.lit.len + atom->u.lit.len);
                memcpy(prev->u.lit.bytes + prev->u.lit.len, atom->u.lit.bytes, atom->u.lit.len);
                prev->u.lit.len += atom->u.lit.len;
                free(atom->u.lit.bytes);
                free(atom);
                continue;
            }
        }
        ire_cat_push(cat, atom);
    }
    return cat;
}

static ire_node_t *parse_alt(re_parser_t *q) {
    ire_node_t *left = parse_concat(q);
    if (re_peek(q) != '|') return left;
    q->p++;
    ire_node_t *right = parse_alt(q);
    ire_node_t *a = ire_new(IRE_ALT);
    a->u.alt.l = left;
    a->u.alt.r = right;
    return a;
}

/* ------------------------------------------------------------------ */
/* First-byte / first-prefix analysis                                  */
/* ------------------------------------------------------------------ */

/* Walk the IR and collect the longest-known fixed literal prefix that
 * any successful match must begin with.  Used by astrogre_parse to
 * emit a memchr- or memmem-prefiltered grep_search variant.
 *
 * Limits (intentional, keep the analysis tractable):
 *   - case-insensitive flag disables the prefilter (we'd need to
 *     scan for both cases, which memchr can't do; doable with a
 *     16-byte PSHUFB scan, deferred).
 *   - alternation, lookahead/behind, backref, optional rep, dot
 *     all stop prefix accumulation.
 *   - anchors (\A ^ $ etc.) are zero-width and we walk through them.
 *   - a `(...)` group inherits its body's prefix. */
typedef struct {
    char bytes[64];
    size_t len;
    bool ci;
} fixed_prefix_t;

static void
fixed_prefix_append(fixed_prefix_t *fp, const char *s, size_t n, bool ci)
{
    if (fp->len > 0 && fp->ci != ci) return;
    size_t cap = sizeof(fp->bytes);
    if (fp->len >= cap) return;
    size_t take = (fp->len + n > cap) ? cap - fp->len : n;
    memcpy(fp->bytes + fp->len, s, take);
    fp->len += take;
    fp->ci = ci;
}

/* If `bm` is a single contiguous run of set bits (like `[a-z]`),
 * write the lo/hi bounds and return true.  Otherwise return false. */
static bool
bm_is_single_range(const uint64_t bm[4], uint8_t *out_lo, uint8_t *out_hi)
{
    int lo = -1, hi = -1;
    bool seen_gap = false;
    for (int i = 0; i < 256; i++) {
        bool set = (bm[i >> 6] >> (i & 63)) & 1ULL;
        if (set) {
            if (lo < 0) lo = i;
            else if (seen_gap) return false;     /* second run starts here */
            hi = i;
        } else if (lo >= 0) {
            seen_gap = true;                     /* the run has ended */
        }
    }
    if (lo < 0) return false;
    *out_lo = (uint8_t)lo;
    *out_hi = (uint8_t)hi;
    return true;
}

/* Collect the set of distinct bytes that could appear at the start of
 * any successful match.  Returns true only when the set is small
 * (≤ 8 distinct bytes) and known with certainty — used to emit
 * node_grep_search_byteset for alt-led patterns like
 * `(if|else|for|while|return)`.
 *
 * Anything that could match an arbitrary byte at position 0 (dot,
 * large class, `/i`-led literal, rep min=0) returns false. */
static bool
ire_collect_first_byte_set(ire_node_t *n, uint8_t *out, int *out_n)
{
    if (!n) return false;
    switch (n->kind) {
    case IRE_LIT:
        if (n->u.lit.len == 0 || n->u.lit.ci) return false;
        {
            uint8_t b = (uint8_t)n->u.lit.bytes[0];
            for (int i = 0; i < *out_n; i++) if (out[i] == b) return true;
            if (*out_n >= 8) return false;
            out[(*out_n)++] = b;
            return true;
        }
    case IRE_ALT:
        return ire_collect_first_byte_set(n->u.alt.l, out, out_n)
            && ire_collect_first_byte_set(n->u.alt.r, out, out_n);
    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) {
            ire_node_t *c = n->u.cat.xs[i];
            switch (c->kind) {
            case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
            case IRE_BOL: case IRE_EOL:
            case IRE_WB: case IRE_NWB:
            case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
            case IRE_EMPTY:
                continue;
            default:
                return ire_collect_first_byte_set(c, out, out_n);
            }
        }
        return false;
    case IRE_GROUP:    return ire_collect_first_byte_set(n->u.group.body, out, out_n);
    case IRE_NCGROUP:  return ire_collect_first_byte_set(n->u.nc.body, out, out_n);
    case IRE_REP:
        if (n->u.rep.min >= 1) return ire_collect_first_byte_set(n->u.rep.body, out, out_n);
        return false;
    case IRE_CLASS: {
        /* Only count classes with ≤8 set bits as a fixed first-byte set. */
        int n_set = 0;
        for (int b = 0; b < 256; b++) {
            if ((n->u.cls.bm[b >> 6] >> (b & 63)) & 1ULL) {
                if (n_set >= 8) return false;
                bool dup = false;
                for (int i = 0; i < *out_n; i++) if (out[i] == (uint8_t)b) { dup = true; break; }
                if (!dup) {
                    if (*out_n >= 8) return false;
                    out[(*out_n)++] = (uint8_t)b;
                }
                n_set++;
            }
        }
        return n_set > 0;
    }
    default: return false;
    }
}

/* If the first thing the IR consumes is an IRE_CLASS, return that
 * class node (so the caller can pick a SIMD-class-scan variant
 * without having to re-walk).  Returns NULL if the first consuming
 * thing isn't a single class. */
static ire_node_t *
ire_first_class(ire_node_t *n)
{
    if (!n) return NULL;
    switch (n->kind) {
    case IRE_CLASS:    return n;
    case IRE_LIT:      return NULL;          /* literal-led — handled by memchr/memmem */
    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) {
            ire_node_t *c = n->u.cat.xs[i];
            switch (c->kind) {
            /* zero-width — walk past */
            case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
            case IRE_BOL: case IRE_EOL:
            case IRE_WB: case IRE_NWB:
            case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
            case IRE_EMPTY:
                continue;
            case IRE_GROUP:    return ire_first_class(c->u.group.body);
            case IRE_NCGROUP:  return ire_first_class(c->u.nc.body);
            case IRE_REP:
                if (c->u.rep.min >= 1) return ire_first_class(c->u.rep.body);
                return NULL;
            case IRE_CLASS:    return c;
            default:           return NULL;
            }
        }
        return NULL;
    case IRE_GROUP:    return ire_first_class(n->u.group.body);
    case IRE_NCGROUP:  return ire_first_class(n->u.nc.body);
    case IRE_REP:
        if (n->u.rep.min >= 1) return ire_first_class(n->u.rep.body);
        return NULL;
    default:           return NULL;
    }
}

/* Returns true when the IR consumes some bytes deterministically — i.e.
 * the prefix is settled and the caller should stop walking siblings.
 * Returns false when the IR is zero-width / empty-able, in which case
 * the caller may continue walking.
 * `*consumes_anything` is set to true if some non-empty consumption
 * happens anywhere in the tree (used to gate the prefilter — pure
 * zero-width patterns shouldn't get a literal scan). */
static bool
ire_collect_prefix(ire_node_t *n, fixed_prefix_t *out, bool *consumes_anything)
{
    if (!n) return false;
    switch (n->kind) {
    case IRE_LIT:
        if (n->u.lit.len == 0) return false;
        if (out->len > 0 && out->ci != n->u.lit.ci) {
            *consumes_anything = true;
            return true;
        }
        fixed_prefix_append(out, n->u.lit.bytes, n->u.lit.len, n->u.lit.ci);
        *consumes_anything = true;
        return true;

    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) {
            bool consumed = false;
            bool done = ire_collect_prefix(n->u.cat.xs[i], out, &consumed);
            if (consumed) *consumes_anything = true;
            if (done) return true;
        }
        return false;

    case IRE_GROUP:
        return ire_collect_prefix(n->u.group.body, out, consumes_anything);
    case IRE_NCGROUP:
        return ire_collect_prefix(n->u.nc.body, out, consumes_anything);

    case IRE_REP:
        /* min >= 1 → body is guaranteed to run at least once and its
         * prefix appears at the start of the match.
         *
         * min == 0 → tricky.  We can't continue past the rep to grab
         * the next sibling's prefix, because the *leftmost match*
         * might start before that sibling (`/(a|ab)*c/` on "abc"
         * matches at offset 0 with iter=1, so the match's first byte
         * is 'a', not 'c').  Stop accumulation right here and let
         * whatever prefix we already have from earlier siblings (if
         * any) drive the prefilter. */
        if (n->u.rep.min >= 1) {
            return ire_collect_prefix(n->u.rep.body, out, consumes_anything);
        }
        return true;

    /* Zero-width — walk past. */
    case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
    case IRE_BOL: case IRE_EOL:
    case IRE_WB: case IRE_NWB:
    case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
    case IRE_EMPTY:
        return false;

    /* Everything else: stops prefix accumulation. */
    case IRE_DOT:
    case IRE_CLASS:
    case IRE_ALT:
    case IRE_BACKREF:
    default:
        *consumes_anything = true;
        return true;
    }
}

/* ------------------------------------------------------------------ */
/* Lower IR -> AST (continuation-passing assembly)                     */
/* ------------------------------------------------------------------ */

typedef struct lower_ctx {
    re_parser_t *q;
    bool ci;
    bool ml;
    agre_encoding_t enc;
    NODE *rep_cont;
} lower_ctx_t;

static NODE *lower(lower_ctx_t *L, ire_node_t *n, NODE *tail);

static NODE *make_dot(lower_ctx_t *L, NODE *tail) {
    if (L->enc == AGRE_ENC_UTF8) {
        return L->ml ? ALLOC_node_re_dot_utf8_m(tail) : ALLOC_node_re_dot_utf8(tail);
    }
    return L->ml ? ALLOC_node_re_dot_m(tail) : ALLOC_node_re_dot(tail);
}

static NODE *
lower_class(lower_ctx_t *L, ire_node_t *n, NODE *tail)
{
    return ALLOC_node_re_class(n->u.cls.bm[0], n->u.cls.bm[1], n->u.cls.bm[2], n->u.cls.bm[3], tail);
}

static NODE *
lower_concat(lower_ctx_t *L, ire_node_t *cat, NODE *tail)
{
    /* Right-to-left: each child's tail = previously-built chain. */
    NODE *t = tail;
    for (size_t i = cat->u.cat.n; i-- > 0;) {
        t = lower(L, cat->u.cat.xs[i], t);
    }
    return t;
}

static NODE *
lower(lower_ctx_t *L, ire_node_t *n, NODE *tail)
{
    if (!n) return tail;
    switch (n->kind) {
    case IRE_EMPTY:    return tail;
    case IRE_LIT: {
        char *bytes = (char *)malloc(n->u.lit.len + 1);
        memcpy(bytes, n->u.lit.bytes, n->u.lit.len);
        bytes[n->u.lit.len] = 0;
        if (n->u.lit.ci) {
            /* Pre-fold to lowercase so the matcher's CI compare is asymmetric. */
            for (uint32_t i = 0; i < n->u.lit.len; i++) {
                if ((uint8_t)bytes[i] >= 'A' && (uint8_t)bytes[i] <= 'Z') bytes[i] += 32;
            }
            return ALLOC_node_re_lit_ci(bytes, n->u.lit.len, tail);
        }
        return ALLOC_node_re_lit(bytes, n->u.lit.len, tail);
    }
    case IRE_DOT:           return make_dot(L, tail);
    case IRE_CLASS:         return lower_class(L, n, tail);
    case IRE_BOS:           return ALLOC_node_re_bos(tail);
    case IRE_EOS:           return ALLOC_node_re_eos(tail);
    case IRE_EOS_NL:        return ALLOC_node_re_eos_nl(tail);
    case IRE_BOL:           return ALLOC_node_re_bol(tail);
    case IRE_EOL:           return ALLOC_node_re_eol(tail);
    case IRE_WB:            return ALLOC_node_re_word_boundary(tail);
    case IRE_NWB:           return ALLOC_node_re_non_word_boundary(tail);
    case IRE_BACKREF:       return ALLOC_node_re_backref(n->u.backref.idx, tail);
    case IRE_CONCAT:        return lower_concat(L, n, tail);
    case IRE_ALT: {
        /* Both branches embed their own (shared) tail. */
        NODE *l = lower(L, n->u.alt.l, tail);
        NODE *r = lower(L, n->u.alt.r, tail);
        return ALLOC_node_re_alt(l, r);
    }
    case IRE_GROUP: {
        /* cap_start; body; cap_end; tail */
        NODE *body = lower(L, n->u.group.body,
                           ALLOC_node_re_cap_end(n->u.group.idx, tail));
        return ALLOC_node_re_cap_start(n->u.group.idx, body);
    }
    case IRE_NCGROUP:       return lower(L, n->u.nc.body, tail);
    case IRE_LOOKAHEAD: {
        NODE *body = lower(L, n->u.nc.body, ALLOC_node_re_succ());
        return ALLOC_node_re_lookahead(body, tail);
    }
    case IRE_NEG_LOOKAHEAD: {
        NODE *body = lower(L, n->u.nc.body, ALLOC_node_re_succ());
        return ALLOC_node_re_neg_lookahead(body, tail);
    }
    case IRE_REP: {
        NODE *body = lower(L, n->u.rep.body, L->rep_cont);
        return ALLOC_node_re_rep(body, tail, n->u.rep.min, n->u.rep.max, n->u.rep.greedy ? 1 : 0);
    }
    }
    return tail;
}

/* ------------------------------------------------------------------ */
/* IR free                                                             */
/* ------------------------------------------------------------------ */

static void ire_free(ire_node_t *n) {
    if (!n) return;
    switch (n->kind) {
    case IRE_LIT: free(n->u.lit.bytes); break;
    case IRE_ALT: ire_free(n->u.alt.l); ire_free(n->u.alt.r); break;
    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) ire_free(n->u.cat.xs[i]);
        free(n->u.cat.xs);
        break;
    case IRE_REP: ire_free(n->u.rep.body); break;
    case IRE_GROUP: ire_free(n->u.group.body); break;
    case IRE_NCGROUP: case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
        ire_free(n->u.nc.body); break;
    default: break;
    }
    free(n);
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

NODE *astrogre_rep_cont_singleton(void);  /* defined in match.c */

astrogre_pattern *
astrogre_parse(const char *pat, size_t pat_len, uint32_t prism_flags)
{
    re_parser_t q = {0};
    q.p   = (const uint8_t *)pat;
    q.end = q.p + pat_len;
    q.case_insensitive = (prism_flags & PR_FLAGS_IGNORE_CASE) != 0;
    q.multiline        = (prism_flags & PR_FLAGS_MULTI_LINE)  != 0;
    q.extended         = (prism_flags & PR_FLAGS_EXTENDED)    != 0;
    if (prism_flags & PR_FLAGS_ASCII_8BIT) q.encoding = AGRE_ENC_ASCII;
    else                                    q.encoding = AGRE_ENC_UTF8;
    /* prism's UTF-8 / forced encoding bits both map to UTF-8 here */

    /* Detect leading \A so search loop can early-out on first iteration. */
    bool anchored_bos = (pat_len >= 2 && pat[0] == '\\' && pat[1] == 'A');

    ire_node_t *ir = parse_alt(&q);
    if (q.error) {
        fprintf(stderr, "%s\n", q.errbuf);
        ire_free(ir);
        return NULL;
    }
    if (q.p != q.end) {
        fprintf(stderr, "regex parse: trailing input at offset %ld\n", (long)(q.p - (const uint8_t *)pat));
        ire_free(ir);
        return NULL;
    }

    lower_ctx_t L = {0};
    L.q = &q;
    L.ci = q.case_insensitive;
    L.ml = q.multiline;
    L.enc = q.encoding;
    L.rep_cont = astrogre_rep_cont_singleton();

    NODE *succ = ALLOC_node_re_succ();
    /* Wrap the whole pattern in capture group 0 so the matcher records
     * the final-match span automatically through cap_end. */
    NODE *body = lower(&L, ir, ALLOC_node_re_cap_end(0, succ));
    body = ALLOC_node_re_cap_start(0, body);

    /* Pick the right wrapper based on what the IR analysis turned up.
     * Order from most specific / fastest to least:
     *
     *   memmem       (>= 4-byte literal prefix, no /i)
     *   memchr       (>= 1-byte literal prefix, no /i)
     *   range scan   (single contiguous-range class as first thing)
     *   plain        (everything else)
     *
     * The specialiser bakes the prefix bytes / first-byte constants
     * / lo-hi into the resulting SD; for the SIMD variants, those
     * constants become the AVX2 set1 immediates so the scan loop is
     * a tight `vmovdqu / vpsubusb / vpminub / vpcmpeqb / vpmovmskb`. */
    fixed_prefix_t fp = {0};
    bool consumes = false;
    ire_collect_prefix(ir, &fp, &consumes);
    NODE *root;
    uint32_t a = anchored_bos ? 1 : 0;
    if (consumes && !fp.ci && fp.len >= 4) {
        char *needle = (char *)malloc(fp.len + 1);
        memcpy(needle, fp.bytes, fp.len); needle[fp.len] = 0;
        root = ALLOC_node_grep_search_memmem(body, needle, (uint32_t)fp.len, a);
    }
    else if (consumes && !fp.ci && fp.len >= 1) {
        root = ALLOC_node_grep_search_memchr(body, (uint32_t)(uint8_t)fp.bytes[0], a);
    }
    else {
        /* Try byteset (small first-byte set, e.g. alt-led patterns). */
        uint8_t bset[8]; int bset_n = 0;
        bool have_byteset = ire_collect_first_byte_set(ir, bset, &bset_n) && bset_n > 0;

        ire_node_t *cls = ire_first_class(ir);
        uint8_t lo = 0, hi = 0;
        bool have_range = cls && bm_is_single_range(cls->u.cls.bm, &lo, &hi);

        if (have_byteset && bset_n <= 8 &&
            /* Prefer range scan when there's only a single contiguous
             * range; range gets one cmp/iter vs N=8 cmps for byteset. */
            !(have_range && (uint32_t)(hi - lo + 1) == (uint32_t)bset_n)) {
            uint64_t packed = 0;
            for (int i = 0; i < bset_n; i++) packed |= ((uint64_t)bset[i]) << (i * 8);
            root = ALLOC_node_grep_search_byteset(body, packed, (uint32_t)bset_n, a);
        }
        else if (have_range) {
            root = ALLOC_node_grep_search_range(body, (uint32_t)lo, (uint32_t)hi, a);
        }
        else {
            root = ALLOC_node_grep_search(body, a);
        }
    }

    int n_groups = q.n_groups;
    ire_free(ir);

    astrogre_pattern *p = (astrogre_pattern *)calloc(1, sizeof(*p));
    p->root = root;
    p->n_groups = n_groups;
    p->case_insensitive = q.case_insensitive;
    p->multiline = q.multiline;
    p->encoding = q.encoding;
    p->anchored_bos = anchored_bos;
    p->pat = (char *)malloc(pat_len + 1);
    memcpy(p->pat, pat, pat_len);
    p->pat[pat_len] = 0;
    return p;
}

astrogre_pattern *
astrogre_parse_fixed(const char *bytes, size_t len, uint32_t prism_flags)
{
    bool ci = (prism_flags & PR_FLAGS_IGNORE_CASE) != 0;
    agre_encoding_t enc = (prism_flags & PR_FLAGS_ASCII_8BIT) ? AGRE_ENC_ASCII : AGRE_ENC_UTF8;

    /* Pre-fold for /i so the matcher's CI compare is asymmetric. */
    char *buf = (char *)malloc(len + 1);
    memcpy(buf, bytes, len);
    buf[len] = 0;
    if (ci) {
        for (size_t i = 0; i < len; i++) {
            uint8_t b = (uint8_t)buf[i];
            if (b >= 'A' && b <= 'Z') buf[i] = (char)(b + 32);
        }
    }

    NODE *succ = ALLOC_node_re_succ();
    NODE *cap_end = ALLOC_node_re_cap_end(0, succ);
    NODE *lit = ci ? ALLOC_node_re_lit_ci(buf, (uint32_t)len, cap_end)
                   : ALLOC_node_re_lit   (buf, (uint32_t)len, cap_end);
    NODE *body = ALLOC_node_re_cap_start(0, lit);
    NODE *root = ALLOC_node_grep_search(body, 0);

    astrogre_pattern *p = (astrogre_pattern *)calloc(1, sizeof(*p));
    p->root = root;
    p->n_groups = 0;
    p->case_insensitive = ci;
    p->multiline = false;
    p->encoding = enc;
    p->anchored_bos = false;
    p->fixed_string = true;
    p->pat = (char *)malloc(len + 1);
    memcpy(p->pat, bytes, len);
    p->pat[len] = 0;
    return p;
}

astrogre_pattern *
astrogre_parse_literal(const char *src, size_t len)
{
    /* /pat/flags */
    if (len < 2 || src[0] != '/') {
        fprintf(stderr, "astrogre: expected /pat/flags syntax\n");
        return NULL;
    }
    size_t i = 1;
    while (i < len && src[i] != '/') {
        if (src[i] == '\\' && i + 1 < len) i++;
        i++;
    }
    if (i >= len) {
        fprintf(stderr, "astrogre: missing closing /\n");
        return NULL;
    }
    size_t pat_start = 1, pat_end = i;
    uint32_t flags = 0;
    for (size_t j = i + 1; j < len; j++) {
        switch (src[j]) {
        case 'i': flags |= PR_FLAGS_IGNORE_CASE; break;
        case 'm': flags |= PR_FLAGS_MULTI_LINE; break;
        case 'x': flags |= PR_FLAGS_EXTENDED; break;
        case 'n': flags |= PR_FLAGS_ASCII_8BIT; break;
        case 'u': flags |= PR_FLAGS_UTF_8; break;
        default: break;
        }
    }
    return astrogre_parse(src + pat_start, pat_end - pat_start, flags);
}

/* ------------------------------------------------------------------ */
/* Prism-driven path                                                   */
/* ------------------------------------------------------------------ */

struct find_re_ctx { const pm_node_t *found; };
static bool find_re_visit(const pm_node_t *node, void *data) {
    struct find_re_ctx *fr = (struct find_re_ctx *)data;
    if (fr->found) return false;
    if (PM_NODE_TYPE(node) == PM_REGULAR_EXPRESSION_NODE) {
        fr->found = node;
        return false;
    }
    return true;  /* recurse into children */
}

static const pm_node_t *find_regex_node(const pm_node_t *node)
{
    struct find_re_ctx fr = { .found = NULL };
    pm_visit_node(node, find_re_visit, &fr);
    return fr.found;
}

astrogre_pattern *
astrogre_parse_via_prism(const char *src, size_t len)
{
    pm_parser_t parser;
    pm_options_t options = {0};
    pm_parser_init(&parser, (const uint8_t *)src, len, &options);
    pm_node_t *root = pm_parse(&parser);

    const pm_node_t *re = find_regex_node(root);
    astrogre_pattern *p = NULL;

    if (re) {
        const pm_regular_expression_node_t *r = (const pm_regular_expression_node_t *)re;
        const uint8_t *cs = pm_string_source(&r->unescaped);
        size_t        cl = pm_string_length(&r->unescaped);
        uint32_t flags = (uint32_t)re->flags;
        p = astrogre_parse((const char *)cs, cl, flags);
    } else {
        fprintf(stderr, "astrogre: no regex literal found in source\n");
    }

    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    return p;
}

void
astrogre_pattern_free(astrogre_pattern *p)
{
    if (!p) return;
    free(p->pat);
    free(p);
}

uint64_t
astrogre_pattern_hash(astrogre_pattern *p)
{
    if (!p || !p->root) return 0;
    return (uint64_t)HASH(p->root);
}

extern void astro_cs_compile(NODE *entry, const char *file);
extern void astro_cs_build(const char *extra_cflags);
extern void astro_cs_reload(void);
extern bool astro_cs_load(NODE *n, const char *file);
extern void astrogre_export_all_sds(void);
extern void astrogre_reload_all_dispatchers(void);

void
astrogre_pattern_aot_compile(astrogre_pattern *p, bool verbose)
{
    if (!p || !p->root) return;
    if (verbose) {
        fprintf(stderr, "astrogre: cs_compile h=%016lx /%s/\n",
                (unsigned long)HASH(p->root), p->pat ? p->pat : "");
    }
    astro_cs_compile(p->root, NULL);
    /* Make every inner SD externally visible so cs_load patches the
     * whole chain, not just the root.  See astrogre_export_sd_wrappers
     * in node.c (borrowed from luastro). */
    astrogre_export_all_sds();
    astro_cs_build(NULL);
    astro_cs_reload();
    /* Re-resolve every node so this very run picks up the freshly-baked
     * SDs (otherwise only the *next* invocation benefits, since the
     * inner nodes' dispatchers were locked in at allocation time). */
    astrogre_reload_all_dispatchers();
}
