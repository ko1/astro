/* koruby_precise — byte-level transcoding between encodings.
 *
 * The rest of the runtime treats a String as bytes + an encoding tag; this file
 * is the one place that knows what those bytes MEAN.  Every conversion goes
 * through Unicode: decode source bytes to a codepoint, encode that codepoint in
 * the target.  Tables come from CRuby itself (tools/gen_transcode.rb).
 */

struct korb_tc_pair { uint32_t bytes; uint8_t n; uint32_t cp; };
struct korb_tc_sb_ent { const char *name; const uint16_t *tab; };
struct korb_tc_mb_ent { const char *name; const struct korb_tc_pair *by_bytes, *by_cp; uint32_t n; };

#include "transcode_tables.h"

enum korb_tc_kind {
    KTC_NONE = 0, KTC_UTF8, KTC_ASCII, KTC_BINARY,
    KTC_UTF16LE, KTC_UTF16BE, KTC_UTF32LE, KTC_UTF32BE,
    KTC_SB, KTC_MB, KTC_ISO2022JP,
};

/* legacy multi-byte families whose byte STRUCTURE is wider than the set of
 * codes that actually map to Unicode (Integer#chr validates structure only) */
#define KTC_FAM_NONE 0u
#define KTC_FAM_SJIS 1u
#define KTC_FAM_EUC  2u

struct korb_tc {
    uint8_t kind;
    uint8_t fam;
    const uint16_t *sb;                       /* KTC_SB: 0x80..0xFF → codepoint */
    const struct korb_tc_pair *by_bytes, *by_cp;   /* KTC_MB */
    uint32_t n;
};

/* Names CRuby accepts that the generated tables don't carry verbatim. */
static const struct { const char *alias, *real; } korb_tc_alias[] = {
    { "BINARY", "ASCII-8BIT" }, { "ASCII", "US-ASCII" }, { "ANSI_X3.4-1968", "US-ASCII" },
    { "UTF8", "UTF-8" }, { "CP65001", "UTF-8" },
    { "SJIS", "Shift_JIS" }, { "CP932", "Windows-31J" }, { "csWindows31J", "Windows-31J" },
    { "eucJP", "EUC-JP" }, { "euc-jp-ms", "eucJP-ms" },
    { "UTF8-MAC", "UTF-8" }, { "UTF8-DoCoMo", "UTF-8" }, { "UTF8-KDDI", "UTF-8" },
    { "UTF8-SoftBank", "UTF-8" }, { "CESU-8", "UTF-8" },
    { "CP50220", "ISO-2022-JP" }, { "CP50221", "ISO-2022-JP" }, { "ISO2022-JP", "ISO-2022-JP" },
    { "CP1250", "Windows-1250" }, { "CP1251", "Windows-1251" }, { "CP1252", "Windows-1252" },
    { "CP1253", "Windows-1253" }, { "CP1254", "Windows-1254" }, { "CP1255", "Windows-1255" },
    { "CP1256", "Windows-1256" }, { "CP1257", "Windows-1257" }, { "CP1258", "Windows-1258" },
    { "CP874", "Windows-874" }, { "CP437", "IBM437" }, { "CP878", "KOI8-R" },
    { "ISO8859-1", "ISO-8859-1" }, { "ISO-8859", "ISO-8859-1" }, { "Latin-1", "ISO-8859-1" },
    { "ISO8859-2", "ISO-8859-2" }, { "ISO8859-15", "ISO-8859-15" },
    { NULL, NULL },
};

