// luastro Lua-pattern matcher.
//
// Implements Lua 5.4's pattern syntax (NOT a full regex):
//   character classes: %a %A %d %D %l %L %s %S %u %U %w %W %p %P %c %C %x %X
//   literal escapes:   %% %( %) %[ %] %. %+ %* %? %^ %$ ... etc.
//   sets:              [abc] [^abc] [a-z] [%d_]
//   any:               .
//   quantifiers:       * + - ?
//   anchors:           ^ $
//   captures:          ( ) (no nested), %1-%9 backrefs
//   %f frontier        not implemented
//   balanced %b        not implemented
//
// Public API matches what string.find / string.match / string.gmatch /
// string.gsub need:
//   bool luapat_match(const char *src, size_t srclen,
//                     const char *pat, size_t patlen,
//                     size_t init,           // 0-based start position
//                     size_t *match_start,
//                     size_t *match_end,
//                     struct luapat_cap *caps, int *ncaps);
//
// Returns true on match.  match_start/match_end are byte offsets into
// src.  caps[] receives up to LUAPAT_MAXCAP captures.

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define LUAPAT_MAXCAP 16

struct luapat_cap {
    long start, len;    // -1 len means "position capture" (start is the byte offset)
};

typedef struct luapat_state {
    const char *src;
    size_t srclen;
    const char *pat;
    size_t patlen;
    int level;
    struct luapat_cap caps[LUAPAT_MAXCAP];
} luapat_state;

static const char *match_pat(luapat_state *ms, const char *s, const char *p);

static int
match_class_char(int c, int cls)
{
    int res;
    switch (cls | 0x20) {  // tolower
    case 'a': res = isalpha(c); break;
    case 'c': res = iscntrl(c); break;
    case 'd': res = isdigit(c); break;
    case 'l': res = islower(c); break;
    case 'p': res = ispunct(c); break;
    case 's': res = isspace(c); break;
    case 'u': res = isupper(c); break;
    case 'w': res = isalnum(c); break;
    case 'x': res = isxdigit(c); break;
    default:  return c == cls;       // literal
    }
    return (cls >= 'A' && cls <= 'Z') ? !res : !!res;
}

static int
match_set(int c, const char *p, const char *ep)
{
    int sig = 1;
    if (*(p + 1) == '^') { sig = 0; p++; }
    p++;
    while (p < ep) {
        if (*p == '%' && p + 1 < ep) {
            if (match_class_char(c, (unsigned char)p[1])) return sig;
            p += 2;
        } else if (p + 2 < ep && p[1] == '-') {
            if ((unsigned char)*p <= c && c <= (unsigned char)p[2]) return sig;
            p += 3;
        } else {
            if ((unsigned char)*p == c) return sig;
            p++;
        }
    }
    return !sig;
}

// Determine end of class at p.  Returns pointer past the class.
static const char *
class_end(luapat_state *ms, const char *p)
{
    int c = (unsigned char)*p++;
    if (c == '%') {
        if (p >= ms->pat + ms->patlen) return p;
        return p + 1;
    }
    if (c == '[') {
        if (*p == '^') p++;
        if (*p == ']') p++;     // first ] is literal
        while (p < ms->pat + ms->patlen && *p != ']') {
            if (*p == '%' && p + 1 < ms->pat + ms->patlen) p++;
            p++;
        }
        return p + 1;
    }
    return p;
}

// True if single-char matches [class] starting at p.
static int
single_match(luapat_state *ms, int c, const char *p, const char *ep)
{
    if (p >= ms->pat + ms->patlen) return 0;
    int pc = (unsigned char)*p;
    switch (pc) {
    case '.':  return 1;
    case '%':  return match_class_char(c, (unsigned char)p[1]);
    case '[':  return match_set(c, p, ep - 1);
    default:   return pc == c;
    }
}

static const char *
match_max_expand(luapat_state *ms, const char *s, const char *p, const char *ep)
{
    long i = 0;
    while (s + i < ms->src + ms->srclen
           && single_match(ms, (unsigned char)s[i], p, ep)) i++;
    while (i >= 0) {
        const char *r = match_pat(ms, s + i, ep + 1);
        if (r) return r;
        i--;
    }
    return NULL;
}

