/* CEL recursive-descent parser.
 *
 * Grammar (operators in low → high precedence):
 *   Expr      = TernaryExpr
 *   Ternary   = LogicalOr ('?' LogicalOr ':' Expr)?
 *   LogicalOr = LogicalAnd ('||' LogicalAnd)*
 *   LogicalAnd= Relation ('&&' Relation)*
 *   Relation  = AddOp (RELOP AddOp)?      -- relops are NOT chained in CEL
 *   AddOp     = MulOp (('+'|'-') MulOp)*
 *   MulOp     = Unary (('*'|'/'|'%') Unary)*
 *   Unary     = ('!'|'-')* Member
 *   Member    = Primary (Selector | Index | Call)*
 *      Selector = '.' IDENT ['(' [ExprList] ')']      -- dotted method call
 *      Index    = '[' Expr ']'
 *      Call     = '(' [ExprList] ')'                  -- only on identifier primaries
 *   Primary   = '(' Expr ')'
 *             | '[' [ExprList] ']'                    -- list literal
 *             | '{' [MapInits] '}'                    -- map literal
 *             | IDENT ['(' [ExprList] ')']            -- ident or function call
 *             | LITERAL
 *
 * Macros (recognised at parse time):
 *   has(field-access)
 *   <iter>.all(<id>, <pred>)
 *   <iter>.exists(<id>, <pred>)
 *   <iter>.exists_one(<id>, <pred>)
 *   <iter>.filter(<id>, <pred>)
 *   <iter>.map(<id>, <transform>)
 *   <iter>.map(<id>, <pred>, <transform>)
 *
 * Functions special-cased to dedicated AST nodes (so AOT can specialize
 * each one's hot loop):
 *   size, type, int, uint, double, string, bool, bytes,
 *   <recv>.startsWith, .endsWith, .contains, .matches
 *
 * Anything else parses as a generic call → node_call (TODO: implement
 * the dispatch table; for now node_call surfaces as `unknown function`
 * at eval time so we get a clear error rather than a silent miss).
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include "parser.h"

/* ---- variadic side array ----------------------------------------- */

NODE   **arcel_node_arr     = NULL;
uint32_t arcel_node_arr_cap = 0;
uint32_t arcel_node_arr_top = 0;

void
arcel_node_arr_reset(void)
{
    arcel_node_arr_top = 0;
    /* keep the buffer malloc'd for reuse — only the high-water mark
     * is rewound */
}

/* ---- constant-list side table ---- */

struct arcel_list **arcel_const_list_arr = NULL;
uint32_t            arcel_const_list_cap = 0;
uint32_t            arcel_const_list_top = 0;

uint32_t
arcel_const_list_push(struct arcel_list *const l)
{
    if (arcel_const_list_top >= arcel_const_list_cap) {
        uint32_t newcap = arcel_const_list_cap ? arcel_const_list_cap * 2 : 16;
        arcel_const_list_arr = (struct arcel_list **)realloc(arcel_const_list_arr,
                                                              sizeof(struct arcel_list *) * newcap);
        if (!arcel_const_list_arr) { fprintf(stderr, "arcel: const_list_arr OOM\n"); exit(1); }
        arcel_const_list_cap = newcap;
    }
    uint32_t idx = arcel_const_list_top++;
    arcel_const_list_arr[idx] = l;
    return idx;
}

void
arcel_const_list_reset(void)
{
    /* Free the cached lists — they were malloc'd in arcel_const_list_build. */
    for (uint32_t i = 0; i < arcel_const_list_top; i++) {
        if (arcel_const_list_arr[i]) {
            free(arcel_const_list_arr[i]->items);
            free(arcel_const_list_arr[i]);
        }
    }
    arcel_const_list_top = 0;
}

uint32_t
arcel_node_arr_push_n(NODE *const *const nodes, const uint32_t n)
{
    if (arcel_node_arr_top + n > arcel_node_arr_cap) {
        uint32_t newcap = arcel_node_arr_cap ? arcel_node_arr_cap * 2 : 64;
        while (newcap < arcel_node_arr_top + n) newcap *= 2;
        arcel_node_arr = (NODE **)realloc(arcel_node_arr, sizeof(NODE *) * newcap);
        if (!arcel_node_arr) { fprintf(stderr, "arcel: node_arr OOM\n"); exit(1); }
        arcel_node_arr_cap = newcap;
    }
    uint32_t idx = arcel_node_arr_top;
    if (n) memcpy(arcel_node_arr + idx, nodes, sizeof(NODE *) * n);
    arcel_node_arr_top += n;
    return idx;
}

/* ---- shared parse state ----------------------------------------- */

typedef enum {
    TK_EOF, TK_INT, TK_UINT, TK_DOUBLE, TK_STR, TK_BYTES,
    TK_IDENT, TK_TRUE, TK_FALSE, TK_NULL,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_IN,
    TK_AND, TK_OR, TK_NOT,
    TK_QMARK, TK_COLON, TK_DOT, TK_COMMA,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
} tok_kind;