/* Resolve an encoding name to a converter.  false = koruby can't transcode it. */
static bool korb_tc_find(const char *name, struct korb_tc *out)
{
    for (size_t i = 0; korb_tc_alias[i].alias; i++)
        if (strcasecmp(name, korb_tc_alias[i].alias) == 0) { name = korb_tc_alias[i].real; break; }
    memset(out, 0, sizeof *out);
    if (strcasecmp(name, "UTF-8") == 0)      { out->kind = KTC_UTF8;    return true; }
    if (strcasecmp(name, "US-ASCII") == 0)   { out->kind = KTC_ASCII;   return true; }
    if (strcasecmp(name, "ASCII-8BIT") == 0) { out->kind = KTC_BINARY;  return true; }
    if (strcasecmp(name, "UTF-16LE") == 0)   { out->kind = KTC_UTF16LE; return true; }
    if (strcasecmp(name, "UTF-16BE") == 0)   { out->kind = KTC_UTF16BE; return true; }
    if (strcasecmp(name, "UTF-32LE") == 0)   { out->kind = KTC_UTF32LE; return true; }
    if (strcasecmp(name, "UTF-32BE") == 0)   { out->kind = KTC_UTF32BE; return true; }
    if (strcasecmp(name, "ISO-2022-JP") == 0) {          /* stateful: JIS X 0208 via the EUC-JP table */
        for (size_t i = 0; korb_tc_mb_table[i].name; i++)
            if (strcasecmp("EUC-JP", korb_tc_mb_table[i].name) == 0) {
                out->kind = KTC_ISO2022JP;
                out->by_bytes = korb_tc_mb_table[i].by_bytes;
                out->by_cp    = korb_tc_mb_table[i].by_cp;
                out->n        = korb_tc_mb_table[i].n;
                return true;
            }
        return false;
    }
    for (size_t i = 0; korb_tc_sb_table[i].name; i++)
        if (strcasecmp(name, korb_tc_sb_table[i].name) == 0) {
            out->kind = KTC_SB; out->sb = korb_tc_sb_table[i].tab; return true;
        }
    for (size_t i = 0; korb_tc_mb_table[i].name; i++)
        if (strcasecmp(name, korb_tc_mb_table[i].name) == 0) {
            out->kind = KTC_MB;
            out->by_bytes = korb_tc_mb_table[i].by_bytes;
            out->by_cp    = korb_tc_mb_table[i].by_cp;
            out->n        = korb_tc_mb_table[i].n;
            out->fam = (strncasecmp(korb_tc_mb_table[i].name, "Shift_JIS", 9) == 0 ||
                        strncasecmp(korb_tc_mb_table[i].name, "Windows-31J", 11) == 0) ? KTC_FAM_SJIS
                     : (strncasecmp(korb_tc_mb_table[i].name, "EUC", 3) == 0 ||
                        strncasecmp(korb_tc_mb_table[i].name, "CP51932", 7) == 0) ? KTC_FAM_EUC : KTC_FAM_NONE;
            return true;
        }
    return false;
}

/* A multi-byte table is keyed by the big-endian value of the byte sequence, so
 * a prefix of the input is looked up at each candidate length (2..4). */
static const struct korb_tc_pair *korb_tc_mb_bytes(const struct korb_tc *tc, uint32_t v, uint8_t n)
{
    uint32_t lo = 0, hi = tc->n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (tc->by_bytes[mid].bytes < v) lo = mid + 1; else hi = mid;
    }
    for (uint32_t i = lo; i < tc->n && tc->by_bytes[i].bytes == v; i++)
        if (tc->by_bytes[i].n == n) return &tc->by_bytes[i];
    return NULL;
}
static const struct korb_tc_pair *korb_tc_mb_cp(const struct korb_tc *tc, uint32_t cp)
{
    uint32_t lo = 0, hi = tc->n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (tc->by_cp[mid].cp < cp) lo = mid + 1; else hi = mid;
    }
    return (lo < tc->n && tc->by_cp[lo].cp == cp) ? &tc->by_cp[lo] : NULL;
}

/* ISO-2022-JP mode: 0 = ASCII/JIS-Roman, 1 = JIS X 0208 (two bytes per char). */
#define KTC_JIS_ASCII 0u
#define KTC_JIS_X0208 1u

/* Consume any ESC sequence at p, updating *mode.  Returns bytes eaten (0 = none). */
static uint32_t korb_tc_jis_esc(const unsigned char *p, size_t len, uint8_t *mode)
{
    if (len < 3 || p[0] != 0x1B) return 0;
    if (p[1] == '$' && (p[2] == '@' || p[2] == 'B')) { *mode = KTC_JIS_X0208; return 3; }
    if (p[1] == '(' && (p[2] == 'B' || p[2] == 'J')) { *mode = KTC_JIS_ASCII; return 3; }
    return 0;
}

