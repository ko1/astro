// wastro — WAT tokenizer + token-to-value helpers
//
// `#include`'d from main.c (single-TU pattern; same idea as node.c
// ingesting the ASTroGen-generated *.c).  Provides the lexer state
// (`Token`, `cur_tok`, `next_token`), parse-error reporting
// (`parse_error`, `expect_*`), and pure converters from numeric /
// string tokens to wasm bit patterns (used by both this WAT parser
// and the .wast harness).

// =====================================================================
// Tokenizer
// =====================================================================

typedef enum {
    T_LPAREN,
    T_RPAREN,
    T_IDENT,    // $foo
    T_KEYWORD,  // module / func / i32.add / ...
    T_INT,
    T_STRING,
    T_EOF,
} token_kind_t;

typedef struct {
    token_kind_t kind;
    const char *start;
    size_t len;
    int64_t int_value;
    double float_value;
    int has_dot;     // 1 if the numeric token contained '.'/'e'/'E' — i.e., a float
} Token;

static const char *src_pos;
static const char *src_end;
static Token cur_tok;

static void
skip_ws_and_comments(void)
{
    for (;;) {
        while (src_pos < src_end && isspace((unsigned char)*src_pos)) src_pos++;
        if (src_pos + 1 < src_end && src_pos[0] == ';' && src_pos[1] == ';') {
            while (src_pos < src_end && *src_pos != '\n') src_pos++;
            continue;
        }
        if (src_pos + 1 < src_end && src_pos[0] == '(' && src_pos[1] == ';') {
            src_pos += 2;
            int depth = 1;
            while (src_pos + 1 < src_end && depth > 0) {
                if (src_pos[0] == '(' && src_pos[1] == ';') { depth++; src_pos += 2; }
                else if (src_pos[0] == ';' && src_pos[1] == ')') { depth--; src_pos += 2; }
                else src_pos++;
            }
            continue;
        }
        break;
    }
}

// Per WAT spec, idchars are alnum plus the punctuation set:
//   ! # $ % & ' * + - . / : < = > ? @ \ ^ _ ` | ~
// We use this for keyword tokens, which is what `offset=N`, `align=N`,
// and `nan:canonical` rely on.
static int
is_keyword_char(int ch)
{
    if (isalnum(ch)) return 1;
    switch (ch) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '/': case ':':
    case '<': case '=': case '>': case '?': case '@': case '\\':
    case '^': case '_': case '`': case '|': case '~':
        return 1;
    }
    return 0;
}