typedef struct {
    tok_kind  kind;
    const char *p;       /* points into source for IDENT, into a heap
                            buffer for STR/BYTES (we own the unescaped form) */
    uint32_t   len;
    int64_t    i64;
    uint64_t   u64;
    double     d;
} tok_t;

typedef struct {
    const char *src;
    uint32_t    pos;
    uint32_t    len;
    int         line;
    int         col;
    tok_t       tok;       /* current */
    char        err_buf[256];
    bool        had_err;
} P;

static __attribute__((format(printf, 2, 3))) void
p_error(P *const p, const char *const fmt, ...)
{
    if (p->had_err) return;
    int n = snprintf(p->err_buf, sizeof(p->err_buf),
                     "<input>:%d:%d: ", p->line, p->col);
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->err_buf + n, sizeof(p->err_buf) - n, fmt, ap);
    va_end(ap);
    p->had_err = true;
}

static char
peek_char(P *const p)
{
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char
advance_char(P *const p)
{
    if (p->pos >= p->len) return '\0';
    char c = p->src[p->pos++];
    if (c == '\n') { p->line++; p->col = 1; } else { p->col++; }
    return c;
}

static void
skip_ws(P *const p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance_char(p);
        else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
        }
        else break;
    }
}

/* String literal lex.  Handles single/double/triple-quoted, raw (`r"..."`),
 * and byte (`b"..."`) prefixes.  CEL escapes:
 *   \a \b \f \n \r \t \v \\ \" \' \?
 *   \xNN  \uNNNN  \UNNNNNNNN  \NNN (octal up to 3 digits) */
static void
lex_string(P *const p)
{
    bool is_raw = false, is_bytes = false;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == 'r' || c == 'R')      { is_raw = true;   p->pos++; p->col++; }
        else if (c == 'b' || c == 'B') { is_bytes = true; p->pos++; p->col++; }
        else break;
    }
    char quote = p->src[p->pos];
    bool triple = false;
    if (p->pos + 2 < p->len && p->src[p->pos + 1] == quote && p->src[p->pos + 2] == quote) {
        triple = true;
        p->pos += 3; p->col += 3;
    } else {
        p->pos++; p->col++;
    }

    /* Build the string into a heap buffer that lives as long as the AST. */
    uint32_t cap = 32;
    char *buf = (char *)malloc(cap);
    uint32_t out = 0;

    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == quote) {
            if (triple) {
                if (p->pos + 2 < p->len && p->src[p->pos + 1] == quote && p->src[p->pos + 2] == quote) {
                    p->pos += 3; p->col += 3;
                    goto done;
                }
                /* single closing quote inside triple-quoted, take literally */
            } else {
                p->pos++; p->col++;
                goto done;
            }
        }
        if (out + 8 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); }

        if (!is_raw && c == '\\' && p->pos + 1 < p->len) {
            char esc = p->src[p->pos + 1];
            p->pos += 2; p->col += 2;
            switch (esc) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 'a':  buf[out++] = '\a'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'v':  buf[out++] = '\v'; break;
                case '\\': buf[out++] = '\\'; break;
                case '\'': buf[out++] = '\''; break;
                case '"':  buf[out++] = '"';  break;
                case '`':  buf[out++] = '`';  break;
                case '?':  buf[out++] = '?';  break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    /* Octal escape `\NNN`.  cel-spec semantics:
                     *   - in a STRING literal, the value is a codepoint
                     *     (UTF-8-encoded into the buffer) — so `\377`
                     *     becomes U+00FF = "\xC3\xBF".
                     *   - in a BYTES literal, the value is a raw byte. */
                    int v = esc - '0';
                    int k = 0;
                    while (k < 2 && p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '7') {
                        v = v * 8 + (p->src[p->pos] - '0');
                        p->pos++; p->col++; k++;
                    }
                    if (is_bytes) {
                        buf[out++] = (char)(v & 0xFF);
                    } else {
                        if (v < 0x80) buf[out++] = (char)v;
                        else if (v < 0x800) {
                            buf[out++] = (char)(0xC0 | (v >> 6));
                            buf[out++] = (char)(0x80 | (v & 0x3F));
                        } else {
                            buf[out++] = (char)(0xE0 | (v >> 12));
                            buf[out++] = (char)(0x80 | ((v >> 6) & 0x3F));
                            buf[out++] = (char)(0x80 | (v & 0x3F));
                        }
                    }
                    break;
                }
                case 'x': case 'X': {
                    /* `\xNN` — same string-vs-bytes split as octal. */
                    if (p->pos + 2 > p->len) { p_error(p, "bad \\x escape"); free(buf); p->had_err = true; return; }
                    int v = 0;
                    for (int k = 0; k < 2; k++) {
                        char h = p->src[p->pos++]; p->col++;
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= h - '0';
                        else if (h >= 'a' && h <= 'f') v |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F') v |= 10 + h - 'A';
                        else { p_error(p, "bad hex in \\x"); free(buf); return; }
                    }
                    if (is_bytes) {
                        buf[out++] = (char)v;
                    } else {
                        if (v < 0x80) buf[out++] = (char)v;
                        else {
                            buf[out++] = (char)(0xC0 | (v >> 6));
                            buf[out++] = (char)(0x80 | (v & 0x3F));
                        }
                    }
                    break;
                }
                case 'u': case 'U': {
                    int width = (esc == 'u') ? 4 : 8;
                    if ((uint32_t)(p->pos + width) > p->len) { p_error(p, "bad \\%c escape", esc); free(buf); return; }
                    uint32_t cp = 0;
                    for (int k = 0; k < width; k++) {
                        char h = p->src[p->pos++]; p->col++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                        else { p_error(p, "bad hex in \\%c", esc); free(buf); return; }
                    }
                    /* UTF-8 encode (no surrogate-pair pairing) */
                    if (cp < 0x80) buf[out++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[out++] = (char)(0xC0 | (cp >> 6));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[out++] = (char)(0xE0 | (cp >> 12));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[out++] = (char)(0xF0 | (cp >> 18));
                        buf[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: buf[out++] = esc; break;   /* lenient */
            }
        } else {
            buf[out++] = c;
            advance_char(p);
        }
    }
    p_error(p, "unterminated string");
