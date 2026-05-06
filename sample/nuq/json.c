/*
 * json.c — JSON parser + printer.
 *
 * Parser is recursive descent over a (src, len) buffer.  On error returns
 * NUQ_NULL and writes a message into *errmsg (caller frees with no-op
 * since GC owns it).  *endp gets the position past the parsed value;
 * the caller can tail-loop for streamed JSON ("a b c" -> three values).
 *
 * Printer supports compact and pretty output.  No escapes for printable
 * ASCII except "\\" and "\""; control chars get the standard \uXXXX
 * escapes.  We don't UTF-8 decode for output — bytes pass through.
 */
#include "context.h"
#include <ctype.h>
#include <math.h>

/* The printer is hot for output-heavy filters (e.g. `[paths]` on
 * deeply-nested JSON); using `*_unlocked` variants of stdio funcs
 * skips the glibc per-call FILE* lock acquire and brings ~30% gain
 * on `tree_paths` and similar.  Safe because nuq is single-threaded. */
#define putc_u(c, fp)        fputc_unlocked(c, fp)
#define puts_u(s, fp)        fputs_unlocked(s, fp)
#define fwrite_u(p, n, fp)   fwrite_unlocked(p, 1, n, fp)
#define vfprintf_u           vfprintf
#define fprintf_u            fprintf

static void
skip_ws(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        else break;
    }
    *pp = p;
}

static char *
fmt_err(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    char *r = (char *)GC_malloc_atomic(n + 1);
    memcpy(r, buf, n + 1);
    return r;
}

/* Re-entrant-ish state for jq-style error formatting.  Set by
 * `nuq_json_parse` so internal helpers can refer back to the start
 * of the source for line/col + "while parsing 'XXX'" suffix. */
static const char *json_parse_src   = NULL;
static const char *json_parse_end   = NULL;

static void
compute_line_col(const char *p, int *line_out, int *col_out)
{
    int line = 1, col = 1;
    for (const char *q = json_parse_src; q < p; q++) {
        if (*q == '\n') { line++; col = 1; }
        else col++;
    }
    *line_out = line;
    *col_out = col;
}

/* Format "<msg> at line N, column M (while parsing 'SRC')" — the
 * jq-canonical fromjson error shape. */
static char *
fmt_err_loc(const char *p, const char *msg)
{
    int line = 1, col = 1;
    if (json_parse_src) compute_line_col(p, &line, &col);
    /* trim source for the "while parsing" suffix to a manageable length. */
    size_t sl = (size_t)(json_parse_end - json_parse_src);
    char head[80];
    size_t copy = sl < sizeof(head) - 4 ? sl : sizeof(head) - 4;
    memcpy(head, json_parse_src, copy);
    if (copy < sl) {
        head[copy++] = '.'; head[copy++] = '.'; head[copy++] = '.';
    }
    head[copy] = '\0';
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s at line %d, column %d (while parsing '%s')",
             msg, line, col, head);
    size_t n = strlen(buf);
    char *r = (char *)GC_malloc_atomic(n + 1);
    memcpy(r, buf, n + 1);
    return r;
}

static VALUE parse_value(const char **pp, const char *end, char **err);

/* jq's fromjson rejects nesting deeper than this with a specific
 * "Exceeds depth limit for parsing" message. */
#define NUQ_JSON_PARSE_MAX_DEPTH 10000

static int json_parse_depth = 0;

static VALUE
parse_string_raw(const char **pp, const char *end, char **err)
{
    const char *p = *pp;
    if (*p != '"') {
        /* jq advances past a would-be `'...'` literal before reporting
         * the error, so the column points at whatever follows the
         * closing `'`.  Mirror that position-walk for messages with
         * leading `'` to match jq's exact message. */
        const char *err_pos = p;
        if (*p == '\'') {
            const char *q = p + 1;
            while (q < end && *q != '\'') q++;
            if (q < end) err_pos = q + 1;
        }
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "Invalid string literal; expected \", but got %c", *p);
        *err = fmt_err_loc(err_pos, msg);
        return NUQ_NULL;
    }
    p++;

    /* Worst-case output is len; build a growable buffer. */
    size_t cap = 32, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);