static void
next_token(void)
{
    skip_ws_and_comments();
    if (src_pos >= src_end) { cur_tok.kind = T_EOF; return; }
    char ch = *src_pos;

    if (ch == '(') { cur_tok.kind = T_LPAREN; cur_tok.start = src_pos++; cur_tok.len = 1; return; }
    if (ch == ')') { cur_tok.kind = T_RPAREN; cur_tok.start = src_pos++; cur_tok.len = 1; return; }

    if (ch == '"') {
        const char *start = ++src_pos;
        while (src_pos < src_end && *src_pos != '"') src_pos++;
        cur_tok.kind = T_STRING;
        cur_tok.start = start;
        cur_tok.len = (size_t)(src_pos - start);
        if (src_pos < src_end) src_pos++; // consume closing "
        return;
    }

    if (ch == '$') {
        const char *start = src_pos++;
        while (src_pos < src_end && (isalnum((unsigned char)*src_pos) || *src_pos == '_' || *src_pos == '$' || *src_pos == '.')) src_pos++;
        cur_tok.kind = T_IDENT;
        cur_tok.start = start;
        cur_tok.len = (size_t)(src_pos - start);
        return;
    }

    if (ch == '-' || ch == '+' || isdigit((unsigned char)ch) ||
        (ch == 'n' && src_pos + 2 < src_end && src_pos[1] == 'a' && src_pos[2] == 'n') ||
        (ch == 'i' && src_pos + 2 < src_end && src_pos[1] == 'n' && src_pos[2] == 'f')) {
        const char *start = src_pos;
        const char *p = src_pos;
        int neg = 0;
        if (*p == '+') { p++; }
        else if (*p == '-') { p++; neg = 1; }
        // Determine numeric span: digits / hex / dot / e / p / sign-after-e.
        int is_float = 0;
        int is_hex = 0;
        // Recognise nan / inf shorthand.
        int is_nan_or_inf = 0;
        if (p + 2 < src_end && p[0] == 'n' && p[1] == 'a' && p[2] == 'n') {
            is_nan_or_inf = 1; is_float = 1;
        }
        else if (p + 2 < src_end && p[0] == 'i' && p[1] == 'n' && p[2] == 'f') {
            is_nan_or_inf = 1; is_float = 1;
        }
        if (!is_nan_or_inf) {
            if (p + 1 < src_end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                is_hex = 1;
                p += 2;
            }
        }
        const char *num_start = p;
        if (is_nan_or_inf) {
            // skip nan / inf and optional `:0xNN` payload (`nan:canonical`,
            // `nan:arithmetic`, `nan:0x...`).
            p += 3;
            if (p < src_end && *p == ':') {
                p++;
                while (p < src_end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            }
        }
        else {
            while (p < src_end && (isalnum((unsigned char)*p) || *p == '.' || *p == '_')) {
                if (*p == '.' || (!is_hex && (*p == 'e' || *p == 'E')) ||
                    (is_hex && (*p == 'p' || *p == 'P'))) is_float = 1;
                if (((*p == 'e' || *p == 'E') && !is_hex) ||
                    ((*p == 'p' || *p == 'P') && is_hex)) {
                    p++;
                    if (p < src_end && (*p == '+' || *p == '-')) p++;
                    continue;
                }
                p++;
            }
        }
        // Build a copy with underscores stripped for strtoull / strtod.
        char numbuf[256];
        size_t bi = 0;
        if (neg && bi < sizeof(numbuf)) numbuf[bi++] = '-';
        if (is_hex) {
            if (bi + 2 < sizeof(numbuf)) { numbuf[bi++] = '0'; numbuf[bi++] = 'x'; }
        }
        for (const char *q = num_start; q < p && bi + 1 < sizeof(numbuf); q++) {
            if (*q == '_') continue;
            numbuf[bi++] = *q;
        }
        numbuf[bi] = 0;

        errno = 0;
        if (is_float) {
            double dv = strtod(numbuf, NULL);
            if (errno != 0 && !is_nan_or_inf) {
                // tolerate inexact
                errno = 0;
            }
            src_pos = p;
            cur_tok.kind = T_INT;
            cur_tok.start = start;
            cur_tok.len = (size_t)(src_pos - start);
            cur_tok.float_value = dv;
            cur_tok.int_value = (int64_t)dv;
            cur_tok.has_dot = 1;
            return;
        }
        else {
            // Integer: parse as unsigned, allow sign separately.  Wasm
            // integer literals can be either signed or unsigned (e.g.
            // `i32.const 0xFFFFFFFF` == -1).  We keep raw bits.
            errno = 0;
            const char *parsep = numbuf;
            int neg2 = 0;
            if (*parsep == '-') { neg2 = 1; parsep++; }
            // Wasm integer literals: leading 0 does NOT denote octal
            // (per wasm spec).  Use base 16 for `0x` / `0X` prefix,
            // otherwise base 10.
            int base = 10;
            if (parsep[0] == '0' && (parsep[1] == 'x' || parsep[1] == 'X')) base = 16;
            unsigned long long uv = strtoull(parsep, NULL, base);
            uint64_t v = neg2 ? -(uint64_t)uv : (uint64_t)uv;
            src_pos = p;
            cur_tok.kind = T_INT;
            cur_tok.start = start;
            cur_tok.len = (size_t)(src_pos - start);
            cur_tok.int_value = (int64_t)v;
            cur_tok.has_dot = 0;
            return;
        }
    }

    // keyword
    const char *start = src_pos;
    while (src_pos < src_end && is_keyword_char((unsigned char)*src_pos)) src_pos++;
    cur_tok.kind = T_KEYWORD;
    cur_tok.start = start;
    cur_tok.len = (size_t)(src_pos - start);
}

static int
tok_is_keyword(const char *kw)
{
    if (cur_tok.kind != T_KEYWORD) return 0;
    size_t kl = strlen(kw);
    return kl == cur_tok.len && memcmp(cur_tok.start, kw, kl) == 0;
}

static int
tok_eq_string(const Token *t, const char *s)
{
    size_t sl = strlen(s);
    return sl == t->len && memcmp(t->start, s, sl) == 0;
}

__attribute__((noreturn))
static void
parse_error(const char *msg)
{
    if (wastro_parse_active) {
        snprintf(wastro_parse_message, sizeof(wastro_parse_message),
                 "%s (near '%.*s')", msg, (int)cur_tok.len, cur_tok.start);
        longjmp(wastro_parse_jmp, 1);
    }
    fprintf(stderr, "wastro: parse error: %s (near '%.*s')\n",
            msg, (int)cur_tok.len, cur_tok.start);
    exit(1);
}

static void
expect_lparen(void) { if (cur_tok.kind != T_LPAREN) parse_error("expected '('"); next_token(); }
static void
expect_rparen(void) { if (cur_tok.kind != T_RPAREN) parse_error("expected ')'"); next_token(); }
static void
expect_keyword(const char *kw) { if (!tok_is_keyword(kw)) parse_error(kw); next_token(); }


// Decode a numeric token text exactly into f32/f64 bits.  Handles:
//   nan / +nan / -nan         — canonical NaN
//   nan:0xPAYLOAD             — NaN with the given mantissa bits
//   nan:canonical             — canonical NaN (treated like `nan`)
//   nan:arithmetic            — arithmetic NaN (treated like `nan`)
//   inf / +inf / -inf         — infinities
//   any other numeric form    — strtod via the pre-parsed float_value
static uint32_t
token_to_f32_bits(const Token *t, double fallback_dv)
{
    int neg = 0;
    const char *p = t->start;
    const char *end = t->start + t->len;
    if (p < end && (*p == '+' || *p == '-')) { if (*p == '-') neg = 1; p++; }
    if (end - p >= 3 && memcmp(p, "nan", 3) == 0) {
        (void)fallback_dv;
        uint32_t bits = 0x7F800000u;          // exponent all 1s
        if (neg) bits |= 0x80000000u;
        p += 3;
        if (p < end && *p == ':') {
            p++;
            if (end - p >= 9 && memcmp(p, "canonical", 9) == 0) {
                bits |= 0x00400000u;          // quiet NaN bit
            }
            else if (end - p >= 10 && memcmp(p, "arithmetic", 10) == 0) {
                bits |= 0x00400000u;
            }
            else {
                // hex payload, possibly with 0x prefix and underscores
                if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                uint32_t payload = 0;
                while (p < end) {
                    char c = *p++;
                    if (c == '_') continue;
                    if (c >= '0' && c <= '9') payload = payload * 16 + (uint32_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') payload = payload * 16 + (uint32_t)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') payload = payload * 16 + (uint32_t)(c - 'A' + 10);
                    else break;
                }
                bits |= (payload & 0x7FFFFFu);
            }
        }
        else {
            bits |= 0x00400000u;              // bare nan = canonical (quiet)
        }
        return bits;
    }
    if (end - p >= 3 && memcmp(p, "inf", 3) == 0) {
        return neg ? 0xFF800000u : 0x7F800000u;
    }
    // Generic numeric — re-parse with strtof directly to f32 to avoid
    // double-rounding artefacts that bite the spec testsuite at
    // the float / max-finite boundary.
    char buf[256];
    size_t bi = 0;
    for (const char *q = t->start; q < end && bi + 1 < sizeof(buf); q++) {
        if (*q != '_') buf[bi++] = *q;
    }
    buf[bi] = 0;
    float fv = strtof(buf, NULL);
    (void)fallback_dv;
    uint32_t b; memcpy(&b, &fv, 4);
    return b;
}

static uint64_t
token_to_f64_bits(const Token *t, double fallback_dv)
{
    int neg = 0;
    const char *p = t->start;
    const char *end = t->start + t->len;
    if (p < end && (*p == '+' || *p == '-')) { if (*p == '-') neg = 1; p++; }
    if (end - p >= 3 && memcmp(p, "nan", 3) == 0) {
        uint64_t bits = 0x7FF0000000000000ull;
        if (neg) bits |= 0x8000000000000000ull;
        p += 3;
        if (p < end && *p == ':') {
            p++;
            if (end - p >= 9 && memcmp(p, "canonical", 9) == 0) {
                bits |= 0x0008000000000000ull;
            }
            else if (end - p >= 10 && memcmp(p, "arithmetic", 10) == 0) {
                bits |= 0x0008000000000000ull;
            }
            else {
                if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                uint64_t payload = 0;
                while (p < end) {
                    char c = *p++;
                    if (c == '_') continue;
                    if (c >= '0' && c <= '9') payload = payload * 16 + (uint64_t)(c - '0');
                    else if (c >= 'a' && c <= 'f') payload = payload * 16 + (uint64_t)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') payload = payload * 16 + (uint64_t)(c - 'A' + 10);
                    else break;
                }
                bits |= (payload & 0xFFFFFFFFFFFFFull);
            }
        }
        else {
            bits |= 0x0008000000000000ull;
        }
        return bits;
    }
    if (end - p >= 3 && memcmp(p, "inf", 3) == 0) {
        return neg ? 0xFFF0000000000000ull : 0x7FF0000000000000ull;
    }
    // Generic numeric — re-parse with strtod for max precision.
    char buf[256];
    size_t bi = 0;
    for (const char *q = t->start; q < end && bi + 1 < sizeof(buf); q++) {
        if (*q != '_') buf[bi++] = *q;
    }
    buf[bi] = 0;
    double dv = strtod(buf, NULL);
    (void)fallback_dv;
    uint64_t b; memcpy(&b, &dv, 8);
    return b;
}

static uint8_t *
decode_wasm_str(const Token *t, uint32_t *out_len)
{
    // Per wasm spec, string escapes are:
    //   \n \t \r \" \' \\ \u{HEX...}  +  \HH  (two hex digits → one byte)
    // Note that there is no `\0` shorthand: the null byte is `\00`.
    uint8_t *buf = malloc(t->len + 1);
    uint32_t bi = 0;
    for (size_t i = 0; i < t->len; i++) {
        unsigned char ch = (unsigned char)t->start[i];
        if (ch == '\\' && i + 1 < t->len) {
            unsigned char e = (unsigned char)t->start[i + 1];
            if (e == 'n')       { buf[bi++] = '\n'; i++; }
            else if (e == 't')  { buf[bi++] = '\t'; i++; }
            else if (e == 'r')  { buf[bi++] = '\r'; i++; }
            else if (e == '\\') { buf[bi++] = '\\'; i++; }
            else if (e == '\'') { buf[bi++] = '\''; i++; }
            else if (e == '"')  { buf[bi++] = '"';  i++; }
            else if (e == 'u' && i + 2 < t->len && t->start[i + 2] == '{') {
                // \u{XXXX} — UTF-8 encode the codepoint.
                size_t j = i + 3;
                uint32_t cp = 0;
                while (j < t->len && t->start[j] != '}') {
                    char cc = t->start[j];
                    if (!isxdigit((unsigned char)cc)) break;
                    cp = cp * 16 + (uint32_t)((cc <= '9') ? (cc - '0') :
                                               (cc <= 'F') ? (cc - 'A' + 10) :
                                                             (cc - 'a' + 10));
                    j++;
                }
                if (j < t->len && t->start[j] == '}') {
                    if (cp < 0x80) buf[bi++] = (uint8_t)cp;
                    else if (cp < 0x800) {
                        buf[bi++] = 0xC0 | (cp >> 6);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    else if (cp < 0x10000) {
                        buf[bi++] = 0xE0 | (cp >> 12);
                        buf[bi++] = 0x80 | ((cp >> 6) & 0x3F);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    else {
                        buf[bi++] = 0xF0 | (cp >> 18);
                        buf[bi++] = 0x80 | ((cp >> 12) & 0x3F);
                        buf[bi++] = 0x80 | ((cp >> 6) & 0x3F);
                        buf[bi++] = 0x80 | (cp & 0x3F);
                    }
                    i = j;
                }
                else buf[bi++] = ch;
            }
            else if (i + 2 < t->len && isxdigit(e) && isxdigit((unsigned char)t->start[i + 2])) {
                char hex[3] = { (char)e, t->start[i + 2], 0 };
                buf[bi++] = (uint8_t)strtoul(hex, NULL, 16);
                i += 2;
            }
            else buf[bi++] = ch;   // unrecognised — keep backslash literal
        }
        else {
            buf[bi++] = ch;
        }
    }
    *out_len = bi;
    return buf;
}