/* Decode one character at p.  Returns bytes consumed, or 0 for an invalid
 * sequence (the caller decides whether that is fatal or replaced). */
static uint32_t korb_tc_decode(const struct korb_tc *tc, const unsigned char *p, size_t len, uint32_t *cp)
{
    if (len == 0) return 0;
    switch (tc->kind) {
      case KTC_UTF8: {
        const unsigned char b = p[0];
        if (b < 0x80) { *cp = b; return 1; }
        uint32_t need, v;
        if ((b & 0xE0) == 0xC0)      { need = 2; v = b & 0x1Fu; }
        else if ((b & 0xF0) == 0xE0) { need = 3; v = b & 0x0Fu; }
        else if ((b & 0xF8) == 0xF0) { need = 4; v = b & 0x07u; }
        else return 0;
        if (len < need) return 0;
        for (uint32_t i = 1; i < need; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            v = (v << 6) | (uint32_t)(p[i] & 0x3F);
        }
        if ((need == 2 && v < 0x80) || (need == 3 && v < 0x800) || (need == 4 && v < 0x10000)) return 0;   /* overlong */
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v; return need;
      }
      case KTC_ASCII:  if (p[0] < 0x80) { *cp = p[0]; return 1; } return 0;
      case KTC_BINARY: *cp = p[0]; return 1;                       /* byte == codepoint (Latin-1-ish) */
      case KTC_SB: {
        if (p[0] < 0x80) { *cp = p[0]; return 1; }
        const uint16_t v = tc->sb[p[0] - 0x80];
        if (v == 0xFFFF) return 0;
        *cp = v; return 1;
      }
      case KTC_MB: {
        if (p[0] < 0x80) { *cp = p[0]; return 1; }
        uint32_t v = 0;
        for (uint8_t n = 1; n <= 4 && n <= len; n++) {
            v = (v << 8) | p[n - 1];
            const struct korb_tc_pair *const e = korb_tc_mb_bytes(tc, v, n);
            if (e) { *cp = e->cp; return n; }
        }
        return 0;
      }
      case KTC_UTF16LE: case KTC_UTF16BE: {
        if (len < 2) return 0;
        const bool le = (tc->kind == KTC_UTF16LE);
        const uint32_t u = le ? ((uint32_t)p[1] << 8 | p[0]) : ((uint32_t)p[0] << 8 | p[1]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (len < 4) return 0;
            const uint32_t l = le ? ((uint32_t)p[3] << 8 | p[2]) : ((uint32_t)p[2] << 8 | p[3]);
            if (l < 0xDC00 || l > 0xDFFF) return 0;
            *cp = 0x10000u + ((u - 0xD800u) << 10) + (l - 0xDC00u);
            return 4;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) return 0;                  /* lone low surrogate */
        *cp = u; return 2;
      }
      case KTC_UTF32LE: case KTC_UTF32BE: {
        if (len < 4) return 0;
        const uint32_t v = (tc->kind == KTC_UTF32LE)
            ? ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0])
            : ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v; return 4;
      }
      default: return 0;
    }
}

/* How many bytes the invalid sequence at p spans: the longest prefix that could
 * still become a valid character (CRuby replaces such a run with ONE
 * replacement, not one per byte). */
static uint32_t korb_tc_errlen(const struct korb_tc *tc, const unsigned char *p, size_t len)
{
    if (tc->kind != KTC_UTF8) return 1;
    const unsigned char b = p[0];
    uint32_t need;
    if ((b & 0xE0) == 0xC0) need = 2;
    else if ((b & 0xF0) == 0xE0) need = 3;
    else if ((b & 0xF8) == 0xF0) need = 4;
    else return 1;                                     /* stray continuation / 0xF8+ */
    uint32_t k = 1;
    while (k < need && k < len && (p[k] & 0xC0) == 0x80) k++;
    return k;
}

