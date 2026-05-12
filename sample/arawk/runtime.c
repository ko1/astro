// arawk runtime — heap allocators, awk-style numeric/string coercion,
// associative arrays, field splitting, input loop.  Linked once into
// the arawk binary; not part of any generated SD .c (those see only
// the inline arithmetic in context.h plus extern hooks for the heap
// path).

#include "context.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Singletons.
// ---------------------------------------------------------------------------

struct arawk_obj ARAWK_UNINIT_OBJ = { .type = ARAWK_T_UNINIT };

CTX *ARAWK_CURRENT_CTX = NULL;

// Default = BYTE for safety; main.c sets to UTF8 if LC_CTYPE looks
// UTF-8.  Tests that don't go through main (rare) get byte mode.
arawk_encoding_t ARAWK_ENCODING = ARAWK_ENC_BYTE;

// ---------------------------------------------------------------------------
// Heap allocators.  GC_malloc traces; GC_malloc_atomic skips tracing
// (use it for char[] / double payloads).
// ---------------------------------------------------------------------------

// arawk_alloc / arawk_make_float / arawk_make_int / arawk_int /
// arawk_make_array are now `static inline` in context.h (they are
// small and very hot — SD-baked bodies inline them directly).

static VALUE
arawk_make_string_typed(const char *s, size_t len, int type)
{
    struct arawk_obj *o = arawk_alloc(type);
    char *buf = (char *)GC_malloc_atomic(len + 1);
    if (s && len) memcpy(buf, s, len);
    buf[len] = '\0';
    o->str.chars = buf;
    o->str.len   = len;
    return ARAWK_OBJ_VAL(o);
}

VALUE arawk_make_string(const char *s, size_t len) { return arawk_make_string_typed(s, len, ARAWK_T_STRING); }
VALUE arawk_make_strnum(const char *s, size_t len) { return arawk_make_string_typed(s, len, ARAWK_T_STRNUM); }

// ---------------------------------------------------------------------------
// Numeric / string coercion.  Awk: every value has a number view and a
// string view; the operator picks one.  `strtod` semantics for parsing:
// leading whitespace is skipped, trailing junk → 0 (unless strnum, in
// which case fully-parseable strings retain numeric character).
// ---------------------------------------------------------------------------

double
arawk_to_num(VALUE v)
{
    if (LIKELY(ARAWK_IS_FIX(v))) return (double)ARAWK_FIX_VAL(v);
    if (v == ARAWK_UNINIT) return 0.0;
    struct arawk_obj *o = ARAWK_PTR(v);
    switch (o->type) {
      case ARAWK_T_FLOAT:  return o->dbl;
      case ARAWK_T_STRING:
      case ARAWK_T_STRNUM: {
        if (o->str.len == 0) return 0.0;
        // strtod recognises "inf" / "infinity" / "nan" as their
        // special floating-point values (C99); awk's rule is the
        // leading numeric prefix only.  A *word* like "informed"
        // starts with "inf" — strtod parses 3 characters into +inf
        // and reports end = chars+3.  To match awk we need to demand
        // that the recognised prefix start with a digit, sign-digit,
        // or `.digit`.  Anything else → 0.
        const char *s = o->str.chars;
        size_t len = o->str.len;
        size_t i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len) return 0.0;
        size_t j = i;
        if (s[j] == '+' || s[j] == '-') j++;
        bool ok = false;
        if (j < len && s[j] >= '0' && s[j] <= '9') ok = true;
        else if (j < len && s[j] == '.' && j + 1 < len && s[j+1] >= '0' && s[j+1] <= '9') ok = true;
        if (!ok) return 0.0;
        char *end;
        errno = 0;
        double d = strtod(s, &end);
        if (end == s) return 0.0;
        return d;
      }
      default: return 0.0;
    }
}

// Render to a caller-provided buffer (used by arawk_concat / printing).
// Floats use CONVFMT (or OFMT) from the active CTX's env if set,
// falling back to `%.6g`.  Awk distinguishes CONVFMT (most string
// contexts) from OFMT (print), but they default to the same value
// and most programs leave them in sync — we use CONVFMT here, which
// is correct for everything except the print-of-bare-float case.
// arawk_print_with_env / node_print also read OFMT directly
// for the truly print-specific path.
const char *
arawk_to_cstr(VALUE v, char *buf, size_t buflen, size_t *out_len)
{
    if (ARAWK_IS_FIX(v)) {
        int n = snprintf(buf, buflen, "%lld", (long long)ARAWK_FIX_VAL(v));
        if (n < 0) n = 0;
        *out_len = (size_t)n;
        return buf;
    }
    if (v == ARAWK_UNINIT) { *out_len = 0; return ""; }
    struct arawk_obj *o = ARAWK_PTR(v);
    switch (o->type) {
      case ARAWK_T_FLOAT: {
        // Integer-valued doubles print without ".0" in awk.
        double d = o->dbl;
        if (d == (double)(long long)d && d >= -1e15 && d <= 1e15) {
            int n = snprintf(buf, buflen, "%lld", (long long)d);
            *out_len = (size_t)(n < 0 ? 0 : n);
        }
        else {
            const char *fmt = "%.6g";
            char fmtbuf[32];
            if (ARAWK_CURRENT_CTX) {
                VALUE cv = ARAWK_CURRENT_CTX->env[ARAWK_GLOB_CONVFMT];
                if (ARAWK_IS_PTR(cv) && cv != ARAWK_UNINIT) {
                    struct arawk_obj *fo = ARAWK_PTR(cv);
                    if ((fo->type == ARAWK_T_STRING || fo->type == ARAWK_T_STRNUM) &&
                        fo->str.len > 0 && fo->str.len < sizeof fmtbuf) {
                        memcpy(fmtbuf, fo->str.chars, fo->str.len);
                        fmtbuf[fo->str.len] = '\0';
                        fmt = fmtbuf;
                    }
                }
            }
            int n = snprintf(buf, buflen, fmt, d);
            *out_len = (size_t)(n < 0 ? 0 : n);
        }
        return buf;
      }
      case ARAWK_T_STRING:
      case ARAWK_T_STRNUM:
        *out_len = o->str.len;
        return o->str.chars;
      case ARAWK_T_ARRAY:
        // awk forbids using an array in a scalar context; we'd
        // normally error here.  Phase 0+1 returns empty.
        *out_len = 0;
        return "";
      default:
        *out_len = 0;
        return "";
    }
}

VALUE
arawk_to_string(VALUE v)
{
    if (ARAWK_IS_PTR(v)) {
        struct arawk_obj *o = ARAWK_PTR(v);
        if (o->type == ARAWK_T_STRING || o->type == ARAWK_T_STRNUM) return v;
    }
    char buf[64];
    size_t len;
    const char *s = arawk_to_cstr(v, buf, sizeof buf, &len);
    return arawk_make_string(s, len);
}

// awk number-shape test: a string is "numeric" iff (after optional
// leading/trailing whitespace) it fully parses as a double.  Used to
// decide numeric vs string comparison for strnum values.
static bool
str_is_numeric_shape(const char *s, size_t len)
{
    if (len == 0) return false;
    // Skip leading whitespace.
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) i++;
    if (i == len) return false;
    char *end;
    errno = 0;
    double d = strtod(s + i, &end);
    (void)d;
    if (end == s + i) return false;
    // Skip trailing whitespace.
    size_t j = (size_t)(end - s);
    while (j < len && isspace((unsigned char)s[j])) j++;
    return j == len;
}

static bool
val_is_numeric(VALUE v)
{
    if (ARAWK_IS_FIX(v)) return true;
    if (v == ARAWK_UNINIT) return true;     // 0 in numeric context
    struct arawk_obj *o = ARAWK_PTR(v);
    if (o->type == ARAWK_T_FLOAT) return true;
    if (o->type == ARAWK_T_STRNUM) return str_is_numeric_shape(o->str.chars, o->str.len);
    return false;
}

