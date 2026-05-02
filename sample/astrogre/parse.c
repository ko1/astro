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
    IRE_LOOKBEHIND,
    IRE_NEG_LOOKBEHIND,
    IRE_ATOMIC,         /* (?>BODY) — body commits, no backtrack */
    IRE_BOS,
    IRE_EOS,
    IRE_EOS_NL,
    IRE_BOL,
    IRE_EOL,
    IRE_WB,
    IRE_NWB,
    IRE_BACKREF,
    IRE_EMPTY,
    IRE_KEEP,           /* \K — reset whole-match start at current pos */
    IRE_LAST_MATCH,     /* \G — anchor at scan_start */
    IRE_CONDITIONAL,    /* (?(N)yes|no) — branch on capture validity */
    IRE_SUBROUTINE,     /* \g<name> / \g<N> — re-evaluate a named group */
    IRE_ABSENCE,        /* (?~BODY) — match while BODY can't match here */
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
        /* Conditional (?(idx)yes|no): if capture group #idx is set,
         * dispatch the `yes` branch, else `no` (which is IRE_EMPTY when
         * only "yes" was given). */
        struct { int idx; struct ire_node *yes; struct ire_node *no; } cond;
        /* Subroutine call: lower walks pattern->groups_by_idx[idx] to
         * build a callable chain (cached) so recursion works at runtime. */
        struct { int idx; } sub;
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

    /* Named-capture (name, idx) pairs.  Names are heap dups; ownership
     * passes to the astrogre_pattern at the end of parsing. */
    char **names;
    int   *name_idx;
    int    n_names;
    int    cap_names;

    /* Group-by-index table — populated when each capture group's
     * IRE_GROUP node is fully built so backward `\g<N>` / `\g<name>`
     * references can resolve.  Forward references error out. */
    struct ire_node **groups_by_idx;
    int               cap_groups;
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
static void bm_and(uint64_t a[4], const uint64_t b[4]) {
    for (int i = 0; i < 4; i++) a[i] &= b[i];
}

/* Apply /i case-fold expansion to a single ASCII char */
static void bm_set_ci(uint64_t bm[4], uint8_t b, bool ci) {
    bm_set(bm, b);
    if (ci) {
        if (b >= 'A' && b <= 'Z') bm_set(bm, b + 32);
        else if (b >= 'a' && b <= 'z') bm_set(bm, b - 32);
    }
}

/* ------------------------------------------------------------------ */
/* Aho-Corasick — multi-literal prefilter.                             */
/* ------------------------------------------------------------------ */
/*
 * Used by node_grep_search_ac when the pattern's leading edge is an
 * alternation of two or more literal byte strings (e.g.
 * `cat|dog|match`, or `(error|warning|fatal):`).  The AC automaton
 * scans the haystack in a single pass and reports the start position
 * of each literal occurrence; the regex matcher then verifies from
 * that position.
 *
 * Trie shape: each state has a 256-entry child table (-1 = no child),
 * a `fail` link (longest proper suffix that's a trie prefix), and an
 * `output_len` (length of the pattern that ends at this state, or 0
 * if none).  No `output_link` chain in this MVP — we assume the
 * literal set is small enough that overlapping outputs are rare; the
 * matcher's body will retry adjacent positions if needed.
 *
 * Memory: ~1 KiB per state.  For the typical "alt of 3-10 short
 * literals" case the automaton has ~30-100 states (~30-100 KiB),
 * comfortable per-pattern.
 *
 * No baking: ASTro's operand encoding is scalar-only, so the AC
 * tables can't be folded into the AOT-specialised SD.  The node
 * carries the `ac_t *` as an opaque void* operand and the table
 * itself lives on the heap, owned by the pattern.  This is fine —
 * the inner AC scan is data-driven (one indirect load per byte),
 * and "data on the heap vs static const" makes no measurable
 * difference for a memory-bound loop.  The body chain that runs
 * AFTER an AC hit is the part that benefits from AOT, and it
 * specialises normally.
 */

typedef struct ac_state {
    int32_t children[256];   /* -1 if no transition */
    int32_t fail;            /* failure-link state index */
    int32_t output_len;      /* > 0 if a pattern ends here */
} ac_state_t;

typedef struct ac_t {
    ac_state_t *states;
    int         n_states;
    int         cap_states;
    int         min_pat_len;      /* shortest pattern, for early skip */
} ac_t;

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

static ac_t *
ac_build(const char *const *needles, const uint32_t *lens, int n)
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

    /* Phase 2: BFS from root to compute failure links. */
    int *queue = (int *)malloc(sizeof(int) * (size_t)ac->n_states);
    int qhead = 0, qtail = 0;
    /* Depth-1 children fail to root. */
    for (int c = 0; c < 256; c++) {
        int child = ac->states[0].children[c];
        if (child < 0) {
            ac->states[0].children[c] = 0;  /* root is its own failure for missing chars */
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
                /* Goto failure: when no transition, fall back to fail
                 * link's transition.  Fold it into children[c] so the
                 * scan loop is branchless (no inner while). */
                ac->states[u].children[c] = ac->states[ac->states[u].fail].children[c];
            } else {
                ac->states[v].fail = ac->states[ac->states[u].fail].children[c];
                /* Inherit output_len if the failure state has a longer
                 * pattern than us (overlap case). */
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

static void
ac_free(ac_t *ac)
{
    if (!ac) return;
    free(ac->states);
    free(ac);
}

/* Non-static — called from node_grep_search_ac in node.def via an
 * extern declaration.  Advances the scan from `*io_pos` until a
 * literal output is found or the haystack ends.  On success returns
 * true and writes the literal's start position to `*out_match_start`,
 * with `*io_pos`/`*io_state` set so the next call resumes immediately
 * after the match.  Returns false at end-of-input. */
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
            *io_pos          = pos;     /* resume one past last matched byte */
            *io_state        = state;
            return true;
        }
    }
    *io_pos = pos;
    *io_state = state;
    return false;
}