/* JIS X 0208 lives in the EUC-JP table shifted by 0x8080, so both directions
 * reuse it. */
static uint32_t korb_tc_jis_decode(const struct korb_tc *tc, const unsigned char *p, size_t len, uint32_t *cp, uint8_t mode)
{
    if (len == 0) return 0;
    if (mode == KTC_JIS_ASCII) { if (p[0] < 0x80) { *cp = p[0]; return 1; } return 0; }
    if (len < 2 || p[0] < 0x21 || p[0] > 0x7E || p[1] < 0x21 || p[1] > 0x7E) return 0;
    const uint32_t euc = ((uint32_t)(p[0] | 0x80) << 8) | (uint32_t)(p[1] | 0x80);
    const struct korb_tc_pair *const e = korb_tc_mb_bytes(tc, euc, 2);
    if (!e) return 0;
    *cp = e->cp; return 2;
}
/* Writes the mode switch when needed; returns bytes written, 0 = undefined. */
static uint32_t korb_tc_jis_encode(const struct korb_tc *tc, uint32_t cp, unsigned char *out, uint8_t *mode)
{
    uint32_t n = 0;
    if (cp < 0x80) {
        if (*mode != KTC_JIS_ASCII) { out[n++] = 0x1B; out[n++] = '('; out[n++] = 'B'; *mode = KTC_JIS_ASCII; }
        out[n++] = (unsigned char)cp;
        return n;
    }
    const struct korb_tc_pair *const e = korb_tc_mb_cp(tc, cp);
    if (!e || e->n != 2) return 0;
    const uint32_t b0 = (e->bytes >> 8) & 0xFF, b1 = e->bytes & 0xFF;
    if (b0 < 0xA1 || b0 > 0xFE || b1 < 0xA1 || b1 > 0xFE) return 0;   /* half-width kana etc. */
    if (*mode != KTC_JIS_X0208) { out[n++] = 0x1B; out[n++] = '$'; out[n++] = 'B'; *mode = KTC_JIS_X0208; }
    out[n++] = (unsigned char)(b0 & 0x7F);
    out[n++] = (unsigned char)(b1 & 0x7F);
    return n;
}

/* True when the bytes at p are a valid but TRUNCATED character: more input
 * would complete it (Converter#primitive_convert reports :incomplete_input). */
static bool korb_tc_truncated(const struct korb_tc *tc, const unsigned char *p, size_t len)
{
    switch (tc->kind) {
      case KTC_UTF8: {
        const unsigned char b = p[0];
        uint32_t need;
        if ((b & 0xE0) == 0xC0) need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else return false;
        uint32_t k = 1;
        while (k < need && k < len && (p[k] & 0xC0) == 0x80) k++;
        return k == len && k < need;
      }
      case KTC_MB:       return p[0] >= 0x80 && len < 4;   /* a lead byte with no room left */
      case KTC_UTF16LE: case KTC_UTF16BE: return len < 2 || (len < 4 && p[len - 2] != 0);
      case KTC_UTF32LE: case KTC_UTF32BE: return len < 4;
      default: return false;
    }
}

/* Bytes the first character of `p` spans in `enc`.  0 means the buffer holds
 * only a truncated prefix, so a byte-at-a-time reader must fetch more; anything
 * the encoding rejects reports 1, so such a reader always makes progress. */
static uint32_t korb_tc_char_len(const char *const enc, const unsigned char *const p, const size_t len)
{
    if (len == 0) return 0;
    struct korb_tc tc;
    if (!korb_tc_find(enc, &tc)) return 1;
    uint32_t cp;
    const uint32_t n = korb_tc_decode(&tc, p, len, &cp);
    if (n != 0) return n;
    return korb_tc_truncated(&tc, p, len) ? 0 : 1;
}

/* Encode one codepoint.  Returns bytes written to out (>= 8 bytes), or 0 when
 * the target encoding has no representation for it. */