bool
arawk_eq(VALUE a, VALUE b)
{
    if (a == b) return true;
    if (val_is_numeric(a) && val_is_numeric(b)) return arawk_to_num(a) == arawk_to_num(b);
    char abuf[64], bbuf[64]; size_t alen, blen;
    const char *as = arawk_to_cstr(a, abuf, sizeof abuf, &alen);
    const char *bs = arawk_to_cstr(b, bbuf, sizeof bbuf, &blen);
    if (alen != blen) return false;
    return memcmp(as, bs, alen) == 0;
}

int
arawk_cmp(VALUE a, VALUE b)
{
    if (val_is_numeric(a) && val_is_numeric(b)) {
        double da = arawk_to_num(a), db = arawk_to_num(b);
        if (da < db) return -1;
        if (da > db) return  1;
        return 0;
    }
    char abuf[64], bbuf[64]; size_t alen, blen;
    const char *as = arawk_to_cstr(a, abuf, sizeof abuf, &alen);
    const char *bs = arawk_to_cstr(b, bbuf, sizeof bbuf, &blen);
    size_t cmplen = alen < blen ? alen : blen;
    int r = memcmp(as, bs, cmplen);
    if (r != 0) return r < 0 ? -1 : 1;
    if (alen < blen) return -1;
    if (alen > blen) return  1;
    return 0;
}

// ---------------------------------------------------------------------------
// String concat.  Result is always ARAWK_T_STRING.
// ---------------------------------------------------------------------------

VALUE
arawk_concat(VALUE a, VALUE b)
{
    char abuf[64], bbuf[64]; size_t alen, blen;
    const char *as = arawk_to_cstr(a, abuf, sizeof abuf, &alen);
    const char *bs = arawk_to_cstr(b, bbuf, sizeof bbuf, &blen);
    struct arawk_obj *o = arawk_alloc(ARAWK_T_STRING);
    char *buf = (char *)GC_malloc_atomic(alen + blen + 1);
    memcpy(buf, as, alen);
    memcpy(buf + alen, bs, blen);
    buf[alen + blen] = '\0';
    o->str.chars = buf;
    o->str.len   = alen + blen;
    return ARAWK_OBJ_VAL(o);
}

// ---------------------------------------------------------------------------
// UTF-8 helpers.  Used when ARAWK_ENCODING == ARAWK_ENC_UTF8.
//
// Strategy:
//   - char_count: 1 pass over bytes, increment only on non-continuation
//     bytes (= bytes whose high two bits are NOT 10).  ASCII string
//     (no high-bit byte) → byte count.
//   - byte_at_char: walk forward N codepoints using lead-byte length.
//     Lead bytes: 0xxx = 1, 110x = 2, 1110 = 3, 1111 0xxx = 4.
//   - Invalid UTF-8 sequences are tolerated: lead byte rules failing
//     causes us to advance 1 byte (treat as a single faux codepoint),
//     matching gawk's permissive behaviour.
// ---------------------------------------------------------------------------

// ASCII fast path: scan 8 bytes at a time looking for any high-bit
// byte.  Pure-ASCII strings (most awk inputs) return immediately
// with byte count = codepoint count.  Otherwise fall through to the
// continuation-byte counting loop.
static size_t
arawk_utf8_char_count(const char *s, size_t bytes)
{
    static const uint64_t high_bit_mask = 0x8080808080808080ULL;
    size_t i = 0;
    while (i + 8 <= bytes) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        if (w & high_bit_mask) goto utf8_slow;
        i += 8;
    }
    for (; i < bytes; i++) if ((unsigned char)s[i] & 0x80) goto utf8_slow;
    return bytes;
utf8_slow:
    {
        size_t cnt = 0;
        for (size_t j = 0; j < bytes; j++)
            if (((unsigned char)s[j] & 0xC0) != 0x80) cnt++;
        return cnt;
    }
}

// Return the byte offset of codepoint `char_pos` (0-based) within
// `s[0..bytes)`.  Returns `bytes` if `char_pos` is at or past the
// end (caller may clamp).
static size_t
arawk_utf8_byte_at_char(const char *s, size_t bytes, size_t char_pos)
{
    size_t byte = 0, cp = 0;
    while (byte < bytes && cp < char_pos) {
        unsigned char b = (unsigned char)s[byte];
        size_t step;
        if      ((b & 0x80) == 0x00) step = 1;
        else if ((b & 0xE0) == 0xC0) step = 2;
        else if ((b & 0xF0) == 0xE0) step = 3;
        else if ((b & 0xF8) == 0xF0) step = 4;
        else                         step = 1;     // invalid lead byte
        if (byte + step > bytes) step = bytes - byte;
        byte += step;
        cp++;
    }
    return byte;
}

// Return the codepoint index of byte offset `byte_pos` (clamped).
// Used by `index` to convert a byte-match result back to char pos.
static size_t
arawk_utf8_char_at_byte(const char *s, size_t bytes, size_t byte_pos)
{
    if (byte_pos > bytes) byte_pos = bytes;
    return arawk_utf8_char_count(s, byte_pos);
}

size_t
arawk_length(VALUE v)
{
    if (ARAWK_IS_FIX(v)) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)ARAWK_FIX_VAL(v));
        return n < 0 ? 0 : (size_t)n;
    }
    if (v == ARAWK_UNINIT) return 0;
    struct arawk_obj *o = ARAWK_PTR(v);
    switch (o->type) {
      case ARAWK_T_STRING:
      case ARAWK_T_STRNUM:
        if (ARAWK_ENCODING == ARAWK_ENC_UTF8)
            return arawk_utf8_char_count(o->str.chars, o->str.len);
        return o->str.len;
      case ARAWK_T_ARRAY:  return o->arr.entry_cnt;
      case ARAWK_T_FLOAT: {
        char buf[64]; size_t len;
        (void)arawk_to_cstr(v, buf, sizeof buf, &len);
        return len;
      }
      default: return 0;
    }
}

VALUE
arawk_substr(VALUE s, int64_t pos, int64_t len)
{
    char buf[64]; size_t slen;
    const char *src = arawk_to_cstr(s, buf, sizeof buf, &slen);
    // awk: 1-based, pos < 1 → adjust len; pos > char_len → "".
    if (pos < 1) { len += (pos - 1); pos = 1; }
    if (len <= 0) return arawk_make_string("", 0);
    if (ARAWK_ENCODING == ARAWK_ENC_BYTE) {
        if ((size_t)pos > slen) return arawk_make_string("", 0);
        size_t start = (size_t)(pos - 1);
        size_t avail = slen - start;
        size_t take  = (size_t)len < avail ? (size_t)len : avail;
        return arawk_make_string(src + start, take);
    }
    // UTF-8: pos / len are codepoint counts.
    size_t start_byte = arawk_utf8_byte_at_char(src, slen, (size_t)(pos - 1));
    if (start_byte >= slen) return arawk_make_string("", 0);
    size_t end_byte   = arawk_utf8_byte_at_char(src + start_byte,
                                                slen - start_byte,
                                                (size_t)len);
    return arawk_make_string(src + start_byte, end_byte);
}

VALUE
arawk_substr2(VALUE s, int64_t pos)
{
    char buf[64]; size_t slen;
    const char *src = arawk_to_cstr(s, buf, sizeof buf, &slen);
    if (pos < 1) pos = 1;
    if (ARAWK_ENCODING == ARAWK_ENC_BYTE) {
        if ((size_t)pos > slen) return arawk_make_string("", 0);
        size_t start = (size_t)(pos - 1);
        return arawk_make_string(src + start, slen - start);
    }
    size_t start_byte = arawk_utf8_byte_at_char(src, slen, (size_t)(pos - 1));
    if (start_byte >= slen) return arawk_make_string("", 0);
    return arawk_make_string(src + start_byte, slen - start_byte);
}

int64_t
arawk_index(VALUE haystack, VALUE needle)
{
    char hbuf[64], nbuf[64]; size_t hlen, nlen;
    const char *h = arawk_to_cstr(haystack, hbuf, sizeof hbuf, &hlen);
    const char *n = arawk_to_cstr(needle,   nbuf, sizeof nbuf, &nlen);
    if (nlen == 0) return 0;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(h + i, n, nlen) == 0) {
            // Byte match — convert byte position to codepoint position
            // for UTF-8 mode so the result is in awk's "character units".
            if (ARAWK_ENCODING == ARAWK_ENC_UTF8)
                return (int64_t)arawk_utf8_char_at_byte(h, hlen, i) + 1;
            return (int64_t)(i + 1);
        }
    }
    return 0;
}