#define PUT(c) do { \
    if (len + 1 >= cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); } \
    buf[len++] = (char)(c); \
} while (0)

    while (p < end && *p != '"') {
        if ((unsigned char)*p == '\\') {
            p++;
            if (p >= end) { *err = fmt_err("unterminated escape"); return NUQ_NULL; }
            switch (*p) {
              case '"':  PUT('"');  p++; break;
              case '\\': PUT('\\'); p++; break;
              case '/':  PUT('/');  p++; break;
              case 'b':  PUT('\b'); p++; break;
              case 'f':  PUT('\f'); p++; break;
              case 'n':  PUT('\n'); p++; break;
              case 'r':  PUT('\r'); p++; break;
              case 't':  PUT('\t'); p++; break;
              case 'u': {
                p++;
                if (p + 4 > end) { *err = fmt_err("short \\u escape"); return NUQ_NULL; }
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p[i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else { *err = fmt_err("bad \\u hex"); return NUQ_NULL; }
                }
                p += 4;
                /* Surrogate pair? */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
                        *err = fmt_err("lone surrogate"); return NUQ_NULL;
                    }
                    p += 2;
                    unsigned lo = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p[i];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= h - '0';
                        else if (h >= 'a' && h <= 'f') lo |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') lo |= h - 'A' + 10;
                        else { *err = fmt_err("bad \\u hex"); return NUQ_NULL; }
                    }
                    p += 4;
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        *err = fmt_err("bad low surrogate"); return NUQ_NULL;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                /* UTF-8 encode */
                if (cp < 0x80)        { PUT(cp); }
                else if (cp < 0x800)  { PUT(0xC0 | (cp >> 6)); PUT(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000){ PUT(0xE0 | (cp >> 12)); PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F)); }
                else                  { PUT(0xF0 | (cp >> 18)); PUT(0x80 | ((cp >> 12) & 0x3F)); PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F)); }
                break;
              }
              default:
                *err = fmt_err("bad escape \\%c", *p); return NUQ_NULL;
            }
        } else {
            PUT(*p); p++;
        }
    }
    if (p >= end || *p != '"') { *err = fmt_err("unterminated string"); return NUQ_NULL; }
    p++;
    buf[len] = '\0';
    *pp = p;
    return nuq_make_string_take(buf, len);
#undef PUT
}

static VALUE
parse_number(const char **pp, const char *end, char **err)
{
    const char *p = *pp;
    const char *start = p;
    if (*p == '-') p++;
    bool has_frac = false, has_exp = false;
    while (p < end && isdigit((unsigned char)*p)) p++;
    if (p < end && *p == '.') {
        has_frac = true;
        p++;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        has_exp = true;
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    char buf[64];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(buf)) { *err = fmt_err("number too long"); return NUQ_NULL; }
    memcpy(buf, start, n);
    buf[n] = '\0';

    *pp = p;
    if (has_frac || has_exp) {
        return nuq_make_double(strtod(buf, NULL));
    }
    /* int parse — try strtoll; fall back to double on overflow.
     * jq stores all numbers as IEEE-754 doubles, so integers > 2^53
     * lose precision.  Match that by switching to double here, even
     * though our fixnum has more range. */
    char *e = NULL;
    long long ll = strtoll(buf, &e, 10);
    const long long jq_int_max = (1LL << 53);
    if (e && *e == '\0' && ll >= -jq_int_max && ll <= jq_int_max) {
        return NUQ_FIX(ll);
    }
    return nuq_make_double(strtod(buf, NULL));
}