static uint32_t korb_tc_encode(const struct korb_tc *tc, uint32_t cp, unsigned char *out)
{
    switch (tc->kind) {
      case KTC_UTF8:
        if (cp < 0x80)    { out[0] = (unsigned char)cp; return 1; }
        if (cp < 0x800)   { out[0] = (unsigned char)(0xC0 | (cp >> 6)); out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
        if (cp < 0x10000) { out[0] = (unsigned char)(0xE0 | (cp >> 12)); out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
        if (cp <= 0x10FFFF) { out[0] = (unsigned char)(0xF0 | (cp >> 18)); out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (unsigned char)(0x80 | (cp & 0x3F)); return 4; }
        return 0;
      case KTC_ASCII:  if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; } return 0;
      case KTC_BINARY: if (cp < 0x100) { out[0] = (unsigned char)cp; return 1; } return 0;
      case KTC_SB:
        if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
        for (uint32_t i = 0; i < 128; i++)
            if (tc->sb[i] == cp) { out[0] = (unsigned char)(0x80 + i); return 1; }
        return 0;
      case KTC_MB: {
        if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
        const struct korb_tc_pair *const e = korb_tc_mb_cp(tc, cp);
        if (!e) return 0;
        for (uint8_t i = 0; i < e->n; i++) out[i] = (unsigned char)((e->bytes >> (8 * (e->n - 1 - i))) & 0xFF);
        return e->n;
      }
      case KTC_UTF16LE: case KTC_UTF16BE: {
        const bool le = (tc->kind == KTC_UTF16LE);
        if (cp < 0x10000) {
            if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
            out[0] = (unsigned char)(le ? (cp & 0xFF) : (cp >> 8));
            out[1] = (unsigned char)(le ? (cp >> 8) : (cp & 0xFF));
            return 2;
        }
        if (cp > 0x10FFFF) return 0;
        const uint32_t v = cp - 0x10000u, hi = 0xD800u + (v >> 10), lo = 0xDC00u + (v & 0x3FFu);
        out[0] = (unsigned char)(le ? (hi & 0xFF) : (hi >> 8));
        out[1] = (unsigned char)(le ? (hi >> 8) : (hi & 0xFF));
        out[2] = (unsigned char)(le ? (lo & 0xFF) : (lo >> 8));
        out[3] = (unsigned char)(le ? (lo >> 8) : (lo & 0xFF));
        return 4;
      }
      case KTC_UTF32LE: case KTC_UTF32BE: {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        const bool le = (tc->kind == KTC_UTF32LE);
        for (int i = 0; i < 4; i++) out[i] = (unsigned char)((cp >> (8 * (le ? i : 3 - i))) & 0xFF);
        return 4;
      }
      default: return 0;
    }
}

/* copy a String's bytes into a NUL-terminated C buffer (names are short) */
ARO_BORROW static void korb_tc_cstr(VALUE v, char *out, size_t cap)
{
    const KorbString *const s = VAL2STR(v);
    size_t n = s->len; if (n > cap - 1) n = cap - 1;
    memcpy(out, korb_strbuf_data(s->buf), n);
    out[n] = '\0';
}

/* growable byte sink for the conversion (plain libc: the loop never allocates
 * Ruby objects, so nothing can move underneath it) */
struct korb_tc_out { char *b; size_t len, capa; };
static void korb_tc_put(struct korb_tc_out *o, const void *p, size_t n)
{
    if (o->len + n + 1 > o->capa) {
        size_t nc = o->capa ? o->capa * 2 : 64;
        while (nc < o->len + n + 1) nc *= 2;
        o->b = realloc(o->b, nc);
        if (!o->b) abort();
        o->capa = nc;
    }
    memcpy(o->b + o->len, p, n);
    o->len += n;
}

/* __transcode(str, from, to, flags, replacement[, max_bytes]) — the single
 * primitive behind String#encode and Encoding::Converter.
 *
 *   flags bit0 invalid: :replace   bit1 undef: :replace   bit2 xml charrefs
 *   max_bytes: cap on the output (nil / negative = unlimited)
 *
 * Returns the converted String when the whole input converted.  Otherwise an
 * Array [partial_output, code, consumed_bytes, codepoint, error_bytes] with
 *   code 0 = invalid byte sequence   1 = undefined conversion
 *        2 = destination buffer full 3 = incomplete input (truncated char)
 * so the caller can raise with CRuby's exact message, run a `fallback:`, or
 * drive a streaming converter. */
static RESULT korb_bi_transcode(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    if (UNLIKELY(VALUE_SLICE_LEN(args) < 5))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 5..6)", VALUE_SLICE_LEN(args));
    const VALUE srcv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(srcv) || !KORB_STRING_P(VALUE_SLICE_GET(args, 1)) || !KORB_STRING_P(VALUE_SLICE_GET(args, 2))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "__transcode expects (String, String, String, Integer, String)");
    char fromb[64], tob[64], replb[64];
    korb_tc_cstr(VALUE_SLICE_GET(args, 1), fromb, sizeof fromb);
    korb_tc_cstr(VALUE_SLICE_GET(args, 2), tob, sizeof tob);
    const uint32_t flags = FIXNUM_P(VALUE_SLICE_GET(args, 3)) ? (uint32_t)FIX2LONG(VALUE_SLICE_GET(args, 3)) : 0;
    /* the replacement arrives ALREADY in the target encoding and may contain
     * NUL bytes (UTF-16/32 targets), so it is copied by length, not strlen */
    uint32_t repl_len = 0;
    if (KORB_STRING_P(VALUE_SLICE_GET(args, 4))) {
        const KorbString *const rs = VAL2STR(VALUE_SLICE_GET(args, 4));
        repl_len = rs->len < sizeof replb ? rs->len : (uint32_t)sizeof replb;
        memcpy(replb, korb_strbuf_data(rs->buf), repl_len);
    }

    korb_sword_t maxb = -1;
    if (VALUE_SLICE_LEN(args) >= 6 && FIXNUM_P(VALUE_SLICE_GET(args, 5))) maxb = FIX2LONG(VALUE_SLICE_GET(args, 5));

    struct korb_tc from, to;
    if (!korb_tc_find(fromb, &from) || !korb_tc_find(tob, &to))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "code converter not found (%s to %s)", fromb, tob);

    /* copy the source bytes out: building the result String allocates */
    const KorbString *const s = VAL2STR(srcv);
    const size_t slen = s->len;
    unsigned char *const src = malloc(slen + 1);
    if (!src) abort();
    memcpy(src, korb_strbuf_data(s->buf), slen);
    src[slen] = 0;

    struct korb_tc_out out = { NULL, 0, 0 };
    size_t i = 0;
    uint8_t smode = KTC_JIS_ASCII, dmode = KTC_JIS_ASCII;   /* ISO-2022-JP shift state */
    int errcode = -1; size_t errpos = 0; uint32_t errcp = 0, errlen = 0;
    while (i < slen) {
        uint32_t cp = 0;
        if (from.kind == KTC_ISO2022JP) {
            const uint32_t esc = korb_tc_jis_esc(src + i, slen - i, &smode);
            if (esc) { i += esc; continue; }
        }
        const uint32_t used = (from.kind == KTC_ISO2022JP)
            ? korb_tc_jis_decode(&from, src + i, slen - i, &cp, smode)
            : korb_tc_decode(&from, src + i, slen - i, &cp);
        if (used == 0) {                                  /* invalid byte sequence in the source */
            /* a truncated tail is only "incomplete input" when the caller
             * still wants to hear about it; invalid: :replace replaces it */
            if (!(flags & 1u) && korb_tc_truncated(&from, src + i, slen - i)) { errcode = 3; errpos = i; errlen = (uint32_t)(slen - i); break; }
            const uint32_t bad = korb_tc_errlen(&from, src + i, slen - i);
            if (!(flags & 1u)) { errcode = 0; errpos = i; errlen = bad; break; }
            korb_tc_put(&out, replb, repl_len);
            i += bad;
            continue;
        }
        unsigned char buf[8];
        const uint32_t wrote = (to.kind == KTC_ISO2022JP)
            ? korb_tc_jis_encode(&to, cp, buf, &dmode)
            : korb_tc_encode(&to, cp, buf);
        if (wrote > 0 && maxb >= 0 && (korb_sword_t)(out.len + wrote) > maxb) { errcode = 2; errpos = i; errlen = 0; break; }
        if (wrote == 0) {                                 /* not representable in the target */
            if (flags & 4u) {                             /* xml: :text / :attr → numeric charref */
                char cr[16]; const int n = snprintf(cr, sizeof cr, "&#x%X;", cp);
                if (n > 0) korb_tc_put(&out, cr, (size_t)n);
                i += used;
                continue;
            }
            if (!(flags & 2u)) { errcode = 1; errpos = i; errcp = cp; errlen = used; break; }
            korb_tc_put(&out, replb, repl_len);
            i += used;
            continue;
        }
        korb_tc_put(&out, buf, wrote);
        i += used;
    }

    if (errcode < 0 && to.kind == KTC_ISO2022JP && dmode != KTC_JIS_ASCII)
        korb_tc_put(&out, "\x1B(B", 3);                    /* ISO-2022-JP must end in ASCII mode */
    if (errcode < 0) {                                    /* success: one String, tagged with the target */
        RESULT r = korb_str_new(c, slots, out.b ? out.b : "", (uint32_t)out.len);
        free(out.b); free(src);
        if (LIKELY(r.state == KORB_NORMAL))
            KORB_STR_ENC_SET(r.value, korb_enc_index_pub(c->vm, tob));
        return r;
    }
    /* failure: hand the caller everything it needs to build the exception or run
     * the fallback and resume */
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 5));
    VALUE_REF a = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, out.b ? out.b : "", (uint32_t)out.len));
    KORB_STR_ENC_SET(slots[1], korb_enc_index_pub(c->vm, tob));
    CHECK(korb_ary_push_val(c, slots + 2, a, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, a, LONG2FIX(errcode)));
    CHECK(korb_ary_push_val(c, slots + 2, a, LONG2FIX((korb_sword_t)errpos)));
    CHECK(korb_ary_push_val(c, slots + 2, a, LONG2FIX((korb_sword_t)errcp)));
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, (const char *)src + errpos, errlen));
    KORB_STR_ENC_SET(slots[1], korb_enc_index_pub(c->vm, fromb));   /* the offending char, in the SOURCE encoding */
    if (errcode == 2) KORB_STR_ENC_SET(slots[1], korb_enc_index_pub(c->vm, fromb));
    CHECK(korb_ary_push_val(c, slots + 2, a, slots[1]));
    free(out.b); free(src);
    return RESULT_OK(VALUE_REF_GET(a));
}