VALUE
arawk_tolower(VALUE v)
{
    char buf[64]; size_t len;
    const char *s = arawk_to_cstr(v, buf, sizeof buf, &len);
    char *out = (char *)GC_malloc_atomic(len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[len] = '\0';
    struct arawk_obj *o = arawk_alloc(ARAWK_T_STRING);
    o->str.chars = out;
    o->str.len = len;
    return ARAWK_OBJ_VAL(o);
}

VALUE
arawk_toupper(VALUE v)
{
    char buf[64]; size_t len;
    const char *s = arawk_to_cstr(v, buf, sizeof buf, &len);
    char *out = (char *)GC_malloc_atomic(len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    out[len] = '\0';
    struct arawk_obj *o = arawk_alloc(ARAWK_T_STRING);
    o->str.chars = out;
    o->str.len = len;
    return ARAWK_OBJ_VAL(o);
}

// arawk_int is `static inline` in context.h.

// printf format: %d %i %u %o %x %X %c %s %f %e %E %g %G %%.  Width
// and precision and flags (- + 0 space #) are handled by delegating
// to snprintf with a per-spec format-string mini-compiler.
static void
arawk_format_to(FILE *fp, char **outbuf, size_t *outcap, size_t *outlen,
              VALUE fmt, VALUE *args, size_t nargs)
{
    #define OUT_PUT(c) do { \
        if (*outlen + 2 > *outcap) { *outcap = *outcap ? *outcap * 2 : 64; *outbuf = (char *)GC_realloc(*outbuf, *outcap); } \
        (*outbuf)[(*outlen)++] = (c); \
    } while (0)
    #define OUT_WRITE(s, n) do { \
        if (*outlen + (n) + 1 > *outcap) { while (*outlen + (n) + 1 > *outcap) *outcap = *outcap ? *outcap * 2 : 64; *outbuf = (char *)GC_realloc(*outbuf, *outcap); } \
        memcpy(*outbuf + *outlen, (s), (n)); *outlen += (n); \
    } while (0)

    char fbuf[64]; size_t flen;
    const char *f = arawk_to_cstr(fmt, fbuf, sizeof fbuf, &flen);
    size_t argi = 0;

    for (size_t i = 0; i < flen; ) {
        char ch = f[i];
        if (ch != '%') { OUT_PUT(ch); i++; continue; }
        if (i + 1 < flen && f[i+1] == '%') { OUT_PUT('%'); i += 2; continue; }

        // Parse a single spec into a local format string we hand to snprintf.
        char spec[64]; size_t sl = 0;
        spec[sl++] = '%';
        i++;
        // Flags.
        while (i < flen && (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0')) {
            if (sl + 2 > sizeof spec) break;
            spec[sl++] = f[i++];
        }
        // Width.
        int width_from_arg = 0;
        if (i < flen && f[i] == '*') { width_from_arg = 1; i++; }
        else while (i < flen && f[i] >= '0' && f[i] <= '9') {
            if (sl + 2 > sizeof spec) break;
            spec[sl++] = f[i++];
        }
        // Precision.
        int prec_from_arg = 0;
        if (i < flen && f[i] == '.') {
            if (sl + 2 > sizeof spec) break;
            spec[sl++] = f[i++];
            if (i < flen && f[i] == '*') { prec_from_arg = 1; i++; }
            else while (i < flen && f[i] >= '0' && f[i] <= '9') {
                if (sl + 2 > sizeof spec) break;
                spec[sl++] = f[i++];
            }
        }
        if (i >= flen) { OUT_PUT('%'); break; }
        char conv = f[i++];

        int wval = 0, pval = 0;
        if (width_from_arg) {
            if (argi < nargs) wval = (int)arawk_to_num(args[argi++]);
            int n = snprintf(spec + sl, sizeof spec - sl, "%d", wval);
            if (n > 0) sl += (size_t)n;
        }
        if (prec_from_arg) {
            if (argi < nargs) pval = (int)arawk_to_num(args[argi++]);
            int n = snprintf(spec + sl, sizeof spec - sl, "%d", pval);
            if (n > 0) sl += (size_t)n;
        }

        VALUE av = argi < nargs ? args[argi++] : ARAWK_UNINIT;
        char tmp[256];
        int wrote = 0;
        switch (conv) {
          case 'd': case 'i': {
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = 'd'; spec[sl] = '\0';
            wrote = snprintf(tmp, sizeof tmp, spec, (long long)arawk_to_num(av));
            break;
          }
          case 'u': case 'o': case 'x': case 'X': {
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = '\0';
            wrote = snprintf(tmp, sizeof tmp, spec, (unsigned long long)(long long)arawk_to_num(av));
            break;
          }
          case 'c': {
            // awk semantics: integer → that ASCII char, string → first char.
            if (ARAWK_IS_PTR(av)) {
                struct arawk_obj *o = ARAWK_PTR(av);
                if (o->type == ARAWK_T_STRING || o->type == ARAWK_T_STRNUM) {
                    char one = o->str.len ? o->str.chars[0] : '\0';
                    spec[sl++] = 'c'; spec[sl] = '\0';
                    wrote = snprintf(tmp, sizeof tmp, spec, (int)(unsigned char)one);
                    break;
                }
            }
            int code = (int)arawk_to_num(av);
            spec[sl++] = 'c'; spec[sl] = '\0';
            wrote = snprintf(tmp, sizeof tmp, spec, code);
            break;
          }
          case 's': {
            char sbuf[256]; size_t slen2;
            const char *s = arawk_to_cstr(av, sbuf, sizeof sbuf, &slen2);
            // snprintf needs NUL-terminated input.
            char heap_buf[1024];
            const char *heap = s;
            if (slen2 + 1 > sizeof heap_buf) {
                char *big = (char *)GC_malloc_atomic(slen2 + 1);
                memcpy(big, s, slen2); big[slen2] = '\0';
                heap = big;
            }
            else if (s != sbuf) {
                memcpy(heap_buf, s, slen2); heap_buf[slen2] = '\0';
                heap = heap_buf;
            }
            spec[sl++] = 's'; spec[sl] = '\0';
            wrote = snprintf(tmp, sizeof tmp, spec, heap);
            break;
          }
          case 'f': case 'e': case 'E': case 'g': case 'G': {
            spec[sl++] = conv; spec[sl] = '\0';
            wrote = snprintf(tmp, sizeof tmp, spec, arawk_to_num(av));
            break;
          }
          default:
            // Unknown spec — emit verbatim.
            OUT_WRITE(spec, sl);
            OUT_PUT(conv);
            continue;
        }
        if (wrote > 0) {
            size_t w = (size_t)wrote < sizeof tmp ? (size_t)wrote : sizeof tmp - 1;
            OUT_WRITE(tmp, w);
        }
    }

    if (fp) {
        fwrite(*outbuf, 1, *outlen, fp);
    }

    #undef OUT_PUT
    #undef OUT_WRITE
}

VALUE
arawk_sprintf_v(VALUE fmt, VALUE *args, size_t nargs)
{
    char *buf = NULL; size_t cap = 0, len = 0;
    arawk_format_to(NULL, &buf, &cap, &len, fmt, args, nargs);
    return arawk_make_string(buf ? buf : "", len);
}

void
arawk_printf(FILE *fp, VALUE fmt, VALUE *args, size_t nargs)
{
    char *buf = NULL; size_t cap = 0, len = 0;
    arawk_format_to(fp, &buf, &cap, &len, fmt, args, nargs);
}

int64_t
arawk_split(VALUE s, VALUE arr, VALUE sep)
{
    if (!ARAWK_IS_PTR(arr) || ARAWK_PTR(arr)->type != ARAWK_T_ARRAY) return 0;
    // Clear the array (split overwrites).
    struct arawk_obj *ao = ARAWK_PTR(arr);
    for (size_t i = 0; i < ao->arr.bucket_cnt; i++) ao->arr.buckets[i] = NULL;
    ao->arr.entry_cnt = 0;

    char sbuf[64], pbuf[64]; size_t slen, plen;
    const char *src = arawk_to_cstr(s,   sbuf, sizeof sbuf, &slen);
    const char *sp  = arawk_to_cstr(sep, pbuf, sizeof pbuf, &plen);

    int64_t n = 0;
    char kbuf[24];
    bool default_fs = (plen == 1 && sp[0] == ' ');
    if (default_fs) {
        size_t i = 0;
        while (i < slen) {
            while (i < slen && isspace((unsigned char)src[i])) i++;
            if (i >= slen) break;
            size_t start = i;
            while (i < slen && !isspace((unsigned char)src[i])) i++;
            int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
            arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + start, i - start));
        }
    }
    else if (plen == 1) {
        char c = sp[0];
        size_t i = 0, start = 0;
        while (i < slen) {
            if (src[i] == c) {
                int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
                arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + start, i - start));
                i++; start = i;
            }
            else i++;
        }
        int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
        arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + start, slen - start));
    }
    else if (plen == 0) {
        // Empty sep → split into individual characters.
        for (size_t i = 0; i < slen; i++) {
            int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
            arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + i, 1));
        }
    }
    else {
        size_t i = 0, start = 0;
        while (i + plen <= slen) {
            if (memcmp(src + i, sp, plen) == 0) {
                int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
                arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + start, i - start));
                i += plen; start = i;
            }
            else i++;
        }
        int kl = snprintf(kbuf, sizeof kbuf, "%lld", (long long)(++n));
        arawk_arr_set(arr, kbuf, (size_t)kl, arawk_make_strnum(src + start, slen - start));
    }
    return n;
}

