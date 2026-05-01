// jstro minimal regex engine.  Backtracking matcher supporting:
//   literal chars, `.`, `*`, `+`, `?`, `{m,n}` (basic), `^`, `$`,
//   character classes `[...]` with ranges and `^` negation,
//   shortcuts `\d \D \s \S \w \W`, escapes `\n \t \r \\ \/`,
//   alternation `|`, grouping `(...)`, non-capturing `(?:...)`.
//
// Flags supported: `i` (case-insensitive), `g` (global), `m` (multiline),
// `s` (dotall), `u` is parsed but ignored.
//
// Not supported: lookahead/lookbehind, backreferences, named groups,
// Unicode property escapes.  These throw at compile time.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "node.h"
#include "context.h"

typedef struct JsRegex {
    struct GCHead    gc;
    struct JsString *source;
    struct JsString *flags;
    bool             ignorecase;
    bool             global;
    bool             multiline;
    bool             dotall;
    int32_t          last_index;   // for /g state
} JsRegex;

#define JV_IS_REGEX(v) JV_IS_HEAP_OF(v, JS_TREGEX)
#define JV_AS_REGEX(v) ((JsRegex *)(uintptr_t)(v))

// ---------- Matcher (recursive backtracking on the pattern source) ----

static bool js_regex_class_match(const char *p_start, const char *p_end, char ch, bool ic);
static int  js_regex_match_at(const char *p, const char *p_end,
                              const char *s, const char *s_start, const char *s_end,
                              bool ic, bool ml, bool da);

// Skip a `[...]` class beginning at `p`; returns ptr to the byte after `]`.
static const char *
js_regex_skip_class(const char *p, const char *p_end)
{
    p++;  // past '['
    if (p < p_end && *p == '^') p++;
    while (p < p_end && *p != ']') {
        if (*p == '\\' && p + 1 < p_end) p += 2;
        else                              p++;
    }
    if (p < p_end) p++;
    return p;
}

// Skip the next "atom" (a single thing that a quantifier can apply to),
// returning ptr to the char after it.
static const char *
js_regex_skip_atom(const char *p, const char *p_end)
{
    if (p >= p_end) return p;
    if (*p == '\\' && p + 1 < p_end) return p + 2;
    if (*p == '[') return js_regex_skip_class(p, p_end);
    if (*p == '(') {
        int depth = 1;
        p++;
        while (p < p_end && depth > 0) {
            if (*p == '\\' && p + 1 < p_end) p += 2;
            else if (*p == '(')  { depth++; p++; }
            else if (*p == ')')  { depth--; p++; }
            else if (*p == '[')  p = js_regex_skip_class(p, p_end);
            else                 p++;
        }
        return p;
    }
    return p + 1;
}