static VALUE
parse_value(const char **pp, const char *end, char **err)
{
    skip_ws(pp, end);
    const char *p = *pp;
    if (p >= end) { *err = fmt_err("unexpected EOF"); return NUQ_NULL; }
    if (*p == '"') return parse_string_raw(pp, end, err);
    /* jq extends JSON with `Infinity` / `-Infinity` / `NaN` / `-NaN` /
     * `nan` literals.  Check these before falling into number parsing
     * so the leading `-` doesn't get consumed by parse_number. */
    if (*p == '-' && end - p >= 9 && memcmp(p, "-Infinity", 9) == 0) {
        *pp = p + 9; return nuq_make_double(-INFINITY);
    }
    if (*p == '-' && end - p >= 4 && memcmp(p, "-NaN", 4) == 0) {
        *pp = p + 4; return nuq_make_double(NAN);
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) return parse_number(pp, end, err);
    if (*p == 't') {
        if (end - p >= 4 && memcmp(p, "true", 4) == 0) { *pp = p + 4; return NUQ_TRUE; }
        *err = fmt_err("expected 'true'"); return NUQ_NULL;
    }
    if (*p == 'f') {
        if (end - p >= 5 && memcmp(p, "false", 5) == 0) { *pp = p + 5; return NUQ_FALSE; }
        *err = fmt_err("expected 'false'"); return NUQ_NULL;
    }
    if (*p == 'n') {
        if (end - p >= 4 && memcmp(p, "null", 4) == 0) { *pp = p + 4; return NUQ_NULL; }
        if (end - p >= 3 && memcmp(p, "nan", 3) == 0) { *pp = p + 3; return nuq_make_double(NAN); }
        *err = fmt_err("expected 'null'"); return NUQ_NULL;
    }
    /* jq extends JSON with `nan`, `NaN`, `Infinity`, `-Infinity` literals. */
    if (*p == 'N' && end - p >= 3 && memcmp(p, "NaN", 3) == 0) {
        *pp = p + 3; return nuq_make_double(NAN);
    }
    if (*p == 'I' && end - p >= 8 && memcmp(p, "Infinity", 8) == 0) {
        *pp = p + 8; return nuq_make_double(INFINITY);
    }
    if (*p == '-' && end - p >= 9 && memcmp(p, "-Infinity", 9) == 0) {
        *pp = p + 9; return nuq_make_double(-INFINITY);
    }
    if (*p == '-' && end - p >= 4 && memcmp(p, "-NaN", 4) == 0) {
        *pp = p + 4; return nuq_make_double(NAN);
    }
    if (*p == '[') {
        if (++json_parse_depth > NUQ_JSON_PARSE_MAX_DEPTH) {
            json_parse_depth--;
            *err = fmt_err("Exceeds depth limit for parsing");
            return NUQ_NULL;
        }
        p++;
        VALUE arr = nuq_make_array(0);
        skip_ws(&p, end);
        if (p < end && *p == ']') { *pp = p + 1; json_parse_depth--; return arr; }
        for (;;) {
            *pp = p;
            VALUE v = parse_value(pp, end, err);
            if (*err) { json_parse_depth--; return NUQ_NULL; }
            nuq_array_push(arr, v);
            p = *pp;
            skip_ws(&p, end);
            if (p < end && *p == ',') { p++; skip_ws(&p, end); continue; }
            if (p < end && *p == ']') { *pp = p + 1; json_parse_depth--; return arr; }
            *err = fmt_err("expected ',' or ']' in array");
            json_parse_depth--;
            return NUQ_NULL;
        }
    }
    if (*p == '{') {
        if (++json_parse_depth > NUQ_JSON_PARSE_MAX_DEPTH) {
            json_parse_depth--;
            *err = fmt_err("Exceeds depth limit for parsing");
            return NUQ_NULL;
        }
        p++;
        VALUE obj = nuq_make_object(0);
        skip_ws(&p, end);
        if (p < end && *p == '}') { *pp = p + 1; json_parse_depth--; return obj; }
        for (;;) {
            skip_ws(&p, end);
            *pp = p;
            VALUE key = parse_string_raw(pp, end, err);
            if (*err) { json_parse_depth--; return NUQ_NULL; }
            p = *pp;
            skip_ws(&p, end);
            if (p >= end || *p != ':') {
                *err = fmt_err("expected ':' in object");
                json_parse_depth--;
                return NUQ_NULL;
            }
            p++;
            *pp = p;
            VALUE val = parse_value(pp, end, err);
            if (*err) { json_parse_depth--; return NUQ_NULL; }
            nuq_object_set(obj, key, val);
            p = *pp;
            skip_ws(&p, end);
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { *pp = p + 1; json_parse_depth--; return obj; }
            *err = fmt_err("expected ',' or '}' in object");
            json_parse_depth--;
            return NUQ_NULL;
        }
    }
    *err = fmt_err("unexpected '%c'", *p);
    return NUQ_NULL;
}

VALUE
nuq_json_parse(const char *src, size_t len, const char **endp, char **errmsg)
{
    /* skip a UTF-8 BOM if present */
    if (len >= 3 && (unsigned char)src[0] == 0xEF
                 && (unsigned char)src[1] == 0xBB
                 && (unsigned char)src[2] == 0xBF) {
        src += 3;
        len -= 3;
    }
    const char *p = src;
    const char *end = src + len;
    char *err = NULL;
    json_parse_depth = 0;
    json_parse_src = src;
    json_parse_end = end;
    VALUE v = parse_value(&p, end, &err);
    if (errmsg) *errmsg = err;
    if (endp) *endp = p;
    return v;
}

/* ---- printer ---- */