// ---------------------------------------------------------------------------
// Associative arrays.  Simple chained hash; rehash on load > 0.75.
// ---------------------------------------------------------------------------

static uint64_t
arawk_str_hash(const char *s, size_t len)
{
    // FNV-1a 64-bit.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void
arawk_arr_rehash(struct arawk_array *a, size_t new_cnt)
{
    struct arawk_array_entry **old_b = a->buckets;
    size_t old_cnt = a->bucket_cnt;
    a->buckets = (struct arawk_array_entry **)GC_malloc(sizeof(struct arawk_array_entry *) * new_cnt);
    a->bucket_cnt = new_cnt;
    for (size_t i = 0; i < old_cnt; i++) {
        struct arawk_array_entry *e = old_b[i];
        while (e) {
            struct arawk_array_entry *next = e->next;
            uint64_t h = arawk_str_hash(e->key, e->key_len);
            size_t b = (size_t)(h & (new_cnt - 1));
            e->next = a->buckets[b];
            a->buckets[b] = e;
            e = next;
        }
    }
}

VALUE
arawk_arr_get(VALUE arr, const char *key, size_t key_len)
{
    if (!ARAWK_IS_PTR(arr)) return ARAWK_UNINIT;
    struct arawk_obj *o = ARAWK_PTR(arr);
    if (o->type != ARAWK_T_ARRAY) return ARAWK_UNINIT;
    uint64_t h = arawk_str_hash(key, key_len);
    size_t b = (size_t)(h & (o->arr.bucket_cnt - 1));
    for (struct arawk_array_entry *e = o->arr.buckets[b]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) return e->val;
    }
    return ARAWK_UNINIT;
}

void
arawk_arr_set(VALUE arr, const char *key, size_t key_len, VALUE val)
{
    if (!ARAWK_IS_PTR(arr)) return;
    struct arawk_obj *o = ARAWK_PTR(arr);
    if (o->type != ARAWK_T_ARRAY) return;
    uint64_t h = arawk_str_hash(key, key_len);
    size_t b = (size_t)(h & (o->arr.bucket_cnt - 1));
    for (struct arawk_array_entry *e = o->arr.buckets[b]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            e->val = val;
            return;
        }
    }
    struct arawk_array_entry *ne = (struct arawk_array_entry *)GC_malloc(sizeof(struct arawk_array_entry));
    char *kbuf = (char *)GC_malloc_atomic(key_len + 1);
    memcpy(kbuf, key, key_len);
    kbuf[key_len] = '\0';
    ne->key = kbuf;
    ne->key_len = key_len;
    ne->val = val;
    ne->next = o->arr.buckets[b];
    o->arr.buckets[b] = ne;
    o->arr.entry_cnt++;
    if (o->arr.entry_cnt * 4 > o->arr.bucket_cnt * 3) {
        arawk_arr_rehash(&o->arr, o->arr.bucket_cnt * 2);
    }
}

bool
arawk_arr_has(VALUE arr, const char *key, size_t key_len)
{
    if (!ARAWK_IS_PTR(arr)) return false;
    struct arawk_obj *o = ARAWK_PTR(arr);
    if (o->type != ARAWK_T_ARRAY) return false;
    uint64_t h = arawk_str_hash(key, key_len);
    size_t b = (size_t)(h & (o->arr.bucket_cnt - 1));
    for (struct arawk_array_entry *e = o->arr.buckets[b]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) return true;
    }
    return false;
}

void
arawk_arr_del(VALUE arr, const char *key, size_t key_len)
{
    if (!ARAWK_IS_PTR(arr)) return;
    struct arawk_obj *o = ARAWK_PTR(arr);
    if (o->type != ARAWK_T_ARRAY) return;
    uint64_t h = arawk_str_hash(key, key_len);
    size_t b = (size_t)(h & (o->arr.bucket_cnt - 1));
    struct arawk_array_entry **pp = &o->arr.buckets[b];
    while (*pp) {
        struct arawk_array_entry *e = *pp;
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            *pp = e->next;
            o->arr.entry_cnt--;
            return;
        }
        pp = &e->next;
    }
}

// ---------------------------------------------------------------------------
// Output.
// ---------------------------------------------------------------------------

void
arawk_print_value(FILE *fp, VALUE v)
{
    char buf[64]; size_t len;
    const char *s = arawk_to_cstr(v, buf, sizeof buf, &len);
    fwrite(s, 1, len, fp);
}

void
arawk_print_record(FILE *fp, VALUE *items, size_t n,
                 const char *ofs, size_t ofs_len,
                 const char *ors, size_t ors_len)
{
    for (size_t i = 0; i < n; i++) {
        if (i) fwrite(ofs, 1, ofs_len, fp);
        arawk_print_value(fp, items[i]);
    }
    fwrite(ors, 1, ors_len, fp);
}

// ---------------------------------------------------------------------------
// Output stream cache.  Each unique (mode, dest) tuple maps to one
// FILE * for the lifetime of the program — repeated `print | "sort"`
// statements share the same popen pipe, which is the awk semantic.
// `arawk_close_all_streams` is called from main() after the program
// finishes; it flushes and pcloses each pipe (so `sort` actually
// reads EOF and writes its output before the process exits).
// ---------------------------------------------------------------------------

// Per-stream read buffer.  Used only by input-side streams (mode 'r'
// or 'i') and for the implicit cur_input; output streams leave these
// zero-initialised.  64 KB is large enough to hold a full disk read
// boundary while staying small enough to avoid wasting memory on
// many simultaneously-open getline sources.
struct arawk_rdbuf {
    char  *data;
    size_t capa;
    size_t len;            // valid bytes in data
    size_t pos;            // next byte to read
};

struct arawk_stream {
    int    mode;
    char  *dest;
    FILE  *fp;
    bool   is_pipe;        // true → pclose, false → fclose
    struct arawk_rdbuf rdbuf;
};

#define ARAWK_RDBUF_SIZE 65536