done:
    p->tok.kind = is_bytes ? TK_BYTES : TK_STR;
    p->tok.p   = buf;
    p->tok.len = out;
}

/* Returns true if at the start of an identifier-like sequence that's
 * actually one of our reserved-as-special idents.  Our lexer always
 * returns IDENT for words; the parser then upgrades certain words
 * (true/false/null/in) by checking `kind` after `lex`. */
static void
lex_ident_or_keyword(P *const p)
{
    uint32_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (isalnum((unsigned char)c) || c == '_') { p->pos++; p->col++; }
        else break;
    }
    p->tok.p   = p->src + start;
    p->tok.len = p->pos - start;
    if (p->tok.len == 4 && memcmp(p->tok.p, "true", 4)  == 0) { p->tok.kind = TK_TRUE;  return; }
    if (p->tok.len == 5 && memcmp(p->tok.p, "false", 5) == 0) { p->tok.kind = TK_FALSE; return; }
    if (p->tok.len == 4 && memcmp(p->tok.p, "null", 4)  == 0) { p->tok.kind = TK_NULL;  return; }
    if (p->tok.len == 2 && memcmp(p->tok.p, "in",   2)  == 0) { p->tok.kind = TK_IN;    return; }
    p->tok.kind = TK_IDENT;
}

static void
lex_number(P *const p)
{
    uint32_t start = p->pos;
    bool is_float = false;
    bool is_hex   = false;

    if (p->src[p->pos] == '0' && p->pos + 1 < p->len &&
        (p->src[p->pos + 1] == 'x' || p->src[p->pos + 1] == 'X')) {
        is_hex = true;
        p->pos += 2; p->col += 2;
        while (p->pos < p->len && isxdigit((unsigned char)p->src[p->pos])) { p->pos++; p->col++; }
    } else {
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) { p->pos++; p->col++; }
        if (p->pos < p->len && p->src[p->pos] == '.' &&
            p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1])) {
            is_float = true;
            p->pos++; p->col++;
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) { p->pos++; p->col++; }
        }
        if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
            is_float = true;
            p->pos++; p->col++;
            if (p->pos < p->len && (p->src[p->pos] == '-' || p->src[p->pos] == '+')) { p->pos++; p->col++; }
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) { p->pos++; p->col++; }
        }
    }

    char tmp[64];
    uint32_t n = p->pos - start;
    if (n >= sizeof(tmp)) { p_error(p, "numeric literal too long"); return; }
    memcpy(tmp, p->src + start, n);
    tmp[n] = '\0';

    bool is_uint = false;
    if (p->pos < p->len && (p->src[p->pos] == 'u' || p->src[p->pos] == 'U')) {
        is_uint = true;
        p->pos++; p->col++;
    }

    if (is_float) {
        p->tok.kind = TK_DOUBLE;
        p->tok.d    = strtod(tmp, NULL);
        return;
    }
    if (is_uint) {
        p->tok.kind = TK_UINT;
        p->tok.u64  = strtoull(tmp, NULL, is_hex ? 16 : 10);
        return;
    }
    p->tok.kind = TK_INT;
    /* Use unsigned parse then bit-cast so INT64_MIN literal (`9223372036854775808`,
     * with leading minus prepended in the grammar) doesn't trip. */
    p->tok.u64 = strtoull(tmp, NULL, is_hex ? 16 : 10);
    p->tok.i64 = (int64_t)p->tok.u64;
}