static void
print_string(FILE *fp, struct nuq_obj *o)
{
    /* Bulk-write the runs of "safe" bytes (printable ASCII, no
     * escape needed) and only call out for individual escapes — much
     * cheaper than per-byte fputc when most of the string is plain
     * ASCII (the common case). */
    putc_u('"', fp);
    const char *bytes = o->str.bytes;
    size_t len = o->str.len;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        const char *esc = NULL;
        char hex[8];
        switch (c) {
          case '"':  esc = "\\\""; break;
          case '\\': esc = "\\\\"; break;
          case '\b': esc = "\\b";  break;
          case '\f': esc = "\\f";  break;
          case '\n': esc = "\\n";  break;
          case '\r': esc = "\\r";  break;
          case '\t': esc = "\\t";  break;
          default:
            if (c < 0x20) {
                snprintf(hex, sizeof(hex), "\\u%04x", c);
                esc = hex;
            }
            break;
        }
        if (esc) {
            if (i > start) fwrite_u(bytes + start, i - start, fp);
            puts_u(esc, fp);
            start = i + 1;
        }
    }
    if (len > start) fwrite_u(bytes + start, len - start, fp);
    putc_u('"', fp);
}

static void
print_number(FILE *fp, VALUE v)
{
    char buf64[32];
    if (NUQ_IS_FIX(v)) {
        int n = snprintf(buf64, sizeof(buf64), "%" PRId64, (int64_t)NUQ_FIX_VAL(v));
        fwrite_u(buf64, n, fp);
        return;
    }
    double d = NUQ_PTR(v)->dbl;
    if (isnan(d)) { puts_u("null", fp); return; }
    if (isinf(d)) { puts_u(d < 0 ? "-1.7976931348623157e+308" : "1.7976931348623157e+308", fp); return; }
    /* match jq's output: integer-valued doubles as "1234", else use %.17g
     * but try shorter forms first for round-tripping. */
    if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15) {
        int n = snprintf(buf64, sizeof(buf64), "%" PRId64, (int64_t)d);
        fwrite_u(buf64, n, fp);
        return;
    }
    char buf[64];
    /* For integer-valued doubles outside int64 range but still finite,
     * jq formats them as the 17-digit %g mantissa shifted into fixed-
     * point with trailing zeros — i.e. `1.2345678901234568e+29` →
     * `123456789012345680000000000000` (mantissa keeps 17 digits, the
     * rest is filled with zeros).  Used by error messages to give a
     * stable, decnum-flavoured rendering rather than the raw IEEE-754
     * bits the OS strtod() would print. */
    if (d == floor(d) && fabs(d) < 1e30) {
        char src[40];
        snprintf(src, sizeof(src), "%.17g", d);
        const char *p = src;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        char mant_int[8] = {0}, mant_frac[40] = {0};
        int exp = 0;
        const char *dot = strchr(p, '.');
        const char *expc = strchr(p, 'e');
        if (dot && expc) {
            size_t il = (size_t)(dot - p);
            memcpy(mant_int, p, il); mant_int[il] = 0;
            size_t fl = (size_t)(expc - dot - 1);
            memcpy(mant_frac, dot + 1, fl); mant_frac[fl] = 0;
            exp = atoi(expc + 1);
        } else if (expc) {
            size_t il = (size_t)(expc - p);
            memcpy(mant_int, p, il); mant_int[il] = 0;
            mant_frac[0] = 0;
            exp = atoi(expc + 1);
        } else {
            /* `%.0f` shape — no exponent.  Fallback to printf as-is. */
            snprintf(buf, sizeof(buf), "%.0f", d);
            fputs(buf, fp);
            return;
        }
        /* Build int_part = mant_int + (exp digits of mant_frac), then
         * pad the rest with zeros to total length exp + len(mant_int). */
        int frac_len = (int)strlen(mant_frac);
        int take_frac = exp < frac_len ? exp : frac_len;
        int zeros = exp - take_frac;
        char *q = buf;
        if (neg) *q++ = '-';
        size_t mil = strlen(mant_int);
        memcpy(q, mant_int, mil); q += mil;
        memcpy(q, mant_frac, take_frac); q += take_frac;
        for (int i = 0; i < zeros; i++) *q++ = '0';
        *q = 0;
        fputs(buf, fp);
        return;
    }
    /* try %g with progressively more precision until round-trip */
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    puts_u(buf, fp);
}

/* jq's tojson refuses to format trees deeper than this — past it,
 * a `"<skipped: too deep>"` placeholder is emitted instead.  Tested
 * exactly: 10000-level nesting still prints, 10001 truncates. */
#define NUQ_JSON_PRINT_MAX_DEPTH 10000

/* Pre-built spaces buffer for indent — written via fwrite once per
 * level rather than fputc-per-space, which is a measurable win for
 * pretty-printed output. */