// Read one RS-delimited record (default RS = "\n") from `fp` using
// `rb` as a chunk-level buffer.  Returns 1 (read), 0 (EOF), -1
// (I/O error).  Replaces the per-character fgetc loop — fread fills
// `rb` once per ~64 KB, then `memchr('\n', ...)` finds record
// boundaries in user space without crossing the PLT each character.
static int
arawk_read_line_buf(FILE *fp, struct arawk_rdbuf *rb,
                    char **out, size_t *out_len, size_t *out_capa)
{
    if (!fp) return -1;
    if (UNLIKELY(rb->data == NULL)) {
        rb->capa = ARAWK_RDBUF_SIZE;
        rb->data = (char *)GC_malloc_atomic(rb->capa);
    }
    size_t len = 0;
    for (;;) {
        if (rb->pos >= rb->len) {
            size_t n = fread(rb->data, 1, rb->capa, fp);
            if (n == 0) {
                if (ferror(fp)) return -1;
                if (len == 0) return 0;
                break;
            }
            rb->len = n;
            rb->pos = 0;
        }
        char *base = rb->data + rb->pos;
        size_t avail = rb->len - rb->pos;
        char *p = (char *)memchr(base, '\n', avail);
        size_t chunk = p ? (size_t)(p - base) : avail;
        if (len + chunk + 1 > *out_capa) {
            size_t cap = *out_capa ? *out_capa * 2 : 256;
            while (cap < len + chunk + 1) cap *= 2;
            *out = (char *)GC_realloc(*out, cap);
            *out_capa = cap;
        }
        memcpy(*out + len, base, chunk);
        len += chunk;
        rb->pos += chunk;
        if (p) { rb->pos++; break; }   // consume the '\n'
    }
    if (len + 1 > *out_capa) {
        size_t cap = *out_capa ? *out_capa * 2 : 256;
        *out = (char *)GC_realloc(*out, cap);
        *out_capa = cap;
    }
    (*out)[len] = '\0';
    *out_len = len;
    return 1;
}

static struct arawk_stream *arawk_streams = NULL;
static size_t arawk_streams_cnt = 0;
static size_t arawk_streams_capa = 0;

// Input-side handles for `getline < file` and `cmd | getline`.
// Symmetric to arawk_streams (defined here so arawk_close_stream can scan
// both).
static struct arawk_stream *arawk_inputs = NULL;
static size_t arawk_inputs_cnt = 0;
static size_t arawk_inputs_capa = 0;

FILE *
arawk_open_stream(int mode, VALUE dest)
{
    char dbuf[256]; size_t dlen;
    const char *dest_s = arawk_to_cstr(dest, dbuf, sizeof dbuf, &dlen);
    // NUL-terminate for popen / fopen.
    char *path = (char *)alloca(dlen + 1);
    memcpy(path, dest_s, dlen); path[dlen] = '\0';

    for (size_t i = 0; i < arawk_streams_cnt; i++) {
        if (arawk_streams[i].mode == mode && strcmp(arawk_streams[i].dest, path) == 0) {
            return arawk_streams[i].fp;
        }
    }
    if (arawk_streams_cnt >= arawk_streams_capa) {
        size_t cap = arawk_streams_capa ? arawk_streams_capa * 2 : 8;
        arawk_streams = (struct arawk_stream *)realloc(arawk_streams, sizeof(struct arawk_stream) * cap);
        arawk_streams_capa = cap;
    }

    FILE *f = NULL;
    bool is_pipe = false;
    if (mode == 'w') {
        f = popen(path, "w");
        is_pipe = true;
    } else if (mode == 'o') {
        f = fopen(path, "w");
    } else if (mode == 'a') {
        f = fopen(path, "a");
    }
    if (!f) {
        fprintf(stderr, "arawk: cannot open `%s` (mode %c)\n", path, mode);
        exit(2);
    }
    char *dst_copy = (char *)malloc(dlen + 1);
    memcpy(dst_copy, path, dlen + 1);
    arawk_streams[arawk_streams_cnt++] = (struct arawk_stream){ mode, dst_copy, f, is_pipe };
    return f;
}

void
arawk_close_all_streams(void)
{
    for (size_t i = 0; i < arawk_streams_cnt; i++) {
        if (arawk_streams[i].is_pipe) pclose(arawk_streams[i].fp);
        else                        fclose(arawk_streams[i].fp);
        free(arawk_streams[i].dest);
    }
    arawk_streams_cnt = 0;
    for (size_t i = 0; i < arawk_inputs_cnt; i++) {
        if (arawk_inputs[i].is_pipe) pclose(arawk_inputs[i].fp);
        else                       fclose(arawk_inputs[i].fp);
        free(arawk_inputs[i].dest);
    }
    arawk_inputs_cnt = 0;
}

int
arawk_close_stream(VALUE dest)
{
    char dbuf[256]; size_t dlen;
    const char *d = arawk_to_cstr(dest, dbuf, sizeof dbuf, &dlen);
    char *path = (char *)alloca(dlen + 1);
    memcpy(path, d, dlen); path[dlen] = '\0';
    // Search output side first.
    for (size_t i = 0; i < arawk_streams_cnt; i++) {
        if (strcmp(arawk_streams[i].dest, path) == 0) {
            int rc = arawk_streams[i].is_pipe ? pclose(arawk_streams[i].fp)
                                            : fclose(arawk_streams[i].fp);
            free(arawk_streams[i].dest);
            if (i + 1 < arawk_streams_cnt) arawk_streams[i] = arawk_streams[arawk_streams_cnt - 1];
            arawk_streams_cnt--;
            return rc;
        }
    }
    // Then input streams (file readers, command-pipe readers).
    for (size_t i = 0; i < arawk_inputs_cnt; i++) {
        if (strcmp(arawk_inputs[i].dest, path) == 0) {
            int rc = arawk_inputs[i].is_pipe ? pclose(arawk_inputs[i].fp)
                                           : fclose(arawk_inputs[i].fp);
            free(arawk_inputs[i].dest);
            if (i + 1 < arawk_inputs_cnt) arawk_inputs[i] = arawk_inputs[arawk_inputs_cnt - 1];
            arawk_inputs_cnt--;
            return rc;
        }
    }
    return -1;
}

int
arawk_fflush_all(void)
{
    int rc = fflush(stdout);
    for (size_t i = 0; i < arawk_streams_cnt; i++) {
        if (fflush(arawk_streams[i].fp) != 0) rc = -1;
    }
    return rc;
}

static void arawk_split_fields(CTX *c);

// ---------------------------------------------------------------------------
// Input stream cache (Phase 1.12, getline).  Symmetric to the output
// cache: every distinct (mode, name) tuple → one FILE * for the run.
// Modes:
//   'r' — popen(cmd, "r")        — for `"cmd" | getline`
//   'i' — fopen(path, "r")       — for `getline < "file"`
//
// `getline` reads share these handles, so repeated calls advance
// through the same stream — exactly the POSIX semantics.
// ---------------------------------------------------------------------------

static FILE *
arawk_open_input(int mode, VALUE dest)
{
    char dbuf[256]; size_t dlen;
    const char *d = arawk_to_cstr(dest, dbuf, sizeof dbuf, &dlen);
    char *path = (char *)alloca(dlen + 1);
    memcpy(path, d, dlen); path[dlen] = '\0';

    for (size_t i = 0; i < arawk_inputs_cnt; i++) {
        if (arawk_inputs[i].mode == mode && strcmp(arawk_inputs[i].dest, path) == 0) {
            return arawk_inputs[i].fp;
        }
    }
    if (arawk_inputs_cnt >= arawk_inputs_capa) {
        size_t cap = arawk_inputs_capa ? arawk_inputs_capa * 2 : 8;
        arawk_inputs = (struct arawk_stream *)realloc(arawk_inputs, sizeof(struct arawk_stream) * cap);
        arawk_inputs_capa = cap;
    }
    FILE *f = NULL;
    bool is_pipe = false;
    if (mode == 'r') { f = popen(path, "r"); is_pipe = true; }
    else if (mode == 'i') { f = fopen(path, "r"); }
    if (!f) return NULL;     // getline can fail → return -1 to caller
    char *dst_copy = (char *)malloc(dlen + 1);
    memcpy(dst_copy, path, dlen + 1);
    arawk_inputs[arawk_inputs_cnt++] = (struct arawk_stream){ mode, dst_copy, f, is_pipe };
    return f;
}