static void
lex(P *const p)
{
    skip_ws(p);
    if (p->pos >= p->len) { p->tok.kind = TK_EOF; return; }
    char c = p->src[p->pos];
    char nxt = (p->pos + 1 < p->len) ? p->src[p->pos + 1] : '\0';

    /* String prefixes: r"..."  R"..."  b"..."  B"..."  br"..."  rb"...". */
    if ((c == 'r' || c == 'R' || c == 'b' || c == 'B') &&
        (nxt == '"' || nxt == '\'' ||
         /* two-letter prefix: br, rb (any case) */
         ((nxt == 'r' || nxt == 'R' || nxt == 'b' || nxt == 'B') &&
          p->pos + 2 < p->len && (p->src[p->pos + 2] == '"' || p->src[p->pos + 2] == '\'')))) {
        lex_string(p);
        return;
    }
    if (c == '"' || c == '\'') { lex_string(p); return; }

    if (isdigit((unsigned char)c)) { lex_number(p); return; }
    if (c == '.' && p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1])) {
        /* leading-dot float */
        lex_number(p);
        return;
    }

    if (isalpha((unsigned char)c) || c == '_') { lex_ident_or_keyword(p); return; }

    /* punctuation and operators */
    p->pos++; p->col++;
    switch (c) {
        case '+': p->tok.kind = TK_PLUS;    return;
        case '-': p->tok.kind = TK_MINUS;   return;
        case '*': p->tok.kind = TK_STAR;    return;
        case '/': p->tok.kind = TK_SLASH;   return;
        case '%': p->tok.kind = TK_PERCENT; return;
        case '?': p->tok.kind = TK_QMARK;   return;
        case ':': p->tok.kind = TK_COLON;   return;
        case '.': p->tok.kind = TK_DOT;     return;
        case ',': p->tok.kind = TK_COMMA;   return;
        case '(': p->tok.kind = TK_LPAREN;  return;
        case ')': p->tok.kind = TK_RPAREN;  return;
        case '[': p->tok.kind = TK_LBRACK;  return;
        case ']': p->tok.kind = TK_RBRACK;  return;
        case '{': p->tok.kind = TK_LBRACE;  return;
        case '}': p->tok.kind = TK_RBRACE;  return;
        case '=':
            if (peek_char(p) == '=') { p->pos++; p->col++; p->tok.kind = TK_EQ; return; }
            p_error(p, "unexpected '=' (use '==')"); return;
        case '!':
            if (peek_char(p) == '=') { p->pos++; p->col++; p->tok.kind = TK_NE; return; }
            p->tok.kind = TK_NOT; return;
        case '<':
            if (peek_char(p) == '=') { p->pos++; p->col++; p->tok.kind = TK_LE; return; }
            p->tok.kind = TK_LT; return;
        case '>':
            if (peek_char(p) == '=') { p->pos++; p->col++; p->tok.kind = TK_GE; return; }
            p->tok.kind = TK_GT; return;
        case '&':
            if (peek_char(p) == '&') { p->pos++; p->col++; p->tok.kind = TK_AND; return; }
            p_error(p, "unexpected '&' (use '&&')"); return;
        case '|':
            if (peek_char(p) == '|') { p->pos++; p->col++; p->tok.kind = TK_OR; return; }
            p_error(p, "unexpected '|' (use '||')"); return;
        default:
            p_error(p, "unexpected character '%c'", c); return;
    }
}

static bool
accept(P *const p, tok_kind k)
{
    if (p->tok.kind == k) { lex(p); return true; }
    return false;
}

static bool
expect(P *const p, tok_kind k, const char *what)
{
    if (p->tok.kind == k) { lex(p); return true; }
    p_error(p, "expected %s", what);
    return false;
}

/* Stable copy of an identifier so the AST owns its name. */
static const char *
intern_ident(const char *src, uint32_t len)
{
    char *const s = (char *)malloc(len + 1);
    memcpy(s, src, len);
    s[len] = '\0';
    return s;
}

/* True iff `n` is a pure literal node — its EVAL has no side effects
 * and produces the same VALUE every time, so the parser can fold it
 * at parse time.  Used to decide whether `[a, b, c]` qualifies for
 * node_const_list optimization (skipping per-eval list rebuild). */
static bool
is_pure_literal(NODE *const n)
{
    const char *const k = n->head.kind->default_dispatcher_name;
    return strcmp(k, "DISPATCH_node_int_lit")    == 0 ||
           strcmp(k, "DISPATCH_node_int64_lit")  == 0 ||
           strcmp(k, "DISPATCH_node_uint_lit")   == 0 ||
           strcmp(k, "DISPATCH_node_double_lit") == 0 ||
           strcmp(k, "DISPATCH_node_bool_lit")   == 0 ||
           strcmp(k, "DISPATCH_node_null_lit")   == 0 ||
           strcmp(k, "DISPATCH_node_str_lit")    == 0 ||
           strcmp(k, "DISPATCH_node_bytes_lit")  == 0;
}

/* Compute the VALUE a pure-literal node would produce.  Mirrors the
 * NODE_DEF bodies for each lit type — must be kept in sync.  Only
 * reads operands from the node struct, never touches CTX (the literal
 * VALUEs don't need arena). */
