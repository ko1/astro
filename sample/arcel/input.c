/* Hand-written JSON parser that builds arcel VALUE trees in the
 * supplied arena.  Used by:
 *   • the `eval` / `bench` subcommands, to parse `-i '<json>'` input
 *   • the `repl` subcommand, to parse the `i:` field of each envelope
 *
 * Why not jansson / cJSON: we already pay for an arena so per-value
 * allocations are basically free; pulling in a third-party JSON parser
 * would also drag a separate allocator.  This handles the JSON subset
 * the test harness emits (numbers, strings, bools, null, arrays,
 * objects) — comments and trailing commas not supported.
 */

#include <math.h>
#include <ctype.h>
#include "input.h"
#include "value.h"

typedef struct {
    const char *p;
    uint32_t    pos;
    uint32_t    len;
    arcel_arena *arena;
    const char *err;
} J;

static VALUE j_value(J *j);

static VALUE
jerr(J *const j, const char *const msg)
{
    if (!j->err) j->err = arcel_arena_msg(j->arena, "json parse: %s at offset %u", msg, j->pos);
    return V_ERR(j->err);
}

static void
j_skip_ws(J *const j)
{
    while (j->pos < j->len) {
        char c = j->p[j->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->pos++;
        else break;
    }
}

static bool
j_match(J *const j, const char *const s)
{
    uint32_t n = (uint32_t)strlen(s);
    if (j->pos + n > j->len) return false;
    if (memcmp(j->p + j->pos, s, n) != 0) return false;
    j->pos += n;
    return true;
}

static VALUE
j_string(J *const j)
{
    if (j->pos >= j->len || j->p[j->pos] != '"') return jerr(j, "expected '\"'");
    j->pos++;
    /* worst case: same number of bytes as input */
    char *const buf = (char *)arcel_arena_alloc(j->arena, j->len - j->pos + 1, 1);
    uint32_t out = 0;
    while (j->pos < j->len && j->p[j->pos] != '"') {
        char c = j->p[j->pos];
        if (c == '\\') {
            if (++j->pos >= j->len) return jerr(j, "trailing backslash");
            switch (j->p[j->pos]) {
                case '"':  buf[out++] = '"';  j->pos++; break;
                case '\\': buf[out++] = '\\'; j->pos++; break;
                case '/':  buf[out++] = '/';  j->pos++; break;
                case 'b':  buf[out++] = '\b'; j->pos++; break;
                case 'f':  buf[out++] = '\f'; j->pos++; break;
                case 'n':  buf[out++] = '\n'; j->pos++; break;
                case 'r':  buf[out++] = '\r'; j->pos++; break;
                case 't':  buf[out++] = '\t'; j->pos++; break;
                case 'u': {
                    if (j->pos + 5 > j->len) return jerr(j, "bad \\u escape");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = j->p[j->pos + 1 + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                        else return jerr(j, "bad hex in \\u");
                    }
                    j->pos += 5;
                    /* UTF-8 encode the codepoint (no surrogate-pair handling
                     * — input from harness should be normalized) */
                    if (cp < 0x80) buf[out++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[out++] = (char)(0xC0 | (cp >> 6));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[out++] = (char)(0xE0 | (cp >> 12));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return jerr(j, "unknown \\ escape");
            }
        } else {
            buf[out++] = c;
            j->pos++;
        }
    }
    if (j->pos >= j->len) return jerr(j, "unterminated string");
    j->pos++;   /* consume closing " */
    buf[out] = '\0';
    return V_STR(buf, out);
}

static VALUE
j_number(J *const j)
{
    uint32_t start = j->pos;
    bool is_float = false;
    if (j->p[j->pos] == '-') j->pos++;
    while (j->pos < j->len && isdigit((unsigned char)j->p[j->pos])) j->pos++;
    if (j->pos < j->len && j->p[j->pos] == '.') {
        is_float = true;
        j->pos++;
        while (j->pos < j->len && isdigit((unsigned char)j->p[j->pos])) j->pos++;
    }
    if (j->pos < j->len && (j->p[j->pos] == 'e' || j->p[j->pos] == 'E')) {
        is_float = true;
        j->pos++;
        if (j->pos < j->len && (j->p[j->pos] == '-' || j->p[j->pos] == '+')) j->pos++;
        while (j->pos < j->len && isdigit((unsigned char)j->p[j->pos])) j->pos++;
    }
    char tmp[64];
    uint32_t n = j->pos - start;
    if (n >= sizeof(tmp)) return jerr(j, "number too long");
    memcpy(tmp, j->p + start, n);
    tmp[n] = '\0';
    if (is_float) return V_DOUBLE(strtod(tmp, NULL));
    return V_INT((int64_t)strtoll(tmp, NULL, 10));
}

static VALUE
j_array(J *const j)
{
    j->pos++;  /* consume '[' */
    j_skip_ws(j);
    /* Buffer values via a temporary growing list, then copy into arena. */
    VALUE   *tmp = NULL;
    uint32_t cap = 0, len = 0;
    if (j->pos < j->len && j->p[j->pos] == ']') { j->pos++; return V_LIST(arcel_list_new(j->arena, 0)); }
    while (1) {
        VALUE v = j_value(j);
        if (v.tag == AC_ERR) { free(tmp); return v; }
        if (len == cap) {
            cap = cap ? cap * 2 : 8;
            tmp = (VALUE *)realloc(tmp, sizeof(VALUE) * cap);
        }
        tmp[len++] = v;
        j_skip_ws(j);
        if (j->pos < j->len && j->p[j->pos] == ',') { j->pos++; j_skip_ws(j); continue; }
        if (j->pos < j->len && j->p[j->pos] == ']') { j->pos++; break; }
        free(tmp);
        return jerr(j, "expected ',' or ']'");
    }
    arcel_list *const out = arcel_list_new(j->arena, len);
    if (len) memcpy(out->items, tmp, sizeof(VALUE) * len);
    free(tmp);
    return V_LIST(out);
}

static VALUE
j_object(J *const j)
{
    j->pos++;  /* consume '{' */
    j_skip_ws(j);
    arcel_map_entry *tmp = NULL;
    uint32_t cap = 0, len = 0;
    if (j->pos < j->len && j->p[j->pos] == '}') { j->pos++; return V_MAP(arcel_map_new(j->arena, 0)); }
    while (1) {
        VALUE k = j_string(j);
        if (k.tag == AC_ERR) { free(tmp); return k; }
        j_skip_ws(j);
        if (j->pos >= j->len || j->p[j->pos] != ':') { free(tmp); return jerr(j, "expected ':'"); }
        j->pos++;
        VALUE v = j_value(j);
        if (v.tag == AC_ERR) { free(tmp); return v; }
        if (len == cap) {
            cap = cap ? cap * 2 : 8;
            tmp = (arcel_map_entry *)realloc(tmp, sizeof(arcel_map_entry) * cap);
        }
        tmp[len].key = k;
        tmp[len].val = v;
        len++;
        j_skip_ws(j);
        if (j->pos < j->len && j->p[j->pos] == ',') { j->pos++; j_skip_ws(j); continue; }
        if (j->pos < j->len && j->p[j->pos] == '}') { j->pos++; break; }
        free(tmp);
        return jerr(j, "expected ',' or '}'");
    }
    arcel_map *const out = arcel_map_new(j->arena, len);
    if (len) memcpy(out->entries, tmp, sizeof(arcel_map_entry) * len);
    free(tmp);
    return V_MAP(out);
}

static VALUE
j_value(J *const j)
{
    j_skip_ws(j);
    if (j->pos >= j->len) return jerr(j, "unexpected EOF");
    char c = j->p[j->pos];
    switch (c) {
        case '"': return j_string(j);
        case '{': return j_object(j);
        case '[': return j_array(j);
        case 't': if (j_match(j, "true"))  return V_BOOL(true);  return jerr(j, "expected 'true'");
        case 'f': if (j_match(j, "false")) return V_BOOL(false); return jerr(j, "expected 'false'");
        case 'n': if (j_match(j, "null"))  return V_NULL();      return jerr(j, "expected 'null'");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return j_number(j);
            return jerr(j, "expected value");
    }
}

VALUE
arcel_parse_json(arcel_arena *const arena, const char *const src, const uint32_t len)
{
    J j = (J){ .p = src, .pos = 0, .len = len, .arena = arena, .err = NULL };
    VALUE v = j_value(&j);
    if (v.tag == AC_ERR) return v;
    j_skip_ws(&j);
    if (j.pos != j.len) return jerr(&j, "trailing characters");
    return v;
}