// Locate the per-stream read buffer by FILE *.  arawk_streams[] is
// linear-scanned (typically 0-2 entries per process).  The implicit
// cur_input stream has its own static rdbuf since it isn't in the
// arawk_inputs[] array.
static struct arawk_rdbuf cur_input_rdbuf;

static struct arawk_rdbuf *
arawk_rdbuf_for(FILE *fp)
{
    for (size_t i = 0; i < arawk_inputs_cnt; i++) {
        if (arawk_inputs[i].fp == fp) return &arawk_inputs[i].rdbuf;
    }
    return &cur_input_rdbuf;
}

// Public arawk_read_record_into: looks up the right per-FILE buffer
// and delegates to arawk_read_line_buf.  Kept for getline helpers
// that operate on already-resolved file handles.
int
arawk_read_record_into(FILE *fp, char **buf, size_t *buf_len, size_t *buf_capa)
{
    return arawk_read_line_buf(fp, arawk_rdbuf_for(fp), buf, buf_len, buf_capa);
}

// Open the next input file if necessary; returns the current FILE *,
// or NULL at EOF across all inputs.  Adapted from arawk_input_next_record's
// internal logic.
static FILE *
arawk_cur_input(CTX *c)
{
    if (!c->cur_input) {
        if (OPTION.input_file_cnt == 0) {
            if (c->cur_input_idx == 0) {
                c->cur_input = stdin;
                c->cur_input_idx = 1;
                c->env[ARAWK_GLOB_FILENAME] = arawk_make_string("", 0);
            }
            return c->cur_input;
        }
        while (c->cur_input_idx < OPTION.input_file_cnt) {
            const char *path = OPTION.input_files[c->cur_input_idx++];
            FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
            if (!f) { fprintf(stderr, "arawk: cannot open `%s`\n", path); continue; }
            c->cur_input = f;
            c->env[ARAWK_GLOB_FILENAME] = arawk_make_string(path, strlen(path));
            c->env[ARAWK_GLOB_FNR] = ARAWK_FIX(0);
            break;
        }
    }
    return c->cur_input;
}

// Internal helper: install a freshly-read record into c->rec and
// update $0 / NF (and NR / FNR if `bump_counters`).
static void
arawk_install_record(CTX *c, const char *line, size_t len, bool bump_counters)
{
    char *rec = (char *)GC_malloc_atomic(len + 1);
    memcpy(rec, line, len); rec[len] = '\0';
    c->rec.record = rec;
    c->rec.record_len = len;
    c->rec.record_v = 0;
    c->rec.fields_split = false;
    c->rec.nf = 0;
    if (bump_counters) {
        int64_t nr  = ARAWK_IS_FIX(c->env[ARAWK_GLOB_NR])  ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_NR])  : 0;
        int64_t fnr = ARAWK_IS_FIX(c->env[ARAWK_GLOB_FNR]) ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_FNR]) : 0;
        c->env[ARAWK_GLOB_NR]  = ARAWK_FIX(nr + 1);
        c->env[ARAWK_GLOB_FNR] = ARAWK_FIX(fnr + 1);
    }
    arawk_split_fields(c);
}

int
arawk_getline_cur(CTX *c, VALUE *out_line)
{
    (void)out_line;
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    for (;;) {
        FILE *fp = arawk_cur_input(c);
        if (!fp) return 0;
        int rc = arawk_read_record_into(fp, &buf, &len, &cap);
        if (rc == 1) {
            arawk_install_record(c, buf, len, true);
            return 1;
        }
        if (rc < 0) return -1;
        // EOF on this stream — try the next input file.
        if (c->cur_input && c->cur_input != stdin) fclose(c->cur_input);
        c->cur_input = NULL;
        if (OPTION.input_file_cnt == 0 || c->cur_input_idx >= OPTION.input_file_cnt) return 0;
    }
}

int
arawk_getline_cur_var(CTX *c, VALUE *out_line)
{
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    for (;;) {
        FILE *fp = arawk_cur_input(c);
        if (!fp) return 0;
        int rc = arawk_read_record_into(fp, &buf, &len, &cap);
        if (rc == 1) {
            int64_t nr  = ARAWK_IS_FIX(c->env[ARAWK_GLOB_NR])  ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_NR])  : 0;
            int64_t fnr = ARAWK_IS_FIX(c->env[ARAWK_GLOB_FNR]) ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_FNR]) : 0;
            c->env[ARAWK_GLOB_NR]  = ARAWK_FIX(nr + 1);
            c->env[ARAWK_GLOB_FNR] = ARAWK_FIX(fnr + 1);
            *out_line = arawk_make_strnum(buf, len);
            return 1;
        }
        if (rc < 0) return -1;
        if (c->cur_input && c->cur_input != stdin) fclose(c->cur_input);
        c->cur_input = NULL;
        if (OPTION.input_file_cnt == 0 || c->cur_input_idx >= OPTION.input_file_cnt) return 0;
    }
}

int
arawk_getline_file(CTX *c, VALUE dest)
{
    FILE *fp = arawk_open_input('i', dest);
    if (!fp) return -1;
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    int rc = arawk_read_record_into(fp, &buf, &len, &cap);
    if (rc != 1) return rc;
    arawk_install_record(c, buf, len, false);  // file getline: NR/FNR not bumped
    return 1;
}

int
arawk_getline_file_var(CTX *c, VALUE dest, VALUE *out_line)
{
    (void)c;
    FILE *fp = arawk_open_input('i', dest);
    if (!fp) return -1;
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    int rc = arawk_read_record_into(fp, &buf, &len, &cap);
    if (rc != 1) return rc;
    *out_line = arawk_make_strnum(buf, len);
    return 1;
}

int
arawk_getline_cmd(CTX *c, VALUE cmd)
{
    FILE *fp = arawk_open_input('r', cmd);
    if (!fp) return -1;
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    int rc = arawk_read_record_into(fp, &buf, &len, &cap);
    if (rc != 1) return rc;
    arawk_install_record(c, buf, len, false);
    return 1;
}

int
arawk_getline_cmd_var(CTX *c, VALUE cmd, VALUE *out_line)
{
    (void)c;
    FILE *fp = arawk_open_input('r', cmd);
    if (!fp) return -1;
    static char *buf = NULL; static size_t cap = 0; size_t len = 0;
    int rc = arawk_read_record_into(fp, &buf, &len, &cap);
    if (rc != 1) return rc;
    *out_line = arawk_make_strnum(buf, len);
    return 1;
}