static VALUE
eval_literal(NODE *const n)
{
    const char *const k = n->head.kind->default_dispatcher_name;
    if (strcmp(k, "DISPATCH_node_int_lit")    == 0) return V_INT((int64_t)n->u.node_int_lit.v);
    if (strcmp(k, "DISPATCH_node_int64_lit")  == 0) return V_INT((int64_t)n->u.node_int64_lit.bits);
    if (strcmp(k, "DISPATCH_node_uint_lit")   == 0) return V_UINT(n->u.node_uint_lit.v);
    if (strcmp(k, "DISPATCH_node_double_lit") == 0) return V_DOUBLE(n->u.node_double_lit.v);
    if (strcmp(k, "DISPATCH_node_bool_lit")   == 0) return V_BOOL(n->u.node_bool_lit.v != 0);
    if (strcmp(k, "DISPATCH_node_null_lit")   == 0) return V_NULL();
    if (strcmp(k, "DISPATCH_node_str_lit")    == 0) return V_STR(n->u.node_str_lit.p, n->u.node_str_lit.p_len);
    if (strcmp(k, "DISPATCH_node_bytes_lit")  == 0) return V_BYTES(n->u.node_bytes_lit.p, n->u.node_bytes_lit.p_len);
    abort();
}

/* ---- forward decls ---- */
static NODE *parse_expr(P *p);
static NODE *parse_member(P *p);

/* ---- primary / member -------------------------------------------- */

static NODE *
parse_primary(P *const p)
{
    if (p->had_err) return NULL;
    switch (p->tok.kind) {
        case TK_INT: {
            int64_t v = p->tok.i64;
            lex(p);
            if (v >= INT32_MIN && v <= INT32_MAX) return ALLOC_node_int_lit((int32_t)v);
            return ALLOC_node_int64_lit((uint64_t)v);
        }
        case TK_UINT: {
            uint64_t v = p->tok.u64;
            lex(p);
            return ALLOC_node_uint_lit(v);
        }
        case TK_DOUBLE: {
            double v = p->tok.d;
            lex(p);
            return ALLOC_node_double_lit(v);
        }
        case TK_STR: {
            const char *s = p->tok.p; uint32_t n = p->tok.len;
            lex(p);
            return ALLOC_node_str_lit(s, n);
        }
        case TK_BYTES: {
            const char *s = p->tok.p; uint32_t n = p->tok.len;
            lex(p);
            return ALLOC_node_bytes_lit(s, n);
        }
        case TK_TRUE:  lex(p); return ALLOC_node_bool_lit(1);
        case TK_FALSE: lex(p); return ALLOC_node_bool_lit(0);
        case TK_NULL:  lex(p); return ALLOC_node_null_lit();
        case TK_LPAREN: {
            lex(p);
            NODE *e = parse_expr(p);
            expect(p, TK_RPAREN, "')'");
            return e;
        }
        case TK_LBRACK: {
            lex(p);
            NODE *items[256];
            uint32_t n = 0;
            if (p->tok.kind != TK_RBRACK) {
                while (1) {
                    if (n >= 256) { p_error(p, "list literal too long"); return NULL; }
                    NODE *e = parse_expr(p);
                    if (!e) return NULL;
                    items[n++] = e;
                    if (!accept(p, TK_COMMA)) break;
                    if (p->tok.kind == TK_RBRACK) break;
                }
            }
            expect(p, TK_RBRACK, "']'");

            /* Constant-list folding: if every element is a pure literal,
             * pre-evaluate the list at parse time and emit a single
             * `node_const_list(idx)` reference.  This skips the per-eval
             * arena alloc + per-element eval that `role in ["admin","user"]`
             * style policies pay every request. */
            bool all_lit = (n > 0);
            for (uint32_t i = 0; i < n; i++) {
                if (!is_pure_literal(items[i])) { all_lit = false; break; }
            }
            if (all_lit) {
                struct arcel_list *const cl = (struct arcel_list *)malloc(sizeof(struct arcel_list));
                cl->len   = n;
                cl->items = (VALUE *)malloc(sizeof(VALUE) * n);
                for (uint32_t i = 0; i < n; i++) cl->items[i] = eval_literal(items[i]);
                uint32_t cidx = arcel_const_list_push(cl);
                return ALLOC_node_const_list(cidx);
            }

            uint32_t idx = arcel_node_arr_push_n(items, n);
            return ALLOC_node_list(idx, n);
        }
        case TK_LBRACE: {
            lex(p);
            NODE *kv[256];
            uint32_t n = 0;
            if (p->tok.kind != TK_RBRACE) {
                while (1) {
                    if (n + 2 > 256) { p_error(p, "map literal too long"); return NULL; }
                    NODE *k = parse_expr(p);
                    if (!k) return NULL;
                    expect(p, TK_COLON, "':'");
                    NODE *v = parse_expr(p);
                    if (!v) return NULL;
                    kv[n++] = k;
                    kv[n++] = v;
                    if (!accept(p, TK_COMMA)) break;
                    if (p->tok.kind == TK_RBRACE) break;
                }
            }
            expect(p, TK_RBRACE, "'}'");
            uint32_t idx = arcel_node_arr_push_n(kv, n);
            return ALLOC_node_map(idx, n / 2);
        }
        case TK_IDENT: {
            const char *id = p->tok.p; uint32_t id_len = p->tok.len;
            lex(p);

            /* Function call form: IDENT '(' args ')' */
            if (p->tok.kind == TK_LPAREN) {
                lex(p);
                NODE *args[16];
                uint32_t n = 0;
                if (p->tok.kind != TK_RPAREN) {
                    while (1) {
                        if (n >= 16) { p_error(p, "too many args"); return NULL; }
                        NODE *e = parse_expr(p);
                        if (!e) return NULL;
                        args[n++] = e;
                        if (!accept(p, TK_COMMA)) break;
                    }
                }
                expect(p, TK_RPAREN, "')'");
                /* dispatch by name */
                #define IS(name) (id_len == sizeof(name) - 1 && memcmp(id, name, id_len) == 0)
                if (IS("size")   && n == 1) return ALLOC_node_size(args[0]);
                if (IS("type")   && n == 1) return ALLOC_node_type(args[0]);
                if (IS("int")    && n == 1) return ALLOC_node_to_int(args[0]);
                if (IS("uint")   && n == 1) return ALLOC_node_to_uint(args[0]);
                if (IS("double") && n == 1) return ALLOC_node_to_double(args[0]);
                if (IS("string") && n == 1) return ALLOC_node_to_string(args[0]);
                if (IS("bool")   && n == 1) return ALLOC_node_to_bool(args[0]);
                if (IS("bytes")  && n == 1) return ALLOC_node_to_bytes(args[0]);
                if (IS("dyn")    && n == 1) return args[0];                 /* identity */
                if (IS("has")    && n == 1) {
                    /* has() arg must be a field-access expression */
                    /* The arg has already been built; we need to peek
                     * at its kind.  Walk the kind name. */
                    const char *k = args[0]->head.kind->default_dispatcher_name;
                    if (k && (strcmp(k, "DISPATCH_node_field") == 0 ||
                              strcmp(k, "DISPATCH_node_index") == 0)) {
                        return ALLOC_node_has_select(args[0]);
                    }
                    p_error(p, "has() argument must be a field selection");
                    return NULL;
                }
                /* Unknown function: leave as a call node to surface as
                 * "no such overload" at eval time.  TODO: implement
                 * node_call with name lookup. */
                p_error(p, "unknown function '%.*s'", (int)id_len, id);
                return NULL;
                #undef IS
            }

            /* Bare identifier */
            return ALLOC_node_ident(intern_ident(id, id_len), id_len);
        }
        default:
            p_error(p, "expected primary expression (got token %d)", p->tok.kind);
            return NULL;
    }
}

