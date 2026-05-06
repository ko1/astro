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

static VALUE parse_value(const char **pp, const char *end, char **err);

static VALUE
parse_string_raw(const char **pp, const char *end, char **err)
{
    const char *p = *pp;
    if (*p != '"') { *err = fmt_err("expected '\"'"); return NUQ_NULL; }
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
    /* int parse — try strtoll; fall back to double on overflow */
    char *e = NULL;
    long long ll = strtoll(buf, &e, 10);
    if (e && *e == '\0' && ll >= NUQ_FIX_MIN && ll <= NUQ_FIX_MAX) {
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
        *err = fmt_err("expected 'null'"); return NUQ_NULL;
    }
    if (*p == '[') {
        p++;
        VALUE arr = nuq_make_array(0);
        skip_ws(&p, end);
        if (p < end && *p == ']') { *pp = p + 1; return arr; }
        for (;;) {
            *pp = p;
            VALUE v = parse_value(pp, end, err);
            if (*err) return NUQ_NULL;
            nuq_array_push(arr, v);
            p = *pp;
            skip_ws(&p, end);
            if (p < end && *p == ',') { p++; skip_ws(&p, end); continue; }
            if (p < end && *p == ']') { *pp = p + 1; return arr; }
            *err = fmt_err("expected ',' or ']' in array"); return NUQ_NULL;
        }
    }
    if (*p == '{') {
        p++;
        VALUE obj = nuq_make_object(0);
        skip_ws(&p, end);
        if (p < end && *p == '}') { *pp = p + 1; return obj; }
        for (;;) {
            skip_ws(&p, end);
            *pp = p;
            VALUE key = parse_string_raw(pp, end, err);
            if (*err) return NUQ_NULL;
            p = *pp;
            skip_ws(&p, end);
            if (p >= end || *p != ':') { *err = fmt_err("expected ':' in object"); return NUQ_NULL; }
            p++;
            *pp = p;
            VALUE val = parse_value(pp, end, err);
            if (*err) return NUQ_NULL;
            nuq_object_set(obj, key, val);
            p = *pp;
            skip_ws(&p, end);
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { *pp = p + 1; return obj; }
            *err = fmt_err("expected ',' or '}' in object"); return NUQ_NULL;
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
    VALUE v = parse_value(&p, end, &err);
    if (errmsg) *errmsg = err;
    if (endp) *endp = p;
    return v;
}

/* ---- printer ---- */

static void
print_string(FILE *fp, struct nuq_obj *o)
{
    fputc('"', fp);
    for (size_t i = 0; i < o->str.len; i++) {
        unsigned char c = (unsigned char)o->str.bytes[i];
        switch (c) {
          case '"':  fputs("\\\"", fp); break;
          case '\\': fputs("\\\\", fp); break;
          case '\b': fputs("\\b", fp);  break;
          case '\f': fputs("\\f", fp);  break;
          case '\n': fputs("\\n", fp);  break;
          case '\r': fputs("\\r", fp);  break;
          case '\t': fputs("\\t", fp);  break;
          default:
            if (c < 0x20) fprintf(fp, "\\u%04x", c);
            else fputc(c, fp);
            break;
        }
    }
    fputc('"', fp);
}

static void
print_number(FILE *fp, VALUE v)
{
    if (NUQ_IS_FIX(v)) {
        fprintf(fp, "%" PRId64, (int64_t)NUQ_FIX_VAL(v));
        return;
    }
    double d = NUQ_PTR(v)->dbl;
    if (isnan(d)) { fputs("null", fp); return; }
    if (isinf(d)) { fputs(d < 0 ? "-1.7976931348623157e+308" : "1.7976931348623157e+308", fp); return; }
    /* match jq's output: integer-valued doubles as "1234", else use %.17g
     * but try shorter forms first for round-tripping. */
    if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15) {
        fprintf(fp, "%" PRId64, (int64_t)d);
        return;
    }
    char buf[64];
    /* try %g with progressively more precision until round-trip */
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    fputs(buf, fp);
}

static void
print_value(FILE *fp, VALUE v, int indent, int depth)
{
    if (NUQ_IS_FIX(v)) { print_number(fp, v); return; }
    struct nuq_obj *o = NUQ_PTR(v);
    switch (o->type) {
      case NUQ_T_NULL:   fputs("null", fp); return;
      case NUQ_T_BOOL:   fputs(o->b ? "true" : "false", fp); return;
      case NUQ_T_DOUBLE: print_number(fp, v); return;
      case NUQ_T_STRING: print_string(fp, o); return;
      case NUQ_T_ARRAY: {
        if (o->arr.len == 0) { fputs("[]", fp); return; }
        fputc('[', fp);
        for (size_t i = 0; i < o->arr.len; i++) {
            if (i) fputc(',', fp);
            if (indent > 0) {
                fputc('\n', fp);
                for (int s = 0; s < (depth+1) * indent; s++) fputc(' ', fp);
            }
            print_value(fp, o->arr.items[i], indent, depth + 1);
        }
        if (indent > 0) {
            fputc('\n', fp);
            for (int s = 0; s < depth * indent; s++) fputc(' ', fp);
        }
        fputc(']', fp);
        return;
      }
      case NUQ_T_OBJECT: {
        if (o->obj.len == 0) { fputs("{}", fp); return; }
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
        fputc('{', fp);
        for (size_t i = 0; i < o->obj.len; i++) {
            size_t ki = order ? order[i] : i;
            if (i) fputc(',', fp);
            if (indent > 0) {
                fputc('\n', fp);
                for (int s = 0; s < (depth+1) * indent; s++) fputc(' ', fp);
            }
            struct nuq_obj *ks = NUQ_PTR(o->obj.keys[ki]);
            print_string(fp, ks);
            fputc(':', fp);
            if (indent > 0) fputc(' ', fp);
            print_value(fp, o->obj.vals[ki], indent, depth + 1);
        }
        if (indent > 0) {
            fputc('\n', fp);
            for (int s = 0; s < depth * indent; s++) fputc(' ', fp);
        }
        fputc('}', fp);
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