// Match a single char against an atom (no quantifier).  Returns 1 if it
// matched (consuming 1 char from `s`), 0 if not.  For groups we recurse.
static int
js_regex_atom_match(const char *p, const char *p_atom_end,
                    const char *s, const char *s_start, const char *s_end,
                    bool ic, bool ml, bool da)
{
    if (s >= s_end) return 0;
    char ch = *s;
    if (*p == '.') {
        if (!da && ch == '\n') return 0;
        return 1;
    }
    if (*p == '\\' && p + 1 < p_atom_end) {
        char e = p[1];
        switch (e) {
        case 'd': return (ch >= '0' && ch <= '9') ? 1 : 0;
        case 'D': return !(ch >= '0' && ch <= '9') ? 1 : 0;
        case 's': return (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') ? 1 : 0;
        case 'S': return !(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') ? 1 : 0;
        case 'w': return (isalnum((unsigned char)ch) || ch == '_') ? 1 : 0;
        case 'W': return !(isalnum((unsigned char)ch) || ch == '_') ? 1 : 0;
        case 'n': return ch == '\n' ? 1 : 0;
        case 't': return ch == '\t' ? 1 : 0;
        case 'r': return ch == '\r' ? 1 : 0;
        case 'b': case 'B': return 0;  // word boundary — not handled inline; require zero-width
        default:
            if (ic) return tolower((unsigned char)ch) == tolower((unsigned char)e);
            return ch == e;
        }
    }
    if (*p == '[') {
        return js_regex_class_match(p, p_atom_end, ch, ic) ? 1 : 0;
    }
    if (ic) return tolower((unsigned char)ch) == tolower((unsigned char)*p);
    return ch == *p;
}

static bool
js_regex_class_match(const char *p_start, const char *p_end, char ch, bool ic)
{
    const char *p = p_start + 1;
    bool negate = false;
    if (p < p_end && *p == '^') { negate = true; p++; }
    bool matched = false;
    while (p < p_end - 1) {  // -1 to skip the closing ']'
        char c1;
        if (*p == '\\' && p + 1 < p_end) {
            char e = p[1];
            p += 2;
            switch (e) {
            case 'd': if (ch >= '0' && ch <= '9') matched = true; continue;
            case 'D': if (!(ch >= '0' && ch <= '9')) matched = true; continue;
            case 's': if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') matched = true; continue;
            case 'S': if (!(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v')) matched = true; continue;
            case 'w': if (isalnum((unsigned char)ch) || ch == '_') matched = true; continue;
            case 'W': if (!(isalnum((unsigned char)ch) || ch == '_')) matched = true; continue;
            case 'n': c1 = '\n'; break;
            case 't': c1 = '\t'; break;
            case 'r': c1 = '\r'; break;
            default:  c1 = e;
            }
        } else {
            c1 = *p++;
        }
        if (p < p_end - 1 && *p == '-' && p[1] != ']') {
            // range c1-c2
            char c2;
            p++;
            if (*p == '\\' && p + 1 < p_end) { c2 = p[1]; p += 2; }
            else                              c2 = *p++;
            char lo = c1, hi = c2;
            if (lo > hi) { char t = lo; lo = hi; hi = t; }
            char tc = ic ? (char)tolower((unsigned char)ch) : ch;
            char tlo = ic ? (char)tolower((unsigned char)lo) : lo;
            char thi = ic ? (char)tolower((unsigned char)hi) : hi;
            if (tc >= tlo && tc <= thi) matched = true;
        } else {
            if (ic ? tolower((unsigned char)ch) == tolower((unsigned char)c1) : ch == c1)
                matched = true;
        }
    }
    return matched ^ negate;
}

// Find the alternation `|` at the top level of the current group (return
// pointer to `|` or NULL).
static const char *
js_regex_find_alt(const char *p, const char *p_end)
{
    int depth = 0;
    while (p < p_end) {
        if (*p == '\\' && p + 1 < p_end) { p += 2; continue; }
        if (*p == '[') { p = js_regex_skip_class(p, p_end); continue; }
        if (*p == '(') { depth++; p++; continue; }
        if (*p == ')') { if (depth == 0) return NULL; depth--; p++; continue; }
        if (*p == '|' && depth == 0) return p;
        p++;
    }
    return NULL;
}

// Match `p..p_end` against `s..s_end`.  Returns the number of bytes
// consumed in `s` if matched, -1 otherwise.
static int
js_regex_match_at(const char *p, const char *p_end,
                  const char *s, const char *s_start, const char *s_end,
                  bool ic, bool ml, bool da)
{
    const char *s_orig = s;
    while (p < p_end) {
        // Alternation at top level
        const char *alt = js_regex_find_alt(p, p_end);
        if (alt) {
            int r1 = js_regex_match_at(p, alt, s, s_start, s_end, ic, ml, da);
            if (r1 >= 0) return (int)(s - s_orig) + r1;
            return js_regex_match_at(alt + 1, p_end, s, s_start, s_end, ic, ml, da);
        }
        // ^ anchor
        if (*p == '^') {
            if (s != s_start && !(ml && s > s_start && s[-1] == '\n')) return -1;
            p++;
            continue;
        }
        // $ anchor
        if (*p == '$') {
            if (s != s_end && !(ml && s < s_end && *s == '\n')) return -1;
            p++;
            continue;
        }
        // Word boundary
        if (*p == '\\' && p + 1 < p_end && (p[1] == 'b' || p[1] == 'B')) {
            bool prev_word = (s > s_start) && (isalnum((unsigned char)s[-1]) || s[-1] == '_');
            bool next_word = (s < s_end) && (isalnum((unsigned char)s[0]) || s[0] == '_');
            bool boundary = prev_word != next_word;
            if (p[1] == 'b' && !boundary) return -1;
            if (p[1] == 'B' &&  boundary) return -1;
            p += 2;
            continue;
        }
        // Group?
        if (*p == '(') {
            const char *atom_end = js_regex_skip_atom(p, p_end);
            // Find inside of group: (... ) — non-capturing if (?:..)
            const char *inner_start = p + 1;
            const char *inner_end = atom_end - 1;
            if (inner_end - inner_start >= 2 && inner_start[0] == '?' && inner_start[1] == ':')
                inner_start += 2;
            // Quantifier?
            char q = (atom_end < p_end) ? *atom_end : 0;
            if (q == '*' || q == '+' || q == '?') {
                // Greedy: try max repetitions first.
                int min = (q == '+') ? 1 : 0;
                int max = (q == '?') ? 1 : INT32_MAX;
                // Try i = max, max-1, ..., min.
                // We don't know max upfront; iterate until no match.
                int matched_count = 0;
                const char *positions[1024];
                positions[0] = s;
                while (matched_count < max) {
                    int r = js_regex_match_at(inner_start, inner_end, s, s_start, s_end, ic, ml, da);
                    if (r < 0) break;
                    if (r == 0) break;  // zero-width to avoid infinite loop
                    s += r;
                    matched_count++;
                    if (matched_count < 1024) positions[matched_count] = s;
                }
                // Now try the rest from each backtrack position.
                for (int i = matched_count; i >= min; i--) {
                    int r = js_regex_match_at(atom_end + 1, p_end, positions[i], s_start, s_end, ic, ml, da);
                    if (r >= 0) return (int)(positions[i] - s_orig) + r;
                }
                return -1;
            }
            int r = js_regex_match_at(inner_start, inner_end, s, s_start, s_end, ic, ml, da);
            if (r < 0) return -1;
            s += r;
            p = atom_end;
            continue;
        }
        // Single atom (with optional quantifier)
        const char *atom_start = p;
        const char *atom_end = js_regex_skip_atom(p, p_end);
        char q = (atom_end < p_end) ? *atom_end : 0;
        if (q == '*' || q == '+' || q == '?' || q == '{') {
            int min = 0, max = INT32_MAX;
            const char *q_end = atom_end + 1;
            if (q == '*') { min = 0; max = INT32_MAX; }
            else if (q == '+') { min = 1; max = INT32_MAX; }
            else if (q == '?') { min = 0; max = 1; }
            else /* '{m,n}' */ {
                const char *qp = atom_end + 1;
                int m = 0;
                while (qp < p_end && *qp >= '0' && *qp <= '9') { m = m * 10 + (*qp - '0'); qp++; }
                int n = m;
                if (qp < p_end && *qp == ',') {
                    qp++;
                    if (qp < p_end && *qp == '}') n = INT32_MAX;
                    else {
                        n = 0;
                        while (qp < p_end && *qp >= '0' && *qp <= '9') { n = n * 10 + (*qp - '0'); qp++; }
                    }
                }
                if (qp < p_end && *qp == '}') qp++;
                else return -1;
                min = m; max = n;
                q_end = qp;
            }
            int matched_count = 0;
            while (matched_count < max && s < s_end) {
                int r = js_regex_atom_match(atom_start, atom_end, s, s_start, s_end, ic, ml, da);
                if (r <= 0) break;
                s += r;
                matched_count++;
            }
            // Backtrack to find a successful tail.
            while (matched_count >= min) {
                int r = js_regex_match_at(q_end, p_end, s, s_start, s_end, ic, ml, da);
                if (r >= 0) return (int)(s - s_orig) + r;
                if (matched_count == 0) return -1;
                s--;
                matched_count--;
            }
            return -1;
        }
        int r = js_regex_atom_match(atom_start, atom_end, s, s_start, s_end, ic, ml, da);
        if (r <= 0) return -1;
        s += r;
        p = atom_end;
    }
    return (int)(s - s_orig);
}

// Top-level: try to find a match starting at any position from `from`.
// Returns start index (>=from) or -1 if no match.
int
js_regex_search(JsRegex *re, struct JsString *s, int32_t from, int32_t *out_len)
{
    const char *p = re->source->data;
    const char *p_end = p + re->source->len;
    const char *str = s->data;
    int32_t len = (int32_t)s->len;
    if (from < 0) from = 0;
    for (int32_t i = from; i <= len; i++) {
        int r = js_regex_match_at(p, p_end, str + i, str, str + len,
                                  re->ignorecase, re->multiline, re->dotall);
        if (r >= 0) {
            if (out_len) *out_len = r;
            return i;
        }
    }
    return -1;
}

// Public ctor.
JsValue
js_regex_new(CTX *c, struct JsString *source, struct JsString *flags)
{
    JsRegex *re = (JsRegex *)js_gc_alloc(c, sizeof(JsRegex), JS_TREGEX);
    re->source = source;
    re->flags = flags ? flags : js_str_intern(c, "");
    re->ignorecase = re->global = re->multiline = re->dotall = false;
    re->last_index = 0;
    if (flags) {
        for (uint32_t i = 0; i < flags->len; i++) {
            switch (flags->data[i]) {
            case 'i': re->ignorecase = true; break;
            case 'g': re->global = true; break;
            case 'm': re->multiline = true; break;
            case 's': re->dotall = true; break;
            case 'u': case 'y': case 'd': break;  // accepted, ignored
            default: js_throw_syntax_error(c, "Invalid regex flag '%c'", flags->data[i]);
            }
        }
    }
    return (JsValue)(uintptr_t)re;
}

// .test(str)
JsValue
js_regex_test(CTX *c, JsValue rev, struct JsString *s)
{
    if (!JV_IS_REGEX(rev)) js_throw_type_error(c, "test: not a RegExp");
    JsRegex *re = JV_AS_REGEX(rev);
    int from = re->global ? re->last_index : 0;
    int len;
    int idx = js_regex_search(re, s, from, &len);
    if (idx < 0) {
        if (re->global) re->last_index = 0;
        return JV_FALSE;
    }
    if (re->global) re->last_index = idx + (len > 0 ? len : 1);
    return JV_TRUE;
}

// .exec(str) — returns [match, ...captures] array or null.  We don't
// track per-capture spans yet, so we just return [matched_substring].
JsValue
js_regex_exec(CTX *c, JsValue rev, struct JsString *s)
{
    if (!JV_IS_REGEX(rev)) js_throw_type_error(c, "exec: not a RegExp");
    JsRegex *re = JV_AS_REGEX(rev);
    int from = re->global ? re->last_index : 0;
    int len;
    int idx = js_regex_search(re, s, from, &len);
    if (idx < 0) {
        if (re->global) re->last_index = 0;
        return JV_NULL;
    }
    if (re->global) re->last_index = idx + (len > 0 ? len : 1);
    struct JsArray *out = js_array_new(c, 1);
    out->dense[0] = JV_STR(js_str_intern_n(c, s->data + idx, len));
    out->length = 1;
    js_object_set(c, out->fallback ? out->fallback : (out->fallback = js_object_new(c, NULL)),
                  js_str_intern(c, "index"), JV_INT(idx));
    js_object_set(c, out->fallback, js_str_intern(c, "input"), JV_STR(s));
    return JV_OBJ(out);
}