static const char nuq_indent_spaces[256] = {
    [0 ... 255] = ' '
};

static void
print_value(FILE *fp, VALUE v, int indent, int depth)
{
    if (NUQ_IS_FIX(v)) { print_number(fp, v); return; }
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:   puts_u("null", fp); return;
      case NUQ_T_BOOL:   puts_u(o->b ? "true" : "false", fp); return;
      case NUQ_T_DOUBLE: print_number(fp, v); return;
      case NUQ_T_STRING: print_string(fp, o); return;
      case NUQ_T_ARRAY: {
        if (depth > NUQ_JSON_PRINT_MAX_DEPTH) {
            puts_u("\"<skipped: too deep>\"", fp);
            return;
        }
        if (o->arr.len == 0) { puts_u("[]", fp); return; }
        putc_u('[', fp);
        for (size_t i = 0; i < o->arr.len; i++) {
            if (i) putc_u(',', fp);
            if (indent > 0) {
                putc_u('\n', fp);
                int sp = (depth + 1) * indent;
                while (sp > 0) {
                    int n = sp < 256 ? sp : 256;
                    fwrite_u(nuq_indent_spaces, n, fp);
                    sp -= n;
                }
            }
            print_value(fp, o->arr.items[i], indent, depth + 1);
        }
        if (indent > 0) {
            putc_u('\n', fp);
            int sp = depth * indent;
            while (sp > 0) {
                int n = sp < 256 ? sp : 256;
                fwrite_u(nuq_indent_spaces, n, fp);
                sp -= n;
            }
        }
        putc_u(']', fp);
        return;
      }
      case NUQ_T_OBJECT: {
        if (depth > NUQ_JSON_PRINT_MAX_DEPTH) {
            puts_u("\"<skipped: too deep>\"", fp);
            return;
        }
        if (o->obj.len == 0) { puts_u("{}", fp); return; }
        /* Default emit-order is insertion order; with `-S` we emit
         * lexicographic-by-key.  Iteration goes through `order[i]` so
         * the same loop body covers both. */
        size_t local_order[16];
        size_t *order = NULL;
        bool need_free = false;
        if (OPTION.sort_keys) {
            if (o->obj.len <= sizeof(local_order)/sizeof(local_order[0])) {
                order = local_order;
            } else {
                order = (size_t *)malloc(o->obj.len * sizeof(size_t));
                need_free = true;
            }
            for (size_t i = 0; i < o->obj.len; i++) order[i] = i;
            /* insertion sort over the index — fine since object keys
             * are usually small (n < 50 in jq use), and this only
             * runs on output. */
            for (size_t i = 1; i < o->obj.len; i++) {
                size_t x = order[i];
                struct nuq_obj *xs = NUQ_PTR(o->obj.keys[x]);
                size_t j = i;
                while (j > 0) {
                    struct nuq_obj *ys = NUQ_PTR(o->obj.keys[order[j-1]]);
                    size_t ml = xs->str.len < ys->str.len ? xs->str.len : ys->str.len;
                    int c = memcmp(xs->str.bytes, ys->str.bytes, ml);
                    if (c == 0) c = (xs->str.len < ys->str.len) ? -1
                              : (xs->str.len > ys->str.len) ? 1 : 0;
                    if (c >= 0) break;
                    order[j] = order[j-1];
                    j--;
                }
                order[j] = x;
            }
        }
        putc_u('{', fp);
        for (size_t i = 0; i < o->obj.len; i++) {
            size_t ki = order ? order[i] : i;
            if (i) putc_u(',', fp);
            if (indent > 0) {
                putc_u('\n', fp);
                int sp = (depth + 1) * indent;
                while (sp > 0) {
                    int n = sp < 256 ? sp : 256;
                    fwrite_u(nuq_indent_spaces, n, fp);
                    sp -= n;
                }
            }
            struct nuq_obj *ks = NUQ_PTR(o->obj.keys[ki]);
            print_string(fp, ks);
            putc_u(':', fp);
            if (indent > 0) putc_u(' ', fp);
            print_value(fp, o->obj.vals[ki], indent, depth + 1);
        }
        if (indent > 0) {
            putc_u('\n', fp);
            int sp = depth * indent;
            while (sp > 0) {
                int n = sp < 256 ? sp : 256;
                fwrite_u(nuq_indent_spaces, n, fp);
                sp -= n;
            }
        }
        putc_u('}', fp);
        if (need_free) free(order);
        return;
      }
    }
}

void
nuq_json_print(FILE *fp, VALUE v, int indent)
{
    print_value(fp, v, indent, 0);
}