/* __transcodable?(name) — true when koruby has a converter for this encoding. */
static RESULT korb_bi_transcodable(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    (void)slots;
    if (VALUE_SLICE_LEN(args) < 1 || !KORB_STRING_P(VALUE_SLICE_GET(args, 0))) return RESULT_OK(KORB_FALSE);
    char nb[64]; korb_tc_cstr(VALUE_SLICE_GET(args, 0), nb, sizeof nb);
    struct korb_tc tc;
    (void)c;
    return RESULT_OK(korb_tc_find(nb, &tc) ? KORB_TRUE : KORB_FALSE);
}

/* C-callable convenience: convert `src` from → to with no error handling.
 * Returns the converted String, or KORB_UNDEF in *out when the conversion is
 * impossible (the caller decides whether that is an error). */
static RESULT korb_tc_convert(CTX *c, VALUE *slots, VALUE src, const char *fromb, const char *tob, bool *ok)
{
    *ok = false;
    struct korb_tc from, to;
    if (!korb_tc_find(fromb, &from) || !korb_tc_find(tob, &to)) return RESULT_OK(src);
    const KorbString *const s = VAL2STR(src);
    const size_t slen = s->len;
    unsigned char *const buf = malloc(slen + 1);
    if (!buf) abort();
    memcpy(buf, korb_strbuf_data(s->buf), slen);
    struct korb_tc_out out = { NULL, 0, 0 };
    uint8_t dmode = KTC_JIS_ASCII, smode = KTC_JIS_ASCII;
    size_t i = 0;
    bool bad = false;
    while (i < slen) {
        uint32_t cp = 0;
        if (from.kind == KTC_ISO2022JP) {
            const uint32_t esc = korb_tc_jis_esc(buf + i, slen - i, &smode);
            if (esc) { i += esc; continue; }
        }
        const uint32_t used = (from.kind == KTC_ISO2022JP)
            ? korb_tc_jis_decode(&from, buf + i, slen - i, &cp, smode)
            : korb_tc_decode(&from, buf + i, slen - i, &cp);
        if (used == 0) { bad = true; break; }
        unsigned char ob[8];
        const uint32_t wrote = (to.kind == KTC_ISO2022JP)
            ? korb_tc_jis_encode(&to, cp, ob, &dmode)
            : korb_tc_encode(&to, cp, ob);
        if (wrote == 0) { bad = true; break; }
        korb_tc_put(&out, ob, wrote);
        i += used;
    }
    if (!bad && to.kind == KTC_ISO2022JP && dmode != KTC_JIS_ASCII) korb_tc_put(&out, "\x1B(B", 3);
    free(buf);
    if (bad) { free(out.b); return RESULT_OK(src); }
    RESULT r = korb_str_new(c, slots, out.b ? out.b : "", (uint32_t)out.len);
    free(out.b);
    if (LIKELY(r.state == KORB_NORMAL)) { KORB_STR_ENC_SET(r.value, korb_enc_index_pub(c->vm, tob)); *ok = true; }
    return r;
}