static NODE *
parse_member(P *const p)
{
    NODE *recv = parse_primary(p);
    while (recv && !p->had_err) {
        if (p->tok.kind == TK_DOT) {
            lex(p);
            if (p->tok.kind != TK_IDENT) { p_error(p, "expected identifier after '.'"); return NULL; }
            const char *name = p->tok.p; uint32_t name_len = p->tok.len;
            lex(p);
            if (p->tok.kind == TK_LPAREN) {
                /* method call: recv.name(args...) */
                lex(p);
                NODE *args[16];
                uint32_t n = 0;
                if (p->tok.kind != TK_RPAREN) {
                    while (1) {
                        if (n >= 16) { p_error(p, "too many args"); return NULL; }
                        NODE *e = parse_expr(p);
                        if (!e) return NULL;
                        args[n++] = e;
                        if (!accept(p, TK_COMMA)) break;
                    }
                }
                expect(p, TK_RPAREN, "')'");
                #define IS(s) (name_len == sizeof(s) - 1 && memcmp(name, s, name_len) == 0)
                if (IS("size")        && n == 0) { recv = ALLOC_node_size(recv); continue; }
                if (IS("startsWith")  && n == 1) { recv = ALLOC_node_starts_with(recv, args[0]); continue; }
                if (IS("endsWith")    && n == 1) { recv = ALLOC_node_ends_with  (recv, args[0]); continue; }
                if (IS("contains")    && n == 1) { recv = ALLOC_node_contains   (recv, args[0]); continue; }
                if (IS("matches")     && n == 1) { recv = ALLOC_node_matches    (recv, args[0]); continue; }
                /* Macros: recv.<macro>(<id>, <body>) */
                /* transformList / transformMap (cel-spec macros2):
                 *   xs.transformList(i, v, t)        → list of t(i, v)  — 3 args
                 *   xs.transformList(i, v, p, t)     → filtered          — 4 args
                 *   m.transformMap (k, v, t)         → map of (k, t)    — 3 args
                 *   m.transformMap (k, v, p, t)      → filtered          — 4 args */
                if ((IS("transformList") || IS("transformMap")) && (n == 3 || n == 4)) {
                    const char *k0 = args[0]->head.kind->default_dispatcher_name;
                    const char *k1 = args[1]->head.kind->default_dispatcher_name;
                    if (!k0 || !k1 || strcmp(k0, "DISPATCH_node_ident") != 0 || strcmp(k1, "DISPATCH_node_ident") != 0) {
                        p_error(p, "%.*s requires (id, id, ...)", (int)name_len, name);
                        return NULL;
                    }
                    const char *bn1 = args[0]->u.node_ident.name; uint32_t bl1 = args[0]->u.node_ident.name_len;
                    const char *bn2 = args[1]->u.node_ident.name; uint32_t bl2 = args[1]->u.node_ident.name_len;
                    NODE *pred  = (n == 4) ? args[2] : NULL;
                    NODE *trans = (n == 4) ? args[3] : args[2];
                    if (IS("transformList")) {
                        recv = pred ? ALLOC_node_transform_list_f(recv, bn1, bl1, bn2, bl2, pred, trans)
                                    : ALLOC_node_map_macro2     (recv, bn1, bl1, bn2, bl2, trans);
                        continue;
                    } else {
                        recv = pred ? ALLOC_node_transform_map_f(recv, bn1, bl1, bn2, bl2, pred, trans)
                                    : ALLOC_node_transform_map  (recv, bn1, bl1, bn2, bl2, trans);
                        continue;
                    }
                }

                if ((IS("all") || IS("exists") || IS("exists_one") || IS("existsOne") ||
                     IS("filter") || IS("map")) && (n == 2 || n == 3)) {
                    /* args[0] must be an ident */
                    const char *k0 = args[0]->head.kind->default_dispatcher_name;
                    if (!k0 || strcmp(k0, "DISPATCH_node_ident") != 0) {
                        p_error(p, "macro bind variable must be an identifier"); return NULL;
                    }
                    const char *bind     = args[0]->u.node_ident.name;
                    uint32_t    bind_len = args[0]->u.node_ident.name_len;

                    /* Two-arg macros (i, v): args[1] must also be an ident
                     * (the element bind), args[2] is the body.  CEL 1.x extension
                     * (cel-spec macros2 file).  Single-arg macros take (var, body). */
                    if (n == 3 && (IS("all") || IS("exists") || IS("exists_one") || IS("existsOne") ||
                                   IS("filter") || IS("map"))) {
                        const char *k1 = args[1]->head.kind->default_dispatcher_name;
                        bool two_arg_form = (k1 && strcmp(k1, "DISPATCH_node_ident") == 0);
                        if (two_arg_form) {
                            const char *bind2     = args[1]->u.node_ident.name;
                            uint32_t    bind2_len = args[1]->u.node_ident.name_len;
                            if (IS("all"))        { recv = ALLOC_node_all2       (recv, bind, bind_len, bind2, bind2_len, args[2]); continue; }
                            if (IS("exists"))     { recv = ALLOC_node_exists2    (recv, bind, bind_len, bind2, bind2_len, args[2]); continue; }
                            if (IS("exists_one") || IS("existsOne")) { recv = ALLOC_node_exists_one2(recv, bind, bind_len, bind2, bind2_len, args[2]); continue; }
                            if (IS("filter"))     { recv = ALLOC_node_filter2    (recv, bind, bind_len, bind2, bind2_len, args[2]); continue; }
                            if (IS("map"))        { recv = ALLOC_node_map_macro2 (recv, bind, bind_len, bind2, bind2_len, args[2]); continue; }
                        }
                    }

                    if (IS("all"))        { recv = ALLOC_node_all       (recv, bind, bind_len, args[1]); continue; }
                    if (IS("exists"))     { recv = ALLOC_node_exists    (recv, bind, bind_len, args[1]); continue; }
                    if (IS("exists_one") || IS("existsOne")) { recv = ALLOC_node_exists_one(recv, bind, bind_len, args[1]); continue; }
                    if (IS("filter"))     { recv = ALLOC_node_filter    (recv, bind, bind_len, args[1]); continue; }
                    if (IS("map") && n == 2) { recv = ALLOC_node_map_macro(recv, bind, bind_len, args[1]); continue; }
                    if (IS("map") && n == 3) {
                        /* map(x, p, t) → filter(x, p).map(x, t) — desugar */
                        NODE *flt = ALLOC_node_filter(recv, bind, bind_len, args[1]);
                        recv = ALLOC_node_map_macro(flt, bind, bind_len, args[2]);
                        continue;
                    }
                }
                p_error(p, "unknown method '%.*s' (or wrong arity)", (int)name_len, name);
                return NULL;
                #undef IS
            }
            /* plain field access */
            recv = ALLOC_node_field(recv, intern_ident(name, name_len), name_len);
        } else if (p->tok.kind == TK_LBRACK) {
            lex(p);
            NODE *k = parse_expr(p);
            expect(p, TK_RBRACK, "']'");
            recv = ALLOC_node_index(recv, k);
        } else {
            break;
        }
    }
    return recv;
}