/* POSIX bracket-class names.  ASCII semantics only (no Unicode). */
static bool
bm_posix_class(const char *restrict name, size_t name_len, uint64_t out[restrict 4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    #define MATCH(s) (name_len == sizeof(s) - 1 && memcmp(name, s, sizeof(s) - 1) == 0)
    if      (MATCH("alpha"))  { bm_set_range(out, 'A', 'Z'); bm_set_range(out, 'a', 'z'); }
    else if (MATCH("digit"))  { bm_set_range(out, '0', '9'); }
    else if (MATCH("alnum"))  { bm_set_range(out, 'A', 'Z'); bm_set_range(out, 'a', 'z'); bm_set_range(out, '0', '9'); }
    else if (MATCH("upper"))  { bm_set_range(out, 'A', 'Z'); }
    else if (MATCH("lower"))  { bm_set_range(out, 'a', 'z'); }
    else if (MATCH("xdigit")) { bm_set_range(out, '0', '9'); bm_set_range(out, 'A', 'F'); bm_set_range(out, 'a', 'f'); }
    else if (MATCH("space"))  { bm_set(out, ' '); bm_set(out, '\t'); bm_set(out, '\n'); bm_set(out, '\v'); bm_set(out, '\f'); bm_set(out, '\r'); }
    else if (MATCH("blank"))  { bm_set(out, ' '); bm_set(out, '\t'); }
    else if (MATCH("print"))  { bm_set_range(out, 0x20, 0x7e); }
    else if (MATCH("graph"))  { bm_set_range(out, 0x21, 0x7e); }
    else if (MATCH("cntrl"))  { bm_set_range(out, 0x00, 0x1f); bm_set(out, 0x7f); }
    else if (MATCH("punct"))  {
        for (int i = 0x21; i <= 0x7e; i++) {
            const int alnum = (i >= '0' && i <= '9') || (i >= 'A' && i <= 'Z') || (i >= 'a' && i <= 'z');
            if (!alnum) bm_set(out, (uint8_t)i);
        }
    }
    else if (MATCH("ascii")) { bm_set_range(out, 0x00, 0x7f); }
    else if (MATCH("word"))  { bm_set_range(out, 'A', 'Z'); bm_set_range(out, 'a', 'z'); bm_set_range(out, '0', '9'); bm_set(out, '_'); }
    else { return false; }
    #undef MATCH
    return true;
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
    /* Onigmo extensions: \h / \H — hex digit shortcut. */
    case 'h':
        bm_set_range(bm, '0', '9');
        bm_set_range(bm, 'A', 'F');
        bm_set_range(bm, 'a', 'f');
        break;
    case 'H':
        for (int i = 0; i < 256; i++) {
            const int hex = (i >= '0' && i <= '9') || (i >= 'A' && i <= 'F') || (i >= 'a' && i <= 'f');
            if (!hex) bm_set(bm, (uint8_t)i);
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
    case 'h': case 'H':
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
    /* Inside a class, `\b` is backspace (0x08), not word-boundary. */
    case 'b': *out_byte = '\b'; return 0;
    case 'x': {
        if (q->p + 2 > q->end) { re_error(q, "bad \\x"); return 0; }
        char h[3] = { (char)q->p[0], (char)q->p[1], 0 };
        q->p += 2;
        *out_byte = (uint8_t)strtol(h, NULL, 16);
        return 0;
    }
    case 'u': {
        /* \uHHHH or \u{H...} inside [].  Our class is byte-level
         * so only ASCII codepoints (cp < 0x80) can be expressed
         * as a single bitmap entry; for multi-byte we would have
         * to special-case the matcher.  Match Onigmo's behavior
         * for ASCII; reject the rest with a clear error. */
        uint32_t cp = 0;
        if (q->p < q->end && *q->p == '{') {
            q->p++;
            int n = 0;
            while (q->p < q->end && *q->p != '}' && n < 8) {
                const int ch = *q->p;
                int d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else { re_error(q, "bad \\u{} hex digit in []"); return 0; }
                cp = (cp << 4) | (uint32_t)d;
                q->p++; n++;
            }
            if (q->p >= q->end || *q->p != '}') { re_error(q, "expected } in \\u{} in []"); return 0; }
            q->p++;
        } else {
            if (q->p + 4 > q->end) { re_error(q, "bad \\u in []: need 4 hex digits"); return 0; }
            for (int n = 0; n < 4; n++) {
                const int ch = q->p[n];
                int d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else { re_error(q, "bad \\u hex digit in []"); return 0; }
                cp = (cp << 4) | (uint32_t)d;
            }
            q->p += 4;
        }
        if (cp >= 0x80) {
            re_error(q, "multi-byte \\u inside [] is not supported");
            return 0;
        }
        *out_byte = (uint8_t)cp;
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

/* Forward decls: parse_class is called recursively for nested `[...]`
 * members and for `&&`-RHS sub-classes; ire_free disposes those
 * temporary nested-class nodes after we copy out their bitmap. */
static ire_node_t *parse_class(re_parser_t *q);
static void ire_free(ire_node_t *n);

/* Parse one class operand into `bm` until ']' (consumed) or '&&' (left
 * unconsumed for the caller).  Already past leading `[` and optional
 * `^` (the caller handles the operand-level negate).  Members include
 * literals, ranges, escapes, POSIX bracket classes, and nested `[...]`
 * (which contributes its bitmap by union).
 *
 * `&&` chains intersection: when seen, recursively parses the RHS
 * operand into a fresh bitmap and ANDs it in.  Recursion is left-
 * associative (`A&&B&&C` = `(A&&B)&&C`), which matches Onigmo. */
static void parse_class_body(re_parser_t *q, uint64_t bm[4]) {
    bool first = true;

    while (q->p < q->end) {
        int c = re_peek(q);

        if (c == ']' && !first) { q->p++; return; }

        /* `&&` set intersection.  Onigmo requires at least one prior
         * member (so `[&&x]` is `&` `&` `x`), which our `!first` guard
         * matches.  After consuming `&&`, recursively parse the RHS
         * into a fresh bitmap, then AND it in.  The recursive call
         * consumes the closing `]`, so we return immediately. */
        if (!first && q->p + 1 < q->end && q->p[0] == '&' && q->p[1] == '&') {
            q->p += 2;
            uint64_t rhs[4] = {0};
            parse_class_body(q, rhs);
            bm_and(bm, rhs);
            return;
        }

        /* POSIX bracket class: `[:NAME:]` or `[:^NAME:]`. */
        if (c == '[' && q->p + 1 < q->end && q->p[1] == ':') {
            const uint8_t *const save = q->p;
            q->p += 2;
            const bool neg = (re_peek(q) == '^');
            if (neg) q->p++;
            const uint8_t *const name_start = q->p;
            while (q->p < q->end && *q->p != ':' && *q->p != ']') q->p++;
            const size_t name_len = (size_t)(q->p - name_start);
            if (q->p + 1 < q->end && q->p[0] == ':' && q->p[1] == ']') {
                uint64_t mask[4];
                if (bm_posix_class((const char *)name_start, name_len, mask)) {
                    q->p += 2;  /* consume `:]` */
                    if (neg) bm_invert(mask);
                    bm_or(bm, mask);
                    first = false;
                    continue;
                }
                re_error(q, "unknown POSIX class");
                return;
            }
            q->p = save;  /* restore; treat `[` as nested class below */
        }

        /* Nested `[...]` member: contributes its bitmap by union.
         * Used both standalone (e.g. `[abc[def]]`) and as the RHS of
         * `&&` (e.g. `[a-z&&[^aeiou]]`). */
        if (c == '[') {
            q->p++;  /* consume '[' */
            ire_node_t *nested = parse_class(q);
            bm_or(bm, nested->u.cls.bm);
            ire_free(nested);
            first = false;
            continue;
        }

        first = false;

        uint8_t byte_val = 0;
        bool got_byte = false;

        if (c == '\\') {
            q->p++;
            if (parse_class_escape(q, bm, &byte_val)) {
                continue;  /* set was applied */
            }
            got_byte = true;
        } else {
            re_get(q);
            byte_val = (uint8_t)c;
            got_byte = true;
        }

        if (!got_byte) continue;

        /* range? — but stop short of `&&` (Onigmo treats `x-&` as not
         * a range; here `&&` ends the operand even if it follows `-`). */
        const bool peek_amp = (q->p + 2 < q->end && q->p[0] == '-' &&
                               q->p[1] == '&' && q->p[2] == '&');
        if (q->p + 1 < q->end && q->p[0] == '-' && q->p[1] != ']' && !peek_amp) {
            q->p++;  /* consume '-' */
            uint8_t hi;
            int c2 = re_peek(q);
            if (c2 == '\\') {
                q->p++;
                uint64_t dummy[4] = {0};
                uint8_t hib;
                if (parse_class_escape(q, dummy, &hib)) {
                    re_error(q, "char-class escape in range");
                    return;
                }
                hi = hib;
            } else {
                re_get(q);
                hi = (uint8_t)c2;
            }
            uint8_t lo = byte_val;
            if (lo > hi) { re_error(q, "bad range"); return; }
            for (int i = lo; i <= hi; i++) {
                bm_set_ci(bm, (uint8_t)i, q->case_insensitive);
            }
        } else {
            bm_set_ci(bm, byte_val, q->case_insensitive);
        }
    }
    re_error(q, "unterminated character class");
}

static ire_node_t *parse_class(re_parser_t *q) {
    /* '[' already consumed */
    ire_node_t *n = ire_new(IRE_CLASS);
    bool negate = false;
    if (re_peek(q) == '^') { negate = true; q->p++; }
    parse_class_body(q, n->u.cls.bm);
    if (negate) bm_invert(n->u.cls.bm);
    return n;
}

static ire_node_t *parse_quantifier_target(re_parser_t *q);
static ire_node_t *parse_concat(re_parser_t *q);

/* Append (name, idx) to the parser's names table.  `name` is duped. */
static void
re_register_name(re_parser_t *q, const uint8_t *name_start, size_t name_len, int idx)
{
    if (q->n_names == q->cap_names) {
        const int new_cap = q->cap_names ? q->cap_names * 2 : 4;
        q->names    = (char **)realloc(q->names,    sizeof(char *) * (size_t)new_cap);
        q->name_idx = (int   *)realloc(q->name_idx, sizeof(int)    * (size_t)new_cap);
        q->cap_names = new_cap;
    }
    char *const dup = (char *)malloc(name_len + 1);
    memcpy(dup, name_start, name_len);
    dup[name_len] = '\0';
    q->names[q->n_names]    = dup;
    q->name_idx[q->n_names] = idx;
    q->n_names++;
}

/* Register a parsed IRE_GROUP for later \g<…> resolution. */
static void
re_register_group(re_parser_t *q, int idx, struct ire_node *g)
{
    if (idx >= q->cap_groups) {
        const int new_cap = idx + 8;
        q->groups_by_idx = (struct ire_node **)realloc(q->groups_by_idx,
                            sizeof(struct ire_node *) * (size_t)new_cap);
        for (int i = q->cap_groups; i < new_cap; i++) q->groups_by_idx[i] = NULL;
        q->cap_groups = new_cap;
    }
    q->groups_by_idx[idx] = g;
}

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
        bool lookbehind = false, neg_lookbehind = false;
        bool atomic = false;
        bool nc = false;
        int saved_ci = q->case_insensitive, saved_ml = q->multiline, saved_x = q->extended;
        bool saved_flags = false;
        const uint8_t *pending_name_start = NULL;
        const uint8_t *pending_name_end   = NULL;

        if (q->p < q->end && *q->p == '?') {
            q->p++;
            if (q->p < q->end && *q->p == '#') {
                /* (?#…) inline comment — read up to the matching ')'.
                 * Onigmo allows arbitrary bytes inside (including ')'
                 * if escaped with backslash); the simple form matches
                 * the docs and what most regexes use. */
                q->p++;
                while (q->p < q->end && *q->p != ')') {
                    if (*q->p == '\\' && q->p + 1 < q->end) q->p++;
                    q->p++;
                }
                if (q->p < q->end) q->p++;  /* consume ')' */
                return ire_new(IRE_EMPTY);
            } else if (q->p < q->end && *q->p == ':') {
                q->p++; capture = false; nc = true;
            } else if (q->p < q->end && *q->p == '=') {
                q->p++; capture = false; lookahead = true;
            } else if (q->p < q->end && *q->p == '!') {
                q->p++; capture = false; neg_lookahead = true;
            } else if (q->p < q->end && *q->p == '>') {
                q->p++; capture = false; atomic = true;
            } else if (q->p < q->end && *q->p == '~') {
                /* (?~BODY) — Onigmo absence operator.  Greedy match
                 * of input bytes such that BODY does not match
                 * starting anywhere inside the matched range.  We
                 * implement the simpler `(?:(?!BODY).)*` variant —
                 * see node_re_absence for the exact semantics. */
                q->p++;
                ire_node_t *body_inner = parse_alt(q);
                if (re_get(q) != ')') re_error(q, "expected ) closing (?~");
                ire_node_t *abs_node = ire_new(IRE_ABSENCE);
                abs_node->u.nc.body = body_inner;
                return abs_node;
            } else if (q->p < q->end && *q->p == '(') {
                /* (?(N)YES) / (?(N)YES|NO) / (?(<name>)YES|NO) — conditional. */
                q->p++;
                int cond_idx = 0;
                if (q->p < q->end && isdigit(*q->p)) {
                    while (q->p < q->end && isdigit(*q->p)) {
                        cond_idx = cond_idx * 10 + (*q->p++ - '0');
                    }
                } else if (q->p < q->end && *q->p == '<') {
                    q->p++;
                    const uint8_t *const ns = q->p;
                    while (q->p < q->end && *q->p != '>') q->p++;
                    const size_t nl = (size_t)(q->p - ns);
                    if (q->p < q->end) q->p++;
                    for (int i = 0; i < q->n_names; i++) {
                        if (strlen(q->names[i]) == nl &&
                            memcmp(q->names[i], ns, nl) == 0) {
                            cond_idx = q->name_idx[i];
                            break;
                        }
                    }
                    if (cond_idx == 0) { re_error(q, "unknown capture name in (?(...))"); return NULL; }
                } else {
                    re_error(q, "unsupported conditional form");
                    return NULL;
                }
                if (q->p >= q->end || *q->p != ')') { re_error(q, "expected ) in (?(...))"); return NULL; }
                q->p++;
                ire_node_t *yes_branch = parse_concat(q);
                ire_node_t *no_branch = NULL;
                if (q->p < q->end && *q->p == '|') {
                    q->p++;
                    no_branch = parse_concat(q);
                } else {
                    no_branch = ire_new(IRE_EMPTY);
                }
                if (q->p >= q->end || *q->p != ')') { re_error(q, "expected ) closing conditional"); return NULL; }
                q->p++;
                ire_node_t *cnode = ire_new(IRE_CONDITIONAL);
                cnode->u.cond.idx = cond_idx;
                cnode->u.cond.yes = yes_branch;
                cnode->u.cond.no  = no_branch;
                return cnode;
            } else if (q->p < q->end && *q->p == '<') {
                /* (?<name>...) named capture, (?<=...) / (?<!...) lookbehind. */
                q->p++;
                if (q->p < q->end && *q->p == '=') {
                    q->p++; capture = false; lookbehind = true;
                } else if (q->p < q->end && *q->p == '!') {
                    q->p++; capture = false; neg_lookbehind = true;
                } else {
                    pending_name_start = q->p;
                    while (q->p < q->end && *q->p != '>') q->p++;
                    pending_name_end = q->p;
                    if (q->p < q->end) q->p++;
                    capture = true;
                }
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
        if (capture) {
            idx = ++q->n_groups;
            if (pending_name_start) {
                re_register_name(q, pending_name_start,
                                 (size_t)(pending_name_end - pending_name_start), idx);
            }
        }
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
            re_register_group(q, idx, g);
            return g;
        } else if (lookahead) {
            ire_node_t *g = ire_new(IRE_LOOKAHEAD);
            g->u.nc.body = body;
            return g;
        } else if (neg_lookahead) {
            ire_node_t *g = ire_new(IRE_NEG_LOOKAHEAD);
            g->u.nc.body = body;
            return g;
        } else if (lookbehind) {
            ire_node_t *g = ire_new(IRE_LOOKBEHIND);
            g->u.nc.body = body;
            return g;
        } else if (neg_lookbehind) {
            ire_node_t *g = ire_new(IRE_NEG_LOOKBEHIND);
            g->u.nc.body = body;
            return g;
        } else if (atomic) {
            ire_node_t *g = ire_new(IRE_ATOMIC);
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
        case 'K': return ire_new(IRE_KEEP);
        case 'G': return ire_new(IRE_LAST_MATCH);
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        case 'h': case 'H': {
            ire_node_t *n = ire_new(IRE_CLASS);
            bm_add_special(n->u.cls.bm, e);
            return n;
        }
        case 'R': {
            /* Generic newline: `\r\n | [\n\v\f\r]`.  ASCII subset only. */
            ire_node_t *crlf = ire_new(IRE_LIT);
            crlf->u.lit.bytes = (char *)malloc(2);
            crlf->u.lit.bytes[0] = '\r';
            crlf->u.lit.bytes[1] = '\n';
            crlf->u.lit.len = 2;
            crlf->u.lit.ci = false;
            ire_node_t *cls = ire_new(IRE_CLASS);
            bm_set(cls->u.cls.bm, '\n'); bm_set(cls->u.cls.bm, '\v');
            bm_set(cls->u.cls.bm, '\f'); bm_set(cls->u.cls.bm, '\r');
            ire_node_t *alt = ire_new(IRE_ALT);
            alt->u.alt.l = crlf;
            alt->u.alt.r = cls;
            return alt;
        }
        case 'g': {
            /* \g<name> or \g<N> — subroutine call. */
            if (q->p >= q->end || *q->p != '<') {
                re_error(q, "expected `<` after `\\g`");
                return NULL;
            }
            q->p++;
            int idx = 0;
            if (q->p < q->end && isdigit(*q->p)) {
                while (q->p < q->end && isdigit(*q->p)) idx = idx * 10 + (*q->p++ - '0');
            } else {
                const uint8_t *const ns = q->p;
                while (q->p < q->end && *q->p != '>') q->p++;
                const size_t nl = (size_t)(q->p - ns);
                for (int i = 0; i < q->n_names; i++) {
                    if (strlen(q->names[i]) == nl && memcmp(q->names[i], ns, nl) == 0) {
                        idx = q->name_idx[i];
                        break;
                    }
                }
            }
            if (q->p < q->end && *q->p == '>') q->p++;
            if (idx == 0) { re_error(q, "unknown subroutine target"); return NULL; }
            ire_node_t *n = ire_new(IRE_SUBROUTINE);
            n->u.sub.idx = idx;
            return n;
        }
        case 'k': {
            /* \k<name> — named backref.  Resolve via q->names; default
             * to group 1 if name is unknown. */
            int idx = 1;
            if (q->p < q->end && *q->p == '<') {
                q->p++;
                const uint8_t *const ns = q->p;
                while (q->p < q->end && *q->p != '>') q->p++;
                const size_t nl = (size_t)(q->p - ns);
                if (q->p < q->end) q->p++;
                for (int i = 0; i < q->n_names; i++) {
                    if (strlen(q->names[i]) == nl && memcmp(q->names[i], ns, nl) == 0) {
                        idx = q->name_idx[i];
                        break;
                    }
                }
            }
            ire_node_t *n = ire_new(IRE_BACKREF);
            n->u.backref.idx = idx;
            return n;
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': {
            ire_node_t *n = ire_new(IRE_BACKREF);
            n->u.backref.idx = e - '0';
            return n;
        }
        case 'u': {
            /* \uHHHH or \u{H...} — Unicode codepoint.  Encode as
             * UTF-8 bytes (1-4) and emit as IRE_LIT.  This way the
             * matcher's existing byte-by-byte comparison works on
             * UTF-8 input without any class-side multibyte support. */
            uint32_t cp = 0;
            if (q->p < q->end && *q->p == '{') {
                q->p++;
                int n = 0;
                while (q->p < q->end && *q->p != '}' && n < 8) {
                    const int c = *q->p;
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else { re_error(q, "bad \\u{} hex digit"); return NULL; }
                    cp = (cp << 4) | (uint32_t)d;
                    q->p++;
                    n++;
                }
                if (q->p >= q->end || *q->p != '}') { re_error(q, "expected } in \\u{}"); return NULL; }
                q->p++;
            } else {
                if (q->p + 4 > q->end) { re_error(q, "bad \\u: need 4 hex digits"); return NULL; }
                for (int n = 0; n < 4; n++) {
                    const int c = q->p[n];
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else { re_error(q, "bad \\u hex digit"); return NULL; }
                    cp = (cp << 4) | (uint32_t)d;
                }
                q->p += 4;
            }
            /* Encode `cp` as UTF-8 (RFC 3629). */
            uint8_t bytes[4];
            int len = 0;
            if (cp < 0x80) {
                bytes[0] = (uint8_t)cp; len = 1;
            } else if (cp < 0x800) {
                bytes[0] = (uint8_t)(0xC0 | (cp >> 6));
                bytes[1] = (uint8_t)(0x80 | (cp & 0x3F));
                len = 2;
            } else if (cp < 0x10000) {
                bytes[0] = (uint8_t)(0xE0 | (cp >> 12));
                bytes[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                bytes[2] = (uint8_t)(0x80 | (cp & 0x3F));
                len = 3;
            } else if (cp < 0x110000) {
                bytes[0] = (uint8_t)(0xF0 | (cp >> 18));
                bytes[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                bytes[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                bytes[3] = (uint8_t)(0x80 | (cp & 0x3F));
                len = 4;
            } else {
                re_error(q, "\\u codepoint out of range");
                return NULL;
            }
            ire_node_t *n = ire_new(IRE_LIT);
            n->u.lit.bytes = (char *)malloc((size_t)len);
            memcpy(n->u.lit.bytes, bytes, (size_t)len);
            n->u.lit.len = (uint32_t)len;
            n->u.lit.ci = q->case_insensitive;
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
    bool braces = false;
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
        braces = true;
    } else {
        return atom;
    }
    bool possessive = false;
    if (re_peek(q) == '?') { q->p++; greedy = false; }
    /* Onigmo treats `\d{m,n}+` as `(\d{m,n})+` (nested rep), NOT
     * possessive — only `*+` `++` `?+` are possessive. */
    else if (!braces && re_peek(q) == '+') { q->p++; possessive = true; }

    ire_node_t *r = ire_new(IRE_REP);
    r->u.rep.body = atom;
    r->u.rep.min = mn;
    r->u.rep.max = mx;
    r->u.rep.greedy = greedy;
    if (possessive) {
        ire_node_t *atom_g = ire_new(IRE_ATOMIC);
        atom_g->u.nc.body = r;
        return atom_g;
    }
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
        /* Loop to allow stacked quantifiers — e.g. `\d{2,4}+` parses
         * as `(\d{2,4})+` (nested rep), not possessive. */
        for (;;) {
            ire_node_t *next_atom = parse_quantifier(q, atom);
            if (next_atom == atom) break;
            atom = next_atom;
        }
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
        if (n->u.lit.len == 0) return false;
        {
            const uint8_t b = (uint8_t)n->u.lit.bytes[0];
            /* For /i, the first byte can be either case of the letter
             * (the lit's stored byte is whatever the user typed; the
             * lower pass folds it to lowercase later, but we run before
             * that).  For non-letter bytes /i has no effect, so we add
             * just the one entry. */
            uint8_t alt = b;
            if (n->u.lit.ci) {
                if (b >= 'a' && b <= 'z')      alt = (uint8_t)(b - 32);
                else if (b >= 'A' && b <= 'Z') alt = (uint8_t)(b + 32);
            }
            const int n_to_add = (alt != b) ? 2 : 1;
            for (int pass = 0; pass < n_to_add; pass++) {
                const uint8_t cur = (pass == 0) ? b : alt;
                bool dup = false;
                for (int i = 0; i < *out_n; i++) if (out[i] == cur) { dup = true; break; }
                if (dup) continue;
                if (*out_n >= 8) return false;
                out[(*out_n)++] = cur;
            }
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
            case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
            case IRE_EMPTY:
            case IRE_KEEP:
            case IRE_LAST_MATCH:
                continue;
            default:
                return ire_collect_first_byte_set(c, out, out_n);
            }
        }
        return false;
    case IRE_GROUP:    return ire_collect_first_byte_set(n->u.group.body, out, out_n);
    case IRE_NCGROUP:
    case IRE_ATOMIC:   return ire_collect_first_byte_set(n->u.nc.body, out, out_n);
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

/* Walks an IR sub-tree and collects literal byte strings if the node
 * is essentially "alt-of-LIT" (possibly wrapped in groups).  On
 * success appends each literal's `{bytes, len}` to the out arrays
 * and returns true.  The pointers borrow from the IR — caller must
 * either copy the bytes before `ire_free` or use them only within
 * the parse-time lifetime.
 *
 * Bails out (returns false) when:
 *   - any branch is a non-literal
 *   - any branch is an empty literal (LIT len == 0)
 *   - the literals mix /i and non-/i (we only AC over consistent
 *     case; mixed-case alternations fall back to the existing
 *     first-byte-set scanner)
 *
 * `out_*` arrays grow geometrically.  `*all_ci` is set on the first
 * literal seen and verified against subsequent ones.
 */
static bool
ire_collect_alt_lits(ire_node_t *n,
                     const char ***out_bytes, uint32_t **out_lens,
                     int *out_n, int *out_cap,
                     bool *all_ci, bool *first)
{
    if (!n) return false;
    switch (n->kind) {
    case IRE_LIT:
        if (n->u.lit.len == 0) return false;
        if (*first) { *all_ci = n->u.lit.ci; *first = false; }
        else if (*all_ci != n->u.lit.ci) return false;
        if (*out_n == *out_cap) {
            *out_cap = *out_cap ? *out_cap * 2 : 4;
            *out_bytes = (const char **)realloc(*out_bytes, sizeof(char *) * (size_t)*out_cap);
            *out_lens  = (uint32_t *)   realloc(*out_lens,  sizeof(uint32_t) * (size_t)*out_cap);
        }
        (*out_bytes)[*out_n] = n->u.lit.bytes;
        (*out_lens)[*out_n]  = n->u.lit.len;
        (*out_n)++;
        return true;
    case IRE_ALT:
        return ire_collect_alt_lits(n->u.alt.l, out_bytes, out_lens, out_n, out_cap, all_ci, first)
            && ire_collect_alt_lits(n->u.alt.r, out_bytes, out_lens, out_n, out_cap, all_ci, first);
    case IRE_GROUP:
        return ire_collect_alt_lits(n->u.group.body, out_bytes, out_lens, out_n, out_cap, all_ci, first);
    case IRE_NCGROUP:
        return ire_collect_alt_lits(n->u.nc.body, out_bytes, out_lens, out_n, out_cap, all_ci, first);
    case IRE_CONCAT:
        /* parse_concat wraps every parsed atom in an IRE_CONCAT, even
         * single-atom branches like the `dog` in `cat|dog|match`.
         * Peel through single-element concats so the collector treats
         * `IRE_CONCAT[LIT "dog"]` as the LIT itself.  Multi-element
         * concats are not pure literals (the suffix would have to
         * verify) — bail. */
        if (n->u.cat.n != 1) return false;
        return ire_collect_alt_lits(n->u.cat.xs[0], out_bytes, out_lens, out_n, out_cap, all_ci, first);
    default:
        return false;
    }
}

/* Top-level entry: try to find an "alt-of-literals" at the leading
 * edge of the pattern.  Returns true (and fills the out arrays) if
 * the leading edge is exactly such an alt with >= 2 distinct
 * literals; false otherwise.
 *
 * "Leading edge" means: the very first thing the matcher consumes
 * is one of the literals.  This covers `(a|b|c)REST`, `a|b`, and
 * `(?:a|b|c)REST` shapes — but NOT `\A(a|b)` (the BOS prefix is
 * fine, we step past it) nor `[abc]REST` (that's class-scan's job,
 * single-byte).
 *
 * Caller frees `*out_bytes` and `*out_lens` (the `*out_bytes`
 * pointers themselves point into the IR — do NOT free those). */
static bool
ire_collect_leading_alt_lits(ire_node_t *root,
                             const char ***out_bytes, uint32_t **out_lens,
                             int *out_n, bool *all_ci)
{
    *out_bytes = NULL; *out_lens = NULL; *out_n = 0; *all_ci = false;
    int cap = 0;
    bool first = true;

    ire_node_t *cur = root;
    /* Peel one level of CONCAT — if the first elt is the alt, use it. */
    if (cur && cur->kind == IRE_CONCAT && cur->u.cat.n > 0) {
        cur = cur->u.cat.xs[0];
    }
    /* Peel the implicit group-0 wrapper that astrogre_parse adds. */
    while (cur && (cur->kind == IRE_GROUP || cur->kind == IRE_NCGROUP)) {
        cur = (cur->kind == IRE_GROUP) ? cur->u.group.body : cur->u.nc.body;
    }

    if (!cur || cur->kind != IRE_ALT) return false;

    if (!ire_collect_alt_lits(cur, out_bytes, out_lens, out_n, &cap, all_ci, &first)
        || *out_n < 2) {
        free(*out_bytes); free(*out_lens);
        *out_bytes = NULL; *out_lens = NULL; *out_n = 0;
        return false;
    }
    return true;
}

/* Build the Truffle nibble-lookup tables T_lo[16] and T_hi[16] from
 * a 256-bit class bitmap, then pack each table into two uint64s for
 * passing as node operands.  See node_grep_search_class_scan. */
static void
build_truffle_tables(const uint64_t bm[4], uint64_t out[4])
{
    uint8_t T_lo[16] = {0}, T_hi[16] = {0};
    for (int b = 0; b < 256; b++) {
        if ((bm[b >> 6] >> (b & 63)) & 1ULL) {
            int lo = b & 0xF, hi = b >> 4;
            uint8_t mask = (uint8_t)(1u << (hi & 7));
            T_lo[lo] |= mask;
            T_hi[hi] |= mask;
        }
    }
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int i = 0; i < 8; i++) {
        out[0] |= ((uint64_t)T_lo[i])     << (i * 8);
        out[1] |= ((uint64_t)T_lo[i + 8]) << (i * 8);
        out[2] |= ((uint64_t)T_hi[i])     << (i * 8);
        out[3] |= ((uint64_t)T_hi[i + 8]) << (i * 8);
    }
}

/* If `n` is an alt-of-single-bytes (or already a single-byte class),
 * fold its first-byte set into `bm` and return true.  Used by the
 * IRE optimizer to rewrite `(a|b|c)` and `[a-c]` into a single CLASS
 * — which then composes with rep+greedy_class for a fast tight-loop.
 * Captured span semantics survive: each candidate is one byte, the
 * resulting captured slice is identical whether we ALT-and-LIT or
 * just CLASS-and-advance. */
static bool
ire_alt_byte_set(const ire_node_t *n, uint64_t bm[4], bool ci)
{
    if (!n) return false;
    /* parse_concat wraps even single atoms in an IRE_CONCAT — peek
     * through.  Multi-element concats can't be class-folded (they'd
     * consume more than one byte). */
    if (n->kind == IRE_CONCAT) {
        if (n->u.cat.n == 1) return ire_alt_byte_set(n->u.cat.xs[0], bm, ci);
        return false;
    }
    switch (n->kind) {
    case IRE_LIT:
        if (n->u.lit.len != 1) return false;
        if (n->u.lit.ci != ci) return false;
        {
            const uint8_t b = (uint8_t)n->u.lit.bytes[0];
            bm[b >> 6] |= (1ULL << (b & 63));
            if (ci) {
                if (b >= 'a' && b <= 'z')      { const uint8_t c = (uint8_t)(b - 32); bm[c >> 6] |= (1ULL << (c & 63)); }
                else if (b >= 'A' && b <= 'Z') { const uint8_t c = (uint8_t)(b + 32); bm[c >> 6] |= (1ULL << (c & 63)); }
            }
            return true;
        }
    case IRE_CLASS:
        bm[0] |= n->u.cls.bm[0];
        bm[1] |= n->u.cls.bm[1];
        bm[2] |= n->u.cls.bm[2];
        bm[3] |= n->u.cls.bm[3];
        return true;
    case IRE_ALT:
        return ire_alt_byte_set(n->u.alt.l, bm, ci) &&
               ire_alt_byte_set(n->u.alt.r, bm, ci);
    case IRE_NCGROUP:
        return ire_alt_byte_set(n->u.nc.body, bm, ci);
    default:
        /* Capturing groups are also single-byte if their body is, but
         * they need different IR rewriting (preserve the cap_start/end);
         * skip for now. */
        return false;
    }
}

/* Walk `n`, replacing alt-of-single-bytes subtrees with IRE_CLASS in
 * place.  Result: simpler IR that the lower pass can map to a single
 * class node (and composes with the greedy_class fast path).
 *
 * We mutate the tree directly; the union members for IRE_CLASS only
 * use `bm[4]` so converting LIT/ALT-shaped storage doesn't leak the
 * old bytes (those allocations leak — acceptable since the IR lives
 * for the lifetime of the parse anyway).  Captures and their bodies
 * (IRE_GROUP) are not collapsed: lower keeps them intact. */
static void
ire_optimize(ire_node_t *n)
{
    if (!n) return;
    switch (n->kind) {
    case IRE_ALT: {
        ire_optimize(n->u.alt.l);
        ire_optimize(n->u.alt.r);
        /* Try to fold into a single class. */
        uint64_t bm[4] = {0};
        /* Pick `ci` from the first leaf we see — only collapse when all
         * branches share /i sensitivity (otherwise mixing's incorrect). */
        const ire_node_t *first_leaf = n->u.alt.l;
        while (first_leaf && (first_leaf->kind == IRE_NCGROUP)) first_leaf = first_leaf->u.nc.body;
        const bool ci = (first_leaf && first_leaf->kind == IRE_LIT) ? first_leaf->u.lit.ci : false;
        const bool ok = ire_alt_byte_set(n, bm, ci);
        if (ok) {
            n->kind = IRE_CLASS;
            memcpy(n->u.cls.bm, bm, sizeof(bm));
        }
        break;
    }
    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) ire_optimize(n->u.cat.xs[i]);
        break;
    case IRE_REP:        ire_optimize(n->u.rep.body);   break;
    case IRE_GROUP:      ire_optimize(n->u.group.body); break;
    case IRE_NCGROUP:
    case IRE_LOOKAHEAD:
    case IRE_NEG_LOOKAHEAD:
    case IRE_LOOKBEHIND:
    case IRE_NEG_LOOKBEHIND:
    case IRE_ATOMIC:
    case IRE_ABSENCE:    ire_optimize(n->u.nc.body); break;
    case IRE_CONDITIONAL:
        ire_optimize(n->u.cond.yes);
        ire_optimize(n->u.cond.no);
        break;
    default: break;
    }
}

/* True iff `n` (and everything it reaches) is safe to MatchCache.
 * The memo records (id, pos) → known-fail; this is sound when the
 * outcome at (id, pos) depends only on local state (pos + str), not on
 * captures or recursion depth.  Backref / atomic / subroutine /
 * conditional break that — different code paths reaching the same
 * (id, pos) may have different captures and different outcomes.  A
 * capture INSIDE a lookaround is also unsound: which capture set
 * survives the lookaround can vary by path, so two memo-keyed-equal
 * subtrees aren't actually equivalent.  Tracked via the `in_la` flag
 * recursing through (?=…), (?!…), (?<=…), (?<!…) bodies. */
static bool
ire_memo_eligible_inner(const ire_node_t *n, bool in_la)
{
    if (!n) return true;
    switch (n->kind) {
    case IRE_BACKREF:
    case IRE_ATOMIC:
    case IRE_SUBROUTINE:
    case IRE_CONDITIONAL:
        return false;
    case IRE_LIT: case IRE_DOT: case IRE_CLASS:
    case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
    case IRE_BOL: case IRE_EOL:
    case IRE_WB: case IRE_NWB:
    case IRE_EMPTY:
    case IRE_KEEP:
    case IRE_LAST_MATCH:
        return true;
    case IRE_GROUP:
        if (in_la) return false;  /* capture inside lookaround → unsound */
        return ire_memo_eligible_inner(n->u.group.body, in_la);
    case IRE_NCGROUP:
        return ire_memo_eligible_inner(n->u.nc.body, in_la);
    case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
    case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
        return ire_memo_eligible_inner(n->u.nc.body, true);
    case IRE_ABSENCE:
        /* The absence body is only ever evaluated as a probe (like a
         * negative lookahead) — captures inside don't survive, so the
         * memo soundness rule for lookarounds applies. */
        return ire_memo_eligible_inner(n->u.nc.body, true);
    case IRE_REP:           return ire_memo_eligible_inner(n->u.rep.body, in_la);
    case IRE_CONCAT:
        for (size_t i = 0; i < n->u.cat.n; i++) {
            if (!ire_memo_eligible_inner(n->u.cat.xs[i], in_la)) return false;
        }
        return true;
    case IRE_ALT:
        return ire_memo_eligible_inner(n->u.alt.l, in_la) &&
               ire_memo_eligible_inner(n->u.alt.r, in_la);
    }
    return false;
}

static bool
ire_memo_eligible(const ire_node_t *n)
{
    return ire_memo_eligible_inner(n, false);
}

/* True iff matching `n` is guaranteed to consume at least one byte on
 * success.  Conservative: returns false when in doubt, which makes the
 * caller take the slow (carve-out-tracking) path. */
static bool
ire_must_consume(const ire_node_t *n)
{
    if (!n) return false;
    switch (n->kind) {
    case IRE_LIT:           return n->u.lit.len > 0;
    case IRE_DOT:
    case IRE_CLASS:         return true;
    case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
    case IRE_BOL: case IRE_EOL:
    case IRE_WB: case IRE_NWB:
    case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
    case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
    case IRE_EMPTY:
    case IRE_KEEP:
    case IRE_LAST_MATCH:
    case IRE_BACKREF:       /* backref of empty group could be 0-byte */
    case IRE_SUBROUTINE:    /* unknown until expanded — be conservative */
    case IRE_ABSENCE:       /* greedy 0..* — empty match always allowed */
        return false;
    case IRE_GROUP:         return ire_must_consume(n->u.group.body);
    case IRE_NCGROUP:
    case IRE_ATOMIC:        return ire_must_consume(n->u.nc.body);
    case IRE_REP:
        if (n->u.rep.min < 1) return false;
        return ire_must_consume(n->u.rep.body);
    case IRE_CONCAT: {
        for (size_t i = 0; i < n->u.cat.n; i++) {
            if (ire_must_consume(n->u.cat.xs[i])) return true;
        }
        return false;
    }
    case IRE_ALT:           return ire_must_consume(n->u.alt.l) && ire_must_consume(n->u.alt.r);
    case IRE_CONDITIONAL:   return ire_must_consume(n->u.cond.yes) && ire_must_consume(n->u.cond.no);
    }
    return false;
}

/* Statically computed byte-length, or -1 if it varies.  Used for
 * lookbehind body sizing — we step pos back by this many bytes before
 * dispatching the body. */
static int
ire_fixed_len(const ire_node_t *n, agre_encoding_t enc)
{
    if (!n) return 0;
    switch (n->kind) {
    case IRE_LIT:           return (int)n->u.lit.len;
    case IRE_CLASS:         return 1;
    case IRE_DOT:           return enc == AGRE_ENC_UTF8 ? -1 : 1;
    case IRE_BACKREF:       return -1;
    case IRE_BOS: case IRE_EOS: case IRE_EOS_NL:
    case IRE_BOL: case IRE_EOL:
    case IRE_WB: case IRE_NWB:
    case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
    case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
    case IRE_EMPTY:
    case IRE_KEEP:
    case IRE_LAST_MATCH:
        return 0;
    case IRE_GROUP:         return ire_fixed_len(n->u.group.body, enc);
    case IRE_NCGROUP:
    case IRE_ATOMIC:        return ire_fixed_len(n->u.nc.body, enc);
    case IRE_REP: {
        if (n->u.rep.min != n->u.rep.max || n->u.rep.min < 0) return -1;
        const int per = ire_fixed_len(n->u.rep.body, enc);
        if (per < 0) return -1;
        return per * n->u.rep.min;
    }
    case IRE_CONCAT: {
        int sum = 0;
        for (size_t i = 0; i < n->u.cat.n; i++) {
            const int sub = ire_fixed_len(n->u.cat.xs[i], enc);
            if (sub < 0) return -1;
            sum += sub;
        }
        return sum;
    }
    case IRE_ALT: {
        const int l = ire_fixed_len(n->u.alt.l, enc);
        const int r = ire_fixed_len(n->u.alt.r, enc);
        if (l < 0 || r < 0 || l != r) return -1;
        return l;
    }
    case IRE_CONDITIONAL: {
        const int y = ire_fixed_len(n->u.cond.yes, enc);
        const int no = ire_fixed_len(n->u.cond.no, enc);
        if (y < 0 || no < 0 || y != no) return -1;
        return y;
    }
    case IRE_SUBROUTINE:    return -1;
    case IRE_ABSENCE:       return -1;
    }
    return -1;
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
            case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
            case IRE_EMPTY:
            case IRE_KEEP:
            case IRE_LAST_MATCH:
                continue;
            case IRE_GROUP:    return ire_first_class(c->u.group.body);
            case IRE_NCGROUP:
            case IRE_ATOMIC:   return ire_first_class(c->u.nc.body);
            case IRE_REP:
                if (c->u.rep.min >= 1) return ire_first_class(c->u.rep.body);
                return NULL;
            case IRE_CLASS:    return c;
            default:           return NULL;
            }
        }
        return NULL;
    case IRE_GROUP:    return ire_first_class(n->u.group.body);
    case IRE_NCGROUP:
    case IRE_ATOMIC:   return ire_first_class(n->u.nc.body);
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
    case IRE_ATOMIC:
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
    case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
    case IRE_EMPTY:
    case IRE_KEEP:
    case IRE_LAST_MATCH:
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
    /* Subroutine references encountered during lower.  After main
     * lowering, the parser walks this and lowers each referenced
     * group's body fresh with sub_return tail.  Recursion-safe: the
     * subroutine_call node looks the chain up via CTX at runtime. */
    bool *sub_needed;
    int   sub_needed_cap;
    /* Counter for ALT / REP node IDs used by the MatchCache memo.
     * Each call to ALLOC_node_re_alt / ALLOC_node_re_rep takes the
     * next id; total ends up in pattern->n_branches. */
    int   next_branch_id;
} lower_ctx_t;

static NODE *lower(lower_ctx_t *L, ire_node_t *n, NODE *tail);

/* Lower (?<=BODY) / (?<!BODY).  Body with statically-known fixed width
 * uses node_re_lookbehind (steps pos back by `width`); alt-of-fixed
 * gets per-branch lookbehinds combined via re_alt; everything else
 * falls back to the variable-width scanner with node_re_lb_check tail. */
static NODE *
lower_lookbehind(lower_ctx_t *L, ire_node_t *body, NODE *tail, bool negative)
{
    if (!body) return tail;
    const int w = ire_fixed_len(body, L->enc);
    if (w >= 0) {
        NODE *const body_nd = lower(L, body, ALLOC_node_re_succ());
        return negative
            ? ALLOC_node_re_neg_lookbehind(body_nd, (uint32_t)w, tail)
            : ALLOC_node_re_lookbehind   (body_nd, (uint32_t)w, tail);
    }
    if (body->kind == IRE_ALT) {
        if (!negative) {
            NODE *const l_path = lower_lookbehind(L, body->u.alt.l, tail, false);
            NODE *const r_path = lower_lookbehind(L, body->u.alt.r, tail, false);
            return ALLOC_node_re_alt(l_path, r_path, (uint32_t)L->next_branch_id++);
        } else {
            NODE *const inner = lower_lookbehind(L, body->u.alt.r, tail, true);
            return lower_lookbehind(L, body->u.alt.l, inner, true);
        }
    }
    if (body->kind == IRE_NCGROUP || body->kind == IRE_GROUP) {
        ire_node_t *const inner = (body->kind == IRE_GROUP)
            ? body->u.group.body : body->u.nc.body;
        return lower_lookbehind(L, inner, tail, negative);
    }
    /* Variable-width fallback: scan candidate widths from longest. */
    NODE *const body_nd = lower(L, body, ALLOC_node_re_lb_check(ALLOC_node_re_succ()));
    return negative
        ? ALLOC_node_re_neg_lookbehind_var(body_nd, tail)
        : ALLOC_node_re_lookbehind_var    (body_nd, tail);
}

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
        return ALLOC_node_re_alt(l, r, (uint32_t)L->next_branch_id++);
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
    case IRE_LOOKBEHIND:
        return lower_lookbehind(L, n->u.nc.body, tail, /* negative = */ false);
    case IRE_NEG_LOOKBEHIND:
        return lower_lookbehind(L, n->u.nc.body, tail, /* negative = */ true);
    case IRE_ATOMIC: {
        /* Run body with its own succ tail so backtracking on a failing
         * outer continuation doesn't reach into body's alternatives. */
        NODE *body = lower(L, n->u.nc.body, ALLOC_node_re_succ());
        return ALLOC_node_re_atomic(body, tail);
    }
    case IRE_ABSENCE: {
        /* (?~body) — the body is invoked as a probe at each candidate
         * position (like a negative lookahead).  Lower with its own
         * succ tail so probe-failure doesn't leak into the outer
         * continuation. */
        NODE *body = lower(L, n->u.nc.body, ALLOC_node_re_succ());
        return ALLOC_node_re_absence(body, tail);
    }
    case IRE_KEEP:          return ALLOC_node_re_keep(tail);
    case IRE_LAST_MATCH:    return ALLOC_node_re_last_match(tail);
    case IRE_CONDITIONAL: {
        NODE *yes_chain = lower(L, n->u.cond.yes, tail);
        NODE *no_chain  = lower(L, n->u.cond.no,  tail);
        return ALLOC_node_re_conditional((uint32_t)n->u.cond.idx, yes_chain, no_chain);
    }
    case IRE_SUBROUTINE: {
        const int idx = n->u.sub.idx;
        if (idx <= 0 || idx >= L->q->cap_groups || L->q->groups_by_idx[idx] == NULL) {
            re_error(L->q, "subroutine target undefined (forward reference?)");
            return tail;
        }
        if (idx >= L->sub_needed_cap) {
            const int new_cap = idx + 8;
            L->sub_needed = (bool *)realloc(L->sub_needed, sizeof(bool) * (size_t)new_cap);
            for (int i = L->sub_needed_cap; i < new_cap; i++) L->sub_needed[i] = false;
            L->sub_needed_cap = new_cap;
        }
        L->sub_needed[idx] = true;
        return ALLOC_node_re_subroutine_call((uint32_t)idx, tail);
    }
    case IRE_REP: {
        /* Specialised greedy/lazy `.` rep — collapses N rep_cont
         * sentinel dispatches into a single tight loop, saving most of
         * the per-byte overhead on /.* / patterns.  Separate ASCII and
         * UTF-8 variants so backward iteration respects codepoint
         * boundaries in UTF-8 mode. */
        if (n->u.rep.body && n->u.rep.body->kind == IRE_DOT) {
            const uint32_t ml = L->ml ? 1 : 0;
            if (n->u.rep.greedy) {
                if (L->enc == AGRE_ENC_UTF8) {
                    return ALLOC_node_re_greedy_dot_utf8(n->u.rep.min, n->u.rep.max, tail, ml);
                } else {
                    return ALLOC_node_re_greedy_dot(n->u.rep.min, n->u.rep.max, tail, ml);
                }
            } else if (L->enc != AGRE_ENC_UTF8) {
                /* Lazy UTF-8 uncommon; fall back to the generic path. */
                return ALLOC_node_re_lazy_dot(n->u.rep.min, n->u.rep.max, tail, ml);
            }
        }
        /* Specialised greedy/lazy single-class rep — same trick for
         * `\w+`, `[a-z]*`, `\d+`, `(?:a|b|c)+` (after ire_optimize) etc.
         * Class members are always one byte (no UTF-8 issue here), so
         * we don't need a separate encoding-specific variant.  Peek
         * through any non-capturing wrapper since `(?:[abc])+` and
         * `[abc]+` should optimise identically.
         *
         * Single-capture-group wrappers (`(X)+`, `(X)*`) get the
         * `_cap` variant: same tight loop, but updates the capture to
         * the span of the last matched byte on each
         * forward/backtrack step — matches Ruby's "last iter wins". */
        const ire_node_t *peeked = n->u.rep.body;
        int wrap_cap_idx = 0;
        while (peeked) {
            if (peeked->kind == IRE_NCGROUP) {
                peeked = peeked->u.nc.body;
            } else if (peeked->kind == IRE_GROUP && wrap_cap_idx == 0) {
                wrap_cap_idx = peeked->u.group.idx;
                peeked = peeked->u.group.body;
            } else {
                break;
            }
        }
        if (peeked && peeked->kind == IRE_CLASS) {
            const ire_node_t *const cls = peeked;
            if (n->u.rep.greedy) {
                if (wrap_cap_idx > 0) {
                    return ALLOC_node_re_greedy_class_cap(
                        cls->u.cls.bm[0], cls->u.cls.bm[1],
                        cls->u.cls.bm[2], cls->u.cls.bm[3],
                        n->u.rep.min, n->u.rep.max,
                        (uint32_t)wrap_cap_idx, tail);
                }
                return ALLOC_node_re_greedy_class(
                    cls->u.cls.bm[0], cls->u.cls.bm[1],
                    cls->u.cls.bm[2], cls->u.cls.bm[3],
                    n->u.rep.min, n->u.rep.max, tail);
            } else if (wrap_cap_idx == 0) {
                /* Lazy + group capture: capture-correctness across
                 * extension steps is fiddly; defer to generic rep. */
                return ALLOC_node_re_lazy_class(
                    cls->u.cls.bm[0], cls->u.cls.bm[1],
                    cls->u.cls.bm[2], cls->u.cls.bm[3],
                    n->u.rep.min, n->u.rep.max, tail);
            }
        }
        NODE *body = lower(L, n->u.rep.body, L->rep_cont);
        return ALLOC_node_re_rep(body, tail, n->u.rep.min, n->u.rep.max,
                                 n->u.rep.greedy ? 1 : 0,
                                 ire_must_consume(n->u.rep.body) ? 1 : 0,
                                 (uint32_t)L->next_branch_id++);
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
    case IRE_NCGROUP:
    case IRE_LOOKAHEAD: case IRE_NEG_LOOKAHEAD:
    case IRE_LOOKBEHIND: case IRE_NEG_LOOKBEHIND:
    case IRE_ATOMIC:
        ire_free(n->u.nc.body); break;
    case IRE_CONDITIONAL:
        ire_free(n->u.cond.yes);
        ire_free(n->u.cond.no);
        break;
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

    /* Pre-lower IR rewrites: alt-of-single-bytes → class, etc.  Cheap
     * tree walk that lets the lower pass see structural simplifications
     * (a|b|c) ↦ [abc] without each lower case having to special-case
     * the alt shape. */
    ire_optimize(ir);

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

    /* Lower may register errors (variable-width lookbehind without
     * fallback, undefined subroutine target).  Bail before building
     * the prefilter so the caller sees a clean parse failure. */
    if (q.error) {
        fprintf(stderr, "%s\n", q.errbuf);
        ire_free(ir);
        free(L.sub_needed);
        return NULL;
    }

    /* Build callable subroutine chains for each `\g<…>` reference.
     * Iterate to closure (mutual recursion may register more). */
    NODE **sub_chains = NULL;
    int sub_chains_n = 0;
    if (L.sub_needed) {
        sub_chains_n = L.sub_needed_cap;
        sub_chains = (NODE **)calloc((size_t)sub_chains_n, sizeof(NODE *));
        bool any_new = true;
        while (any_new && !q.error) {
            any_new = false;
            const int snapshot_cap = L.sub_needed_cap;
            for (int idx = 1; idx < snapshot_cap; idx++) {
                if (!L.sub_needed[idx] || sub_chains[idx]) continue;
                ire_node_t *const target = q.groups_by_idx[idx];
                if (!target) continue;
                NODE *const ret = ALLOC_node_re_sub_return();
                sub_chains[idx] = lower(&L, target, ret);
                any_new = true;
            }
            if (L.sub_needed_cap > sub_chains_n) {
                sub_chains = (NODE **)realloc(sub_chains, sizeof(NODE *) * (size_t)L.sub_needed_cap);
                for (int i = sub_chains_n; i < L.sub_needed_cap; i++) sub_chains[i] = NULL;
                sub_chains_n = L.sub_needed_cap;
            }
        }
        if (q.error) {
            fprintf(stderr, "%s\n", q.errbuf);
            free(sub_chains);
            free(L.sub_needed);
            ire_free(ir);
            return NULL;
        }
    }

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
    ac_t *ac_handle = NULL;       /* set when AC scan is selected */
    uint32_t a = anchored_bos ? 1 : 0;

    /* Try AC build up-front when the leading edge is alt-of-literals
     * that won't fit in the 8-entry byteset (or the literals share
     * prefixes so AC's structural filter beats byteset's per-byte
     * one).  We stash the result and consult it inside the cascade
     * below.  For "few literals with distinct first bytes" — e.g.
     * `ERROR|WARN|FATAL` — the SIMD byteset wins over AC's scalar
     * loop, so byteset takes priority. */
    if (!fp.ci) {
        const char **lits = NULL;
        uint32_t    *lens = NULL;
        int          n_lits = 0;
        bool         all_ci = false;
        if (ire_collect_leading_alt_lits(ir, &lits, &lens, &n_lits, &all_ci)
            && n_lits >= 2 && !all_ci) {
            uint32_t min_len = lens[0];
            for (int i = 1; i < n_lits; i++) if (lens[i] < min_len) min_len = lens[i];
            if (min_len >= 2) {
                ac_handle = ac_build(lits, lens, n_lits);
            }
        }
        free(lits);
        free(lens);
    }

    if (consumes && !fp.ci && fp.len >= 4) {
        char *needle = (char *)malloc(fp.len + 1);
        memcpy(needle, fp.bytes, fp.len); needle[fp.len] = 0;
        root = ALLOC_node_grep_search_memmem(body, needle, (uint32_t)fp.len, a);
        if (ac_handle) { ac_free(ac_handle); ac_handle = NULL; }  /* memmem wins */
    }
    else if (consumes && !fp.ci && fp.len >= 1) {
        root = ALLOC_node_grep_search_memchr(body, (uint32_t)(uint8_t)fp.bytes[0], a);
        if (ac_handle) { ac_free(ac_handle); ac_handle = NULL; }
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
            if (ac_handle) { ac_free(ac_handle); ac_handle = NULL; }
        }
        else if (have_range) {
            root = ALLOC_node_grep_search_range(body, (uint32_t)lo, (uint32_t)hi, a);
            if (ac_handle) { ac_free(ac_handle); ac_handle = NULL; }
        }
        /* AC fills the slot above the byteset's 8-byte cliff:
         * patterns whose literals collectively need >= 9 distinct
         * first bytes (e.g. a 12-way alternation).  Build was done
         * up-front; if `ac_handle` is set here, no other prefilter
         * fired. */
        else if (ac_handle) {
            root = ALLOC_node_grep_search_ac(body, (void *)ac_handle, a);
        }
        else if (cls) {
            /* First thing is a class but not a single contiguous range
             * (e.g. \w == [A-Za-z0-9_], or any other multi-region
             * class).  Use Truffle-style PSHUFB SIMD scan. */
            uint64_t tt[4];
            build_truffle_tables(cls->u.cls.bm, tt);
            root = ALLOC_node_grep_search_class_scan(body, tt[0], tt[1], tt[2], tt[3], a);
        }
        else {
            root = ALLOC_node_grep_search(body, a);
        }
    }

    int n_groups = q.n_groups;
    /* Compute memo eligibility BEFORE we free the IR — the walk reads
     * the same kind/union we're about to release. */
    const bool memo_eligible = ire_memo_eligible(ir);
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

    /* Transfer ownership of named-capture table + subroutine chains. */
    p->group_names    = q.names;
    p->group_name_idx = q.name_idx;
    p->n_named        = q.n_names;
    p->sub_chains     = sub_chains;
    p->sub_chains_n   = sub_chains_n;
    /* MatchCache memo metadata.  n_branches counts every ALT and REP
     * node lower assigned an id to.  memo_eligible is the static
     * verdict — false locks the cache off for this pattern. */
    p->n_branches     = L.next_branch_id;
    p->memo_eligible  = memo_eligible;
    /* Aho-Corasick automaton (NULL if not used).  Pattern owns the
     * heap allocation; freed in astrogre_pattern_free. */
    p->ac = (void *)ac_handle;
    free(L.sub_needed);
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
    for (int i = 0; i < p->n_named; i++) free(p->group_names[i]);
    free(p->group_names);
    free(p->group_name_idx);
    free(p->sub_chains);  /* node memory owned by framework's side array */
    ac_free((ac_t *)p->ac);
    free(p);
}

int astrogre_pattern_n_named(const astrogre_pattern *p) { return p ? p->n_named : 0; }

const char *
astrogre_pattern_named_at(const astrogre_pattern *p, int i, int *out_idx)
{
    if (!p || i < 0 || i >= p->n_named) return NULL;
    if (out_idx) *out_idx = p->group_name_idx[i];
    return p->group_names[i];
}

const char *
astrogre_pattern_source(const astrogre_pattern *p, size_t *out_len)
{
    if (!p || !p->pat) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = strlen(p->pat);
    return p->pat;
}

bool astrogre_pattern_case_insensitive(const astrogre_pattern *p) { return p && p->case_insensitive; }
bool astrogre_pattern_multiline(const astrogre_pattern *p)        { return p && p->multiline; }

uint64_t
astrogre_pattern_hash(astrogre_pattern *p)
{
    if (!p || !p->root) return 0;
    return (uint64_t)HASH(p->root);
}

bool
astrogre_pattern_has_prefilter(astrogre_pattern *p)
{
    if (!p || !p->root) return false;
    const char *name = p->root->head.kind->default_dispatcher_name;
    if (!name) return false;
    return strcmp(name, "DISPATCH_node_grep_search_memchr")     == 0
        || strcmp(name, "DISPATCH_node_grep_search_memmem")     == 0
        || strcmp(name, "DISPATCH_node_grep_search_byteset")    == 0
        || strcmp(name, "DISPATCH_node_grep_search_range")      == 0
        || strcmp(name, "DISPATCH_node_grep_search_class_scan") == 0
        || strcmp(name, "DISPATCH_node_grep_search_ac")         == 0;
}

bool
astrogre_pattern_pure_literal(astrogre_pattern *p,
                               const char **out_bytes, size_t *out_len)
{
    if (!p || !p->root) return false;
    NODE *root = p->root;
    const char *root_name = root->head.kind->default_dispatcher_name;
    if (!root_name) return false;

    NODE *body = NULL;
    const char *needle = NULL;
    size_t needle_len = 0;

    if (strcmp(root_name, "DISPATCH_node_grep_search_memmem") == 0) {
        if (root->u.node_grep_search_memmem.anchored_bos) return false;
        body = root->u.node_grep_search_memmem.body;
        needle = root->u.node_grep_search_memmem.prefix;
        needle_len = root->u.node_grep_search_memmem.prefix_len;
    } else if (strcmp(root_name, "DISPATCH_node_grep_search_memchr") == 0) {
        if (root->u.node_grep_search_memchr.anchored_bos) return false;
        body = root->u.node_grep_search_memchr.body;
        /* needle filled in from the inner lit node below */
    } else if (strcmp(root_name, "DISPATCH_node_grep_search") == 0) {
        /* -F mode: astrogre_parse_fixed builds the plain search loop
         * directly (no prefix detection).  The literal still lives
         * in the inner lit node and the shape is otherwise identical,
         * so the count-lines AST rewrite applies. */
        if (root->u.node_grep_search.anchored_bos) return false;
        body = root->u.node_grep_search.body;
        /* needle filled in from the inner lit node below */
    } else {
        return false;
    }

    /* body must be cap_start(0, ...) */
    if (strcmp(body->head.kind->default_dispatcher_name,
               "DISPATCH_node_re_cap_start") != 0) return false;
    if (body->u.node_re_cap_start.idx != 0) return false;
    NODE *lit = body->u.node_re_cap_start.next;

    /* lit must be node_re_lit (not lit_ci) */
    if (strcmp(lit->head.kind->default_dispatcher_name,
               "DISPATCH_node_re_lit") != 0) return false;
    if (needle == NULL) {
        needle     = lit->u.node_re_lit.bytes;
        needle_len = lit->u.node_re_lit.len;
    }

    /* lit's next must be cap_end(0, ...) */
    NODE *cap_end = lit->u.node_re_lit.next;
    if (strcmp(cap_end->head.kind->default_dispatcher_name,
               "DISPATCH_node_re_cap_end") != 0) return false;
    if (cap_end->u.node_re_cap_end.idx != 0) return false;

    /* cap_end's next must be node_re_succ (the terminator) */
    NODE *succ = cap_end->u.node_re_cap_end.next;
    if (strcmp(succ->head.kind->default_dispatcher_name,
               "DISPATCH_node_re_succ") != 0) return false;

    if (out_bytes) *out_bytes = needle;
    if (out_len)   *out_len   = needle_len;
    return true;
}

extern void astro_cs_compile(NODE *entry, const char *file);
extern void astro_cs_build(const char *extra_cflags);
extern void astro_cs_reload(void);
extern bool astro_cs_load(NODE *n, const char *file);
extern void astrogre_export_all_sds(void);
extern void astrogre_reload_all_dispatchers(void);

long
astrogre_pattern_count_lines(astrogre_pattern *p, const char *str, size_t len)
{
    if (!p) return -1;
    if (!p->count_lines_root) {
        const char *needle;
        size_t needle_len;
        if (!astrogre_pattern_pure_literal(p, &needle, &needle_len)) return -1;
        if (needle_len == 0) return -1;
        /* Body chain for `-c PURE_LITERAL`:
         *
         *   count → lineskip → continue
         *
         * Scanner verifies the literal via dual-byte filter + memcmp,
         * dispatches body at each match.  count++ increments
         * c->count_result, lineskip advances c->pos past the next \n,
         * continue returns 2 to signal "scanner: resume from c->pos".
         *
         * The needle pointer points into the existing AST's lit node
         * and is stable for the pattern's lifetime; no copy needed. */
        NODE *cont    = ALLOC_node_action_continue();
        NODE *skip    = ALLOC_node_action_lineskip(cont);
        NODE *counter = ALLOC_node_action_count(skip);
        p->count_lines_root = ALLOC_node_scan_lit_dual_byte(counter, needle, (uint32_t)needle_len);
        /* Pick up any code-store SDs that may already be loaded for
         * these structural hashes (e.g. from a previous --aot-compile
         * run).  Each node in the chain is registered, so cs_load on
         * each keeps things consistent. */
        astro_cs_load(p->count_lines_root, NULL);
        astro_cs_load(counter, NULL);
        astro_cs_load(skip, NULL);
        astro_cs_load(cont, NULL);
    }

    CTX c;
    c.str = (const uint8_t *)str;
    c.str_len = len;
    c.pos = 0;
    c.case_insensitive = p->case_insensitive;
    c.multiline = p->multiline;
    c.encoding = p->encoding;
    c.n_groups = 0;
    c.rep_top = NULL;
    c.rep_cont_sentinel = NULL;
    c.count_result = 0;
    EVAL(&c, p->count_lines_root);
    return c.count_result;
}

long
astrogre_pattern_print_lines(astrogre_pattern *p, const char *str, size_t len,
                              const char *fname, FILE *out, uint32_t emit_opts)
{
    if (!p) return -1;
    if (p->print_lines_root && p->print_lines_opts != emit_opts) {
        /* opts changed (rare — usually fixed for the whole CLI run);
         * the framework can't reuse the SD because emit_opts is part of
         * the structural hash.  Drop the cached root and rebuild. */
        p->print_lines_root = NULL;
    }
    if (!p->print_lines_root) {
        const char *needle;
        size_t needle_len;
        if (!astrogre_pattern_pure_literal(p, &needle, &needle_len)) return -1;
        if (needle_len == 0) return -1;
        /* Body: count → emit_match_line(needle_len, opts) → lineskip → continue.
         * `count` lets the caller read the matching-line total back via
         * c->count_result; emit prints; lineskip jumps past the matched
         * line so the scanner's next chunk starts at the next line. */
        NODE *cont    = ALLOC_node_action_continue();
        NODE *skip    = ALLOC_node_action_lineskip(cont);
        NODE *emit    = ALLOC_node_action_emit_match_line(skip, (uint32_t)needle_len, emit_opts);
        NODE *counter = ALLOC_node_action_count(emit);
        p->print_lines_root = ALLOC_node_scan_lit_dual_byte(counter, needle, (uint32_t)needle_len);
        p->print_lines_opts = emit_opts;
        astro_cs_load(p->print_lines_root, NULL);
        astro_cs_load(counter, NULL);
        astro_cs_load(emit, NULL);
        astro_cs_load(skip, NULL);
        astro_cs_load(cont, NULL);
    }

    CTX c;
    c.str = (const uint8_t *)str;
    c.str_len = len;
    c.pos = 0;
    c.case_insensitive = p->case_insensitive;
    c.multiline = p->multiline;
    c.encoding = p->encoding;
    c.n_groups = 0;
    c.rep_top = NULL;
    c.rep_cont_sentinel = NULL;
    c.count_result = 0;
    c.fname = fname;
    c.out = out;
    c.lineno = 0;
    c.lineno_pos = 0;
    EVAL(&c, p->print_lines_root);
    return c.count_result;
}

void
astrogre_pattern_aot_compile(astrogre_pattern *p, bool verbose)
{
    if (!p || !p->root) return;
    if (verbose) {
        fprintf(stderr, "astrogre: cs_compile h=%016lx /%s/\n",
                (unsigned long)HASH(p->root), p->pat ? p->pat : "");
    }
    astro_cs_compile(p->root, NULL);
    /* If the pattern is pure-literal-shaped, also pre-build and bake
     * the count_lines variant — the CLI's `-c PURE_LITERAL` path picks
     * it up.  Done eagerly here so AOT compile catches it; otherwise
     * the lazy build in astrogre_pattern_count_lines would happen
     * after cs_build and miss the SD. */
    {
        const char *needle;
        size_t needle_len;
        if (astrogre_pattern_pure_literal(p, &needle, &needle_len) && needle_len > 0
            && !p->count_lines_root) {
            NODE *cont    = ALLOC_node_action_continue();
            NODE *skip    = ALLOC_node_action_lineskip(cont);
            NODE *counter = ALLOC_node_action_count(skip);
            p->count_lines_root = ALLOC_node_scan_lit_dual_byte(counter, needle, (uint32_t)needle_len);
            if (verbose) {
                fprintf(stderr, "astrogre: cs_compile h=%016lx (count_lines)\n",
                        (unsigned long)HASH(p->count_lines_root));
            }
            astro_cs_compile(p->count_lines_root, NULL);
        }
    }
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