/* Encode one codepoint in `enc` (Integer#chr, String#encode's replacement).
 * Returns bytes written to out (>= 8 bytes), 0 when not representable. */
static uint32_t korb_tc_encode_name(const char *enc, uint32_t cp, unsigned char *out)
{
    struct korb_tc tc;
    if (!korb_tc_find(enc, &tc)) return 0;
    if (tc.kind == KTC_ISO2022JP) { uint8_t m = KTC_JIS_ASCII; return korb_tc_jis_encode(&tc, cp, out, &m); }
    return korb_tc_encode(&tc, cp, out);
}

/* Integer#chr on a non-Unicode encoding treats the integer as the (big-endian)
 * BYTE SEQUENCE, not a codepoint.  Writes it out and validates it decodes. */
static uint32_t korb_tc_bytes_chr(const char *enc, uint32_t v, unsigned char *out, bool *unicode)
{
    struct korb_tc tc;
    *unicode = false;
    if (!korb_tc_find(enc, &tc)) return 0;
    if (tc.kind == KTC_UTF8 || tc.kind == KTC_UTF16LE || tc.kind == KTC_UTF16BE ||
        tc.kind == KTC_UTF32LE || tc.kind == KTC_UTF32BE) { *unicode = true; return 0; }
    uint32_t n = 1;
    if (v > 0xFFFFFF) n = 4; else if (v > 0xFFFF) n = 3; else if (v > 0xFF) n = 2;
    for (uint32_t i = 0; i < n; i++) out[i] = (unsigned char)((v >> (8 * (n - 1 - i))) & 0xFF);
    uint32_t cp = 0;
    if (tc.kind == KTC_ISO2022JP) return n;                  /* stateful: accept as given */
    if (korb_tc_decode(&tc, out, n, &cp) == n) return n;
    /* CRuby validates the byte STRUCTURE, not that the pair maps to Unicode:
     * an unassigned but well-formed two-byte code is still a character. */
    if (tc.kind == KTC_MB && n == 2) {
        const unsigned char b0 = out[0], b1 = out[1];
        if (tc.fam == KTC_FAM_SJIS &&
            ((b0 >= 0x81 && b0 <= 0x9F) || (b0 >= 0xE0 && b0 <= 0xFC)) &&
            ((b1 >= 0x40 && b1 <= 0x7E) || (b1 >= 0x80 && b1 <= 0xFC))) return n;
        if (tc.fam == KTC_FAM_EUC && b0 >= 0xA1 && b0 <= 0xFE && b1 >= 0xA1 && b1 <= 0xFE) return n;
    }
    return 0;
}