int
arawk_fflush_stream(VALUE dest)
{
    char dbuf[256]; size_t dlen;
    const char *d = arawk_to_cstr(dest, dbuf, sizeof dbuf, &dlen);
    if (dlen == 0) return arawk_fflush_all();
    char *path = (char *)alloca(dlen + 1);
    memcpy(path, d, dlen); path[dlen] = '\0';
    // awk historical: fflush("stdout") / "stderr" → those streams.
    if (strcmp(path, "stdout") == 0) return fflush(stdout);
    if (strcmp(path, "stderr") == 0) return fflush(stderr);
    for (size_t i = 0; i < arawk_streams_cnt; i++) {
        if (strcmp(arawk_streams[i].dest, path) == 0) return fflush(arawk_streams[i].fp);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Special-variable read/write helpers.
// ---------------------------------------------------------------------------

// `NF = N` — grow or shrink the current record.  POSIX semantics:
//   N > NF: extend with "" fields, then rebuild $0 using OFS
//   N < NF: drop trailing fields, rebuild $0
//   N == NF: no-op
//   N < 0:  error
static void arawk_rebuild_record(CTX *c);

// Grow the 3 parallel field arrays so that index `target` (0-based)
// fits, doubling capacity from the current value.  Called from
// arawk_set_nf / arawk_set_field.
static void
arawk_grow_fields(CTX *c, int target)
{
    if (target < c->rec.fields_capa) return;
    int cap = c->rec.fields_capa ? c->rec.fields_capa * 2 : 8;
    while (cap <= target) cap *= 2;
    int   *ns = (int   *)GC_malloc_atomic(sizeof(int)   * cap);
    int   *nl = (int   *)GC_malloc_atomic(sizeof(int)   * cap);
    VALUE *nv = (VALUE *)GC_malloc       (sizeof(VALUE) * cap);
    if (c->rec.field_starts) memcpy(ns, c->rec.field_starts, sizeof(int)   * c->rec.fields_capa);
    if (c->rec.field_lens)   memcpy(nl, c->rec.field_lens,   sizeof(int)   * c->rec.fields_capa);
    if (c->rec.fields)       memcpy(nv, c->rec.fields,       sizeof(VALUE) * c->rec.fields_capa);
    c->rec.field_starts = ns;
    c->rec.field_lens   = nl;
    c->rec.fields       = nv;
    c->rec.fields_capa  = cap;
}

void
arawk_set_nf(CTX *c, VALUE v)
{
    int64_t new_nf = ARAWK_IS_FIX(v) ? ARAWK_FIX_VAL(v) : (int64_t)arawk_to_num(v);
    if (new_nf < 0) {
        fprintf(stderr, "arawk: NF cannot be negative\n");
        exit(2);
    }
    arawk_split_fields(c);
    if (new_nf > 0) arawk_grow_fields(c, (int)new_nf - 1);
    // Extend with empty fields: boundary (0, 0) + sentinel 0 = lazy "".
    for (int i = c->rec.nf; i < (int)new_nf; i++) {
        c->rec.field_starts[i] = 0;
        c->rec.field_lens[i]   = 0;
        c->rec.fields[i]       = 0;
    }
    c->rec.nf = (int)new_nf;
    c->env[ARAWK_GLOB_NF] = ARAWK_FIX(new_nf);
    arawk_rebuild_record(c);
}

// Phase 0+1 default FS = " " (awk's "any run of whitespace,
// leading/trailing trimmed").  Single-char FS and regex FS are Phase 2.
//
// We currently split eagerly on every record read.  NF needs to be
// available before any $N is accessed (e.g. `{ wc += NF }`), so a
// purely-lazy approach would force a separate counting pass.  Eager
// strnum allocation is fine for now; the cost is one strnum object
// per field which libgc collects cheaply.  Phase 2 can split lazily
// for $N while still pre-computing NF.
// Lazy split.  We only record the boundary (offset, length) of each
// field; the strnum VALUE is materialised on first read in
// arawk_get_field.  Scripts that touch NF / $0 only — or just one or
// two $N — used to pay the cost of allocating all NF strnums; now
// they pay just for the fields they actually read.
//
// fields_capa now sizes 3 parallel arrays: field_starts / field_lens
// / fields.  Capacity grows by doubling at the same threshold.
static void
arawk_split_fields(CTX *c)
{
    if (c->rec.fields_split) return;
    c->rec.fields_split = true;

    // Read FS from env (slot 2).  Default to " ".
    VALUE fsv = c->env[ARAWK_GLOB_FS];
    char fsbuf[32]; size_t fslen;
    const char *fs = arawk_to_cstr(fsv, fsbuf, sizeof fsbuf, &fslen);
    bool fs_default = (fslen == 1 && fs[0] == ' ');

    const char *r = c->rec.record;
    size_t rlen = c->rec.record_len;
    int nf = 0;

    // Capacity grow helper: 3 parallel arrays grow together.  Both
    // int[] are atomic (no pointers); only the VALUE[] is traced.
    #define GROW_IF_NEEDED() do { \
        if (nf >= c->rec.fields_capa) { \
            int new_capa = c->rec.fields_capa ? c->rec.fields_capa * 2 : 8; \
            int   *ns = (int   *)GC_malloc_atomic(sizeof(int)   * new_capa); \
            int   *nl = (int   *)GC_malloc_atomic(sizeof(int)   * new_capa); \
            VALUE *nv = (VALUE *)GC_malloc       (sizeof(VALUE) * new_capa); \
            if (c->rec.field_starts) memcpy(ns, c->rec.field_starts, sizeof(int)   * c->rec.fields_capa); \
            if (c->rec.field_lens)   memcpy(nl, c->rec.field_lens,   sizeof(int)   * c->rec.fields_capa); \
            if (c->rec.fields)       memcpy(nv, c->rec.fields,       sizeof(VALUE) * c->rec.fields_capa); \
            c->rec.field_starts = ns; \
            c->rec.field_lens   = nl; \
            c->rec.fields       = nv; \
            c->rec.fields_capa  = new_capa; \
        } \
    } while (0)
    #define RECORD_FIELD(start, length) do { \
        GROW_IF_NEEDED(); \
        c->rec.field_starts[nf] = (int)(start); \
        c->rec.field_lens[nf]   = (int)(length); \
        c->rec.fields[nf]       = 0;            /* lazy sentinel */ \
        nf++; \
    } while (0)

    if (fs_default) {
        // Default FS: skip whitespace runs; fields are non-ws spans.
        size_t i = 0;
        while (i < rlen) {
            while (i < rlen && isspace((unsigned char)r[i])) i++;
            if (i >= rlen) break;
            size_t start = i;
            while (i < rlen && !isspace((unsigned char)r[i])) i++;
            RECORD_FIELD(start, i - start);
        }
    }
    else if (fslen == 1) {
        char sep = fs[0];
        size_t i = 0, start = 0;
        while (i < rlen) {
            if (r[i] == sep) {
                RECORD_FIELD(start, i - start);
                i++;
                start = i;
            }
            else i++;
        }
        RECORD_FIELD(start, rlen - start);
    }
    else {
        // Multi-char FS: regex split (Phase 2).  For now treat as
        // literal-string separator.
        size_t i = 0, start = 0;
        while (i + fslen <= rlen) {
            if (memcmp(r + i, fs, fslen) == 0) {
                RECORD_FIELD(start, i - start);
                i += fslen;
                start = i;
            }
            else i++;
        }
        RECORD_FIELD(start, rlen - start);
    }
    #undef RECORD_FIELD
    #undef GROW_IF_NEEDED

    c->rec.nf = nf;
    c->env[ARAWK_GLOB_NF] = ARAWK_FIX(nf);
}

VALUE
arawk_get_field(CTX *c, int64_t n)
{
    if (n == 0) {
        if (c->rec.record_v) return c->rec.record_v;
        c->rec.record_v = arawk_make_strnum(c->rec.record, c->rec.record_len);
        return c->rec.record_v;
    }
    if (n < 0) {
        fprintf(stderr, "arawk: negative field index $%lld\n", (long long)n);
        exit(2);
    }
    arawk_split_fields(c);
    if (n > c->rec.nf) return arawk_make_string("", 0);
    int idx = (int)n - 1;
    VALUE v = c->rec.fields[idx];
    if (v == 0) {
        v = arawk_make_strnum(c->rec.record + c->rec.field_starts[idx],
                              c->rec.field_lens[idx]);
        c->rec.fields[idx] = v;
    }
    return v;
}

VALUE
arawk_get_field_v(CTX *c, VALUE idx)
{
    int64_t n;
    if (ARAWK_IS_FIX(idx)) n = ARAWK_FIX_VAL(idx);
    else                 n = (int64_t)arawk_to_num(idx);
    return arawk_get_field(c, n);
}

// Rebuild $0 from the current fields[] using OFS.  Called lazily
// after a $N (N>0) assignment, when the next reader of $0 wants the
// updated record.  Sets c->rec.record / record_v / record_len.
//
// Reads every field through arawk_get_field so that lazy boundaries
// and cached VALUEs are both honoured.  This forces materialisation
// of all fields — unavoidable when joining the full record.
static void
arawk_rebuild_record(CTX *c)
{
    arawk_split_fields(c);
    VALUE ofs_v = c->env[ARAWK_GLOB_OFS];
    char obuf[32]; size_t olen;
    const char *ofs = arawk_to_cstr(ofs_v, obuf, sizeof obuf, &olen);

    size_t total = 0;
    for (int i = 0; i < c->rec.nf; i++) {
        char fbuf[64]; size_t flen;
        (void)arawk_to_cstr(arawk_get_field(c, i + 1), fbuf, sizeof fbuf, &flen);
        total += flen;
    }
    if (c->rec.nf > 0) total += (size_t)(c->rec.nf - 1) * olen;
    char *buf = (char *)GC_malloc_atomic(total + 1);
    size_t k = 0;
    for (int i = 0; i < c->rec.nf; i++) {
        if (i) { memcpy(buf + k, ofs, olen); k += olen; }
        char fbuf[64]; size_t flen;
        const char *fs = arawk_to_cstr(arawk_get_field(c, i + 1), fbuf, sizeof fbuf, &flen);
        memcpy(buf + k, fs, flen); k += flen;
    }
    buf[k] = '\0';
    c->rec.record = buf;
    c->rec.record_len = k;
    c->rec.record_v = 0;
}