/* ---- precedence climb -------------------------------------------- */

static NODE *
parse_unary(P *const p)
{
    if (p->tok.kind == TK_NOT) {
        lex(p);
        NODE *v = parse_unary(p);
        return v ? ALLOC_node_not(v) : NULL;
    }
    if (p->tok.kind == TK_MINUS) {
        lex(p);
        /* Special-case `-INT_LIT` and `-DOUBLE_LIT` so we can fold the
         * sign into the literal at parse time.  This matters for
         * `-9223372036854775808` (= INT64_MIN), which can't be expressed
         * as `-(positive)` because positive INT64_MAX+1 isn't a valid
         * int64 — and for `-0.0` so we get an exact negative zero. */
        if (p->tok.kind == TK_INT) {
            uint64_t bits = p->tok.u64;
            lex(p);
            /* Negate as int64.  bits == INT64_MAX+1 == 0x8000000000000000
             * cleanly bit-casts to INT64_MIN and the negate (as
             * unsigned) wraps to itself, which is what we want. */
            int64_t v = -(int64_t)bits;
            if (v >= INT32_MIN && v <= INT32_MAX) return ALLOC_node_int_lit((int32_t)v);
            return ALLOC_node_int64_lit((uint64_t)v);
        }
        if (p->tok.kind == TK_DOUBLE) {
            double v = -p->tok.d;
            lex(p);
            return ALLOC_node_double_lit(v);
        }
        NODE *v = parse_unary(p);
        return v ? ALLOC_node_neg(v) : NULL;
    }
    return parse_member(p);
}