static const char *
match_min_expand(luapat_state *ms, const char *s, const char *p, const char *ep)
{
    for (;;) {
        const char *r = match_pat(ms, s, ep + 1);
        if (r) return r;
        if (s < ms->src + ms->srclen && single_match(ms, (unsigned char)*s, p, ep)) s++;
        else return NULL;
    }
}

static const char *
match_capture(luapat_state *ms, const char *s, int l)
{
    l -= 1;
    if (l < 0 || l >= ms->level) return NULL;
    long len = ms->caps[l].len;
    if ((long)(ms->srclen - (s - ms->src)) >= len &&
        memcmp(ms->src + ms->caps[l].start, s, len) == 0)
        return s + len;
    return NULL;
}

static const char *
start_capture(luapat_state *ms, const char *s, const char *p, int what)
{
    if (ms->level >= LUAPAT_MAXCAP) return NULL;
    ms->caps[ms->level].start = s - ms->src;
    ms->caps[ms->level].len   = (what == '(') ? -1 : 0;   // -1 unfinished marker
    int saved = ms->level;
    ms->level++;
    const char *r = match_pat(ms, s, p);
    if (!r) ms->level = saved;
    return r;
}

static const char *
end_capture(luapat_state *ms, const char *s, const char *p)
{
    int l = ms->level - 1;
    while (l >= 0 && ms->caps[l].len != -1) l--;
    if (l < 0) return NULL;
    ms->caps[l].len = (s - ms->src) - ms->caps[l].start;
    const char *r = match_pat(ms, s, p);
    if (!r) ms->caps[l].len = -1;
    return r;
}

static const char *
match_pat(luapat_state *ms, const char *s, const char *p)
{
    if (p >= ms->pat + ms->patlen) return s;
    switch (*p) {
    case '(':
        if (p + 1 < ms->pat + ms->patlen && p[1] == ')') {
            return start_capture(ms, s, p + 2, ')');   // position capture
        }
        return start_capture(ms, s, p + 1, '(');
    case ')':
        return end_capture(ms, s, p + 1);
    case '$':
        if (p + 1 >= ms->pat + ms->patlen) {
            return (s == ms->src + ms->srclen) ? s : NULL;
        }
        break;   // fall through, treat $ as literal
    case '%':
        if (p + 1 < ms->pat + ms->patlen && p[1] >= '1' && p[1] <= '9') {
            const char *r = match_capture(ms, s, p[1] - '0');
            if (!r) return NULL;
            return match_pat(ms, r, p + 2);
        }
        break;
    }
    // default: single class then optional quantifier
    const char *ep = class_end(ms, p);
    if (s < ms->src + ms->srclen && single_match(ms, (unsigned char)*s, p, ep)) {
        switch (*ep) {
        case '?': {
            const char *r = match_pat(ms, s + 1, ep + 1);
            if (r) return r;
            return match_pat(ms, s, ep + 1);
        }
        case '+': return match_max_expand(ms, s + 1, p, ep);
        case '*': return match_max_expand(ms, s, p, ep);
        case '-': return match_min_expand(ms, s, p, ep);
        default:  return match_pat(ms, s + 1, ep);
        }
    } else {
        if (*ep == '?' || *ep == '*' || *ep == '-') return match_pat(ms, s, ep + 1);
        return NULL;
    }
}

// Public: try to match pat against src starting at init.  On match,
// store the byte offsets of the match start/end and the captures.
int
luapat_match(const char *src, size_t srclen,
             const char *pat, size_t patlen,
             size_t init,
             size_t *out_start, size_t *out_end,
             struct luapat_cap *out_caps, int *out_ncaps)
{
    luapat_state ms = {0};
    ms.src    = src;
    ms.srclen = srclen;
    ms.pat    = pat;
    ms.patlen = patlen;

    bool anchor = (patlen > 0 && pat[0] == '^');
    const char *p0 = pat + (anchor ? 1 : 0);
    const char *s = src + init;

    do {
        ms.level = 0;
        const char *r = match_pat(&ms, s, p0);
        if (r) {
            *out_start = s - src;
            *out_end   = r - src;
            for (int i = 0; i < ms.level; i++) out_caps[i] = ms.caps[i];
            *out_ncaps = ms.level;
            return 1;
        }
        s++;
    } while (!anchor && s <= src + srclen);

    return 0;
}