void
arawk_set_field(CTX *c, int64_t n, VALUE v)
{
    if (n < 0) {
        fprintf(stderr, "arawk: negative field index $%lld\n", (long long)n);
        exit(2);
    }
    if (n == 0) {
        // $0 = ...: store new record and clear field split.
        char buf[64]; size_t len;
        const char *s = arawk_to_cstr(v, buf, sizeof buf, &len);
        char *nb = (char *)GC_malloc_atomic(len + 1);
        memcpy(nb, s, len);
        nb[len] = '\0';
        c->rec.record = nb;
        c->rec.record_len = len;
        c->rec.record_v = 0;
        c->rec.fields_split = false;
        c->rec.nf = 0;
        // Re-split so NF is up to date.
        arawk_split_fields(c);
        return;
    }
    // $N = ... — make sure fields[] exists, grow if needed, set the
    // element, rebuild $0 lazily.
    arawk_split_fields(c);
    arawk_grow_fields(c, (int)n - 1);
    // Pad intervening fields lazily: boundary (0, 0) + sentinel 0 →
    // "" generated on demand.
    for (int i = c->rec.nf; i < (int)n - 1; i++) {
        c->rec.field_starts[i] = 0;
        c->rec.field_lens[i]   = 0;
        c->rec.fields[i]       = 0;
    }
    c->rec.fields[n - 1] = v;
    if ((int)n > c->rec.nf) {
        c->rec.nf = (int)n;
        c->env[ARAWK_GLOB_NF] = ARAWK_FIX(c->rec.nf);
    }
    arawk_rebuild_record(c);
}

// ---------------------------------------------------------------------------
// Input loop.
// ---------------------------------------------------------------------------

static bool
arawk_open_next_input(CTX *c)
{
    if (c->cur_input && c->cur_input != stdin) {
        fclose(c->cur_input);
        c->cur_input = NULL;
    }
    // The cur_input rdbuf belonged to the previous file (or to the
    // previous stdin run).  Drop any residual buffered bytes so the
    // next file doesn't see leftover data — `nextfile` in particular
    // triggers this path mid-buffer.
    cur_input_rdbuf.len = 0;
    cur_input_rdbuf.pos = 0;
    if (OPTION.input_file_cnt == 0) {
        if (c->cur_input_idx == 0) {
            c->cur_input = stdin;
            c->cur_input_idx = 1;
            c->env[ARAWK_GLOB_FILENAME] = arawk_make_string("", 0);
            return true;
        }
        return false;
    }
    while (c->cur_input_idx < OPTION.input_file_cnt) {
        const char *path = OPTION.input_files[c->cur_input_idx++];
        FILE *f;
        if (strcmp(path, "-") == 0) f = stdin;
        else                        f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "arawk: cannot open `%s`\n", path);
            continue;
        }
        c->cur_input = f;
        c->env[ARAWK_GLOB_FILENAME] = arawk_make_string(path, strlen(path));
        c->env[ARAWK_GLOB_FNR] = ARAWK_FIX(0);
        return true;
    }
    return false;
}

bool
arawk_input_next_record(CTX *c)
{
    // Default RS = "\n" — read line-by-line.  Phase 2: regex RS, RS="".
    if (c->input_done) return false;
    if (!c->cur_input) {
        if (!arawk_open_next_input(c)) { c->input_done = true; return false; }
    }

    static char *line_buf = NULL;
    static size_t line_capa = 0;
    size_t len = 0;

    for (;;) {
        int rc = arawk_read_line_buf(c->cur_input, &cur_input_rdbuf,
                                     &line_buf, &len, &line_capa);
        if (rc == 1) break;
        if (rc < 0) { c->input_done = true; return false; }
        // EOF on this source — try the next input file.  Reset the
        // buffer because the next FILE * is independent (and we
        // wouldn't be able to seek backward on a pipe anyway).
        cur_input_rdbuf.len = 0;
        cur_input_rdbuf.pos = 0;
        if (!arawk_open_next_input(c)) { c->input_done = true; return false; }
    }

    // Copy into GC-traced storage (line_buf is reused next iter).
    char *rec = (char *)GC_malloc_atomic(len + 1);
    memcpy(rec, line_buf, len);
    rec[len] = '\0';
    c->rec.record = rec;
    c->rec.record_len = len;
    c->rec.record_v = 0;
    c->rec.fields_split = false;
    c->rec.nf = 0;

    // Update NR / FNR.
    int64_t nr = ARAWK_IS_FIX(c->env[ARAWK_GLOB_NR]) ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_NR]) : 0;
    int64_t fnr = ARAWK_IS_FIX(c->env[ARAWK_GLOB_FNR]) ? ARAWK_FIX_VAL(c->env[ARAWK_GLOB_FNR]) : 0;
    c->env[ARAWK_GLOB_NR] = ARAWK_FIX(nr + 1);
    c->env[ARAWK_GLOB_FNR] = ARAWK_FIX(fnr + 1);

    // Eagerly split — NF must be readable before any $N access.
    arawk_split_fields(c);
    return true;
}

// ---------------------------------------------------------------------------
// Arithmetic slow paths.
// ---------------------------------------------------------------------------

VALUE
arawk_add_slow(VALUE a, VALUE b)
{
    return arawk_make_float(arawk_to_num(a) + arawk_to_num(b));
}

VALUE
arawk_sub_slow(VALUE a, VALUE b)
{
    return arawk_make_float(arawk_to_num(a) - arawk_to_num(b));
}

VALUE
arawk_mul_slow(VALUE a, VALUE b)
{
    return arawk_make_float(arawk_to_num(a) * arawk_to_num(b));
}

VALUE
arawk_div_slow(VALUE a, VALUE b)
{
    double db = arawk_to_num(b);
    if (db == 0.0) {
        fprintf(stderr, "arawk: division by zero\n");
        exit(2);
    }
    return arawk_make_float(arawk_to_num(a) / db);
}

VALUE
arawk_mod_slow(VALUE a, VALUE b)
{
    double db = arawk_to_num(b);
    if (db == 0.0) {
        fprintf(stderr, "arawk: division by zero in %%\n");
        exit(2);
    }
    return arawk_make_float(fmod(arawk_to_num(a), db));
}

VALUE
arawk_pow_slow(VALUE a, VALUE b)
{
    return arawk_make_float(pow(arawk_to_num(a), arawk_to_num(b)));
}

VALUE
arawk_neg_slow(VALUE a)
{
    return arawk_make_float(-arawk_to_num(a));
}

// rand / srand — process-global LCG state.  POSIX: rand returns [0,1).
static int64_t arawk_rand_seed = 0;
static bool    arawk_rand_seeded = false;

double
arawk_rand(void)
{
    if (!arawk_rand_seeded) { srand((unsigned)time(NULL)); arawk_rand_seeded = true; }
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

int64_t
arawk_srand(int64_t seed)
{
    int64_t prev = arawk_rand_seed;
    arawk_rand_seed = seed;
    arawk_rand_seeded = true;
    srand((unsigned)seed);
    return prev;
}

int64_t
arawk_srand_time(void)
{
    int64_t prev = arawk_rand_seed;
    arawk_rand_seed = (int64_t)time(NULL);
    arawk_rand_seeded = true;
    srand((unsigned)arawk_rand_seed);
    return prev;
}