static NODE *
parse_mul(P *const p)
{
    NODE *l = parse_unary(p);
    while (l && !p->had_err) {
        tok_kind k = p->tok.kind;
        if (k != TK_STAR && k != TK_SLASH && k != TK_PERCENT) break;
        lex(p);
        NODE *r = parse_unary(p);
        if (!r) return NULL;
        l = (k == TK_STAR) ? ALLOC_node_mul(l, r)
          : (k == TK_SLASH) ? ALLOC_node_div(l, r)
          : ALLOC_node_mod(l, r);
    }
    return l;
}

static NODE *
parse_add(P *const p)
{
    NODE *l = parse_mul(p);
    while (l && !p->had_err) {
        tok_kind k = p->tok.kind;
        if (k != TK_PLUS && k != TK_MINUS) break;
        lex(p);
        NODE *r = parse_mul(p);
        if (!r) return NULL;
        l = (k == TK_PLUS) ? ALLOC_node_add(l, r) : ALLOC_node_sub(l, r);
    }
    return l;
}

static NODE *
parse_rel(P *const p)
{
    NODE *l = parse_add(p);
    if (!l || p->had_err) return l;
    tok_kind k = p->tok.kind;
    if (k == TK_EQ || k == TK_NE || k == TK_LT || k == TK_LE ||
        k == TK_GT || k == TK_GE || k == TK_IN) {
        lex(p);
        NODE *r = parse_add(p);
        if (!r) return NULL;
        switch (k) {
            case TK_EQ: return ALLOC_node_eq(l, r);
            case TK_NE: return ALLOC_node_ne(l, r);
            case TK_LT: return ALLOC_node_lt(l, r);
            case TK_LE: return ALLOC_node_le(l, r);
            case TK_GT: return ALLOC_node_gt(l, r);
            case TK_GE: return ALLOC_node_ge(l, r);
            case TK_IN: return ALLOC_node_in_op(l, r);
            default:    return NULL;
        }
    }
    return l;
}

static NODE *
parse_and(P *const p)
{
    NODE *l = parse_rel(p);
    while (l && !p->had_err && p->tok.kind == TK_AND) {
        lex(p);
        NODE *r = parse_rel(p);
        if (!r) return NULL;
        l = ALLOC_node_and(l, r);
    }
    return l;
}

static NODE *
parse_or(P *const p)
{
    NODE *l = parse_and(p);
    while (l && !p->had_err && p->tok.kind == TK_OR) {
        lex(p);
        NODE *r = parse_and(p);
        if (!r) return NULL;
        l = ALLOC_node_or(l, r);
    }
    return l;
}

static NODE *
parse_expr(P *const p)
{
    NODE *cond = parse_or(p);
    if (!cond || p->had_err) return cond;
    if (p->tok.kind == TK_QMARK) {
        lex(p);
        NODE *t = parse_or(p);
        if (!t) return NULL;
        expect(p, TK_COLON, "':'");
        NODE *f = parse_expr(p);
        if (!f) return NULL;
        return ALLOC_node_cond(cond, t, f);
    }
    return cond;
}

NODE *
arcel_parse_n(const char *const src, const uint32_t len, const char **const out_err)
{
    static char err_storage[sizeof(((P){0}).err_buf)];
    *out_err = NULL;

    P p = (P){
        .src     = src,
        .pos     = 0,
        .len     = len,
        .line    = 1,
        .col     = 1,
        .had_err = false,
    };
    lex(&p);
    NODE *root = parse_expr(&p);
    if (p.had_err || !root) {
        memcpy(err_storage, p.err_buf, sizeof(err_storage));
        *out_err = err_storage;
        return NULL;
    }
    if (p.tok.kind != TK_EOF) {
        snprintf(err_storage, sizeof(err_storage),
                 "<input>:%d:%d: trailing tokens", p.line, p.col);
        *out_err = err_storage;
        return NULL;
    }
    return root;
}

NODE *
arcel_parse(const char *const src, const char **const out_err)
{
    return arcel_parse_n(src, (uint32_t)strlen(src), out_err);
}
